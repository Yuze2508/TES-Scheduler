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
 *          - 重构事件响应机制，使用FIFO环形列表
 *          - 优化内存占用：将 API 结构体 tes 移至 ROM（Flash）
 *            所有外部调用接口 tes.xxx() 完全保持不变，用户代码无需任何修改
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
 *          - Refactor the event response mechanism to use a FIFO circular queue.
 *          - Memory optimization: moved API structure 'tes' to ROM (Flash).
 *            All external call interfaces tes.xxx() remain unchanged, no user code modification required.
 * 
 * @see TES.h
 */

#include "TES.h"

/* ==================== 私有数据 ==================== */
/* ==================== Private Data ==================== */

/**
 * @brief 调度器内部数据结构
 * @brief Scheduler internal data structure
 * @details 包含对外 API 指针和私有任务控制块。
 * @details Contains public API pointers and private task control blocks.
 */
static struct {

    volatile uint16_t system_tick;       /* 全局系统tick，每次调用tes.tick()递增 / Global system tick, incremented on each tes.tick() */

    struct {
        /* 时间任务控制块 / Time task control block */
        struct {
            void (*entry)(void);    /* 任务函数入口 / Task function entry */
            uint16_t taskcyc;       /* 任务周期（单位：tick） / Task period (unit: tick) */
            uint16_t next_tick;     /* 下次执行的绝对tick时刻（环形时间轴） / Absolute tick of next execution (circular timeline) */
            TaskState taskflag;     /* 任务状态 / Task state */
			volatile uint16_t cache;    /* 数据缓存区（任务间通信） / Data cache (inter-task communication) */
        } time_list[TASK_MAX];

        /* 事件任务待执行列表（环形FIFO） / Pending event task list (circular FIFO) */
		struct {
			void (*event_queue[EVENT_QUEUE_LEN])(void);
			volatile uint8_t event_head;      // 读指针（下次出队的位置） / Read index (next dequeue position)
			volatile uint8_t event_tail;      // 写指针（下次入队的位置） / Write index (next enqueue position)
			volatile uint8_t event_cnt;       // 当前队列中的事件数量 / Current number of events in queue
		} eventFIFO;

		uint8_t time_num;           /* 当前时间任务数量 / Number of currently created time tasks */

    } pri;

} dat;  /* 内部数据实例，模块私有 / Internal data instance, private to module */

/* ==================== 环形时间比较宏 ==================== */
/* ==================== Circular Time Comparison Macro ==================== */

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

/* ==================== 私有函数实现 ==================== */
/* ==================== Private Function Implementations ==================== */

/**
 * @brief 查找任务在对应列表中的索引
 * @brief Find the index of a task in the list
 * @param entry 任务函数指针 / Task function pointer
 * @return 索引（>=0）或 -1（未找到/参数错误） / Index (>=0) on success, -1 if not found or invalid parameter
 */
static int8_t SearchIndex(void (*entry)(void))
{
    uint8_t i;

    if (entry == 0) return -1;

    for (i = 0; i < dat.pri.time_num; i++) {
        if (dat.pri.time_list[i].entry == entry) return i;
    }

    return -1;
}

/* ==================== 事件队列操作函数 ==================== */
/* ==================== Event Queue Operation Functions ==================== */

/**
 * @brief 检查事件队列是否为空
 * @brief Check if event queue is empty
 * @return 1:空 / empty, 0:非空 / not empty
 */
static uint8_t is_event_queue_empty(void) {
    return (dat.pri.eventFIFO.event_cnt == 0);
}

/**
 * @brief 检查事件队列是否已满
 * @brief Check if event queue is full
 * @return 1:满 / full, 0:未满 / not full
 */
static uint8_t is_event_queue_full(void) {
    return (dat.pri.eventFIFO.event_cnt == EVENT_QUEUE_LEN);
}

/**
 * @brief 事件入队（发布事件）
 * @brief Enqueue an event (publish an event)
 * @param func 事件任务函数指针 / Event task function pointer
 * @return 1:成功 / success, 0:失败（队列满）/ failure (queue full)
 */
static uint8_t event_enqueue(void (*func)(void)) {
    if (is_event_queue_full()) return 0;
    dat.pri.eventFIFO.event_queue[dat.pri.eventFIFO.event_tail] = func;
    dat.pri.eventFIFO.event_tail = (dat.pri.eventFIFO.event_tail + 1) & EVENT_QUEUE_MASK;
    dat.pri.eventFIFO.event_cnt++;
    return 1;
}

