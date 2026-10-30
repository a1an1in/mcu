# =============================================================================
# 业务模块（module）聚合
# 每个模块对应一个独立功能单元，只依赖 OSAL 与 drivers 接口，
# 与 RTOS / MCU 解耦，便于移植与复用。
# 注：新模块只需在此加入对应源文件。
# =============================================================================

add_library(modules OBJECT
    hello/hello.c
    led/led.c
    key/key.c
    storage/eeprom_storage.c
)

# msh 命令属于「给 shell 用的适配」，只在有 shell 的 RTOS 后端编译。
# 命令本身只调 module 接口，不碰硬件（见 msh/storage_cmd.c 头注释）。
if(RTOS STREQUAL "rt-thread")
    target_sources(modules PRIVATE
        msh/storage_cmd.c
    )
endif()

# 模块需要 OSAL 与 driver 头文件；msh/ 只依赖 module 接口，这里补上 storage 头目录
target_include_directories(modules PUBLIC
    ${CMAKE_SOURCE_DIR}/osal/include
    ${CMAKE_SOURCE_DIR}/drivers
    ${CMAKE_SOURCE_DIR}/modules/storage
)

# 让模块编译期获得 rtt_kernel 的 include（本工程模块虽不直接 include RT-Thread，
# 但保留此依赖以兼容某些需要内核宏的模块）
target_link_libraries(modules PRIVATE osal ${RTOS_KERNEL_TARGET})