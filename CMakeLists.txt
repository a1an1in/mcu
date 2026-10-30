cmake_minimum_required(VERSION 3.16)

# =============================================================================
# 多 RTOS 可扩展工程：应用层只依赖 OSAL，RT-Thread / Zephyr / FreeRTOS 为可插拔后端。
# 构建时通过 -DRTOS=rt-thread|zephyr|freertos 选择后端。
#
# ⚠️ 装配模式（重要，避免误读本文件）：
#   本 CMakeLists.txt 只负责「裸 CMake 类后端」的装配 —— 即 RT-Thread（未来 FreeRTOS 同此），
#   路径为 add_subdirectory(...) → add_executable(stm32f103.elf)，链接由本文件拥有。
#   **Zephyr 不走这个文件**：Zephyr 是独立构建宿主（内核/驱动/设备树/链接脚本/最终 elf 全归它），
#   它被下方 RTOS=zephyr 分支主动挡掉（FATAL_ERROR），改由独立应用工程
#   `applications/zephyr/CMakeLists.txt` 装配（`cmake -S applications/zephyr`）。
#   共用点只在「源码」：两侧都引用同一份 applications/main.c、modules/、drivers/gpio、osal/。
# =============================================================================

project(stm32f103 LANGUAGES C ASM)

# -----------------------------------------------------------------------------
# 目标板：bsp/<BOARD>/ 下的板级目录。不同板各自编译、产物互不影响。
# 当前已实现: stm32f103。新增板子时 bsp/<BOARD>/ 提供 board.c/board.h + 启动/链接脚本，
# 并在 bsp/CMakeLists.txt 增加对应分支。
# -----------------------------------------------------------------------------
set(BOARD "stm32f103" CACHE STRING
    "目标板（对应 bsp/<BOARD>/ 板级目录），当前支持: stm32f103")
if(NOT IS_DIRECTORY ${CMAKE_SOURCE_DIR}/bsp/${BOARD})
    message(FATAL_ERROR
        "未知板子 BOARD=${BOARD}：当前仅支持 stm32f103（存在 bsp/${BOARD}/）")
endif()

# -----------------------------------------------------------------------------
# 可配置项
# -----------------------------------------------------------------------------
set(RTOS "rt-thread" CACHE STRING
    "RTOS 后端，可选: rt-thread / zephyr / freertos")

# 芯片相关文件（启动文件/链接脚本）随板级支持放于 bsp/stm32f103/，
# 与 RT-Thread 官方“板卡模板自带”的组织方式一致

# ARM 交叉编译工具链（在配置阶段前通过 -DCMAKE_TOOLCHAIN_FILE 指定，
# 默认使用系统 PATH 中的 arm-none-eabi-*）
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# -----------------------------------------------------------------------------
# 全局编译选项
# -----------------------------------------------------------------------------
add_compile_options(-mcpu=cortex-m3 -mthumb -Wall -ffunction-sections -fdata-sections)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-std=gnu11>)
# --build-id=none：禁用 GNU build-id。部分 GCC（如 Xilinx arm-none-eabi 10.2）默认
# 生成 .note.gnu.build-id 段，因链接脚本未给该段指定位置，会被 ld 摆到 0x08000000，
# 把中断向量表挤偏 0x24 字节，导致复位从错误地址取向量、无法启动。显式禁用。
add_link_options(-mcpu=cortex-m3 -mthumb -Wl,--gc-sections -Wl,--build-id=none -Wl,-Map=${CMAKE_BINARY_DIR}/stm32f103.map)

# -----------------------------------------------------------------------------
# 通用 include 路径（被 OSAL、modules、applications 共用）
# -----------------------------------------------------------------------------
include_directories(
    ${CMAKE_SOURCE_DIR}/osal/include
    ${CMAKE_SOURCE_DIR}/drivers/gpio
    ${CMAKE_SOURCE_DIR}/drivers/uart
    ${CMAKE_SOURCE_DIR}/drivers/net/ch395
    ${CMAKE_SOURCE_DIR}/drivers/i2c
    ${CMAKE_SOURCE_DIR}/drivers/spi
    ${CMAKE_SOURCE_DIR}/drivers/eeprom
    ${CMAKE_SOURCE_DIR}/drivers/flash
    ${CMAKE_SOURCE_DIR}/drivers/sd
    ${CMAKE_SOURCE_DIR}/drivers
    ${CMAKE_SOURCE_DIR}/bsp/${BOARD}
)

# -----------------------------------------------------------------------------
# 选择并引入对应 RTOS 后端
# -----------------------------------------------------------------------------
if(RTOS STREQUAL "rt-thread")
    set(RTOS_TYPE "rt-thread")
    # rtconfig.h / lwipopts.h 由本工程 bsp/${BOARD} 提供，覆盖 RT-Thread / lwIP 默认行为
    include_directories(${CMAKE_SOURCE_DIR}/bsp/${BOARD})
    add_subdirectory(rtos/rt-thread
                     ${CMAKE_BINARY_DIR}/rtos/rt-thread)