/**
 * @brief 事件出队（获取一个待处理事件）
 * @brief Dequeue an event (get a pending event)
 * @return 事件任务函数指针，队列空则返回0 / Event task function pointer, or 0 if queue empty
 */
static void (*event_dequeue(void))(void) {
    void (*func)(void);
    if (is_event_queue_empty()) return 0;
    func = dat.pri.eventFIFO.event_queue[dat.pri.eventFIFO.event_head];
    dat.pri.eventFIFO.event_head = (dat.pri.eventFIFO.event_head + 1) & EVENT_QUEUE_MASK;
    dat.pri.eventFIFO.event_cnt--;
    return func;
}

/**
 * @brief 滴答定时器中断服务函数（需挂载到硬件定时器中断）
 * @brief Tick timer interrupt service function (to be attached to hardware timer interrupt)
 * @note  只递增全局system_tick
 * @note  Only increments global system_tick
 */
static void SysTick(void)
{
    dat.system_tick++;
}

/**
 * @brief 交替调度策略核心
 * @brief Alternate scheduling policy core
 * @details 每次调度执行一个事件任务和一个时间任务，循环交替。
 *          静态索引实现轮询，带有边界安全检查，防止数组越界。
 *          对16位system_tick的读取进行临界区保护，防止8位机撕裂。
 * @details Executes one event task and one time task per schedule, alternating.
 *          Static index implements round-robin with boundary checks to prevent array overflow.
 *          Critical section protection for reading 16-bit system_tick to prevent tearing on 8-bit MCUs.
 */
static void sch_alt(void)
{
    static uint8_t time_i = 0;
    static uint8_t alt_phase = 0;   /* 0: 先事件后时间 / event first then time, 1: 先时间后事件 / time first then event */
    uint8_t i;
    uint8_t idx;
    uint16_t now;   /* 存放安全的系统tick快照 / Safe system tick snapshot */

    /* ----- 阶段0：处理一个事件 / Phase 0: handle one event ----- */
    if (alt_phase == 0) {
		
		void (*func)(void);

		TIMER_INTERRUPT_DISABLE();
		func = event_dequeue();
		TIMER_INTERRUPT_ENABLE();

		if (func != 0) {func();}
		
		alt_phase = 1;
	}

    /* ----- 阶段1：执行一个到点的时间任务 / Phase 1: execute one due time task ----- */
    if (alt_phase == 1) {
        if (time_i >= dat.pri.time_num) time_i = 0;

        for (i = 0; i < dat.pri.time_num; i++) {
            idx = time_i++;
            if (time_i >= dat.pri.time_num) time_i = 0;

            if (dat.pri.time_list[idx].taskflag == SUSPEND) continue;

            /* 原子读取系统tick（防止8位机撕裂） / Atomic read of system tick (prevents tearing on 8-bit MCUs) */
            TIMER_INTERRUPT_DISABLE();
            now = dat.system_tick;
            TIMER_INTERRUPT_ENABLE();

            /* 环形时间比较：若未到执行时刻则跳过 / Circular time comparison: skip if not yet due */
            if (!TICK_TIMEOUT(now, dat.pri.time_list[idx].next_tick)) continue;

            /* 执行时间任务 / Execute time task */
            dat.pri.time_list[idx].taskflag = RUN;
            dat.pri.time_list[idx].entry();

            /* 防御性处理：若任务内部执行了删除/挂起自身，则taskflag不再是RUN，
               此时不应再修改next_tick，避免污染被尾部覆盖的新任务 */
            /* Defensive handling: if the task deletes/suspends itself inside, taskflag is no longer RUN,
               then next_tick should not be modified to avoid polluting the new task overwritten at the tail */
            if (dat.pri.time_list[idx].taskflag == RUN) {
                dat.pri.time_list[idx].taskflag = NOT_RUN;

                /* 重新获取当前系统tick并设置下次执行时刻 / Re-read current system tick and set next execution time */
                TIMER_INTERRUPT_DISABLE();
                now = dat.system_tick;
                TIMER_INTERRUPT_ENABLE();
                dat.pri.time_list[idx].next_tick = now + dat.pri.time_list[idx].taskcyc;
            }

            alt_phase = 0;  /* 下一轮执行事件任务 / Next round execute event task */
            break;
        }
        if (i >= dat.pri.time_num) alt_phase = 0;
    }
}

/**
 * @brief 调度器主入口（由用户主循环调用）
 * @brief Scheduler entry function (to be called from main loop)
 */
