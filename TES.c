/**
 * @file TES.c
 * @brief 时间与事件调度器 (TES) - 实现文件
 *        Time and Event Scheduler (TES) - Implementation file
 * 
 * @details 实现了一个轻量级协作式任务调度器，包含时间触发任务和事件触发任务的管理、
 *          两种调度策略（批处理/交替）、任务间通信等功能。所有内部函数均为静态，
 *          仅通过 TES_Init() 注册到全局 API 结构体 tes 中供用户调用。
 *          Implements a lightweight cooperative task scheduler, including management of time-triggered
 *          and event-triggered tasks, two scheduling policies (batch/alternate), inter-task communication, etc.
 *          All internal functions are static and registered to the global API structure 'tes' via TES_Init().
 * 
 * @note 调度器的核心数据（任务列表、状态等）存储在静态结构体 dat 中，对外不可见。
 *       The scheduler's core data (task lists, states, etc.) is stored in the static structure 'dat',
 *       invisible to the outside.
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
    SchedulerAPI Public;    ///< 公共 API 指针集合 / Public API pointer set

    struct {
        /**
         * @brief 时间任务控制块数组
         * @brief Time task control block array
         */
        struct {
            void (*entry)(void);    ///< 任务函数指针 / Task function pointer
            uint16_t taskcyc;       ///< 周期（单位：tick） / Period (unit: tick)
            uint16_t tasktick;      ///< 当前倒计时（减到 0 时执行） / Current countdown (executes when reaches 0)
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

        uint8_t time_num;   ///< 当前已创建的时间任务数量 / Number of currently created time tasks
        uint8_t event_num;  ///< 当前已创建的事件任务数量 / Number of currently created event tasks

    } pri;  ///< 私有部分 / Private part

} dat;

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
 * @brief 滴答计时器更新函数
 * @brief Tick timer update function
 * @details 应由硬件定时器中断周期性调用（如每 1ms）。
 *          Should be called periodically by a hardware timer interrupt (e.g., every 1ms).
 *          遍历所有未挂起的时间任务，将 tasktick 减 1（若大于 0）。
 *          Iterates through all non-suspended time tasks and decrements tasktick by 1 (if > 0).
 */
static void SysTick(void) {
    uint8_t i;
    for (i = 0; i < dat.pri.time_num; i++) {
        if (dat.pri.time_list[i].taskflag == SUSPEND) continue;
        if (dat.pri.time_list[i].tasktick > 0) {
            dat.pri.time_list[i].tasktick--;
        }
    }
}

/**
 * @brief 交替调度策略
 * @brief Alternate scheduling policy
 * @details 每次调度执行一个事件任务和一个时间任务，交替进行。
 *          Executes one event task and one time task per schedule, alternating.
 *          使用静态索引记录上次查找位置，并包含边界检查以防止数组越界。
 *          Uses static indices to record the last lookup position and includes boundary checks to prevent array overflow.
 */
static void sch_alt(void) {
    static uint8_t event_i = 0, time_i = 0;
    static uint8_t alt_phase = 0;   // 0:先事件后时间, 1:先时间后事件 / 0: event first then time, 1: time first then event

    // 阶段0：查找并执行一个事件任务 / Phase 0: find and execute one event task
    if (alt_phase == 0) {
        uint8_t i;

        // 边界检查：防止索引越界 / Boundary check: prevent index out of bounds
        if (event_i >= dat.pri.event_num) event_i = 0;

        for (i = 0; i < dat.pri.event_num; i++) {
            uint8_t index = event_i;
            event_i++;
            if (event_i >= dat.pri.event_num) event_i = 0;

            if (dat.pri.event_list[index].taskflag == SUSPEND) continue;
            if (dat.pri.event_list[index].taskflag != READY) continue;

            // 执行事件任务 / Execute event task
            dat.pri.event_list[index].taskflag = RUN;
            dat.pri.event_list[index].entry();
            if (dat.pri.event_list[index].taskflag == RUN) {
                dat.pri.event_list[index].taskflag = NOT_RUN;
            }

            alt_phase = 1;  // 切换到时间任务阶段 / Switch to time task phase
            break;
        }

        if (i >= dat.pri.event_num) alt_phase = 1;   // 无事件任务就绪 / No event task ready
    }

    // 阶段1：查找并执行一个时间任务 / Phase 1: find and execute one time task
    if (alt_phase == 1) {
        uint8_t i;

        if (time_i >= dat.pri.time_num) time_i = 0;

        for (i = 0; i < dat.pri.time_num; i++) {
            uint8_t index = time_i;
            time_i++;
            if (time_i >= dat.pri.time_num) time_i = 0;

            if (dat.pri.time_list[index].taskflag == SUSPEND) continue;
            if (dat.pri.time_list[index].tasktick != 0) continue;

            // 重载周期 / Reload period
            dat.pri.time_list[index].tasktick = dat.pri.time_list[index].taskcyc;

            // 执行时间任务 / Execute time task
            dat.pri.time_list[index].taskflag = RUN;
            dat.pri.time_list[index].entry();
            if (dat.pri.time_list[index].taskflag == RUN) {
                dat.pri.time_list[index].taskflag = NOT_RUN;
            }

            alt_phase = 0;  // 切换回事件任务阶段 / Switch back to event task phase
            break;
        }

        if (i >= dat.pri.time_num) alt_phase = 0;   // 无时间任务到点 / No time task due
    }
}

