# 脚本工具说明

统一开发/运维入口 `devops.sh`（子命令方式）：

| 子命令 | 作用 |
|--------|------|
| `devops.sh build` | **Docker 交叉编译（唯一构建方式）**，镜像含完整工具链，宿主机零依赖，产物统一到 `build/<BOARD>/`（默认 `build/stm32f103/`）。选项：`--board <板>`（默认 stm32f103，可简写 f103）、`--rtos <内核>`（默认 rt-thread）、`--gpio-backend <名>`、`--host`（或 `--test`/`-t`，额外**编译** host 单测程序，仅编译不运行）；也可用同名环境变量 `BOARD`/`RTOS`/`GPIO_BACKEND` 作默认值 |
| `devops.sh flash` | 用 OpenOCD + ST-Link 烧录 `build/<BOARD>/stm32f103.bin`（默认 `build/stm32f103/stm32f103.bin`）到 0x08000000。环境变量：`BIN`、`CONFIG` |
| `devops.sh test` | **只运行** `build --host` 已编译好的 host 单元测试（容器内 ctest，不重新编译）；产物在 `build/<BOARD>/tests-gpio/` |

其他脚本：
- `apply-patches.sh`  构建前被 `devops.sh build` 自动调用，应用 `patches/*.patch`（幂等）。可用 `APPLY_PATCHES=0` 跳过。
- `openocd-stm32f1.cfg`  ST-Link 连接 STM32F1 的配置模板

## 需要预装的工具

`build` 与 `tests` 全部在 Docker 镜像内执行，**宿主机只需安装 Docker**；
`flash` 烧录需要 `openocd`（在此基础上用 ST-Link）。

```bash
# Debian / Ubuntu（只需 Docker + openocd；无需 cmake/gcc/arm 交叉工具链）
sudo apt install docker.io openocd
```

编译与测试共享的镜像 `stm32f103-toolchain` 内置：cmake、ninja、宿主 gcc、
`gcc-arm-none-eabi`（+ newlib）、git。首次运行任意子命令自动构建，之后走缓存。