/*
 * at24c02_config.h - AT24C02 的板级事实与可调参数 <drivers/eeprom/>
 *
 * 属于四维模型里的「板」：换板子只改本文件（或构建时 -D 覆盖），
 * 不动 at24c02.c。和 i2c_config.h 同样的定位。
 *
 * 引脚来源：正点原子《STM32F103 战舰开发指南 V1.3》实验23 IIC实验
 *   —— 板载 24C02，A0/A1/A2 全部接地 → 7 位地址 0x50；
 *      WP 接地 → 写保护关闭（可以正常写）。
 */
#ifndef DRIVERS_AT24C02_CONFIG_H__
#define DRIVERS_AT24C02_CONFIG_H__

/* 7 位从机地址。A0~A2 接地 = 0x50；改接线时改这里。 */
#ifndef AT24C02_BOARD_ADDR
#define AT24C02_BOARD_ADDR      0x50u
#endif

/* ACK 轮询的最大次数。
 *
 * 单次探测的耗时 = (1 + adapter->retries) × 一次「START + 地址 + STOP」。
 * 本板 i2c0 的 retries = 3，100kHz 下一次传输约 130µs（含 START/STOP 的建立保持），
 * 所以器件忙时一次探测约 520µs。
 *
 * tWR 最大 5ms，32 × 520µs ≈ 17ms，覆盖 5ms 并留 3 倍余量；
 * 器件不存在时也只阻塞 ~17ms 就报错，不会卡死。
 * 若把 I2C_DELAY_US 调小（总线提速），按比例加大本值。 */
#ifndef AT24C02_ACK_POLL_MAX
#define AT24C02_ACK_POLL_MAX    32u
#endif

#endif /* DRIVERS_AT24C02_CONFIG_H__ */
