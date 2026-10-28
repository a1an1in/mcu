# =============================================================================
# UART 外设驱动组件：对外提供统一接口 uart.h（上层只依赖此接口）。
# 实现通过 -DUART_BACKEND=<backend> 在构建期切换，切换时调用方零改动：
#   stm32f1 - 自研寄存器实现 stm32f1_uart.c（默认，需 RT-Thread 内核的 rt_device/rt_sem）
#
# 与 GPIO 组件不同，串口驱动不是「无操作系统实现」：控制台本质上要用到
# 接收信号量和字符设备，所以这里直接依赖内核，不再做裸机后端。
# =============================================================================

set(UART_BACKEND "stm32f1" CACHE STRING
    "UART 实现后端: stm32f1(自研寄存器)")

set(UART_SRCS)
if(UART_BACKEND STREQUAL "stm32f1")
    list(APPEND UART_SRCS stm32f1_uart.c)
else()
    message(FATAL_ERROR "UART_BACKEND=${UART_BACKEND} 尚未实现，可选: stm32f1")
endif()

message(STATUS "[stm32f103] UART 实现后端 = ${UART_BACKEND}")

add_library(uart OBJECT ${UART_SRCS})

# 组件自身需要找到同目录的接口头 uart.h
target_include_directories(uart PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 需要 RT-Thread 内核头（rtthread.h / rtdevice.h）与内核符号（rt_sem_*、rt_device_*）
target_link_libraries(uart PRIVATE ${RTOS_KERNEL_TARGET})
