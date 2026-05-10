/**
 * @file TES.c
 * @brief 时间与事件调度器 (TES) - 实现文件
 *        Time and Event Scheduler (TES) - Implementation file
 * 
 * @details 实现了一个轻量级协作式任务调度器，包含时间触发任务和事件触发任务的管理、
 *          交替调度策略、环形绝对时间轴、任务间通信等功能。所有内部函数均为静态，
 *          仅通过 TES_Init() 注册到全局 API 结构体 tes 中供用户调用。
 *          
 *          本版本相比早期版本：
 *          - 采用环形绝对 tick 时间轴（next_tick 存储绝对时刻），正确应对 tick 溢出
 *          - SysTick 仅递增全局计数器，不再遍历任务列表
 *          - 调度器在任务执行后读取当前 tick 并计算下次执行时刻，消除累积误差
 *          - 仅保留交替调度策略（每次调度执行一个事件任务 + 一个时间任务）
 *          
 *          Implements a lightweight cooperative task scheduler, including management of time-triggered
 *          and event-triggered tasks, alternating scheduling policy, circular absolute timeline,
 *          inter-task communication, etc. All internal functions are static and registered to the
 *          global API structure 'tes' via TES_Init().
 *          
 *          Improvements over earlier versions:
 *          - Uses circular absolute tick timeline (next_tick stores absolute time), handles tick overflow
 *          - SysTick only increments global counter, no task list traversal
 *          - Scheduler reads current tick after task execution and calculates next time, eliminating drift
 *          - Only alternating scheduling policy (one event task + one time task per schedule)
 * 
 * @see TES.h
 */

#include "TES.h"

// ==================== 全局 API 实例 ====================
// ==================== Global API Instance ====================

SchedulerAPI tes;   ///< 对外提供的 API 结构体 / Externally provided API structure

// ==================== 私有数据存储 ====================
// ==================== Private Data Storage ====================

/**
 * @brief 调度器内部数据结构
 * @brief Scheduler internal data structure
 * @details 包含对外 API 指针和私有任务控制块。
 * @details Contains public API pointers and private task control blocks.
 */
static struct {
    SchedulerAPI Public;        ///< 公共 API 指针集合 / Public API pointer set
    volatile uint16_t system_tick;       ///< 全局系统 tick，每次调用 tes.tick() 递增 / Global system tick, incremented on each tes.tick()

    struct {
        /**
         * @brief 时间任务控制块数组
         * @brief Time task control block array
         */
        struct {
            void (*entry)(void);    ///< 任务函数指针 / Task function pointer
            uint16_t taskcyc;       ///< 周期（单位：tick） / Period (unit: tick)
            uint16_t next_tick;     ///< 下次执行的绝对 tick 时刻（环形时间轴） / Absolute tick of next execution (circular timeline)
            TaskState taskflag;     ///< 任务状态 / Task state
            uint16_t cache;         ///< 数据缓存区（通信） / Data cache (communication)
        } time_list[TASK_MAX];

        /**
         * @brief 事件任务控制块数组
         * @brief Event task control block array
         */
        struct {
            void (*entry)(void);    ///< 任务函数指针 / Task function pointer
            TaskState taskflag;     ///< 任务状态（增加 READY 态） / Task state (adds READY state)
            uint16_t cache;         ///< 数据缓存区 / Data cache
        } event_list[TASK_MAX];

        uint8_t time_num;           ///< 当前已创建的时间任务数量 / Number of currently created time tasks
        uint8_t event_num;          ///< 当前已创建的事件任务数量 / Number of currently created event tasks

    } pri;  ///< 私有部分 / Private part

} dat;

// ==================== 环形时间比较宏 ====================
// ==================== Circular Time Comparison Macro ====================

/**
 * @brief 环形时间比较宏，正确处理 16 位无符号溢出
 * @brief Circular time comparison macro, correctly handles 16-bit unsigned overflow
 * @param now  当前系统 tick / Current system tick
 * @param next 下次执行的绝对时刻 / Absolute time of next execution
 * @return 非0表示已到期（now >= next） / Non-zero indicates timeout (now >= next)
 * @note 只要实际时间差 < 2^15 tick，比较结果正确，一般任务周期远小于此值。
 * @note Works correctly as long as the actual time difference < 2^15 ticks.
 */
