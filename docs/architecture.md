# 架构分层 / 硬件假设 / RTOS 切换

## 1. 架构分层

```
┌──────────────────────────────────────────────────────┐
│ applications  (. 主程序 / 系统编排)                    │
├──────────────────────────────────────────────────────┤
│ modules  (. 业务模块：led、hello、中间件/协议栈 ...)  │  ← 只依赖 osal.h 与 drivers
├──────────────────────────────────────────────────────┤
│ drivers (外设驱动，与 MCU/OS 解耦)                       │
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

## 3. 切换到 Zephyr

Zephyr 与 RT-Thread 的构建方式**根本不同**：Zephyr 是构建系统宿主（内核、驱动、
设备树、链接脚本、时钟全由它接管），无法像 RT-Thread 那样被顶层 `CMakeLists.txt`
用 `add_subdirectory` 集成。因此 Zephyr 构建走独立应用工程 `applications/zephyr/`：

1. 内核子模块已在 `rtos/zephyr`（submodule，Zephyr 4.4）。
2. OSAL→Zephyr 适配已实现于 `osal/port/zephyr/`（7 个文件，见其 `README.md`）。
3. Zephyr 应用入口 `applications/zephyr/` 以 `target_sources` 复用 `applications/`、`modules/`、
   `drivers/gpio/zephyr_gpio.c`、`osal/port/zephyr/*`，零改动。
4. 构建：`python3 tools/devops.py build --rtos zephyr`
   （容器内自动按需拉取 hal_stm32 + cmsis（`ZEPHYR_MODULES` 挂载，非 west 全量）
    + 用 cross-compile 工具链编译；产物 `build/<board>/zephyr/zephyr/`）。

**应用层（applications/ 与 modules/）在 RT-Thread 与 Zephyr 之间无需任何改动。**

> FreeRTOS 接入方式与 RT-Thread 同属"源码可被 add_subdirectory"系，待后续按
> RT-Thread 模板接入（本工程顶层 CMake 的 `freertos` 分支仍为占位）。