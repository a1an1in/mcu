# Zephyr 应用工程（`--rtos zephyr`）

本目录是把本仓库**原样复用到 Zephyr 构建体系**的应用入口。由于 Zephyr 是
**构建系统宿主**（内核、驱动、设备树、链接脚本、时钟全由 Zephyr 接管），
无法像 RT-Thread 那样被顶层 `CMakeLists.txt` 用 `add_subdirectory` 集成，
因此 Zephyr 下从这里构建。

仓库所有**应用层代码零改动**复用：
- `applications/main.c`（主程序）
- `modules/led.c` / `modules/hello.c`（示例业务模块）
- `drivers/gpio/zephyr_gpio.c`（GPIO 的 Zephyr 后端）
- `osal/port/zephyr/*.c`（OSAL → Zephyr 适配，7 个文件）

## 前置条件

1. Zephyr 依赖 Python 工具链（推荐直接用 `build-zephyr.sh`，它会自动处理）：
   - 首次构建自动在 `build/<board>/zephyr/.venv-zephyr` 创建 Python venv，并按 Zephyr
     官方 `requirements-base.txt` 装齐依赖（pyyaml/jsonschema/pyelftools 等）；
   - 若要手工，可：
     ```bash
     python3 -m venv build/stm32f103/zephyr/.venv-zephyr
     . build/stm32f103/zephyr/.venv-zephyr/bin/activate
     pip install pyelftools pyyaml
     export ZEPHYR_BASE=$PWD/rtos/zephyr
     ```
   （不需要安装 west。）
2. 第三方模块按需拉取。stm32 只需 `hal_stm32`（加 `cmsis`），版本自动对齐
   Zephyr `west.yml`；`build-zephyr.sh` 会在首次构建时把它们 clone 到
   `build/stm32f103/zephyr/external/` 并用 `ZEPHYR_MODULES` 挂载。
3. 交叉编译器。用 Zephyr 的 cross-compile 工具链变体挂上 arm-none-eabi-gcc
   （Zephyr 4.4 不再有 generic 变体）：
   ```bash
   export ZEPHYR_TOOLCHAIN_VARIANT=cross-compile
   export CROSS_COMPILE=arm-none-eabi-
   ```

## 编译（Zephyr 官方 board）

Zephyr 自带多款 STM32F103 板（如 `stm32f103_mini` / `stm32_min_dev`）。
```bash
export ZEPHYR_MODULES="$PWD/build/stm32f103/zephyr/external/hal/stm32;$PWD/build/stm32f103/zephyr/external/hal/cmsis"
cmake -S applications/zephyr -B build/stm32f103/zephyr \
      -DBOARD=stm32f103_mini \
      -DZEPHYR_MODULES="$ZEPHYR_MODULES" \
      -DZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
      -DCROSS_COMPILE=arm-none-eabi-
cmake --build build/stm32f103/zephyr
```
产物在 `build/stm32f103/zephyr/zephyr/`（`zephyr.elf` / `zephyr.bin` / `zephyr.hex`）。

## 一步式

```bash
./tools/devops.sh build --rtos zephyr            # Docker 内：按需拉取模块 + 配置 + 编译
./tools/devops.sh build --rtos zephyr --zephyr-board stm32f103_mini
```

> 说明：Zephyr 编译必须经历其 Kconfig/设备树生成流程，且依赖
> `hal_stm32` 等外部模块（本工程以 `ZEPHYR_MODULES` 按需挂载，而非 `west update`
> 全量拉取 80+ 个厂商模块）。首次构建需网络以 clone 上述模块与安装 python 依赖。