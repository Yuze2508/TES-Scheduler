/**
 * @file TES.h
 * @brief 时间与事件调度器 (TES) - 头文件
 *        Time and Event Scheduler (TES) - Header file
 * 
 * @details 轻量级协作式任务调度器，支持时间触发和事件触发任务，提供任务间通信功能。
 *          本版本采用环形绝对 tick 时间轴与交替调度策略，无需每个 tick 遍历任务列表，效率更高。
 *          Lightweight cooperative task scheduler supporting time-triggered and event-triggered tasks,
 *          with inter-task communication. This version uses a circular absolute tick timeline and
 *          alternating scheduling policy, without traversing task list on every tick.
 * 
 * @note 移植到新 MCU 仅需修改本文件中的硬件相关部分。
 *       Porting to a new MCU only requires modifying the hardware-related sections in this file.
 */

#ifndef __TES_H__
#define __TES_H__

// ==================== 移植需要改动的部分 ====================
// ==================== Porting Required Modifications ====================

/**
 * @name 移植配置
 * @name Porting Configuration
 * @{
 */

/** 目标 MCU 头文件（可根据实际平台删除或替换） */
/** Target MCU header file (can be removed or replaced according to the actual platform) */
#include <STC15F2K60S2.H>

/**
 * @brief 类型重定义（若目标平台已提供 stdint.h，可直接包含并注释以下定义）
 * @brief Type redefinitions (if the target platform provides stdint.h, just include it and comment out the following)
 */
typedef unsigned char   uint8_t;   ///< 无符号8位整数 (0-255) / Unsigned 8-bit integer (0-255)
typedef char            int8_t;    ///< 有符号8位整数 (-128-127) / Signed 8-bit integer (-128-127)
typedef unsigned int    uint16_t;  ///< 无符号16位整数 (0-65535) / Unsigned 16-bit integer (0-65535)
typedef int             int16_t;   ///< 有符号16位整数 (-32768-32767) / Signed 16-bit integer (-32768-32767)
typedef unsigned long   uint32_t;  ///< 无符号32位整数 (0-4294967295) / Unsigned 32-bit integer (0-4294967295)
typedef long            int32_t;   ///< 有符号32位整数 (-2147483648-2147483647) / Signed 32-bit integer (-2147483648-2147483647)

/**
 * @brief 使能总中断（替换为目标平台的中断使能语句）
 * @brief Enable global interrupt (replace with the target platform's interrupt enable statement)
 */
#define TIMER_INTERRUPT_ENABLE()  do { EA = 1; } while(0)

/**
 * @brief 禁用总中断（替换为目标平台的中断禁用语句）
 * @brief Disable global interrupt (replace with the target platform's interrupt disable statement)
 */
#define TIMER_INTERRUPT_DISABLE() do { EA = 0; } while(0)

/** @} */

// ==================== 用户可配置部分 ====================
// ==================== User Configurable Section ====================

/**
 * @name 配置选项
 * @name Configuration Options
 * @{
 */

/** 时间任务列表和事件列表各自的最大容量（每个列表最多存放 TASK_MAX 个） */
/** Maximum size of time task list and event list (each can hold up to TASK_MAX) */
#define TASK_MAX (8)

/** @} */

// ==================== 枚举类型定义 ====================
// ==================== Enumeration Type Definitions ====================

/**
 * @brief 任务状态
 * @brief Task state
 */
typedef enum {
    NOT_RUN,    /* 空闲 / idle */
    RUN,        /* 正在执行 / running */
    SUSPEND,    /* 挂起 / suspended */
} TaskState;

/**
 * @brief 操作返回值
 * @brief Operation return status
 */
typedef enum {
    OPS_NO = 0, ///< 操作失败 / Operation failed
    OPS_OK      ///< 操作成功 / Operation succeeded
} FCstate;

/**
 * @brief 数据接收模式
 * @brief Data receive mode
 */
typedef enum {
    READ_ONLY = 0,  ///< 只读模式（不清除缓存） / Read-only mode (does not clear cache)
    AUTO_CLEAR      ///< 读取后自动清除缓存数据 / Automatically clear cache after reading
} ReceiveMode;

// ==================== API 结构体 ====================
// ==================== API Structure ====================

/**
 * @brief 用户 API 接口结构体
 * @brief User API interface structure
 * @details 所有调度器功能通过此结构体的函数指针调用。
 *          All scheduler functions are called through function pointers in this structure.
 */
