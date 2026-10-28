# =============================================================================
# 外设驱动组件集合（drivers）
# 每个外设一个目录，自包含「对外接口 + 各芯片实现」，上层只依赖组件对外接口。
#   gpio/  —— GPIO 组件：gpio.h(接口) + stm32f1_gpio.c(STM32F1 实现)
#   uart/  —— UART 组件：uart.h(接口) + stm32f1_uart.c(STM32F1 实现)
#   net/   —— 网络组件：ch395/(CH395Q 以太网，SPI1 + MACRAW + lwIP)
# =============================================================================

add_subdirectory(gpio)
add_subdirectory(uart)
add_subdirectory(net)