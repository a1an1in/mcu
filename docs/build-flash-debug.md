# 编译 / 烧录 / 调试

本工程所有构建 / 运维操作统一经 `tools/devops.sh`。
**构建一律在 Docker 交叉编译镜像内完成，宿主机无需安装任何交叉工具链。**

---

## 1. 前置：首次拉取子模块

```bash
git submodule update --init
```

## 2. 编译

```bash
./tools/devops.sh build                    # Docker 交叉编译（唯一构建方式）
./tools/devops.sh build --gpio-backend rtthread   # 切换 GPIO 后端
./tools/devops.sh build --rtos rt-thread          # 指定 RTOS
./tools/devops.sh build --board f103              # 指定板子（默认 stm32f103）编译固件
./tools/devops.sh build --host                     # 编译固件 + 编译 host 单测程序（仅编译，不运行）
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
| `TEST_BUILD` | `$ROOT/build/<BOARD>/tests-gpio` | 容器内 host 单测的构建目录（`build --host` 用） |
| `IMG` | `stm32f103-toolchain:latest` | Docker 编译/测试共用镜像名 |
| `DOCKERFILE` | `docker/Dockerfile` | Dockerfile 路径 |

> **单元测试**（与编译共用**同一镜像**，镜像含宿主 gcc 与交叉工具链）分两步：
> ① 编译 host 单测程序：`./tools/devops.sh build --host`（或 `--test`，宿主 gcc 把 `tests/gpio`
>    编成 x86 程序，产物在 `build/<BOARD>/tests-gpio/`，**不运行**）；
> ② 运行断言：`./tools/devops.sh test`（只运行已编译好的单测，不重新编译）。

## 3. 烧录（刷包）

硬件：ST-Link（SWD 四线连目标板 SWDIO/SWCLK/GND/3V3）。

```bash
./tools/devops.sh flash
# 指定其它镜像 / 配置
BIN=build/stm32f103/stm32f103.bin ./tools/devops.sh flash
CONFIG=tools/openocd-stm32f1.cfg ./tools/devops.sh flash
```

- 通过 OpenOCD 将 `build/<BOARD>/stm32f103.bin`（默认 `build/stm32f103/stm32f103.bin`）写入 `0x08000000`，随后 `verify` + `reset` 运行。
- 找不到目标 bin 时脚本会报错并提醒先 `build`。

## 4. 调试

### 4.1 串口控制台

- 引脚：`PA9(TX)`（UART1），参数 **115200-8-N-1**。
- `rt_kprintf` / `osal_printf` 的输出会出现在这里。

```bash
# Linux 下任选其一
screen /dev/ttyUSB0 115200
minicom -D /dev/ttyUSB0 -b 115200
```

### 4.2 OpenOCD + GDB 在线调试（推荐）

**终端 A**：启动调试器（保持运行）

```bash
openocd -f tools/openocd-stm32f1.cfg
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

## 5. 常见问题

- **`flash` 找不到 bin**：先 `./tools/devops.sh build`。
- **Docker 权限**：脚本已 `-u $(id -u):$(id -g)`，产物归宿主当前用户。
- **镜像构建失败/网络**：请确认本机可访问 Docker Hub；构建只在首次进行，之后走缓存。
- **需要干净重编**：删除对应板目录 `build/<BOARD>/`（如 `build/stm32f103/`）后重新 `./tools/devops.sh build` 即可全量重建。
- **内核 `rt_sched_*` 隐式声明告警**：RT-Thread 该版本（v5.2.2）已知情况，功能由新调度
  核心提供，不阻断链接；可作为后续清理项。
