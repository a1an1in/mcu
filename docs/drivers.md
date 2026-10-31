# 驱动分层与目录约定

本文规定 `drivers/` 下每个外设组件的目录结构、命名后缀、依赖方向和构建期选择方式。
新增驱动、或给已有驱动加一块板 / 一颗芯片 / 一个 OS 时，按本文执行。

## 1. 四个维度，不要混在一起

一份"驱动"实际上同时涉及四件不同的事，变化原因各不相同：

| 维度 | 例子 | 什么时候变 |
|---|---|---|
| **器件**（本驱动的主角） | CH395 命令码、INIT 序列、SPI 模式 3、INT 低有效 | 换同型号芯片也不变 |
| **SoC**（用哪颗 MCU 的外设） | SPI1 的 CR1/SR/DR、GPIO 的 CRH、EXTI 的 AFIO_EXTICR | 换 MCU 才变 |
| **OS**（怎么接入操作系统框架） | RT-Thread 的 `eth_device` / `rt_pin_*`，Zephyr 的 `net_if` / GPIO API | 换 OS 才变 |
| **板**（器件焊在哪） | CS=PG9、用 SPI1、HCLK=72MHz、IRQ23、与 VS1053 共用 SPI1 | **换板就变** |

结论：**"驱动与板子无关"指的是前三维的代码里不出现第四维的值**，不是第四维不存在。
第四维的值由 BSP 提供（配置头或初始化参数），驱动只消费。

## 2. 目录结构

```text
drivers/
├── gpio/
│   ├── gpio.h                接口（芯片/OS 都无关）
│   ├── stm32f1_gpio.c        芯片实现（OS 无关）
│   ├── rtthread_gpio.c       OS 适配 → rt_pin_*
│   └── zephyr_gpio.c         OS 适配 → Zephyr GPIO
│
├── uart/
│   ├── uart.h                接口
│   ├── stm32f1_uart.c        芯片实现（寄存器收发）
│   └── rtthread_uart.c       OS 适配（rt_device + rt_sem + 控制台）
│
├── i2c/
│   ├── i2c.h                 接口（Linux 风格 i2c_msg / i2c_transfer）
│   ├── i2c_core.c            通用逻辑（adapter 注册表 + 重试 + 寄存器读写）
│   ├── i2c_bit.c             通用 bit-bang 时序（含时钟延展与 tTIMEOUT）
│   ├── i2c_config.h          板级事实（SCL/SDA 引脚、速率、延展超时）
│   ├── smbus.h               严格 SMBus 协议接口（11 种协议压成 2 个读写入口 + PEC）
│   ├── smbus.c               SMBus 协议实现（CRC-8 PEC / byte-count / 小端）
│   ├── stm32f1_i2c.h         芯片实现入口声明
│   ├── stm32f1_i2c.c         芯片实现（GPIO 原语 + DWT 微秒延时）
│   └── （无 OS 适配文件：总线锁走 osal_mutex，见 i2c_core.c；msh 命令在 modules/msh/i2c_cmd.c）
│
├── spi/
│   ├── spi.h                 接口（Linux 风格 spi_device / spi_transfer / spi_message）
│   ├── spi_core.c            通用逻辑（controller 注册表 + CS 时隙编排 + 总线锁）
│   ├── spi_config.h          板级事实（本板用哪路 SPI、引脚）
│   ├── stm32f1_spi.h         芯片实现入口声明
│   └── stm32f1_spi.c         芯片实现（SPI1/2/3 寄存器 + CS 引脚 + DWT 延时）
│
├── eeprom/
│   ├── at24c02.h             AT24C02 接口（256B / 8 字节每页，OS 无关）
│   ├── at24c02.c             器件协议（页切分写 + ACK 轮询）+ at24c02_register()
│   ├── at24c02_config.h      板级事实（7 位地址、轮询次数）
│   ├── eeprom.h              字节级访问抽象（struct eeprom_ops + eeprom_dev）
│   └── eeprom.c              通用核心：参数/越界校验 + 分发（不含器件知识）
│
├── flash/
│   ├── w25q128.h             25Q128 接口（16MB / 256B 页 / 4KB 扇区，OS 无关）
│   ├── w25q128.c             器件协议（读 / 按页写 / 扇区擦除 / JEDEC / BUSY 轮询）
│   └── w25q128_config.h      板级事实（片选 PB12、模式 3、速率、轮询上限）
│
├── block/
│   ├── blkdev.h              块设备接口（与介质无关，512B 扇区 + 注册表）
│   └── blkdev.c              静态注册表 / 惰性 init / 越界检查
│
├── audio/
│   ├── vs1053.h              VS1053B 接口（SCI 寄存器 / SDI 数据 / 音量 / 切歌）
│   ├── vs1053.c              芯片协议（OS 无关，走 drivers/spi）
│   └── vs1053_config.h       板级事实（XCS/XDCS/DREQ/RST、SPI 速率）
│
├── sd/
│   ├── sd.h                  卡接口（sd_init / 块读写 / CID-CSD 解析）
│   ├── sd.c                  卡协议（上电识别、容量、CMD17/18/24/25）
│   ├── sdio.h                主机控制器接口（SoC 无关）
│   ├── sd_config.h           板级事实（CMD/CLK/D0~D3 引脚、时钟）
│   ├── sd_blkdev.h/.c        SD → 块设备适配（文件系统的入口）
│   └── stm32f1_sdio.{h,c}    芯片实现（SDIO 寄存器 + FIFO 轮询搬运）
│
└── net/ch395/
    ├── ch395.h               器件命令码（OS 无关）
    ├── ch395_cmd.c           器件协议（OS 无关）
    ├── ch395_port.c          SoC 传输层（SPI/GPIO 寄存器）
    ├── rtthread_ch395.c      OS 适配（eth_device）
    └── zephyr_ch395.c        OS 适配（net_if，将来）
```

