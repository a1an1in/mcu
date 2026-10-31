# 编译 / 烧录 / 调试

本工程所有构建 / 运维操作统一经 `tools/devops.py`（纯 Python 3 跨平台实现）。
**构建一律在 Docker 交叉编译镜像内完成，宿主机无需安装任何交叉工具链。**

> **命令前缀（重要）**：本文示例用 `python3`。在 **Windows PowerShell/CMD** 下，`python3`
> 默认是 Microsoft Store 占位符（运行会静默无输出），请改用 `python`（或 `py`）；若想
> 让 `python3` 也能用，可在 Python 安装目录把 `python.exe` 复制为 `python3.exe`（本机
> 已这样处理）。Linux / WSL / macOS 保持 `python3`。
>
> **编译 vs 烧录的环境差异**：`build` / `test` 在 Docker 容器内完成，宿主机零工具链；
> `flash` / `reboot` 在**宿主机**执行（需直接看到调试器 USB），因此宿主机必须安装
> `openocd`。本板正点原子 ATK-FS CMSIS-DAP（VID:PID `04d8:00df`，FW 2.0.0）用系统自带
> 驱动即可访问，**无需 Zadig 换 WinUSB**；`tools/openocd-stm32f1-dap.cfg` 已内置
> `cmsis_dap_vid_pid`。Windows 侧直接烧录不需要 usbipd；仅当「想在 WSL 里烧、但调试器
> 只接在 Windows 上」才需要 usbipd 转发。

---

## 1. 前置：首次拉取子模块

```bash
git submodule update --init
usbipd list
usbipd bind --busid 1-3
usbipd attach --wsl --busid 1-3
```

## 2. 编译

```bash
python3 tools/devops.py build                    # Docker 交叉编译（唯一构建方式）
python3 tools/devops.py build --gpio-backend rtthread   # 切换 GPIO 后端
python3 tools/devops.py build --rtos rt-thread          # 指定 RTOS
python3 tools/devops.py build --board f103              # 指定板子（默认 stm32f103）编译固件
python3 tools/devops.py build --host                     # 编译固件 + 编译 host 单测程序（仅编译，不运行）
python3 tools/devops.py build --rtos zephyr
```

- 首次自动 `docker build` 生成工具链镜像：Ubuntu 24.04 + cmake + ninja +
  `gcc-arm-none-eabi` + `libnewlib-arm-none-eabi`，版本锁定、可复现；之后走层缓存。
- 以**当前用户 UID** 挂载工程根目录，产物直接写回宿主 `build/<BOARD>/`（默认 `build/stm32f103/`）。

### 产物（均在 `build/<BOARD>/` 下，默认为 `build/stm32f103/`）

| 文件 | 说明 |
|------|------|
| `stm32f103` | 主 ELF（含调试符号，供 GDB 加载） |
| `stm32f103.bin` | 二进制镜像（烧录用） |
| `stm32f103.hex` | Intel HEX |
| `stm32f103.map` | 链接映射 |

> 多块板子：通过 `BOARD=<名称>` 各自编译，产物分目录，互不覆盖（`build/<BOARD>/…`）。

### 环境变量

| 变量 | 默认 | 说明 |
|------|------|------|
| `BOARD` | `stm32f103` | 目标板（对应 `bsp/<BOARD>/`），决定产物目录 `build/<BOARD>/` |
| `RTOS` | `rt-thread` | 内核后端 |
| `GPIO_BACKEND` | `stm32f1` | GPIO 实现后端（见 `docs/gpio.md`） |
| `BUILD_DIR` | `$ROOT/build/<BOARD>` | 构建 / 产物目录 |
| `GEN` | `Ninja` | CMake 生成器 |
| `TOOLCHAIN` | `cmake/toolchain-arm-none-eabi.cmake` | 交叉工具链文件（容器内） |
| `TEST_BUILD` | `$ROOT/build/<BOARD>/rt-thread/tests` | 容器内 host 单测的构建目录（`build --host` 用） |
| `IMG` | `stm32f103-dev:latest` | Docker 编译/测试共用镜像名 |
| `DOCKERFILE` | `docker/Dockerfile` | Dockerfile 路径 |

