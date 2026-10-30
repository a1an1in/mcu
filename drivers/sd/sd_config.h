/*
 * sd_config.h - SD 卡的板级事实 <drivers/sd/>
 *
 * 属于四维模型里的「板」：换板子只改本文件（或构建时 -D 覆盖），
 * 不动 stm32f1_sdio.c。
 *
 * 引脚来源：正点原子《STM32F103 战舰开发指南》第 49 章 SD 卡实验（实验37）
 *   —— 板载 Micro SD 座接 SDIO：
 *        D0 = PC8   D1 = PC9   D2 = PC10  D3 = PC11
 *        CLK = PC12
 *        CMD = PD2
 *      全部复用推挽 + 内部上拉。
 */
#ifndef DRIVERS_SD_CONFIG_H__
#define DRIVERS_SD_CONFIG_H__

/* 端口编号 0=A 1=B 2=C 3=D 4=E 5=F 6=G */
#ifndef SD_D0_PORT_IDX
#define SD_D0_PORT_IDX      2u      /* PC */
#endif
#ifndef SD_D0_PIN
#define SD_D0_PIN           8u
#endif

#ifndef SD_D1_PORT_IDX
#define SD_D1_PORT_IDX      2u
#endif
#ifndef SD_D1_PIN
#define SD_D1_PIN           9u
#endif

#ifndef SD_D2_PORT_IDX
#define SD_D2_PORT_IDX      2u
#endif
#ifndef SD_D2_PIN
#define SD_D2_PIN           10u
#endif

#ifndef SD_D3_PORT_IDX
#define SD_D3_PORT_IDX      2u
#endif
#ifndef SD_D3_PIN
#define SD_D3_PIN           11u
#endif

#ifndef SD_CLK_PORT_IDX
#define SD_CLK_PORT_IDX     2u
#endif
#ifndef SD_CLK_PIN
#define SD_CLK_PIN          12u
#endif

#ifndef SD_CMD_PORT_IDX
#define SD_CMD_PORT_IDX     3u      /* PD */
#endif
#ifndef SD_CMD_PIN
#define SD_CMD_PIN          2u
#endif

/* 初始化阶段时钟：SD 规范要求上电识别阶段 ≤ 400kHz */
#ifndef SD_INIT_CLOCK_HZ
#define SD_INIT_CLOCK_HZ    400000u
#endif

/* 数据传输阶段时钟。SDIO 的 CK = HCLK / (CLKDIV + 2)，驱动会取不超过本值的
 * 最接近分频。
 *
 * 取 24MHz：SD 默认速率模式的规范上限是 25MHz，24MHz 在 72MHz 主频下正好
 * 整除（/3）。实测这张 SanDisk SD64G 在 4 位 + 24MHz 下读写都稳定。
 * 注意这要求 CPU 跟得上：4 位 24MHz = 12MB/s，FIFO 轮询循环必须够快
 * （本工程 -O2 + 72MHz 有足够余量；如果换到很慢的主频，把这个值调小）。 */
#ifndef SD_TRANSFER_CLOCK_HZ
#define SD_TRANSFER_CLOCK_HZ 24000000u
#endif

#endif /* DRIVERS_SD_CONFIG_H__ */
