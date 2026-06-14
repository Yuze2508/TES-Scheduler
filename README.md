# TES 调度器 (Time and Event Scheduler) 中文+英文双语文档（Chinese-English bilingual version）

# TES – 时间与事件调度器

**TES** 是一款专为资源受限单片机（如 8051、STC、AVR、ARM Cortex‑M0）设计的轻量级协作式调度器。它融合了**时间触发**（周期性任务）与**事件触发**（单次响应任务）两种模型，采用**交替公平调度**策略，并提供简单的任务间通信机制。

---

## 📌 特性

- ✅ 双任务模型：`TIME`（周期执行）与 `EVENT`（触发执行，无需预先创建）
- ✅ 交替调度：每次调度执行一个事件 + 一个时间任务，确保两类任务都能及时响应
- ✅ 16位环形绝对时间轴：正确处理 tick 计数器溢出，长期运行无漂移
- ✅ 动态任务管理：创建、删除、挂起、恢复、修改时间任务周期
- ✅ 任务间通信：16位数据缓存（仅时间任务），支持 `send` / `receive`（自动/只读模式）
- ✅ 低资源占用：全静态内存，无动态分配
- ✅ 可移植性：只需提供基础类型和中断开关宏
- ✅ 可配置独立事件队列容量 `EVENT_QUEUE_LEN`（须为2的幂）
---

## 🏗️ 架构概述

TES 分别管理**时间任务**和**事件**：

- **时间任务**：静态数组 `time_list` 存储任务控制块，包含入口函数、周期、下次绝对 tick、状态和缓存。
- **事件**：使用一个固定大小的FIFO环形队列 `eventFIFO`（容量 `EVENT_QUEUE_LEN`，必须是2的幂）。

调度核心 `sch_alt()` 在两个阶段之间交替：

1. **事件阶段**：如果事件列表非空，则取出一个函数指针并执行，然后切换到时间阶段。
2. **时间阶段**：按轮询顺序寻找第一个已到期（`now >= next_tick`）且未挂起的时间任务，执行后重新计算 `next_tick = now + taskcyc`，然后切换回事件阶段。

系统 tick 由硬件定时器中断驱动，仅递增全局 `system_tick`。调度器在主循环中反复调用 `tes.scheduler()`。

---

## 📖 API 接口

所有 API 均通过全局结构体 `tes` 调用。

| 函数 | 描述 |
|------|------|
| `tes.tick()` | 滴答中断服务函数，每次中断调用一次，递增系统 tick。 |
| `tes.scheduler()` | 主调度器，需在主循环中无限调用。 |
| `tes.create_time(entry, cycle)` | 创建时间任务，首次执行延迟一个周期。 |
| `tes.del(entry)` | 删除时间任务（支持自删除）。 |
| `tes.cycle(entry, new_cycle)` | 修改时间任务的周期（下次执行生效）。 |
| `tes.suspend(entry)` | 挂起时间任务。 |
| `tes.recovery(entry)` | 恢复被挂起的时间任务（重新从当前 tick + 周期开始计时）。 |
| `tes.release(entry)` | 发布事件：将函数指针 `entry` 放入事件列表，由调度器执行。 |
| `tes.send(entry, data)` | 向指定时间任务发送 16 位数据（0xFFFF 为保留值）。 |
| `tes.receive(entry, mode)` | 接收本时间任务的数据（`READ_ONLY` 或 `AUTO_CLEAR`）。 |
| `tes.clear(entry)` | 清空时间任务的数据缓存（设为 0xFFFF）。 |

详细用法请参考“快速入门”示例。

---

## 🚀 快速入门

### 1. 移植配置

在你的工程中包含 `TES.h` 和 `TES.c`，并根据目标平台修改：

- **基础类型**：确保 `uint8_t`、`uint16_t` 等定义正确（可包含 `<stdint.h>` 或自定义）。
- **中断开关宏**：
  ```c
  #define TIMER_INTERRUPT_ENABLE()   do { EA = 1; } while(0)
  #define TIMER_INTERRUPT_DISABLE()  do { EA = 0; } while(0)
  ```
- **头文件**：代码中是 `#include <STC15F2K60S2.H>`，按需更改为对应MCU的头文件即可。