#define TICK_TIMEOUT(now, next)  ((int16_t)((now) - (next)) >= 0)

// ==================== 私有函数实现 ====================
// ==================== Private Function Implementations ====================

/**
 * @brief 查找任务在对应列表中的索引
 * @brief Find the index of a task in the corresponding list
 * @param entry 任务函数指针 / Task function pointer
 * @param type  任务类型（TIME 或 EVENT） / Task type (TIME or EVENT)
 * @return 索引值（>=0）表示成功，-1 表示未找到或参数无效 / Index (>=0) on success, -1 if not found or invalid parameter
 */
static int8_t SearchIndex(void (*entry)(void), TaskType type) {
    uint8_t i;

    if (entry == 0) return -1;

    if (type == TIME) {
        for (i = 0; i < dat.pri.time_num; i++) {
            if (dat.pri.time_list[i].entry == entry) return i;
        }
    } else { // EVENT
        for (i = 0; i < dat.pri.event_num; i++) {
            if (dat.pri.event_list[i].entry == entry) return i;
        }
    }
    return -1;
}

/**
 * @brief 滴答定时器中断服务函数（需挂载到硬件定时器中断）
 * @brief Tick timer interrupt service function (to be attached to hardware timer interrupt)
 * @note  只递增全局 system_tick，极轻量。
 * @note  Only increments global system_tick, very lightweight.
 */
static void SysTick(void) {
    dat.system_tick++;
}

/**
 * @brief 交替调度策略核心
 * @brief Alternate scheduling policy core
 * @details 每次调度执行一个事件任务和一个时间任务，循环交替。
 *          静态索引实现轮询，带有边界安全检查，防止数组越界。
 *          对 16 位 system_tick 的读取进行临界区保护，防止 8 位机撕裂。
 * @details Executes one event task and one time task per schedule, alternating.
 *          Static indices implement round-robin with boundary checks to prevent array overflow.
 *          Critical section protection for reading 16-bit system_tick to prevent tearing on 8-bit MCUs.
 */
static void sch_alt(void) {
    static uint8_t event_i = 0, time_i = 0;
    static uint8_t alt_phase = 0;   /* 0: 先事件后时间 / event first then time, 1: 先时间后事件 / time first then event */
    uint8_t i;
    uint8_t idx;
    uint16_t now;   /* 存放安全的系统 tick 快照 / Safe system tick snapshot */

    /* ----- 阶段0：执行一个就绪的事件任务 / Phase 0: execute one ready event task ----- */
    if (alt_phase == 0) {
        if (event_i >= dat.pri.event_num) event_i = 0;

        for (i = 0; i < dat.pri.event_num; i++) {
            idx = event_i++;
            if (event_i >= dat.pri.event_num) event_i = 0;

            if (dat.pri.event_list[idx].taskflag == SUSPEND) continue;
            if (dat.pri.event_list[idx].taskflag != READY) continue;

            /* 执行事件任务 / Execute event task */
            dat.pri.event_list[idx].taskflag = RUN;
            dat.pri.event_list[idx].entry();
            if (dat.pri.event_list[idx].taskflag == RUN) {
                dat.pri.event_list[idx].taskflag = NOT_RUN;
            }

            alt_phase = 1;  /* 下一轮执行时间任务 / Next round execute time task */
            break;
        }
        if (i >= dat.pri.event_num) alt_phase = 1;   /* 无事件任务就绪 / No event task ready */
    }

    /* ----- 阶段1：执行一个到点的时间任务 / Phase 1: execute one due time task ----- */
    if (alt_phase == 1) {
        if (time_i >= dat.pri.time_num) time_i = 0;

        for (i = 0; i < dat.pri.time_num; i++) {
            idx = time_i++;
            if (time_i >= dat.pri.time_num) time_i = 0;

            if (dat.pri.time_list[idx].taskflag == SUSPEND) continue;

            /* 原子读取系统 tick（防止 8 位机撕裂） / Atomic read of system tick (prevents tearing on 8-bit MCUs) */
            TIMER_INTERRUPT_DISABLE();
            now = dat.system_tick;
            TIMER_INTERRUPT_ENABLE();

            /* 环形时间比较：若未到执行时刻则跳过 / Circular time comparison: skip if not yet due */
            if (!TICK_TIMEOUT(now, dat.pri.time_list[idx].next_tick)) continue;

            /* 执行时间任务 / Execute time task */
            dat.pri.time_list[idx].taskflag = RUN;
            dat.pri.time_list[idx].entry();

            /**
             * @brief 防御性处理：若任务内部执行了删除/挂起自身，则 taskflag 不再是 RUN，
             *        此时不应再修改 next_tick，避免污染被尾部覆盖的新任务。
             * @brief Defensive handling: if the task deletes/suspends itself inside, taskflag is no longer RUN,
             *        then next_tick should not be modified to avoid polluting the new task overwritten at the tail.
             */
            if (dat.pri.time_list[idx].taskflag == RUN) {
                dat.pri.time_list[idx].taskflag = NOT_RUN;

                /* 重新获取当前系统 tick 并设置下次执行时刻 / Re-read current system tick and set next execution time */
                TIMER_INTERRUPT_DISABLE();
                now = dat.system_tick;
                TIMER_INTERRUPT_ENABLE();
                dat.pri.time_list[idx].next_tick = now + dat.pri.time_list[idx].taskcyc;
            }

            alt_phase = 0;  /* 下一轮执行事件任务 / Next round execute event task */
            break;
        }
        if (i >= dat.pri.time_num) alt_phase = 0;   /* 无时间任务到点 / No time task due */
    }
}

