/*
 * stm32f1_spi.h - STM32F1 的 SPI 控制器实现入口 <drivers/spi/>
 *
 * 本文件只声明「怎么在 STM32F1 上造一条 SPI 总线」；总线的用法一律走
 * spi.h（spi_sync / spi_write / spi_write_then_read）。
 *
 * 引脚由调用方（bsp/<board>/board.c）传入，驱动里不写死任何一个端口：
 * 换板只改 spi_config.h 与 BSP 的几行装配代码。
 */
#ifndef DRIVERS_STM32F1_SPI_H__
#define DRIVERS_STM32F1_SPI_H__

#include <stdint.h>

#include "spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一条 SPI 的三根信号线；端口编号 0=A 1=B 2=C 3=D 4=E 5=F 6=G */
struct stm32f1_spi_pins {
    uint8_t sck_port;
    uint8_t sck_pin;
    uint8_t miso_port;
    uint8_t miso_pin;
    uint8_t mosi_port;
    uint8_t mosi_pin;
};

/* 配置引脚 + SPI 外设并注册成一条总线。
 *   spi_hw   1 = SPI1（APB2），2 = SPI2，3 = SPI3（都在 APB1）
 *   pclk_hz  该 SPI 所在 APB 的时钟频率（SPI1 用 PCLK2，SPI2/3 用 PCLK1），
 *            由 BSP 从 RCC 推算后注入 —— 驱动不假设主频
 *   hclk_hz  HCLK，用来把 DWT 周期数换算成 µs（段间 delay_usecs 用）
 * 成功返回已注册的控制器（可用 spi_get_controller(nr) 再取到），失败返回 NULL。
 *
 * 同一路 spi_hw 重复调用是幂等的：第二次直接返回第一次建好的控制器。
 */
struct spi_controller *stm32f1_spi_init(unsigned int spi_hw, uint32_t pclk_hz,
                                        uint32_t hclk_hz,
                                        const struct stm32f1_spi_pins *pins);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_STM32F1_SPI_H__ */
