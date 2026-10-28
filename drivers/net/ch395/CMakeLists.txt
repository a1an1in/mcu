# =============================================================================
# CH395Q 以太网驱动（SPI1 + MACRAW + lwIP）
#   ch395_port.c — 传输层：GPIO/SPI1（寄存器级）/延时/CS 互斥
#   ch395_cmd.c  — 命令层：CH395 命令码封装
#   ch395_eth.c  — 网卡层：eth_device + lwIP，EXTI6 收包通知
# =============================================================================

add_library(ch395 OBJECT
    ch395_port.c
    ch395_cmd.c
    ch395_eth.c
)

# 头文件对本目录内的源文件可见；其他组件要用时走顶层 include_directories
target_include_directories(ch395 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 需要 RT-Thread 内核头（rtthread.h / rtdevice.h）、lwIP 头、netdev
target_link_libraries(ch395 PRIVATE ${RTOS_KERNEL_TARGET})
