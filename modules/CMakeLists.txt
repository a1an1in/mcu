# =============================================================================
# 业务模块（module）聚合
# 每个模块对应一个独立功能单元，只依赖 OSAL 与 drivers 接口，
# 与 RTOS / MCU 解耦，便于移植与复用。
# 注：新模块只需在此加入对应源文件。
# =============================================================================

add_library(modules OBJECT
    hello/hello.c
    led/led.c
)

# 模块需要 OSAL 与 driver 头文件
target_include_directories(modules PUBLIC
    ${CMAKE_SOURCE_DIR}/osal/include
    ${CMAKE_SOURCE_DIR}/drivers
)

# 让模块编译期获得 rtt_kernel 的 include（本工程模块虽不直接 include RT-Thread，
# 但保留此依赖以兼容某些需要内核宏的模块）
target_link_libraries(modules PRIVATE osal ${RTOS_KERNEL_TARGET})