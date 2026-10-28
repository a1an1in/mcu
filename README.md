# STM32F103 多 RTOS 可扩展工程

## 项目简介

基于 **RT-Thread**（当前默认）与 **Zephyr**（已具备 OSAL 适配 + 独立应用工程）的
STM32F103（Cortex-M3）嵌入式工程骨架。
核心目标：**跨 RTOS 可扩展** —— 应用代码只依赖自研的 **OSAL 抽象层**，
因此可平滑迁移到 **Zephyr / FreeRTOS**，业务代码无需改动。

- 一键编译：`python3 tools/devops.py build`（Docker 交叉编译，宿主机零工具链；产物 `build/<BOARD>/`，默认板 `stm32f103`）。板子/内核/GPIO 后端用选项指定：`build --board f103 --rtos rt-thread`，也可 `build --rtos zephyr`（走 Zephyr 自带构建体系，见 `applications/zephyr/`）
- 一键烧录：`python3 tools/devops.py flash`
- 详细说明见 `docs/`（下表导航）。

## 目录结构（文件导航）

| 目录 | 作用 | 详见 |
|------|------|------|
| `applications/` | 应用主程序 `main.c`，系统级初始化与编排 | `docs/modules.md` |
| `modules/` | 业务模块（led、key、hello …）及中间件 / 协议栈，只依赖 OSAL | `docs/modules.md` |
| `drivers/` | 外设驱动组件（每个外设一个目录，器件/芯片/OS 三层拆分） | `docs/drivers.md` |
| `drivers/gpio/` | GPIO 接口 + 三种后端实现 | `docs/gpio.md` |
| `drivers/uart/` | UART 接口 + 寄存器收发 + RT-Thread 控制台设备 | `docs/drivers.md` |
| `drivers/net/` | 网络驱动（CH395Q：SPI + MACRAW + lwIP） | `docs/drivers.md` |
| `bsp/` | 板卡分派入口（`CMakeLists.txt`）；每块板的配置在其子目录 | `docs/architecture.md` |
| `bsp/stm32f103/` | 这块板的全部内容：`rtconfig.h`、`lwipopts.h`、`board.c/.h`、startup 汇编、链接脚本 | `docs/build-flash-debug.md` |
| `rtos/rt-thread/` | RT-Thread 源码（submodule，v5.2.2）+ 纯 CMake 集成 | `docs/architecture.md` |
| `rtos/zephyr/` | Zephyr 源码（submodule，4.4）——作为构建宿主 | `applications/zephyr/` |
| `applications/zephyr/` | Zephyr 应用工程（`--rtos zephyr` 编译入口，复用 `applications/main.c`） | `applications/zephyr/README.md` |
| `osal/` | 统一 OS 抽象层（接口 + `port/<os>` 适配：rt-thread / zephyr） | `docs/architecture.md` |
| `cmake/` | 工具链、构建辅助文件 | — |
| `docker/` | 可移植编译环境（`Dockerfile`） | `docs/build-flash-debug.md` |
| `patches/` | 针对 submodule 的自制补丁 | `docs/dependencies-patches.md` |
| `tools/` | `devops.py`（Docker编译/烧录/测试统一入口，纯 Python 3 跨平台）+ OpenOCD 配置 | `docs/build-flash-debug.md` |
| `docs/` | 文档（见下方导航） | 本页下方 |

## 文档导航

| 文档 | 内容 |
|------|------|
| [`docs/build-flash-debug.md`](docs/build-flash-debug.md) | **编译 / 烧录 / 调试** |
| [`docs/architecture.md`](docs/architecture.md) | 架构分层、硬件假设、RTOS 切换 |
| [`docs/modules.md`](docs/modules.md) | 如何新增业务模块 |
| [`docs/dependencies-patches.md`](docs/dependencies-patches.md) | 依赖维护与补丁策略 |
| [`docs/gpio.md`](docs/gpio.md) | GPIO 三后端与测试 |
| [`docs/drivers.md`](docs/drivers.md) | **驱动分层与目录约定**（器件/芯片/OS 三层、换板/换芯片/换 OS） |
| [`docs/roadmap.md`](docs/roadmap.md) | 当前候选待办 |

## 一分钟上手

> **Windows 用户**：请把下面的 `python3` 换成 `python`（或 `py`）——Windows 上的 `python3`
> 默认是 Microsoft Store 占位符，运行会静默无输出（想保留 `python3` 可在 Python 安装目录
> 把 `python.exe` 复制为 `python3.exe`）。烧录(`flash`) 在宿主机执行，需另装 `openocd` 并
> 接好调试器（本板 CMSIS-DAP 配置已内置 VID:PID，Windows 直接烧录不需要 usbipd 或 Zadig）；
> 编译(`build`/`test`) 在 Docker 内完成，无需宿主机工具链。

```bash
git submodule update --init           # 首次拉取内核
python3 tools/devops.py build               # Docker 交叉编译固件（RT-Thread，只编译固件）
python3 tools/devops.py build --rtos zephyr # 改用 Zephyr 构建（容器内按需拉取模块 + 编译，需网络）
python3 tools/devops.py build --host        # 编译固件 + 编译 host 单测程序（仅编译，不运行）
python3 tools/devops.py build --clean       # 干净重建：先清掉本次固件构建目录再编译（zephyr 保留 .venv-zephyr/external，无需联网重拉模块）
python3 tools/devops.py test                # 只运行已编译的 host 单测（需先 build --host）
python3 tools/devops.py flash               # 烧录（ST-Link）
```

## 许可证

- 本工程骨架代码：随使用场景自定（Apache-2.0 参考）。
- RT-Thread：Apache-2.0（见 `rtos/rt-thread/LICENSE`）。
