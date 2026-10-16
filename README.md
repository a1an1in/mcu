# STM32F103 多 RTOS 可扩展工程

## 项目简介

基于 **RT-Thread**（当前默认）与 **Zephyr**（已具备 OSAL 适配 + 独立应用工程）的
STM32F103（Cortex-M3）嵌入式工程骨架。
核心目标：**跨 RTOS 可扩展** —— 应用代码只依赖自研的 **OSAL 抽象层**，
因此可平滑迁移到 **Zephyr / FreeRTOS**，业务代码无需改动。

- 一键编译：`./tools/devops.sh build`（Docker 交叉编译，宿主机零工具链；产物 `build/<BOARD>/`，默认板 `stm32f103`）。板子/内核/GPIO 后端用选项指定：`build --board f103 --rtos rt-thread`，也可 `build --rtos zephyr`（走 Zephyr 自带构建体系，见 `applications/zephyr/`）
- 一键烧录：`./tools/devops.sh flash`
- 详细说明见 `docs/`（下表导航）。

## 目录结构（文件导航）

| 目录 | 作用 | 详见 |
|------|------|------|
| `applications/` | 应用主程序 `main.c`，系统级初始化与编排 | `docs/modules.md` |
| `modules/` | 业务模块（led、hello …）及中间件 / 协议栈，只依赖 OSAL | `docs/modules.md` |
| `drivers/` | 外设驱动组件（每个外设一个目录） | — |
| `drivers/gpio/` | GPIO 接口 + 三种后端实现 | `docs/gpio.md` |
| `bsp/` | RTOS 配置 `rtconfig.h` + CMake 入口 | — |
| `bsp/stm32f103/` | 板级/芯片：`board.c/.h`、startup 汇编、链接脚本 | `docs/build-flash-debug.md` |
| `rtos/rt-thread/` | RT-Thread 源码（submodule，v5.2.2）+ 纯 CMake 集成 | `docs/architecture.md` |
| `rtos/zephyr/` | Zephyr 源码（submodule，4.4）——作为构建宿主 | `applications/zephyr/` |
| `applications/zephyr/` | Zephyr 应用工程（`--rtos zephyr` 编译入口，复用 `applications/main.c`） | `applications/zephyr/README.md` |
| `osal/` | 统一 OS 抽象层（接口 + `port/<os>` 适配：rt-thread / zephyr） | `docs/architecture.md` |
| `cmake/` | 工具链、构建辅助文件 | — |
| `docker/` | 可移植编译环境（`Dockerfile`） | `docs/build-flash-debug.md` |
| `patches/` | 针对 submodule 的自制补丁 | `docs/dependencies-patches.md` |
| `tools/` | `devops.sh`（Docker编译/烧录/测试统一入口）+ OpenOCD 配置 | `docs/build-flash-debug.md` |
| `docs/` | 文档（见下方导航） | 本页下方 |

## 文档导航

| 文档 | 内容 |
|------|------|
| [`docs/build-flash-debug.md`](docs/build-flash-debug.md) | **编译 / 烧录 / 调试** |
| [`docs/architecture.md`](docs/architecture.md) | 架构分层、硬件假设、RTOS 切换 |
| [`docs/modules.md`](docs/modules.md) | 如何新增业务模块 |
| [`docs/dependencies-patches.md`](docs/dependencies-patches.md) | 依赖维护与补丁策略 |
| [`docs/gpio.md`](docs/gpio.md) | GPIO 三后端与测试 |
| [`docs/roadmap.md`](docs/roadmap.md) | 当前候选待办 |

## 一分钟上手

```bash
git submodule update --init           # 首次拉取内核
./tools/devops.sh build               # Docker 交叉编译固件（RT-Thread，只编译固件）
./tools/devops.sh build --rtos zephyr # 改用 Zephyr 构建（容器内 west init/update + 编译，需网络）
./tools/devops.sh build --host        # 编译固件 + 编译 host 单测程序（仅编译，不运行）
./tools/devops.sh build --clean       # 干净重建：先清掉本次固件构建目录再编译（zephyr 保留 .venv-zephyr/external，无需联网重拉模块）
./tools/devops.sh test                # 只运行已编译的 host 单测（需先 build --host）
./tools/devops.sh flash               # 烧录（ST-Link）
```

## 许可证

- 本工程骨架代码：随使用场景自定（Apache-2.0 参考）。
- RT-Thread：Apache-2.0（见 `rtos/rt-thread/LICENSE`）。
