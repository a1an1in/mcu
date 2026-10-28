# 脚本工具说明

统一开发/运维入口 `devops.py`（**纯 Python 3 跨平台实现**，子命令方式）。

> **用法：** `python3 tools/devops.py <子命令> [选项]` —— Linux / macOS / Windows 均可执行（仅需 Python 3）。

| 子命令 | 作用 |
|--------|------|
| `devops.py build` | **Docker 交叉编译（唯一构建方式）**，镜像含完整工具链，宿主机零依赖，产物统一到 `build/<BOARD>/`（默认 `build/stm32f103/`）。选项：`--board <板>`（默认 stm32f103，可简写 f103）、`--rtos <内核>`（默认 rt-thread）、`--gpio-backend <名>`、`--host`（或 `--test`/`-t`，额外**编译** host 单测程序，仅编译不运行）；也可用同名环境变量 `BOARD`/`RTOS`/`GPIO_BACKEND` 作默认值 |
| `devops.py flash` | 用 OpenOCD 烧录（默认 ST-Link；`--flasher dap`=CMSIS-DAP，`--flasher isp`=串口 ISP(stm32flash，需 `apt install stm32flash` + `BOOT0=1` + CH340/UART1 `/dev/ttyUSB0`)）。默认 bin：`build/<BOARD>/rt-thread/stm32f103.bin`。环境变量：`BIN`、`CONFIG`、`PORT`（isp 用）、`BAUD` |
| `devops.py test` | **只运行** `build --host` 已编译好的 host 单元测试（容器内 ctest，不重新编译）；产物在 `build/<BOARD>/rt-thread/tests/` |
| `devops.py shell` | 进入 `stm32f103-dev` 镜像的**交互式 shell**（`docker run -it`，复用 build 同款挂载，工程目录即工作目录），便于手动跑 cmake / arm-none-eabi-gcc 或排查环境；非交互终端会给出提示并退出。容器默认访不到 USB，烧录/探测 DAP 仍在宿主机 |

其他脚本：
- `apply_patches.py`  构建前被 `devops.py build` 自动调用，应用 `patches/*.patch`（幂等）。可用 `APPLY_PATCHES=0` 跳过。
- `build_zephyr.py`  `--rtos zephyr` 时在容器内被调用，负责按需拉取 Zephyr 模块 + 编译。
- `openocd-stm32f1.cfg`  ST-Link 连接 STM32F1 的配置模板
- `openocd-stm32f1-dap.cfg`  CMSIS-DAP（正点原子 DAP）连接 STM32F1 的配置模板

## 需要预装的工具

`build` 与 `test` 全部在 Docker 镜像内执行，**宿主机只需安装 Docker**；
`flash` 烧录需要 `openocd`（在此基础上用 ST-Link）。

```bash
# Debian / Ubuntu（只需 Docker + openocd；无需 cmake/gcc/arm 交叉工具链）
sudo apt install docker.io openocd
```

编译与测试共享的镜像 `stm32f103-dev` 内置：cmake、ninja、宿主 gcc、
`gcc-arm-none-eabi`（+ newlib）、git。首次运行任意子命令自动构建，之后走缓存。