/**
 * @brief 批处理调度策略
 * @brief Batch scheduling policy
 * @details 先执行所有就绪的事件任务，再执行所有到点的时间任务。
 *          Executes all ready event tasks first, then all due time tasks.
 *          每个任务在一个调度周期内最多执行一次。
 *          Each task is executed at most once per scheduling cycle.
 */
static void sch_batch(void) {
    uint8_t i;

    // 执行所有就绪的事件任务 / Execute all ready event tasks
    for (i = 0; i < dat.pri.event_num; i++) {
        if (dat.pri.event_list[i].taskflag == SUSPEND) continue;
        if (dat.pri.event_list[i].taskflag != READY) continue;

        dat.pri.event_list[i].taskflag = RUN;
        dat.pri.event_list[i].entry();
        if (dat.pri.event_list[i].taskflag == RUN) {
            dat.pri.event_list[i].taskflag = NOT_RUN;
        }
    }

    // 执行所有到点的时间任务 / Execute all due time tasks
    for (i = 0; i < dat.pri.time_num; i++) {
        if (dat.pri.time_list[i].taskflag == SUSPEND) continue;
        if (dat.pri.time_list[i].tasktick != 0) continue;

        dat.pri.time_list[i].tasktick = dat.pri.time_list[i].taskcyc;
        dat.pri.time_list[i].taskflag = RUN;
        dat.pri.time_list[i].entry();
        if (dat.pri.time_list[i].taskflag == RUN) {
            dat.pri.time_list[i].taskflag = NOT_RUN;
        }
    }
}

/**
 * @brief 调度器入口函数
 * @brief Scheduler entry function
 * @details 根据编译时选择的调度策略调用相应的实现。
 * @details Calls the corresponding implementation based on the scheduling policy selected at compile time.
 */
static void Scheduler(void) {
#if SCHEDULER_POLICY == SCHEDULER_POLICY_BAT
    sch_batch();
#elif SCHEDULER_POLICY == SCHEDULER_POLICY_ALT
    sch_alt();
#endif
}

/**
 * @brief 创建时间触发任务
 * @brief Create a time-triggered task
 * @param entry 任务函数指针 / Task function pointer
 * @param time  执行周期（tick 数，不能为 0） / Execution period (number of ticks, cannot be 0)
 * @return 操作结果 / Operation result
 */
static FCstate Create_time(void (*entry)(void), uint16_t time) {
    if (time == 0) return OPS_NO;
    if (SearchIndex(entry, TIME) >= 0) return OPS_NO;
    if (dat.pri.time_num >= TASK_MAX) return OPS_NO;
    if (entry == 0) return OPS_NO;

    TIMER_INTERRUPT_DISABLE();

    dat.pri.time_list[dat.pri.time_num].entry   = entry;
    dat.pri.time_list[dat.pri.time_num].taskcyc = time;
    dat.pri.time_list[dat.pri.time_num].tasktick = time;
    dat.pri.time_list[dat.pri.time_num].taskflag = NOT_RUN;
    dat.pri.time_list[dat.pri.time_num].cache   = 0xFFFF;
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
    if (entry == 0) return OPS_NO;
    if (SearchIndex(entry, EVENT) >= 0) return OPS_NO;
    if (dat.pri.event_num >= TASK_MAX) return OPS_NO;

    TIMER_INTERRUPT_DISABLE();

    dat.pri.event_list[dat.pri.event_num].entry    = entry;
    dat.pri.event_list[dat.pri.event_num].taskflag = NOT_RUN;
    dat.pri.event_list[dat.pri.event_num].cache    = 0xFFFF;
    dat.pri.event_num++;

    TIMER_INTERRUPT_ENABLE();
    return OPS_OK;
}

/**
 * @brief 删除任务
 * @brief Delete a task
 * @param entry 要删除的任务函数指针 / Task function pointer to delete
 * @return 操作结果 / Operation result
 * @note 采用末尾元素覆盖法，O(1) 复杂度。
 * @note Uses the last-element overwrite method, O(1) complexity.
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
 */
static FCstate TaskRecover(void (*entry)(void)) {
    int8_t index;

    if (entry == 0) return OPS_NO;

    if ((index = SearchIndex(entry, TIME)) >= 0) {
        if (dat.pri.time_list[index].taskflag != SUSPEND) return OPS_NO;
        TIMER_INTERRUPT_DISABLE();
        dat.pri.time_list[index].taskflag = NOT_RUN;
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
    uint8_t index;

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
    uint8_t index;

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
    uint8_t index;

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
 *          将内部函数注册到全局 API 结构体 tes 中，并清空任务计数器。
 *          Registers internal functions into the global API structure 'tes' and clears task counters.
 */
void TES_Init(void) {
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

    dat.pri.time_num  = 0;
    dat.pri.event_num = 0;

    tes = dat.Public;
}