typedef struct {
    /**
     * @brief 滴答计时器更新函数（需在定时器中断中调用）
     * @brief Tick update function (to be called in timer ISR)
     * @note 仅递增系统 tick，极轻量。
     * @note Only increments system tick, very lightweight.
     */
    void (*tick)(void);

    /**
     * @brief 主调度器函数（在主循环中反复调用）
     * @brief Main scheduler function (to be called repeatedly in main loop)
     */
    void (*scheduler)(void);

    /**
     * @brief 创建时间触发任务
     * @brief Create a time-triggered task
     * @param entry     任务函数指针 / Task function pointer
     * @param taskcycle 执行周期（单位：tick），必须 >0 / Execution period (unit: tick), must be >0
     * @return 操作结果 / Operation result
     * @note 任务创建后，第一次执行将延迟一个周期。
     * @note The first execution will be delayed by one period.
     */
    FCstate (*create_time)(void (*entry)(void), uint16_t taskcycle);

    /**
     * @brief 删除任务
     * @brief Delete a task
     * @param entry 要删除的任务函数指针 / Task function pointer to delete
     * @return 操作结果 / Operation result
     * @note 采用末尾覆盖法，O(1) 复杂度，删除后任务控制块被覆盖。
     * @note Uses the last-element overwrite method, O(1) complexity. The task control block is overwritten.
     */
    FCstate (*del)(void (*entry)(void));

    /**
     * @brief 修改时间任务的周期
     * @brief Change the period of a time-triggered task
     * @param entry    时间任务函数指针 / Time task function pointer
     * @param newcycle 新周期（tick），必须 >0 / New period (unit: tick), must be >0
     * @return 操作结果 / Operation result
     * @note 新周期从下次执行开始生效，不影响本次已安排的执行时刻。
     * @note The new period takes effect from the next execution, does not affect the already scheduled next_tick.
     */
    FCstate (*cycle)(void (*entry)(void), uint16_t newcycle);

    /**
     * @brief 挂起任务（调度器将跳过该任务）
     * @brief Suspend a task (scheduler will skip it)
     * @param entry 任务函数指针 / Task function pointer
     * @return 操作结果 / Operation result
     * @note 挂起的时间任务会停止倒计时。
     * @note Suspended time tasks stop counting down.
     */
    FCstate (*suspend)(void (*entry)(void));

    /**
     * @brief 恢复被挂起的任务
     * @brief Resume a suspended task
     * @param entry 任务函数指针 / Task function pointer
     * @return 操作结果 / Operation result
     * @note 恢复时间任务时，重新从“当前时间 + 周期”开始计时。
     * @note When resuming a time task, it restarts counting from current tick + period.
     */
    FCstate (*recovery)(void (*entry)(void));

    /**
     * @brief 发布事件（触发指定任务函数执行）
     * @brief Publish an event (trigger the execution of a task function)
     * @param entry 事件任务函数指针 / Event task function pointer
     * @return 操作结果 / Operation result
     * @note 发布事件并不会立即执行任务，而是将任务函数指针放入事件列表，由调度器执行。
     * @note Publishing an event does not execute the task immediately; the function pointer is placed into the event list and will be executed by the scheduler.
     */
    FCstate (*release)(void (*entry)(void));

    /**
     * @brief 向指定任务发送数据（16位）
     * @brief Send 16-bit data to a specified task
     * @param entry 接收数据的任务函数指针 / Task function pointer of the receiver
     * @param d     要发送的数据（0xFFFF 为保留值，表示“空”，不允许发送） / Data to send (0xFFFF is reserved, cannot be sent)
     * @return 操作结果 / Operation result
     * @note 数据存入目标任务的 cache 字段，接收方通过 tes.receive() 读取。
     * @note Data is stored in the target task's cache field; the receiver reads it via tes.receive().
     */
    FCstate (*send)(void (*entry)(void), uint16_t d);

    /**
     * @brief 接收本任务的数据
     * @brief Receive data for the current task
     * @param entry 本任务的函数指针 / Current task function pointer
     * @param mode  读取模式（READ_ONLY 或 AUTO_CLEAR） / Read mode (READ_ONLY or AUTO_CLEAR)
     * @return 缓存中的数据，若为 0xFFFF 表示无数据或错误 / Data in cache, 0xFFFF indicates no data or error
     * @note 通常在一个任务的开头调用，获取其他任务发送过来的数据。
     * @note Usually called at the beginning of a task to get data sent by other tasks.
     */
    uint16_t (*receive)(void (*entry)(void), ReceiveMode mode);

    /**
     * @brief 清空任务的数据缓存区（设为 0xFFFF）
     * @brief Clear the task's data cache (set to 0xFFFF)
     * @param entry 任务函数指针 / Task function pointer
     * @return 操作结果 / Operation result
     * @note 可用于主动丢弃未处理的数据。
     * @note Can be used to actively discard unprocessed data.
     */
    FCstate (*clear)(void (*entry)(void));

} SchedulerAPI;

/* 全局实例声明 / Global instance declarations */
extern void TES_Init(void);
extern SchedulerAPI tes;

#endif /* __TES_H__ */