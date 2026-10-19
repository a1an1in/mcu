# =============================================================================
# Zephyr 应用工程（构建宿主 = Zephyr）——位于 applications/zephyr/
#
# Zephyr 与 RT-Thread 的构建方式根本不同：
#   - RT-Thread：作为源码 add_subdirectory 进顶层 CMakeLists（自建裸 CMake）；
#   - Zephyr  ：Zephyr 自己是构建系统宿主（提供内核/驱动/设备树/链接脚本/时钟）。
# 因此在 Zephyr 下构建必须以本目录为应用入口，通过 Zephyr 构建体系编译，
# 而**不能**走工程顶层 CMakeLists.txt。Zephyr 内核源码在 rtos/zephyr/（只读，
# 不改动）；应用工程在仓库 applications/zephyr/（本目录），不落入 rtos/ 源码树。
# RTOS 无关的业务源码仍在 applications/main.c。
#
# 编译方式（示例，Zephyr 4.4 / ARM GCC cross-compile 工具链，精简模块模式）：
#   export ZEPHYR_BASE=$ROOT/rtos/zephyr
#   python3 tools/build_zephyr.py      # 自动：venv(pyelftools/pyyaml) + 按需拉取
#                                #       hal_stm32/cmsis 到 build/stm32f103/zephyr/external/
#                                #       + ZEPHYR_MODULES 挂载 + cmake 配置编译
#   等价手工程序：
#     export ZEPHYR_MODULES="$ROOT/build/stm32f103/zephyr/external/hal/stm32;$ROOT/build/stm32f103/zephyr/external/hal/cmsis"
#     cmake -S applications/zephyr/ -B build/stm32f103/zephyr \
#           -DBOARD=stm32f103_mini \
#           -DZEPHYR_MODULES="$ZEPHYR_MODULES" \
#           -DZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
#           -DCROSS_COMPILE=arm-none-eabi-
#     cmake --build build/stm32f103/zephyr
#
# 也可直接用 python3 tools/devops.py build --rtos zephyr（内部完成上述步骤）。
# =============================================================================
cmake_minimum_required(VERSION 3.20.0)

# 找到 Zephyr 构建系统（ZEPHYR_BASE 指向 rtos/zephyr）
find_package(Zephyr REQUIRED HINTS ${ZEPHYR_BASE} $ENV{ZEPHYR_BASE})

project(stm32f103)

# ---------------------------------------------------------------------------
# 复用本仓库与 RTOS 无关的代码（仓库根为其 ../../）：
#   ../../applications/main.c 、../../modules/(led,hello) 、
#   ../../drivers/gpio(zephyr 后端) 、../../osal/port/zephyr/(适配)
# ---------------------------------------------------------------------------
set(OSAL_Z  ${CMAKE_CURRENT_SOURCE_DIR}/../../osal/port/zephyr)

target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../applications/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/hello/hello.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/led/led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/gpio/zephyr_gpio.c
    ${OSAL_Z}/osal_thread.c
    ${OSAL_Z}/osal_sem.c
    ${OSAL_Z}/osal_mutex.c
    ${OSAL_Z}/osal_queue.c
    ${OSAL_Z}/osal_timer.c
    ${OSAL_Z}/osal_memory.c
    ${OSAL_Z}/osal_print.c
)

# 头文件路径：OSAL 接口 + 模块/驱动头
zephyr_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../osal/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/hello
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/led
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/gpio
)