**每个驱动目录自包含**：删掉一个驱动 = 删掉一个目录，不需要去别处找它的实现。

> 上面的树是**目标结构**；当前代码与它的差距见第 8 节「现状对照」。

**不设 `drivers/os/` 目录**。按 OS 分目录会把同一个驱动的三份实现拆到多处，破坏自包含；
而且 OS 抽象层已经在 `osal/port/<os>/` 存在一份（那里管线程/信号量/打印等内核设施），
再来一个 `drivers/os/` 会让"OS 相关代码在哪"出现两个答案。

## 3. 命名后缀

| 文件名形式 | 含义 | 允许 include |
|---|---|---|
| `gpio.h` / `uart.h` / `ch395.h` | 对上层接口 / 器件命令码 | 不允许任何 OS 头、不允许 bsp 头 |
| `ch395_cmd.c`（无前缀） | 器件协议层 | 只 include 自己的头 + 标准库 |
| `stm32f1_uart.c`（**芯片名**开头） | SoC 实现层 | 只写寄存器，不 include OS 头 |
| `rtthread_uart.c`（**OS 名**开头） | OS 适配层 | 可以 include 该 OS 的头（`<rtthread.h>` 等） |

规则：**前缀 = 谁提供这份实现**（芯片名或 OS 名），后缀 = 设备名。

## 4. 判断这段代码放哪

| 代码性质 | 位置 |
|---|---|
| 器件协议（命令码、时序、状态机） | `drivers/<dev>/`，无前缀 |
| 直接操作寄存器 | `drivers/<dev>/stm32f1_<dev>.c` |
| 接入 OS 框架（`eth_device` / `net_if` / `rt_pin`） | `drivers/<dev>/<os>_<dev>.c` |
| 多个驱动共享的 OS 设施（总线锁、中断注册表） | `drivers/bus/` 或 BSP |
| 内核抽象（线程/信号量/打印） | `osal/port/<os>/` |
| 引脚 / 时钟 / 实例选择 / 共享总线隔离 | BSP（`bsp/<board>/`）或驱动目录内的 `*_config.h` |

