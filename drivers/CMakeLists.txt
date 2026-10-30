# =============================================================================
# 外设驱动组件集合（drivers）
# 每个外设一个目录，自包含「对外接口 + 各芯片实现」，上层只依赖组件对外接口。
#   gpio/  —— GPIO 组件：gpio.h(接口) + stm32f1_gpio.c(STM32F1 实现)
#   uart/  —— UART 组件：uart.h(接口) + stm32f1_uart.c(STM32F1 实现)
#   net/   —— 网络组件：ch395/(CH395Q 以太网，SPI1 + MACRAW + lwIP)
#   i2c/   —— I2C 组件：i2c.h(接口) + i2c_core.c + i2c_bit.c + stm32f1_i2c.c
#   spi/   —— SPI 组件：spi.h(接口, spi_message/spi_sync) + spi_core.c + stm32f1_spi.c
#   eeprom/ —— EEPROM 器件组件：at24c02.c（挂在 i2c 总线上，无 SoC 层）
#   flash/ —— NOR FLASH 器件组件：w25q128.c（挂在 spi 总线上，无 SoC 层）
# =============================================================================

add_subdirectory(gpio)
add_subdirectory(uart)
add_subdirectory(net)
add_subdirectory(i2c)
add_subdirectory(spi)
add_subdirectory(eeprom)
add_subdirectory(flash)