/**
 * @brief 调度器入口函数
 * @brief Scheduler entry function
 */
static void Scheduler(void) {
    sch_alt();
}

// ==================== API 实现 ====================
// ==================== API Implementations ====================

/**
 * @brief 创建时间触发任务
 * @brief Create a time-triggered task
 * @param entry 任务函数指针 / Task function pointer
 * @param time  执行周期（tick 数，不能为 0） / Execution period (number of ticks, cannot be 0)
 * @return 操作结果 / Operation result
 * @note 第一次执行将延迟一个周期。 / The first execution will be delayed by one period.
 */
static FCstate Create_time(void (*entry)(void), uint16_t time) {
    uint16_t now;
    int8_t idx;

    if (time == 0) return OPS_NO;
    if (SearchIndex(entry, TIME) >= 0) return OPS_NO;
    if (dat.pri.time_num >= TASK_MAX) return OPS_NO;
    if (entry == 0) return OPS_NO;

    TIMER_INTERRUPT_DISABLE();
    now = dat.system_tick;
    idx = dat.pri.time_num;
    dat.pri.time_list[idx].entry     = entry;
    dat.pri.time_list[idx].taskcyc   = time;
    dat.pri.time_list[idx].next_tick = now + time;   /* 第一次执行延迟一个周期 / First execution delayed by one period */
    dat.pri.time_list[idx].taskflag  = NOT_RUN;
    dat.pri.time_list[idx].cache     = 0xFFFF;
    dat.pri.time_num++;
    TIMER_INTERRUPT_ENABLE();
    return OPS_OK;
}

/**
 * @brief 创建事件触发任务
 * @brief Create an event-triggered task
 * @param entry 任务函数指针 / Task function pointer
 * @return 操作结果 / Operation result
 */
static FCstate Create_event(void (*entry)(void)) {
    int8_t idx;

    if (entry == 0) return OPS_NO;
    if (SearchIndex(entry, EVENT) >= 0) return OPS_NO;
    if (dat.pri.event_num >= TASK_MAX) return OPS_NO;

    TIMER_INTERRUPT_DISABLE();
    idx = dat.pri.event_num;
    dat.pri.event_list[idx].entry    = entry;
    dat.pri.event_list[idx].taskflag = NOT_RUN;
    dat.pri.event_list[idx].cache    = 0xFFFF;
    dat.pri.event_num++;
    TIMER_INTERRUPT_ENABLE();
    return OPS_OK;
}

/**
 * @brief 删除任务
 * @brief Delete a task
 * @param entry 要删除的任务函数指针 / Task function pointer to delete
 * @return 操作结果 / Operation result
 * @note 采用末尾元素覆盖法，O(1) 复杂度。支持任务自删除（末尾覆盖保证安全）。
 * @note Uses the last-element overwrite method, O(1) complexity. Supports task self-deletion (tail overwrite ensures safety).
 */