> **单元测试**（与编译共用**同一镜像**，镜像含宿主 gcc 与交叉工具链）分两步：
> ① 编译 host 单测程序：`python3 tools/devops.py build --host`（或 `--test`，宿主 gcc 把 `tests/`
>    下的 `gpio/`（三后端接口一致性）与 `hw/`（模块行为，如「按 KEY1 点亮 DS0」）编成 x86 程序，
>    产物在 `build/<BOARD>/rt-thread/tests/{gpio,hw}/`，**不运行**）；
> ② 运行断言：`python3 tools/devops.py test`（只运行已编译好的单测，不重新编译）。

## 3. 烧录（刷包）

烧录通过宿主机 OpenOCD 完成。支持两种调试器，用 `--flasher` 选择：

| 烧录器 | `--flasher` | OpenOCD 配置 |
|--------|-------------|--------------|
| ST-Link（默认） | `stlink`（默认） | `tools/openocd-stm32f1.cfg` |
| 正点原子 CMSIS-DAP（DAP-Link） | `dap` | `tools/openocd-stm32f1-dap.cfg` |

> **本板当前实际使用正点原子 ATK-FS CMSIS-DAP（SWD）烧录**，请加 `--flasher dap`。
> USART1 串口走板载 CH340（`/dev/ttyUSB0`）；DAP 的虚拟串口（`/dev/ttyACM0`）不是 UART1。

```bash
# ST-Link（默认烧录器）；缺省 --rtos 时烧 rt-thread 固件
python3 tools/devops.py flash
# 指定烧哪种固件：--rtos 自动选中对应镜像
python3 tools/devops.py flash --flasher dap --rtos rt-thread   # 烧 rt-thread
python3 tools/devops.py flash --flasher dap --rtos zephyr      # 烧 zephyr
# 串口 ISP 烧录（绕开 DAP 复位问题；需 BOOT0=1、接 CH340/UART1）
python3 tools/devops.py flash --flasher isp --rtos zephyr
# 用 --bin 直接指定任意镜像（优先级高于 --rtos）
python3 tools/devops.py flash --flasher dap --bin build/stm32f103/zephyr/zephyr/zephyr.bin
python3 tools/devops.py flash --flasher dap --bin ./tools/atk_f103.bin
```

- `--rtos <rt-thread|zephyr>` 决定脚本**自动推导的默认 bin**（缺省 rt-thread）：
  - `rt-thread` → `build/<BOARD>/rt-thread/stm32f103.bin`
  - `zephyr` → `build/<BOARD>/zephyr/zephyr/zephyr.bin`
  显式 `--bin <file>` 优先级最高，可覆盖上述推导（也可用环境变量 `BIN` / `FLASH_RTOS`）。
- 底层命令（`.bin` 写 Flash 基址 `0x08000000`，`program ... verify` 校验）：
  - `stlink`：`program <bin> 0x08000000 verify reset exit`（烧完自动复位运行）。
  - `dap`：默认 `program <bin> 0x08000000 verify exit`（**不复位**——这颗 DAP 复位会进 ROM
    BootLoader，见下节；烧完请按板上 RESET 键 / 断电上电启动）。接好 RST 线可用
    `RESET_MODE=srst` 自动硬件复位。
- 找不到目标 bin 时脚本会报错并提醒先 `build --rtos <内核>`。

### 3.1 确认当前系统是 zephyr 还是 rt-thread

- **串口首屏**：Zephyr 打印 `*** Booting Zephyr OS build ... ***`；RT-Thread 打印
  `\ | /` ASCII 大横幅。⚠️ 旧固件只看 main 心跳行分不出，但**新版 main 已把系统名
  编进心跳**（`[app28-zephyr]` / `[app28-rt-thread]`，编译期按 `__ZEPHYR__` 判定）。
- **在线调试只读 Flash**（免串口/免启动/免重新烧录）：
  ```bash
  ./tools/fw-identify.sh                     # 读向量表秒判；DAP 不稳可重试
  ./tools/fw-identify.sh --strings           # 改搜系统品牌/版本字符串兜底
  ./tools/fw-identify.sh --flasher stlink    # 或 --flasher dap
  ```

> **配套：隔空重启看启动横幅（DAP，无需抢按 RESET）**：
> ```bash
> python3 tools/devops.py reboot --rtos zephyr     # 或 reboot（默认 rt-thread）
> ```
> 原理：halt 后把 `SP/PC` 设为固件 bin 文件头的启动向量再 `resume`，绕开这颗 DAP 软复位
> 会进 ROM BootLoader 的问题，等于"隔空按 RESET"。重启后串口重打内核横幅，可借此确认系统
> （zephyr 的 `Booting Zephyr OS build` / rt-thread 的 `\ | /` 横幅）。

