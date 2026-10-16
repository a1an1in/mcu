# 架构分层 / 硬件假设 / RTOS 切换

## 1. 架构分层

```
┌──────────────────────────────────────────────────────┐
│ applications  (. 主程序 / 系统编排)                    │
├──────────────────────────────────────────────────────┤
│ modules  (. 业务模块：led、hello ...)                 │  ← 只依赖 osal.h 与 drivers
├──────────────────────────────────────────────────────┤
│ middleware, drivers (外设/协议栈，与 MCU/OS 解耦)      │
├──────────────────────────────────────────────────────┤
│ OSAL  (统一接口头 + port/<os> 适配实现)                 │  ← 解耦的关键
├──────────────────────────────────────────────────────┤
│ rtos/ (RT-Thread 子模块) │ drivers/(gpio…) │ bsp/(含 stm32f103 芯片文件) │
└──────────────────────────────────────────────────────┘
```

- **应用层（applications / modules）**：`#include <osal.h>`，绝不直接 `#include` 任何 RTOS 头文件。
- **OSAL**：定义线程 / 信号量 / 互斥量 / 消息队列 / 定时器 / 动态内存 / 打印的统一接口。
- **port/<os>**：把 OSAL 接口映射到具体 RTOS（当前为 RT-Thread），每一套适配层是独立的。
- 换 RTOS 时，只需在 `rtos/` 挂新内核、新增一套 `osal/port/<os>/` 实现，顶层 `-DRTOS=` 切换。

## 2. 硬件假设（默认 Blue Pill 板）

- 系统时钟：内部 HSI **8MHz**（不依赖外部晶振，便于通用跑通）
- UART1 `PA9(TX)`：`115200-8-N-1`，作 `rt_kprintf`/`osal_printf` 控制台输出
- 板载 LED：`PC13`（低电平点亮），示例 `led` 模块默认使用
- Flash 64KB / SRAM 20KB：如需更大容量芯片，同步修改 `bsp/board.h` 与 `bsp/stm32f103/stm32f103.ld`

## 3. 切换到 Zephyr / FreeRTOS（未来扩展）

1. 挂内核子模块：`git submodule add <url> rtos/zephyr`（或 `rtos/freertos`）。
2. 编写内核的 CMake 目标，使其对外导出 `RTOS_KERNEL_TARGET` 变量（参考 `rtos/rt-thread/CMakeLists.txt`）。
3. 在 `osal/port/zephyr/`（或 `freertos/`）实现 7 个适配文件（见该目录 `README.md`）。
4. 顶层构建：`./tools/devops.sh build --rtos zephyr`。

**应用层（applications/ 与 modules/）无需任何改动。**