static FCstate TaskDel(void (*entry)(void)) {
    int8_t index;

    if (entry == 0) return OPS_NO;

    // 删除时间任务 / Delete time task
    if ((index = SearchIndex(entry, TIME)) >= 0) {
        TIMER_INTERRUPT_DISABLE();
        dat.pri.time_num--;
        dat.pri.time_list[index] = dat.pri.time_list[dat.pri.time_num];
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }

    // 删除事件任务 / Delete event task
    if ((index = SearchIndex(entry, EVENT)) >= 0) {
        TIMER_INTERRUPT_DISABLE();
        dat.pri.event_num--;
        dat.pri.event_list[index] = dat.pri.event_list[dat.pri.event_num];
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }

    return OPS_NO;
}

/**
 * @brief 修改时间任务的周期
 * @brief Change the period of a time-triggered task
 * @param entry   任务函数指针 / Task function pointer
 * @param newtime 新周期（tick 数，不能为 0） / New period (number of ticks, cannot be 0)
 * @return 操作结果 / Operation result
 * @note 新周期从下次执行开始生效，不影响当前已安排的 next_tick。
 * @note The new period takes effect from the next execution, does not affect the already scheduled next_tick.
 */
static FCstate TaskCycle(void (*entry)(void), uint16_t newtime) {
    int8_t index;

    if (newtime == 0) return OPS_NO;
    if (entry == 0) return OPS_NO;

    index = SearchIndex(entry, TIME);
    if (index == -1) return OPS_NO;

    dat.pri.time_list[index].taskcyc = newtime;
    return OPS_OK;
}

/**
 * @brief 挂起任务
 * @brief Suspend a task
 * @param entry 任务函数指针 / Task function pointer
 * @return 操作结果 / Operation result
 */
static FCstate TaskSuspend(void (*entry)(void)) {
    int8_t index;

    if (entry == 0) return OPS_NO;

    if ((index = SearchIndex(entry, TIME)) >= 0) {
        TIMER_INTERRUPT_DISABLE();
        dat.pri.time_list[index].taskflag = SUSPEND;
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }

    if ((index = SearchIndex(entry, EVENT)) >= 0) {
        TIMER_INTERRUPT_DISABLE();
        dat.pri.event_list[index].taskflag = SUSPEND;
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }

    return OPS_NO;
}

/**
 * @brief 恢复被挂起的任务
 * @brief Resume a suspended task
 * @param entry 任务函数指针 / Task function pointer
 * @return 操作结果 / Operation result
 * @note 对于时间任务，恢复后重新开始计时（当前 tick + 周期）。
 * @note For time tasks, restart counting from current tick + period.
 */
static FCstate TaskRecover(void (*entry)(void)) {
    int8_t index;

    if (entry == 0) return OPS_NO;

    if ((index = SearchIndex(entry, TIME)) >= 0) {
        if (dat.pri.time_list[index].taskflag != SUSPEND) return OPS_NO;
        TIMER_INTERRUPT_DISABLE();
        dat.pri.time_list[index].taskflag = NOT_RUN;
        dat.pri.time_list[index].next_tick = dat.system_tick + dat.pri.time_list[index].taskcyc;
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }

    if ((index = SearchIndex(entry, EVENT)) >= 0) {
        if (dat.pri.event_list[index].taskflag != SUSPEND) return OPS_NO;
        TIMER_INTERRUPT_DISABLE();
        dat.pri.event_list[index].taskflag = NOT_RUN;
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }

    return OPS_NO;
}

/**
 * @brief 发布事件（触发事件任务执行）
 * @brief Publish an event (trigger an event task)
 * @param entry 事件任务函数指针 / Event task function pointer
 * @return 操作结果（任务状态不为 NOT_RUN 时会失败） / Operation result (fails if task state is not NOT_RUN)
 */
static FCstate TaskRelease(void (*entry)(void)) {
    int8_t index;

    if (entry == 0) return OPS_NO;

    index = SearchIndex(entry, EVENT);
    if (index == -1) return OPS_NO;

    if (dat.pri.event_list[index].taskflag != NOT_RUN) return OPS_NO;

    TIMER_INTERRUPT_DISABLE();
    dat.pri.event_list[index].taskflag = READY;
    TIMER_INTERRUPT_ENABLE();

    return OPS_OK;
}