## 4. 调试

### 4.1 串口控制台

- 引脚：`PA9(TX)`（UART1），参数 **115200-8-N-1**。
- `rt_kprintf` / `osal_printf` 的输出会出现在这里。

```bash
# Linux 下任选其一
screen /dev/ttyUSB0 115200
minicom -D /dev/ttyUSB0 -b 115200

# windows 串口可以看启动日志
powershell -ExecutionPolicy Bypass -File "C:\Users\a1an1in\workspace\mcu\tools\serial-mon.ps1" -Port COM4
```

### 4.2 OpenOCD + GDB 在线调试（推荐）

**终端 A**：启动调试器（保持运行；本板实际用正点原子 DAP，换用对应配置）

```bash
openocd -f tools/openocd-stm32f1.cfg            # ST-Link
# openocd -f tools/openocd-stm32f1-dap.cfg      # 正点原子 CMSIS-DAP（本板）
```

**终端 B**：连接并加载固件

```bash
arm-none-eabi-gdb build/stm32f103/stm32f103
(gdb) target remote localhost:3333    # 连接 OpenOCD（默认 3333 端口）
(gdb) monitor reset halt             # 复位并暂停在复位入口
(gdb) load                            # 装载 ELF 到目标 Flash/RAM
(gdb) continue                        # 运行（配合断点）
```

常用命令：

| 操作 | 命令 |
|------|------|
| 设断点 | `b main`、`b rt_hw_board_init` |
| 单步 | `step` / `next` / `stepi` |
| 读寄存器 | `info registers` |
| 看内存 | `x/8wx 0x20000000` |

### 4.3 固件与占用检查

```bash
arm-none-eabi-size build/stm32f103/stm32f103           # text / data / bss 占用
arm-none-eabi-objdump -h build/stm32f103/stm32f103     # 段布局（核对向量表位置）
```

## 5.常用测试命令
```
python3 tools/devops.py build --rtos rt-thread    
python3 tools/devops.py flash --flasher dap --rtos rt-thread  
powershell -ExecutionPolicy Bypass -File "C:\Users\a1an1in\workspace\mcu\tools\serial-mon.ps1" -Port COM4
```

> `serial-mon.ps1` 默认把键盘转发给板子：可以直接在该窗口敲 msh 命令，
> ↑/↓ 翻历史，Ctrl+C 退出；只收不发时加 `-NoInput`。


## 6. 常见问题

- **烧录后 / 用 DAP 复位后串口无日志**：本板用正点原子 CMSIS-DAP 时，OpenOCD 的 `reset`/`halt` 会把芯片带进 ROM BootLoader 或调试暂停态，**并不会让 Flash 固件真正重新运行**，串口因此无输出属正常。请**物理断电 2~3 秒再上电**（BOOT0=0 不变），即恢复 `heartbeat` 日志；调试期间对芯片停过 `halt` 也同理。
- **想免断电让固件重新常驻运行**：唯一可靠办法是**硬件复位**（只有 nRST 拉低才会重新采样 `BOOT0` 从主 Flash 启动）。请把 **DAP 的 RST(nRST) 引脚接到开发板 RESET**，然后改用
  `./tools/flash-reset-run-dap.sh`（内部执行 `reset_config srst_only` + `program ... reset run`）。
  未接 RST 线时 `nRESET` 复位前后恒为 1，硬件复位不生效、仍进 ROM——此时只能物理断电。

- **`flash` 找不到 bin**：先 `python3 tools/devops.py build`。
- **Docker 权限**：脚本已 `-u $(id -u):$(id -g)`，产物归宿主当前用户。
- **镜像构建失败/网络**：请确认本机可访问 Docker Hub；构建只在首次进行，之后走缓存。
- **需要干净重编**：删除对应板目录 `build/<BOARD>/`（如 `build/stm32f103/`）后重新 `python3 tools/devops.py build` 即可全量重建。
- **内核 `rt_sched_*` 隐式声明告警**：RT-Thread 该版本（v5.2.2）已知情况，功能由新调度
  核心提供，不阻断链接；可作为后续清理项。
