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
- **文件系统**：`modules/fs`（FatFs R0.14b，裁剪成 OS 无关）+ `drivers/block` 块设备接口；
  分层、配置取舍与 msh 用法见 `docs/fatfs.md`。
- **音频**：`modules/music`（播放线程）+ `drivers/audio`（VS1053B，SPI1 与 CH395 共用），
  用法与硬件连线见 `docs/music-player.md`；**录音**见 `modules/recorder` 与
  `docs/recorder.md`（WAV / 8kHz / 16bit / 单声道）。
- **对话**：`modules/chat`（明文 HTTP 客户端：文本 /ask + 语音 /voice）
  + `tools/llm_bridge.py`（PC 端 HTTPS 转发，API Key 不落板子）。
  文本见 `docs/chat.md`；按住 KEY0 流式录音 → PC 做 ASR/LLM/TTS → 边下边播，
  见 `docs/voice.md`。
- **OSAL**：定义线程 / 信号量 / 互斥量 / 消息队列 / 定时器 / 动态内存 / 打印的统一接口。
- **port/<os>**：把 OSAL 接口映射到具体 RTOS（当前为 RT-Thread），每一套适配层是独立的。
- 换 RTOS 时，只需在 `rtos/` 挂新内核、新增一套 `osal/port/<os>/` 实现，顶层 `-DRTOS=` 切换。

## 2. 硬件假设（默认目标板：正点原子战舰 STM32F103 V4）

- 系统时钟：HSE 8MHz × PLL9 = **72MHz**（AHB 72 / APB1 36 / APB2 72）；
  HSE 起振失败时自动退回内部 HSI 8MHz（各驱动都按 RCC 现算频率，不用改代码）。
  原理、寄存器位定义、切换顺序与踩过的坑见 `docs/stm32f1-clock.md`
- UART1 `PA9(TX)`：`115200-8-N-1`，作 `rt_kprintf`/`osal_printf` 控制台输出
- 板载 LED：`DS0 → PB5`（红）、`DS1 → PE5`（绿），均为**低电平点亮**
- 板载按键：`KEY0 → PE4`、`KEY1 → PE3`、`KEY2 → PE2`（按下接地，低有效）、
  `WK_UP → PA0`（按下接 3.3V，高有效）；`key` 模块内部统一开上/下拉，不依赖板上外部上拉
- Flash 512KB / SRAM 64KB：换其它容量芯片时同步修改 `bsp/stm32f103/board.h` 与 `bsp/stm32f103/stm32f103.ld`

> 上表是 `modules/led`、`modules/key` 里那张引脚映射表的依据（由原理图 LED / KEY 块得出，
> 见 `docs/gpio.md` 的 host 单测如何把这套极性钉死）。

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

## 4. 驱动分层（drivers/ 内部）

`drivers/` 下每个外设组件都按「器件 / SoC / OS」三层拆分，板级事实由 BSP 提供：

```text
drivers/uart/
├── uart.h                接口（芯片/OS 都无关）
├── stm32f1_uart.c        芯片实现（寄存器收发，不含 OS 头）
└── rtthread_uart.c       OS 适配（rt_device + rt_sem + 控制台）
```

- **依赖方向**：`bsp → drivers → osal`；驱动不 include `board.h`，芯片实现不 include OS 头
- **换板子** → 改配置，不加文件；**换芯片** → 加 `stm32xx_<dev>.c`；**换 OS** → 加 `<os>_<dev>.c`
- **构建期选择**：OS 适配文件跟随顶层 `-DRTOS=` 自动选中，不额外设手填开关

完整约定（命名后缀、判断表、现状对照、新增驱动检查清单）见
[`docs/drivers.md`](drivers.md)。