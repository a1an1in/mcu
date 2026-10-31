/*
 * vs1053_config.h - VS1053 的板级事实 <drivers/audio/>
 *
 * 属于四维模型里的「板」：换板只改本文件，不动 vs1053.c。
 *
 * 本板（正点原子 战舰 STM32F103 V4）：
 *   VS1053 与 CH395Q 共用 SPI1（PA5/PA6/PA7）：
 *     XCS  = PF7（命令片选）
 *     XDCS = PF6（数据片选）
 *     DREQ = PC13（输入，高 = 可以收数据）
 *     RST  = PE6（低有效）
 *   喇叭功放 HT6872 由 VS1053 的 GPIO4 控制（软件写 RAM，不占 MCU 引脚）。
 */
#ifndef DRIVERS_AUDIO_VS1053_CONFIG_H__
#define DRIVERS_AUDIO_VS1053_CONFIG_H__

#include "gpio.h"
#include "spi.h"

/* GPIO 编码：GPIO_PIN(端口, 引脚) */
#define VS1053_RST_PIN          GPIO_PIN(GPIO_PORT_E, 6)    /* PE6  */
#define VS1053_DREQ_PIN         GPIO_PIN(GPIO_PORT_C, 13)   /* PC13 */

/* 片选：SPI_CS_PIN(端口编号, 引脚)，端口 0=A ... 5=F */
#define VS1053_XCS_PIN          SPI_CS_PIN(5, 7)            /* PF7 */
#define VS1053_XDCS_PIN         SPI_CS_PIN(5, 6)            /* PF6 */

/* SPI 时钟：VS1053B 上限是 55.296/6 ≈ 9.216MHz。
 * 命令用 2MHz 左右，数据用 8MHz（STM32F1 在 72MHz PCLK2 下实际是 2.25/9MHz）。 */
#ifndef VS1053_SPI_CMD_HZ
#define VS1053_SPI_CMD_HZ       2000000u
#endif
#ifndef VS1053_SPI_DATA_HZ
#define VS1053_SPI_DATA_HZ      8000000u
#endif

/* DREQ 等待上限（毫秒）：正常解码时几毫秒就会变高 */
#ifndef VS1053_DREQ_TIMEOUT_MS
#define VS1053_DREQ_TIMEOUT_MS  500u
#endif

#endif /* DRIVERS_AUDIO_VS1053_CONFIG_H__ */
