/**
 * @file TES.h
 * @brief 时间与事件调度器 (TES) - 头文件
 *        Time and Event Scheduler (TES) - Header file
 * 
 * @details 轻量级协作式任务调度器，支持时间触发和事件触发任务，提供任务间通信功能。
 *          Lightweight cooperative task scheduler supporting time-triggered and event-triggered tasks,
 *          with inter-task communication.
 *          移植到新 MCU 仅需修改本文件中的硬件相关部分。
 *          Porting to a new MCU only requires modifying the hardware-related sections in this file.
 * 
 * @note 编译器优化等级建议设置为 1 或 2，避免过度优化导致事件响应异常。
 *       Compiler optimization level is recommended to be set to 1 or 2 to avoid abnormal event response.
 * 
 * @see TES.c
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

/** 1. 目标 MCU 头文件 */
/** 1. Target MCU header file */
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

/** 最大任务数量（时间任务 + 事件任务的总和不超过此值） */
/** Maximum number of tasks (total of time tasks + event tasks should not exceed this value) */
#define TASK_MAX (8)

/**
 * @brief 调度策略选择
 * @brief Scheduling policy selection
 * @details 默认为交替策略，可通过修改宏值切换。
 * @details Default is alternate policy, can be changed by modifying the macro value.
 */
#ifndef SCHEDULER_POLICY
#define SCHEDULER_POLICY SCHEDULER_POLICY_ALT
#endif

/** @} */

// ==================== 调度策略宏定义 ====================
// ==================== Scheduling Policy Macros ====================

#define SCHEDULER_POLICY_BAT     0   ///< 批处理策略：一次性处理完所有就绪任务 / Batch policy: process all ready tasks at once
#define SCHEDULER_POLICY_ALT     1   ///< 交替策略：每次调度只执行一个事件任务和一个时间任务 / Alternate policy: execute one event task and one time task per schedule

// ==================== 枚举类型定义 ====================
// ==================== Enumeration Type Definitions ====================

/**
 * @brief 任务状态
 * @brief Task state
 */
typedef enum {
    NOT_RUN,    ///< 未运行（空闲） / Not running (idle)
    RUN,        ///< 正在执行（临时状态） / Running (temporary state)
    SUSPEND,    ///< 挂起（调度器跳过） / Suspended (scheduler skips)
    READY       ///< 就绪（仅事件任务，表示有事件发生） / Ready (event task only, indicates an event has occurred)
} TaskState;

/**
 * @brief 任务类型（用于内部查找）
 * @brief Task type (for internal lookup)
 */
typedef enum {
    EVENT,      ///< 事件触发任务 / Event-triggered task
    TIME        ///< 时间触发任务 / Time-triggered task
} TaskType;

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
 * @details All scheduler functions are called through function pointers in this structure.
 */
typedef struct {
    void (*tick)(void);     ///< 滴答计时器更新函数（需在定时器中断中调用） / Tick update function (to be called in timer ISR)
    void (*scheduler)(void);///< 主调度器函数（在主循环中反复调用） / Main scheduler function (to be called repeatedly in main loop)

    /**
     * @brief 创建时间触发任务
     * @brief Create a time-triggered task
     * @param entry     任务函数指针 / Task function pointer
     * @param taskcycle 执行周期（单位：tick） / Execution period (unit: tick)
     * @return 操作结果 / Operation result
     */
    FCstate (*create_time)(void (*entry)(void), uint16_t taskcycle);

    /**
     * @brief 创建事件触发任务
     * @brief Create an event-triggered task
     * @param entry 任务函数指针 / Task function pointer
     * @return 操作结果 / Operation result
     */
    FCstate (*create_event)(void (*entry)(void));

    /**
     * @brief 删除任务
     * @brief Delete a task
     * @param entry 要删除的任务函数指针 / Task function pointer to delete
     * @return 操作结果 / Operation result
     */
    FCstate (*del)(void (*entry)(void));

    /**
     * @brief 修改时间任务的周期
     * @brief Change the period of a time-triggered task
     * @param entry    任务函数指针 / Task function pointer
     * @param newcycle 新周期（单位：tick） / New period (unit: tick)
     * @return 操作结果 / Operation result
     */
    FCstate (*cycle)(void (*entry)(void), uint16_t newcycle);

    /**
     * @brief 挂起任务（调度器将跳过该任务）
     * @brief Suspend a task (scheduler will skip it)
     * @param entry 任务函数指针 / Task function pointer
     * @return 操作结果 / Operation result
     */
    FCstate (*suspend)(void (*entry)(void));

    /**
     * @brief 恢复被挂起的任务
     * @brief Resume a suspended task
     * @param entry 任务函数指针 / Task function pointer
     * @return 操作结果 / Operation result
     */
    FCstate (*recovery)(void (*entry)(void));

    /**
     * @brief 发布事件（触发事件任务）
     * @brief Publish an event (trigger an event task)
     * @param entry 事件任务函数指针 / Event task function pointer
     * @return 操作结果（任务状态不为 NOT_RUN 时会失败） / Operation result (fails if task state is not NOT_RUN)
     */
    FCstate (*release)(void (*entry)(void));

    /**
     * @brief 向指定任务发送数据（16位）
     * @brief Send 16-bit data to a specified task
     * @param entry 接收数据的任务函数指针 / Task function pointer of the receiver
     * @param d     要发送的数据（0xFFFF 为保留值，不可发送） / Data to send (0xFFFF is reserved, cannot be sent)
     * @return 操作结果 / Operation result
     */
    FCstate (*send)(void (*entry)(void), uint16_t d);

    /**
     * @brief 接收本任务的数据
     * @brief Receive data for the current task
     * @param entry 本任务函数指针 / Current task function pointer
     * @param mode  读取模式（READ_ONLY 或 AUTO_CLEAR） / Read mode (READ_ONLY or AUTO_CLEAR)
     * @return 缓存中的数据，若为 0xFFFF 表示无数据或错误 / Data in cache, 0xFFFF indicates no data or error
     */
    uint16_t (*receive)(void (*entry)(void), ReceiveMode mode);

    /**
     * @brief 清空任务的数据缓存区（设为 0xFFFF）
     * @brief Clear the task's data cache (set to 0xFFFF)
     * @param entry 任务函数指针 / Task function pointer
     * @return 操作结果 / Operation result
     */
    FCstate (*clear)(void (*entry)(void));

} SchedulerAPI;

// ==================== 全局变量和函数声明 ====================
// ==================== Global Variables and Function Declarations ====================

/**
 * @brief 调度器初始化函数
 * @brief Scheduler initialization function
 * @details 必须在创建任何任务之前调用，用于注册内部 API。
 * @details Must be called before creating any tasks, used to register internal APIs.
 */
extern void TES_Init(void);

/**
 * @brief 全局 API 结构体实例
 * @brief Global API structure instance
 * @details 用户通过此结构体调用所有调度器功能。
 * @details Users call all scheduler functions through this structure.
 */
extern SchedulerAPI tes;

#endif /* __TES_H__ */