## 5. 依赖方向（铁律）

```text
bsp  ──►  drivers  ──►  osal
 │           │
 │           └──►  该 OS 的头（仅限 <os>_<dev>.c）
 └──►  RTOS 内核头（board.c 等板级文件可以）

drivers ──✗──► bsp        （驱动不知道自己被焊在哪块板）
stm32f1_*.c / *_cmd.c ──✗──► <rtthread.h> 等 OS 头
```

- `board.c` 可以 `#include "uart.h"` 并调用 `uart_hw_init()` — 这是编排，允许
- `stm32f1_uart.c` 不可以 `#include "board.h"` — 这是反向依赖，禁止

## 6. 三种"换"，对应三种动作

| 变化 | 加文件？ | 动作 |
|---|---|---|
| **换板子**（同芯片，引脚/实例不同） | ❌ 不加 | 改配置：实例号、TX/RX 引脚、波特率、时钟 |
| **换芯片**（F103 → F407） | ✅ 加 `stm32f4_uart.c` | 新增芯片实现，接口不动 |
| **换 OS**（RT-Thread → Zephyr） | ✅ 加 `zephyr_uart.c` | 新增 OS 适配，接口不动 |
| **换器件**（24C02 → 24C64 / FRAM / 内部 Flash） | ✅ 加 `<dev>.c` | 器件文件里加一个 `eeprom_ops` 表 + `xxx_register()`；`eeprom.c` 与上层模块不动 |

> **自检**：如果换一块同芯片的板子逼得你新增或修改驱动实现文件，说明这个驱动的
> 板级事实还没抽干净。此时应补 `*_config.h`（或改成初始化传参），而不是加文件。

唯一的例外：中断向量名（如 `USART2_IRQHandler`、`EXTI9_5_IRQHandler`）由
`bsp/<board>/startup_*.S` 决定，"入口在 BSP、实现在驱动"是正常现象，不算耦合。

## 7. 构建期选择

OS 适配层跟着顶层 `-DRTOS=` 自动选，**不要为每个驱动再加一个手填开关**：

```cmake
# drivers/CMakeLists.txt —— 一处决定，各驱动共用
if(RTOS STREQUAL "zephyr")
    set(DRIVER_OS "zephyr")
else()
    set(DRIVER_OS "rtthread")     # 未来 freertos 同此
endif()
```

```cmake
# drivers/uart/CMakeLists.txt
set(UART_SRCS stm32f1_uart.c)                  # 芯片层
list(APPEND UART_SRCS ${DRIVER_OS}_uart.c)     # OS 适配层，自动选
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${DRIVER_OS}_uart.c)
    message(FATAL_ERROR "uart 缺少 ${DRIVER_OS} 后端")
endif()
```

Zephyr 侧不走 `drivers/CMakeLists.txt`（它是独立构建宿主），在
`applications/zephyr/CMakeLists.txt` 里显式列出所需文件即可，与现有
`drivers/gpio/zephyr_gpio.c` 的写法一致。

## 8. 现状对照

