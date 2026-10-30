/*
 * stm32f1_sdio.h - STM32F1 SDIO 主机实现入口 <drivers/sd/>
 *
 * 本文件只声明「怎么在 STM32F1 上造一个 SDIO 主机」；用卡一律走 sd.h。
 */
#ifndef DRIVERS_STM32F1_SDIO_H__
#define DRIVERS_STM32F1_SDIO_H__

#include <stdint.h>

#include "sdio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 配置 SDIO 引脚与外设并上电（此时还没有和卡通信），返回主机句柄。
 *   hclk_hz  HCLK 频率，用来算 SDIO_CK 分频（SDIO 挂在 AHB 上）
 * 失败返回 NULL。同一块板重复调用返回同一个句柄。 */
struct sdio_host *stm32f1_sdio_init(uint32_t hclk_hz);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_STM32F1_SDIO_H__ */
