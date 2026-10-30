/*
 * spi_config.h - SPI 的板级事实 <drivers/spi/>
 *
 * 属于四维模型里的「板」：换板子只改本文件（或构建时 -D 覆盖），
 * 不动 stm32f1_spi.c。
 *
 * 本板（正点原子 战舰 STM32F103 V4）的 SPI 现状：
 *   SPI1（PA5/PA6/PA7）上挂了 CH395Q 与 VS1053，由各自的驱动自己配置 ——
 *   不适合再让通用 SPI 层去当「总线主人」。
 *   SPI2（PB13/PB14/PB15）是板载 25Q128 的总线（片选 PB12，见
 *   drivers/flash/w25q128_config.h），也就是本工程的通用总线 spi0：
 *   这颗 NOR FLASH 就是 spi0 上的第一个设备。
 *
 * 注意一条线上挂多颗器件时，CS 归设备（struct spi_device.cs_pin），
 * 不归总线 —— 换器件只改设备的 cs_pin，不用动这里。
 */
#ifndef DRIVERS_SPI_CONFIG_H__
#define DRIVERS_SPI_CONFIG_H__

/* 用哪一路 SPI 外设：1 = SPI1(APB2)，2 = SPI2(APB1)，3 = SPI3(APB1) */
#ifndef SPI0_HW
#define SPI0_HW                 2u
#endif

/* 引脚：端口编号 0=A 1=B 2=C 3=D 4=E 5=F 6=G（与 stm32f1_spi.c 的端口表一致） */
#ifndef SPI0_SCK_PORT_IDX
#define SPI0_SCK_PORT_IDX       1u
#endif
#ifndef SPI0_SCK_PIN
#define SPI0_SCK_PIN            13u
#endif

#ifndef SPI0_MISO_PORT_IDX
#define SPI0_MISO_PORT_IDX      1u
#endif
#ifndef SPI0_MISO_PIN
#define SPI0_MISO_PIN           14u
#endif

#ifndef SPI0_MOSI_PORT_IDX
#define SPI0_MOSI_PORT_IDX      1u
#endif
#ifndef SPI0_MOSI_PIN
#define SPI0_MOSI_PIN           15u
#endif

#endif /* DRIVERS_SPI_CONFIG_H__ */
