# TES3 调度器 (Time and Event Scheduler) 中文+英文双语文档（Chinese-English bilingual version）

<!-- 以下为中文文档 -->
# TES3 – 时间与事件调度器

**TES3** 是一款专为资源受限单片机（如 8051、STC、AVR、ARM Cortex‑M0）设计的轻量级协作式调度器。它融合了**时间触发**（周期性任务）与**事件触发**（单次响应任务）两种模型，采用**交替公平调度**策略，并提供简单的任务间通信机制。

---

## 📌 特性

- ✅ 双任务模型：`TIME`（周期执行）与 `EVENT`（触发执行）
- ✅ 交替调度：每次调度执行一个事件任务 + 一个时间任务，确保两类任务都能及时响应
- ✅ 16位环形绝对时间轴：正确处理 tick 计数器溢出，长期运行无漂移
- ✅ 动态任务管理：创建、删除、挂起、恢复、修改周期
- ✅ 任务间通信：16位数据缓存，支持 `send` / `receive`（自动/只读模式）
- ✅ 低资源占用：全静态内存，无动态分配
- ✅ 可移植性：只需提供基础类型和中断开关宏

---

## 🏗️ 架构概述

TES3 采用**双列表**结构：一个数组存放时间任务（`time_list`），另一个数组存放事件任务（`event_list`）。每个任务控制块（TCB）包含：

- 任务入口函数指针 `entry`
- 任务状态 `taskflag`（`NOT_RUN` / `RUN` / `SUSPEND` / `READY`）
- 周期（仅时间任务）`taskcyc`
- 下次执行的绝对 tick（仅时间任务）`next_tick`
- 16位数据缓存 `cache`

调度核心 `sch_alt()` 在两个阶段之间交替：

1. **事件阶段**：按轮询顺序寻找第一个 `READY` 状态的事件任务，执行后将其恢复为 `NOT_RUN`。
2. **时间阶段**：按轮询顺序寻找第一个已到期（`now >= next_tick`）且未挂起的时间任务，执行后重新计算 `next_tick = now + taskcyc`。

系统 tick 由硬件定时器中断驱动，仅递增全局 `system_tick`。调度器在主循环中反复调用 `tes.scheduler()`。

---

## 📖 API 接口

所有 API 均通过全局结构体 `tes` 调用。

| 函数 | 描述 |
|------|------|
| `tes.tick()` | 滴答中断服务函数，每次中断调用一次，递增系统 tick。 |
| `tes.scheduler()` | 主调度器，需在主循环中无限调用。 |
| `tes.create_time(entry, cycle)` | 创建时间任务，首次执行延迟一个周期。 |
| `tes.create_event(entry)` | 创建事件任务，初始为 `NOT_RUN`。 |
| `tes.del(entry)` | 删除任务（支持自删除）。 |
| `tes.cycle(entry, new_cycle)` | 修改时间任务的周期（下次执行生效）。 |
| `tes.suspend(entry)` | 挂起任务。 |
| `tes.recovery(entry)` | 恢复挂起的任务（时间任务重新开始计时）。 |
| `tes.release(entry)` | 发布事件（将事件任务状态设为 `READY`）。 |
| `tes.send(entry, data)` | 向指定任务发送 16 位数据（0xFFFF 为保留值）。 |
| `tes.receive(entry, mode)` | 接收本任务的数据（`READ_ONLY` 或 `AUTO_CLEAR`）。 |
| `tes.clear(entry)` | 清空任务的数据缓存（设为 0xFFFF）。 |

详细用法请参考“快速入门”示例。

---

## 🚀 快速入门

### 1. 移植配置

在你的工程中包含 `TES3.h` 和 `TES3.c`，并根据目标平台修改：

- **基础类型**：确保 `uint8_t`、`uint16_t` 等定义正确（可包含 `<stdint.h>` 或自定义）。
- **中断开关宏**：
  ```c
  #define TIMER_INTERRUPT_ENABLE()   do { EA = 1; } while(0)
  #define TIMER_INTERRUPT_DISABLE()  do { EA = 0; } while(0)
  ```
- **头文件**：若无需特定 MCU 头文件，可移除 `#include <STC15F2K60S2.H>`。

### 2. 硬件定时器配置

配置一个硬件定时器，使其以固定周期（如 1ms）产生中断。在中断服务函数中调用：

```c
void Timer0_ISR(void) interrupt 1 {
    tes.tick();   // 仅递增 system_tick
}
```

### 3. 编写任务函数

所有任务必须为 `void func(void)` 类型，且**不能阻塞**（无延时循环、无等待标志）。

```c
void task_100ms(void) {
    static uint8_t cnt = 0;
    cnt++;
}

void event_proc(void) {
    uint16_t data = tes.receive(event_proc, AUTO_CLEAR);
    // 处理数据 ...
}
```

### 4. 初始化并创建任务

```c
void main(void) {
    TES_Init();
    tes.create_time(task_100ms, 100);
    tes.create_event(event_proc);
    while (1) {
        tes.scheduler();
    }
}
```

### 5. 触发事件与发送数据

```c
tes.release(event_proc);
tes.send(event_proc, 0x1234);
```

---

## 🔧 移植指南

TES3 的移植只需满足以下依赖：

| 依赖项 | 说明 |
|--------|------|
| 基础类型 | `uint8_t`, `int8_t`, `uint16_t`, `int16_t`, `uint32_t`, `int32_t` |
| 中断控制宏 | `TIMER_INTERRUPT_ENABLE()` / `DISABLE()` 必须能全局开关中断 |
| 硬件定时器中断 | 周期调用 `tes.tick()`，频率决定时间精度 |
| 无标准库依赖 | 完全自包含，无需 `malloc` / `printf` |