static void Scheduler(void)
{
    sch_alt();
}

/* ==================== API 实现 ==================== */
/* ==================== API Implementations ==================== */

/**
 * @brief 创建时间触发任务
 * @brief Create a time-triggered task
 * @param entry 任务函数指针 / Task function pointer
 * @param time  执行周期（tick数，必须 >0） / Execution period (number of ticks, must be >0)
 * @return OPS_OK / OPS_NO
 */
static FCstate Create_time(void (*entry)(void), uint16_t time)
{
    uint16_t now;
    uint8_t idx;

    if (time == 0) return OPS_NO;
    if (SearchIndex(entry) >= 0) return OPS_NO;
    if (dat.pri.time_num >= TASK_MAX) return OPS_NO;
    if (entry == 0) return OPS_NO;

    TIMER_INTERRUPT_DISABLE();
    now = dat.system_tick;
    idx = dat.pri.time_num;
    dat.pri.time_list[idx].entry     = entry;
    dat.pri.time_list[idx].taskcyc   = time;
    dat.pri.time_list[idx].next_tick = now + time;  /* 第一次执行延迟一个周期 / First execution delayed by one period */
    dat.pri.time_list[idx].taskflag  = NOT_RUN;
    dat.pri.time_list[idx].cache     = 0xFFFF;
    dat.pri.time_num++;
    TIMER_INTERRUPT_ENABLE();
    return OPS_OK;
}

/**
 * @brief 删除任务
 * @brief Delete a task
 * @param entry 要删除的任务函数指针 / Task function pointer to delete
 * @return OPS_OK / OPS_NO
 */
static FCstate TaskDel(void (*entry)(void))
{
    int8_t idx;

    if (entry == 0) return OPS_NO;

    if ((idx = SearchIndex(entry)) >= 0) {
        TIMER_INTERRUPT_DISABLE();
        dat.pri.time_num--;
        dat.pri.time_list[idx] = dat.pri.time_list[dat.pri.time_num];
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }

    return OPS_NO;
}

/**
 * @brief 修改时间任务的周期
 * @brief Change the period of a time-triggered task
 * @param entry    任务函数指针 / Task function pointer
 * @param newtime  新周期（tick数，必须 >0） / New period (number of ticks, must be >0)
 * @return OPS_OK / OPS_NO
 * @note  新周期从下次执行开始生效，不影响当前已安排的next_tick
 * @note  The new period takes effect from the next execution, does not affect the already scheduled next_tick
 */
static FCstate TaskCycle(void (*entry)(void), uint16_t newtime)
{
    int8_t idx;

    if (newtime == 0 || entry == 0) return OPS_NO;
    idx = SearchIndex(entry);
    if (idx == -1) return OPS_NO;
    dat.pri.time_list[idx].taskcyc = newtime;
    return OPS_OK;
}

/**
 * @brief 挂起任务（调度器将跳过该任务）
 * @brief Suspend a task (scheduler will skip it)
 * @param entry 任务函数指针 / Task function pointer
 * @return OPS_OK / OPS_NO
 */
static FCstate TaskSuspend(void (*entry)(void))
{
    int8_t idx;

    if (entry == 0) return OPS_NO;
    if ((idx = SearchIndex(entry)) >= 0) {
        TIMER_INTERRUPT_DISABLE();
        dat.pri.time_list[idx].taskflag = SUSPEND;
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }
    return OPS_NO;
}

/**
 * @brief 恢复被挂起的任务
 * @brief Resume a suspended task
 * @param entry 任务函数指针 / Task function pointer
 * @return OPS_OK / OPS_NO
 * @note  对于时间任务，恢复后重新开始计时（当前tick + 周期）
 * @note  For time tasks, restart counting from current tick + period
 */
static FCstate TaskRecover(void (*entry)(void))
{
    int8_t idx;

    if (entry == 0) return OPS_NO;
    if ((idx = SearchIndex(entry)) >= 0) {
        if (dat.pri.time_list[idx].taskflag != SUSPEND) return OPS_NO;
        TIMER_INTERRUPT_DISABLE();
        dat.pri.time_list[idx].taskflag = NOT_RUN;
        dat.pri.time_list[idx].next_tick = dat.system_tick + dat.pri.time_list[idx].taskcyc;
        TIMER_INTERRUPT_ENABLE();
        return OPS_OK;
    }
    
    return OPS_NO;
}

