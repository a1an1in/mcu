/*
 * w25q128_config.h - W25Q128 的板级事实与可调参数 <drivers/flash/>
 *
 * 属于四维模型里的「板」：换板子只改本文件（或构建时 -D 覆盖），
 * 不动 w25q128.c。
 *
 * 引脚来源：正点原子《STM32F103 战舰开发指南》实验24 SPI实验
 *   —— 板载 25Q128（NM25Q128，兼容 W25Q128）
 *      SCK  = PB13   MISO = PB14   MOSI = PB15
 *      CS   = PB12（低有效）
 *      SPI2，模式 3（CPOL=1 / CPHA=1），MSB 先发
 *   SPI2 挂 APB1，72MHz 主频下 PCLK1 = 36MHz，厂商例程用 /2 = 18MHz。
 *   本工程当前 HSI 8MHz，PCLK1 = 8MHz，驱动会自动选不超过上限的最快分频。
 */
#ifndef DRIVERS_W25Q128_CONFIG_H__
#define DRIVERS_W25Q128_CONFIG_H__

#include "spi.h"

/* 片选引脚：端口编号 0=A 1=B 2=C 3=D 4=E 5=F 6=G */
#ifndef W25Q128_CS_PORT_IDX
#define W25Q128_CS_PORT_IDX     1u      /* PB */
#endif
#ifndef W25Q128_CS_PIN
#define W25Q128_CS_PIN          12u
#endif

/* 挂在哪条 SPI 总线上（取 spi_get_controller(n) 的编号，板级装配时决定） */
#ifndef W25Q128_SPI_BUS
#define W25Q128_SPI_BUS         0u
#endif

/* 这颗片子能接受的最高 SCLK（数据手册 25Q128：普通读 50MHz、快读 104MHz，
 * 这里取厂商例程的 18MHz，留足余量）。实际速率由 SPI 层按 PCLK 分频。 */
#ifndef W25Q128_SPEED_HZ
#define W25Q128_SPEED_HZ        18000000u
#endif

/* 25Q 系列固定为模式 3（CPOL=1、CPHA=1） */
#ifndef W25Q128_SPI_MODE
#define W25Q128_SPI_MODE        SPI_MODE_3
#endif

/* 等 BUSY 落下的最大轮询次数。
 * 一次轮询 = 读状态（2 字节 SPI 事务），4MHz 下约 6µs；
 * 扇区擦除最大 400ms ⇒ 需要 ~70000 次，这里给到 10 万次（约 0.6s @4MHz）。
 * 主频/速率变化时这个「时间」会跟着变，它只是防死循环的上限。 */
#ifndef W25Q128_BUSY_POLL_MAX
#define W25Q128_BUSY_POLL_MAX   100000u
#endif

/* 整片擦除的上限：16MB 全擦典型 20~40s，轮询按 ~4µs/次估算，
 * 8M 次 ≈ 32s（4MHz SCLK），留足余量。它只是防死循环的上限。 */
#ifndef W25Q128_CHIP_ERASE_POLL_MAX
#define W25Q128_CHIP_ERASE_POLL_MAX 8000000u
#endif

#endif /* DRIVERS_W25Q128_CONFIG_H__ */