| 驱动 | 已符合约定 | 待办 |
|---|---|---|
| `drivers/gpio/` | ✅ `gpio.h` + `stm32f1_gpio.c` + `rtthread_gpio.c` + `zephyr_gpio.c` | 把 `GPIO_BACKEND` 手填开关改为跟随 `DRIVER_OS` |
| `drivers/uart/` | ⚠️ `uart.h` + `stm32f1_uart.c` | `stm32f1_uart.c` 目前混有 `rt_sem` / `rt_device` / `rt_console_set_device`，待拆出 `rtthread_uart.c`；引脚与 USART 实例待抽 `uart_config.h` |
| `drivers/net/ch395/` | ⚠️ `ch395.h` + `ch395_cmd.c` + `ch395_port.c` + `ch395_eth.c` | `ch395_eth.c` 含 `eth_device` 接入与调试命令，待拆为「核心 + `rtthread_ch395.c`」；`ch395_port.c` 的引脚/主频待抽 `ch395_config.h`；VS1053 隔离逻辑应上移 BSP；`ch395_cmd.c` 里的 `rt_kprintf` 待换日志宏 |
| `drivers/i2c/` | ✅ `i2c.h` + `i2c_core.c` + `i2c_bit.c` + `i2c_config.h` + `smbus.{h,c}` + `stm32f1_i2c.{h,c}` | 总线层（bit-bang，PB6/PB7，100kHz，SCL/SDA 均开漏，支持时钟延展与 tTIMEOUT）与严格 SMBus 协议层（11 种协议只需 `smbus_read`/`smbus_write` 两个入口，另加 `smbus_process_call`/`smbus_block_process_call`，含 PEC）完成。总线锁走 `osal_mutex_*`（i2c_core.c），所以没有 `rtthread_i2c.c` 这类 OS 适配文件 |
| `drivers/eeprom/` | ✅ `at24c02.h` + `at24c02.c` + `at24c02_config.h` + `eeprom.h` + `eeprom.c` | AT24C02（256B / 8 字节每页）：自动按页切分写、ACK 轮询等写周期（不依赖 OS 延时）、越界检查。`eeprom.h` 是**与器件无关**的字节级接口：`struct eeprom_ops`（read/write，首参 `struct eeprom_dev *`）+ `eeprom_read/eeprom_write`（统一做参数与越界校验）。`at24c02_read/at24c02_write` 就是 ops 的实现（器件自己的 `struct at24c02_dev *` 从 `dev->ctx` 取），因此 `at24c02_eeprom_ops` 原样指向它们、没有转形状的壳，注册只需 `at24c02_register()`；新增器件照此在自己的 .c 里实现 ops + register 即可（核心与 `modules/storage` 不动）。无 OS 适配层、无 SoC 层（总线已由 i2c 组件抽象） |
| `drivers/spi/` | ✅ `spi.h` + `spi_core.c` + `spi_config.h` + `stm32f1_spi.{h,c}` | 以 Linux 的 `spi_message` 为收发单位：`spi_message_init()` + `spi_message_add_tail()` 组段，`spi_sync()` 一次消息 = 一个 CS 时隙（进入拉片选、退出释放，段间支持 `delay_usecs` / `cs_change`）。`spi_write` / `spi_read` / `spi_write_then_read` 按 Linux 语义返回 0 / 负值。片选属于设备（`spi_device.cs_pin`，`SPI_CS_PIN(port,pin)` / `SPI_CS_NONE`），不属于总线；速率/模式/位序每次消息前由 SoC 层重新应用，同一条总线上不同器件可以各用各的参数。TIMEOUT 用自旋计数而非 OS 延时，驱动保持 OS 无关；总线锁走 `osal_mutex_*`，同样没有 OS 适配文件。与 I2C 的差异：SPI 没有 ACK，所以不做重试 |
| `drivers/flash/` | ✅ `w25q128.h` + `w25q128.c` + `w25q128_config.h` | 板载 NM25Q128 / W25Q128（16MB，256B 页 / 4KB 扇区，SPI2 + PB12 片选，模式 3）：读、按 256 字节页切分写（跨页会回卷，已处理）、扇区擦除、整片擦除、JEDEC ID 校验，BUSY 用反复读状态寄存器轮询（不依赖 OS 延时）。`w25q128_write()` 要求区域已擦除（NOR 只能 1→0），`w25q128_write_erase()` 做读-改-写并支持跨扇区，4KB scratch 由调用方提供，驱动里不常驻大缓冲。无 SoC 层（总线已由 spi 组件抽象）、无 OS 层 |
| `drivers/sd/` | ✅ `sd.h` + `sd.c` + `sdio.h` + `sd_config.h` + `sd_blkdev.{h,c}` + `stm32f1_sdio.{h,c}` | SD 卡走 **SDIO**（PC8~PC12 + PD2，不是 SPI）：CMD0 → CMD8 → ACMD41 识别，CMD2/3/9 取 CID/CSD 并算容量，CMD7 选中，ACMD6 切 4 位；块读写用 CMD17/18/24/25，写完用 CMD13 等卡内部编程结束。上板踩过的点：① ACMD41 的 R3 响应**没有 CRC7**，必须忽略 CCRCFAIL；② 写数据必须「先发命令 → 置 DTEN 并预填 FIFO」，提前 DTEN 会在命令前把数据打出去；③ FIFO 只有 128 字节，搬运全程要关中断，否则被 tcpip/etx 线程抢占导致 RXOVERR/TXUNDERR；④ **4 位对 CPU 速度很敏感**：4 位 24MHz = 12MB/s，搬运循环跟不上就溢出 —— 本工程靠 `-O2` + 72MHz 主频解决，慢速配置下把 `SD_BUS_WIDTH_4` 置 0 退回 1 位。实测：64GB SDHC，4 位 + 24MHz 读 MBR、末块写读比对均通过。`sd_blkdev.c` 把卡包成 `drivers/block` 的通用块设备（`sd0`），供文件系统使用；FatFs 分层见 `docs/fatfs.md`。**遗留优化**（CMD6 切高速模式 + DMA → 36MHz / 18MB/s）见 `docs/issues/sd-highspeed-dma-todo.md` |
| `drivers/block/` | ✅ `blkdev.h` + `blkdev.c` | 与介质无关的 512B 块设备：`struct blkdev_ops`（init/read/write）+ 静态注册表（`BLKDEV_MAX = 4`，不分配内存、不加锁）。`blkdev_register()` 只登记、**不做 I/O**，识别介质推迟到 `blkdev_init()`（调度器起来之后），避免 board 初始化阶段碰 OS 对象。扇区固定 512，lba 是块地址；读写统一做未初始化与越界检查。文件系统侧只有 `modules/fs/diskio.c` 一个消费者，drive N ↔ 第 N 个块设备 |
| `drivers/audio/` | ✅ `vs1053.h` + `vs1053.c` + `vs1053_config.h` | VS1053B 解码器（战舰 V4 板载，SPI1 与 CH395 共用）：SCI 寄存器读写、SDI 音频数据（32B/块 + DREQ）、软/硬复位、音量（0~100 映射到 0~250 档）、高低音、板载 HT6872 功放（VS1053 GPIO4）、SM_CANCEL 切歌、码率/解码时间、正弦测试。**录音**：MIC 线性 PCM 8kHz/16bit/单声道，`AICTRL0~3` + `MODE=0x1804` + 官方 WAV patch，`HDAT1/HDAT0` 取 FIFO。片选走 `struct spi_device.cs_pin`（XCS=PF7、XDCS=PF6），总线锁由 SPI 核心提供；`ch395_port.c` 已改为借同一把锁。播放/录音线程与 FatFs 在 `modules/music`、`modules/recorder`，详见 `docs/music-player.md`、`docs/recorder.md` |

## 9. 新增一个驱动时的检查清单

1. 接口头放 `drivers/<dev>/<dev>.h`，**不含** OS / bsp 任何内容
2. 器件协议单独成文件（无前缀），由它调用传输层
3. 芯片实现单独成文件（`stm32xx_<dev>.c`），只写寄存器
4. OS 适配单独成文件（`<os>_<dev>.c`），只做框架对接
5. 板级事实（引脚/实例/时钟）写进 `*_config.h` 或由 BSP 传参
6. `drivers/CMakeLists.txt` 无需新增开关，`DRIVER_OS` 会自动选中 OS 适配文件
7. 依赖检查：`grep -r "board.h" drivers/<dev>/` 必须为空