/**
 * @brief 向指定任务发送数据
 * @brief Send data to a specified task
 * @param entry 接收任务函数指针 / Receiver task function pointer
 * @param d     要发送的数据（0xFFFF 为保留值，表示无数据） / Data to send (0xFFFF is reserved, indicates no data)
 * @return 操作结果 / Operation result
 */
static FCstate Senddat(void (*entry)(void), uint16_t d) {
    int8_t index;

    if (entry == 0) return OPS_NO;
    if (d == 0xFFFF) return OPS_NO;

    if ((index = SearchIndex(entry, TIME)) >= 0) {
        dat.pri.time_list[index].cache = d;
        return OPS_OK;
    }

    if ((index = SearchIndex(entry, EVENT)) >= 0) {
        dat.pri.event_list[index].cache = d;
        return OPS_OK;
    }

    return OPS_NO;
}

/**
 * @brief 接收本任务的数据
 * @brief Receive data for the current task
 * @param entry 本任务函数指针 / Current task function pointer
 * @param mode  读取模式（READ_ONLY 或 AUTO_CLEAR） / Read mode (READ_ONLY or AUTO_CLEAR)
 * @return 缓存中的数据，0xFFFF 表示无数据或错误 / Data in cache, 0xFFFF indicates no data or error
 */
static uint16_t Receive(void (*entry)(void), ReceiveMode mode) {
    int8_t index;
    uint16_t d;

    if (entry == 0) return 0xFFFF;
    if (mode != AUTO_CLEAR && mode != READ_ONLY) return 0xFFFF;

    // 尝试时间任务 / Try time task
    if ((index = SearchIndex(entry, TIME)) >= 0) {
        d = dat.pri.time_list[index].cache;
        if (mode == AUTO_CLEAR) {
            dat.pri.time_list[index].cache = 0xFFFF;
        }
        return d;
    }

    // 尝试事件任务 / Try event task
    if ((index = SearchIndex(entry, EVENT)) >= 0) {
        d = dat.pri.event_list[index].cache;
        if (mode == AUTO_CLEAR) {
            dat.pri.event_list[index].cache = 0xFFFF;
        }
        return d;
    }

    return 0xFFFF;
}

/**
 * @brief 清空任务的数据缓存区
 * @brief Clear the task's data cache
 * @param entry 任务函数指针 / Task function pointer
 * @return 操作结果 / Operation result
 */
static FCstate Clear(void (*entry)(void)) {
    int8_t index;

    if (entry == 0) return OPS_NO;

    if ((index = SearchIndex(entry, TIME)) >= 0) {
        dat.pri.time_list[index].cache = 0xFFFF;
        return OPS_OK;
    }

    if ((index = SearchIndex(entry, EVENT)) >= 0) {
        dat.pri.event_list[index].cache = 0xFFFF;
        return OPS_OK;
    }

    return OPS_NO;
}

// ==================== 初始化函数 ====================
// ==================== Initialization Function ====================

/**
 * @brief 调度器初始化函数
 * @brief Scheduler initialization function
 * @details 必须在创建任何任务之前调用。
 *          Must be called before creating any tasks.
 *          将内部函数注册到全局 API 结构体 tes 中，并清空任务计数器和系统 tick。
 *          Registers internal functions into the global API structure 'tes' and clears task counters and system tick.
 */
void TES_Init(void) {
    dat.system_tick = 0;
    dat.pri.time_num = 0;
    dat.pri.event_num = 0;

    dat.Public.tick         = SysTick;
    dat.Public.scheduler    = Scheduler;
    dat.Public.create_time  = Create_time;
    dat.Public.create_event = Create_event;
    dat.Public.del          = TaskDel;
    dat.Public.cycle        = TaskCycle;
    dat.Public.suspend      = TaskSuspend;
    dat.Public.recovery     = TaskRecover;
    dat.Public.release      = TaskRelease;
    dat.Public.send         = Senddat;
    dat.Public.receive      = Receive;
    dat.Public.clear        = Clear;

    tes = dat.Public;
}