### 2. 硬件定时器配置

配置一个硬件定时器，使其以固定周期（如 1ms）产生中断。在中断服务函数中调用：

```c
void Timer0_ISR(void) interrupt 1 {
    tes.tick();   // 仅递增 system_tick
}
```

### 3. 编写任务函数

**时间任务**：周期执行，不可阻塞。

```c
void task_100ms(void) {
    static uint8_t cnt = 0;
    cnt++;
}
```

**事件响应函数**：由 `tes.release()` 触发，同样不可阻塞。

```c
void event_handler(void) {
    // 按键处理、数据通知等
}
```

### 4. 初始化并创建任务

```c
void main(void) {
    TES_Init();
    tes.create_time(task_100ms, 100);
    // 不需要预先创建事件任务，直接发布即可
    while (1) {
        tes.scheduler();
    }
}
```

### 5. 触发事件与发送数据

```c
tes.release(event_handler);        // 发布事件，稍后执行
tes.send(task_100ms, 0x1234);      // 向时间任务发送数据
```
#### **事件触发机制说明**
相较于上一版本，此版本无需创建事件任务，事件任务函数直接使用`tes.release()`语句来触发执行。

---

## 🔧 移植指南

TES 的移植只需满足以下依赖：

| 依赖项 | 说明 |
|--------|------|
| 基础类型 | `uint8_t`, `int8_t`, `uint16_t`, `int16_t`, `uint32_t`, `int32_t` |
| 中断控制宏 | `TIMER_INTERRUPT_ENABLE()` / `DISABLE()` 必须能全局开关中断 |
| 硬件定时器中断 | 周期调用 `tes.tick()`，频率决定时间精度 |
| 无标准库依赖 | 完全自包含，无需 `malloc` / `printf` |

**步骤：**

1. 将 `TES.h` 和 `TES.c` 添加到工程。
2. 在 `TES.h` 中修改类型定义（可改为 `#include <stdint.h>`）。
3. 根据编译器修改中断开关宏（例如 ARM 使用 `__disable_irq()` / `__enable_irq()`）。
4. 更改 MCU 头文件 `#include <STC15F2K60S2.H>`为对应的头文件。
5. 配置定时器中断，调用 `tes.tick()`。
6. 主循环中调用 `tes.scheduler()`。

---

## 📄 许可证

MIT 许可证。可自由用于商业和开源项目。

---


# TES – Time and Event Scheduler

**TES** is a lightweight cooperative scheduler designed for resource‑constrained microcontrollers (e.g., 8051, STC, AVR, ARM Cortex‑M0). It combines **time‑triggered** (periodic) and **event‑driven** (one‑shot) task models, uses an **alternating fair scheduling** policy, and provides simple inter‑task communication.

---

## ✨ Features

- ✅ Dual task models: `TIME` (periodic) and `EVENT` (triggered, no pre‑creation required)
- ✅ Alternating scheduling: one event + one time task per round – fair to both types
- ✅ 16‑bit circular absolute timeline: correctly handles tick overflow, no long‑term drift
- ✅ Dynamic task management: create, delete, suspend, resume, change period for time tasks
- ✅ Inter‑task communication: 16‑bit data cache (time tasks only) with `send` / `receive` (auto‑clear / read‑only)
- ✅ Low memory usage: Completely statically allocated.
- ✅ Portable: only requires basic types and interrupt control macros
- ✅ Configurable independent event queue size `EVENT_QUEUE_LEN` (must be a power of 2)

---

## 🏗️ Architecture Overview

TES manages **time tasks** and **events** separately:

- **Time tasks**: static array `time_list` stores TCBs (entry, period, absolute next tick, state, cache).
- **Events**: a fixed‑size FIFO circular queue `eventFIFO` (size `EVENT_QUEUE_LEN`, must be a power of 2). .

The core scheduler `sch_alt()` alternates between two phases:

1. **Event phase**: If the event list is not empty, take out a function pointer, execute it, and then switch to the time phase.
2. **Time phase**: round‑robin scan for the first non‑suspended time task whose `next_tick` has been reached (`now >= next_tick`), execute it, recalculate `next_tick = now + taskcyc`, then switch back to event phase.