elseif(RTOS STREQUAL "zephyr")
    # Zephyr 与 RT-Thread 构建方式不同：Zephyr 是构建系统宿主（内核/驱动/设备树/链接脚本全接管），
    # 无法像 RT-Thread 那样在本 CMakeLists 中 add_subdirectory 集成。Zephyr 编译请改走
    # 独立应用工程 applications/zephyr/（含 OSAL→Zephyr 适配 osal/port/zephyr/）：
    #   python3 tools/devops.py build --rtos zephyr          # (build_zephyr.py 按需拉取模块 + 编译)
    # 手工方式见 applications/zephyr/README.md。
    message(FATAL_ERROR "RTOS=zephyr 请改用 python3 tools/devops.py build --rtos zephyr "
        "（Zephyr 是独立构建宿主，应用在 applications/zephyr/，不在本 CMakeLists 编译）")
elseif(RTOS STREQUAL "freertos")
    message(FATAL_ERROR "FreeRTOS 后端尚未接入，请先按 README 完成 git submodule 与 CMake 适配")
else()
    message(FATAL_ERROR "未知 RTOS: '${RTOS}'。可选: rt-thread / zephyr / freertos")
endif()
message(STATUS "[stm32f103] 当前 RTOS 后端 = ${RTOS_TYPE}")

# -----------------------------------------------------------------------------
# 板级支持 (bsp)：含板级初始化 + 芯片启动文件/链接脚本(bsp/stm32f103/)
# 外设驱动 (drivers)：按外设组件组织，自包含接口与芯片实现
# -----------------------------------------------------------------------------
add_subdirectory(bsp)
add_subdirectory(drivers)

# -----------------------------------------------------------------------------
# 应用层
# -----------------------------------------------------------------------------
# [仅裸 CMake 模式（RT-Thread / 未来 FreeRTOS）] 应用层装配。
# 应用层源码：applications/main.c + osal + modules + drivers。
# ⚠️ 这段 add_subdirectory(...) + add_executable(stm32f103.elf)
#    只会在 RT-Thread 等裸 CMake 后端的编译里执行；RTOS=zephyr 时上面分支已终止，
#    由独立工程 applications/zephyr/ 另行装配同一份源码，不经过这里。
# -----------------------------------------------------------------------------
add_subdirectory(osal)
add_subdirectory(modules)
add_subdirectory(applications)

# -----------------------------------------------------------------------------
# 顶层可执行文件
# -----------------------------------------------------------------------------
if(NOT EXISTS ${BSP_LINK_SCRIPT})
    message(FATAL_ERROR "找不到链接脚本: ${BSP_LINK_SCRIPT}")
endif()

add_executable(stm32f103.elf
    $<TARGET_OBJECTS:board>
    $<TARGET_OBJECTS:gpio>
    $<TARGET_OBJECTS:uart>
    $<TARGET_OBJECTS:ch395>
    $<TARGET_OBJECTS:i2c>
    $<TARGET_OBJECTS:spi>
    $<TARGET_OBJECTS:eeprom>
    $<TARGET_OBJECTS:flash>
    $<TARGET_OBJECTS:sd>
    $<TARGET_OBJECTS:osal>
    $<TARGET_OBJECTS:modules>
    $<TARGET_OBJECTS:applications>
    ${MCU_STARTUP_FILE}
)
target_link_libraries(stm32f103.elf
    PRIVATE
    # --whole-archive 整个内核库：让链接器拉进所有 .o（含 FINSH 的 .rti_fn.* INIT section
    # 和 FSymTab 命令表），否则链接器只 pull 被直接引用的 .o，FINSH shell.o 因 INIT_EXPORT
    # 机制是运行期自动调用、无编译期显式引用，会被 --gc-sections 丢弃。
    -Wl,--whole-archive ${RTOS_KERNEL_TARGET} -Wl,--no-whole-archive
)
target_link_options(stm32f103.elf
    PRIVATE -T ${BSP_LINK_SCRIPT}
)
set_target_properties(stm32f103.elf PROPERTIES
    OUTPUT_NAME stm32f103
)

# -----------------------------------------------------------------------------
# 输出镜像烧录文件
# -----------------------------------------------------------------------------
find_program(ARM_OBJCOPY arm-none-eabi-objcopy)
if(ARM_OBJCOPY)
    add_custom_command(TARGET stm32f103.elf POST_BUILD
        COMMAND ${ARM_OBJCOPY} -O ihex $<TARGET_FILE:stm32f103.elf> ${CMAKE_BINARY_DIR}/stm32f103.hex
        COMMAND ${ARM_OBJCOPY} -O binary $<TARGET_FILE:stm32f103.elf> ${CMAKE_BINARY_DIR}/stm32f103.bin
        COMMENT "生成烧录镜像 stm32f103.hex / stm32f103.bin")
endif()