/**
 * @brief 发布事件（触发指定任务函数执行）
 * @brief Publish an event (trigger the execution of a task function)
 * @param entry 事件任务函数指针 / Event task function pointer
 * @return OPS_OK / OPS_NO
 */
static FCstate TaskRelease(void (*entry)(void))
{
	uint8_t ret;
	
    if (entry == 0) return OPS_NO;

    TIMER_INTERRUPT_DISABLE();
    ret = event_enqueue(entry);
    TIMER_INTERRUPT_ENABLE();

    return ret ? OPS_OK : OPS_NO;
}

/**
 * @brief 向指定任务发送数据（16位）
 * @brief Send 16-bit data to a specified task
 * @param entry 接收任务函数指针 / Receiver task function pointer
 * @param d     要发送的数据（0xFFFF为保留值，不可发送） / Data to send (0xFFFF is reserved, cannot be sent)
 * @return OPS_OK / OPS_NO
 */
static FCstate Senddat(void (*entry)(void), uint16_t d)
{
    uint8_t idx;

    if (entry == 0 || d == 0xFFFF) return OPS_NO;
    if ((idx = SearchIndex(entry)) >= 0) {
        dat.pri.time_list[idx].cache = d;
        return OPS_OK;
    }

    return OPS_NO;
}

/**
 * @brief 接收本任务的数据
 * @brief Receive data for the current task
 * @param entry 本任务函数指针 / Current task function pointer
 * @param mode  读取模式（READ_ONLY 或 AUTO_CLEAR） / Read mode (READ_ONLY or AUTO_CLEAR)
 * @return 缓存中的数据，0xFFFF表示无数据或错误 / Data in cache, 0xFFFF indicates no data or error
 */
static uint16_t Receive(void (*entry)(void), ReceiveMode mode)
{
    int8_t idx;
    uint16_t d;

    if (entry == 0 || (mode != AUTO_CLEAR && mode != READ_ONLY)) return 0xFFFF;
    if ((idx = SearchIndex(entry)) >= 0) {
        d = dat.pri.time_list[idx].cache;
        if (mode == AUTO_CLEAR) dat.pri.time_list[idx].cache = 0xFFFF;
        return d;
    }
   
    return 0xFFFF;
}

/**
 * @brief 清空任务的数据缓存区（设为0xFFFF）
 * @brief Clear the task's data cache (set to 0xFFFF)
 * @param entry 任务函数指针 / Task function pointer
 * @return OPS_OK / OPS_NO
 */
static FCstate Clear(void (*entry)(void))
{
    uint8_t idx;

    if (entry == 0) return OPS_NO;
    if ((idx = SearchIndex(entry)) >= 0) {
        dat.pri.time_list[idx].cache = 0xFFFF;
        return OPS_OK;
    }
    
    return OPS_NO;
}

// ==================== 全局 API 实例（常量，放入 ROM） ====================
// ==================== Global API Instance (constant, placed in ROM) ====================
/**
 * @brief 调度器 API 结构体，所有函数指针在此集中定义。
 * @note  使用 const 修饰，编译器将其分配至只读存储区（Flash/ROM），不占用 RAM。
 * @note  顺序初始化必须与 SchedulerAPI 结构体成员顺序严格一致。
 */
const SchedulerAPI tes = {
    SysTick,        // tick
    Scheduler,      // scheduler
    Create_time,    // create_time
    TaskDel,        // del
    TaskCycle,      // cycle
    TaskSuspend,    // suspend
    TaskRecover,    // recovery
    TaskRelease,    // release
    Senddat,        // send
    Receive,        // receive
    Clear           // clear
};

/* ==================== 初始化函数 ==================== */
/* ==================== Initialization Function ==================== */

/**
 * @brief 调度器初始化，必须在创建任何任务前调用
 * @brief Scheduler initialization, must be called before creating any tasks
 * @details 清零所有内部数据（系统tick、任务计数、事件队列状态）。
 *          API 结构体 tes 已在 ROM 中静态初始化，无需在此注册。
 * @details Clears all internal data (system tick, task count, event queue state).
 *          The API structure 'tes' is statically initialized in ROM, no registration needed here.
 */
void TES_Init(void)
{
    /* 清零系统tick和任务计数器 / Clear system tick and task counters */
    dat.system_tick = 0;
	dat.pri.time_num = 0;
	dat.pri.eventFIFO.event_head = 0;
	dat.pri.eventFIFO.event_tail = 0;
	dat.pri.eventFIFO.event_cnt = 0;
}