The system tick is driven by a hardware timer interrupt, which simply increments the global `system_tick`. The scheduler must be called repeatedly from the main loop (`tes.scheduler()`).

---

## 📖 API Reference

All functions are accessed via the global structure `tes`.

| Function | Description |
|----------|-------------|
| `tes.tick()` | Tick ISR – increments system tick, call from timer interrupt. |
| `tes.scheduler()` | Main scheduler – call indefinitely from main loop. |
| `tes.create_time(entry, cycle)` | Create a time task; first execution delayed by one period. |
| `tes.del(entry)` | Delete a time task (supports self‑deletion). |
| `tes.cycle(entry, new_cycle)` | Change period of a time task (effective next execution). |
| `tes.suspend(entry)` | Suspend a time task. |
| `tes.recovery(entry)` | Resume a suspended time task (restart counting from current tick + period). |
| `tes.release(entry)` | Publish an event: put the function pointer `entry` into the event list, and it will be executed by the scheduler. |
| `tes.send(entry, data)` | Send 16‑bit data to a time task (0xFFFF is reserved). |
| `tes.receive(entry, mode)` | Receive data for the calling time task (`READ_ONLY` or `AUTO_CLEAR`). |
| `tes.clear(entry)` | Clear the time task's data cache (set to 0xFFFF). |

See the “Quick Start” section for usage examples.

---

## 🚀 Quick Start

### 1. Porting configuration

Include `TES.h` and `TES.c` in your project, then adjust for your target:

- **Basic types**: ensure `uint8_t`, `uint16_t` etc. are defined (you can include `<stdint.h>`).
- **Interrupt control macros**:
  ```c
  #define TIMER_INTERRUPT_ENABLE()   do { EA = 1; } while(0)
  #define TIMER_INTERRUPT_DISABLE()  do { EA = 0; } while(0)
  ```
- **MCU header**: remove `#include <STC15F2K60S2.H>` if not needed.

### 2. Timer interrupt setup

Configure a hardware timer to generate interrupts at a fixed period (e.g., 1ms). Inside the ISR, call:

```c
void Timer0_ISR(void) interrupt 1 {
    tes.tick();   // only increments system_tick
}
```

### 3. Write task functions

**Time task** (periodic, must not block):

```c
void task_100ms(void) {
    static uint8_t cnt = 0;
    cnt++;
}
```

**Event handler** (triggered by `tes.release`, also non‑blocking):

```c
void event_handler(void) {
    // button handling, data notification, etc.
}
```

### 4. Initialize and create tasks

```c
void main(void) {
    TES_Init();
    tes.create_time(task_100ms, 100);
    // No need to pre‑create event tasks; just call tes.release() later.
    while (1) {
        tes.scheduler();
    }
}
```

### 5. Trigger events and send data

```c
tes.release(event_handler);        // publish an event, will be executed soon
tes.send(task_100ms, 0x1234);      // send data to a time task
```

#### Event Trigger Mechanism Description
Compared to the previous version, this version does not require creating event tasks. Event task functions are directly triggered for execution using the `tes.release()` statement.

---

## 🔧 Porting Guide

TES depends only on the following:

| Dependency | Description |
|------------|-------------|
| Basic types | `uint8_t`, `int8_t`, `uint16_t`, `int16_t`, `uint32_t`, `int32_t` |
| Interrupt macros | `TIMER_INTERRUPT_ENABLE()` / `DISABLE()` must globally mask/unmask interrupts |
| Hardware timer ISR | Must call `tes.tick()` periodically (ISR frequency defines time resolution) |
| No standard library | Fully self‑contained, no `malloc` / `printf` |

**Steps:**

1. Add `TES.h` and `TES.c` to your project.
2. Modify type definitions in `TES.h` (or simply `#include <stdint.h>`).
3. Adapt interrupt control macros for your compiler (e.g., ARM: `__disable_irq()` / `__enable_irq()`).
4. Change the MCU header file `#include <STC15F2K60S2.H>` to the corresponding header file.
5. Setup a timer interrupt, call `tes.tick()` inside it.
6. Call `tes.scheduler()` in your main loop.

---

## 📄 License

MIT License. Free for commercial and open‑source use.
