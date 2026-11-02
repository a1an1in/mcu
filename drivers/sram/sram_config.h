/*
 * sram_config.h - 本板外部 SRAM 事实 <drivers/sram/>
 *
 * 战舰 V4：IS62WV51216（512K × 16bit = 1MB），挂在 FSMC Bank1 NE3。
 * 如果换板/换 Bank，只改这里。
 */
#ifndef DRIVERS_SRAM_CONFIG_H__
#define DRIVERS_SRAM_CONFIG_H__

#define SRAM_BASE_ADDR      0x68000000u     /* Bank1 NE3 */
#define SRAM_SIZE_BYTES     (512u * 1024u * 2u)   /* 1MB */

/* FSMC 时序（HCLK=72MHz，单位：HCLK 周期）：
 *   ADDSET = 1，DATAST = 15，ACCmode A
 *   IS62WV51216 是 55ns 器件，这个余量足够稳。 */
#define SRAM_FSMC_BCR       0x00001011u     /* MBKEN | MWID=16bit | WREN */
#define SRAM_FSMC_BTR       0x00000F01u     /* DATAST=15, ADDSET=1 */

#endif /* DRIVERS_SRAM_CONFIG_H__ */