**步骤：**

1. 将 `TES3.h` 和 `TES3.c` 添加到工程。
2. 在 `TES3.h` 中修改类型定义（可改为 `#include <stdint.h>`）。
3. 根据编译器修改中断开关宏（例如 ARM 使用 `__disable_irq()` / `__enable_irq()`）。
4. 若不需要特定 MCU 头文件，删除 `#include <STC15F2K60S2.H>`。
5. 配置定时器中断，调用 `tes.tick()`。
6. 主循环中调用 `tes.scheduler()`。

---

## 📄 许可证

MIT 许可证。可自由用于商业和开源项目。

---

<!-- 以下为英文文档 -->
# TES3 – Time and Event Scheduler

**TES3** is a lightweight cooperative scheduler designed for resource‑constrained microcontrollers (e.g., 8051, STC, AVR, ARM Cortex‑M0). It combines **time‑triggered** (periodic) and **event‑driven** (one‑shot) task models, uses an **alternating fair scheduling** policy, and provides simple inter‑task communication.

---

## ✨ Features

- ✅ Dual task models: `TIME` (periodic) and `EVENT` (triggered)
- ✅ Alternating scheduling: one event task + one time task per round – fair to both types
- ✅ 16‑bit circular absolute timeline: correctly handles tick overflow, no long‑term drift
- ✅ Dynamic task management: create, delete, suspend, resume, change period
- ✅ Inter‑task communication: 16‑bit data cache with `send` / `receive` (auto‑clear / read‑only)
- ✅ Low memory usage: Completely statically allocated.
- ✅ Portable: only requires basic types and interrupt control macros

---

## 🏗️ Architecture Overview

TES3 maintains two separate arrays: one for time tasks (`time_list`) and one for event tasks (`event_list`). Each Task Control Block (TCB) contains:

- Task function pointer `entry`
- Task state `taskflag` (`NOT_RUN` / `RUN` / `SUSPEND` / `READY`)
- Period (only for time tasks) `taskcyc`
- Absolute tick of next execution (only for time tasks) `next_tick`
- 16‑bit data cache `cache`

The core scheduler `sch_alt()` alternates between two phases:

1. **Event phase**: scans for the first `READY` event task, executes it, then sets it back to `NOT_RUN`.
2. **Time phase**: scans for the first time task that is not suspended and whose `next_tick` has been reached (`now >= next_tick`), executes it, then recalculates `next_tick = now + taskcyc`.

The system tick is driven by a hardware timer interrupt, which simply increments the global `system_tick`. The scheduler must be called repeatedly from the main loop (`tes.scheduler()`).

---

## 📖 API Reference

All functions are accessed via the global structure `tes`.

| Function | Description |
|----------|-------------|
| `tes.tick()` | Tick ISR – increments system tick, call from timer interrupt. |
| `tes.scheduler()` | Main scheduler – call indefinitely from main loop. |
| `tes.create_time(entry, cycle)` | Create a time task; first execution delayed by one period. |
| `tes.create_event(entry)` | Create an event task; initial state is `NOT_RUN`. |
| `tes.del(entry)` | Delete a task (supports self‑deletion). |
| `tes.cycle(entry, new_cycle)` | Change period of a time task (effective next execution). |
| `tes.suspend(entry)` | Suspend a task. |
| `tes.recovery(entry)` | Resume a suspended task; time tasks restart counting. |
| `tes.release(entry)` | Post an event – sets event task to `READY`. |
| `tes.send(entry, data)` | Send 16‑bit data to a task (0xFFFF is reserved). |
| `tes.receive(entry, mode)` | Receive data for the calling task (`READ_ONLY` or `AUTO_CLEAR`). |
| `tes.clear(entry)` | Clear the task's data cache (set to 0xFFFF). |

See the “Quick Start” section for usage examples.

---

## 🚀 Quick Start

### 1. Porting configuration

Include `TES3.h` and `TES3.c` in your project, then adjust for your target:

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

All tasks must be of type `void func(void)` and **must not block** (no busy‑wait loops).

```c
void task_100ms(void) {
    static uint8_t cnt = 0;
    cnt++;
}

void event_proc(void) {
    uint16_t data = tes.receive(event_proc, AUTO_CLEAR);
    // process data ...
}
```

### 4. Initialize and create tasks

```c
void main(void) {
    TES_Init();
    tes.create_time(task_100ms, 100);
    tes.create_event(event_proc);
    while (1) {
        tes.scheduler();
    }
}
```

### 5. Trigger events and send data

```c
tes.release(event_proc);
tes.send(event_proc, 0x1234);
```

---

## 🔧 Porting Guide

TES3 depends only on the following:

| Dependency | Description |
|------------|-------------|
| Basic types | `uint8_t`, `int8_t`, `uint16_t`, `int16_t`, `uint32_t`, `int32_t` |
| Interrupt macros | `TIMER_INTERRUPT_ENABLE()` / `DISABLE()` must globally mask/unmask interrupts |
| Hardware timer ISR | Must call `tes.tick()` periodically (ISR frequency defines time resolution) |
| No standard library | Fully self‑contained, no `malloc` / `printf` |

**Steps:**

1. Add `TES3.h` and `TES3.c` to your project.
2. Modify type definitions in `TES3.h` (or simply `#include <stdint.h>`).
3. Adapt interrupt control macros for your compiler (e.g., ARM: `__disable_irq()` / `__enable_irq()`).
4. Remove `#include <STC15F2K60S2.H>` if not used.
5. Setup a timer interrupt, call `tes.tick()` inside it.
6. Call `tes.scheduler()` in your main loop.

---

## 📄 License

MIT License. Free for commercial and open‑source use.
