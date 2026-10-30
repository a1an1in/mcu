/*
 * i2c_config.h - I2C 的板级事实 <drivers/i2c/>
 *
 * 这里只放「这块板子把总线焊在哪」这类信息，属于四维模型里的「板」。
 * 换板子只改本文件的宏（或构建时 -D 覆盖），不动 stm32f1_i2c.c。
 *
 * 引脚来源：正点原子《STM32F103 战舰开发指南 V1.3》实验23 IIC实验
 *   —— SCL = PB6，SDA = PB7，板载 24C02（两条线各有上拉电阻）
 */
#ifndef DRIVERS_I2C_CONFIG_H__
#define DRIVERS_I2C_CONFIG_H__

/* 端口编号：0=A 1=B 2=C 3=D 4=E（与 stm32f1_gpio.c 的端口表一致） */
#ifndef I2C_SCL_PORT_IDX
#define I2C_SCL_PORT_IDX    1u
#endif
#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN         6u
#endif

#ifndef I2C_SDA_PORT_IDX
#define I2C_SDA_PORT_IDX    1u
#endif
#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN         7u
#endif

/* 一个 SCL 半周期的延时（µs）。
 *   5µs → 100kHz：SMBus 允许的最高速率，默认选它。
 *   2µs → 约 250kHz：只跑普通 I2C 时可以用，但超出 SMBus 规范上限。 */
#ifndef I2C_DELAY_US
#define I2C_DELAY_US        5u
#endif

/* 从机拉低 SCL（时钟延展）的最长等待，单位 µs。
 * SMBus 规范 tTIMEOUT = 25ms（最小），超过就应中止本次传输。 */
#ifndef I2C_STRETCH_TIMEOUT_US
#define I2C_STRETCH_TIMEOUT_US  25000u
#endif

/* CRL / CRH 每个引脚 4bit = CNF[1:0] + MODE[1:0]（见 RM0008 Table 20）
 *
 *   SCL：通用开漏输出，MODE=10(2MHz) CNF=01 → 0b0110 = 0x6
 *   SDA：通用开漏输出，MODE=10(2MHz) CNF=01 → 0b0110 = 0x6
 *
 * 为什么两条线都用开漏：
 *   1) SDA 必须开漏 —— 从机要在 ACK 和读数据时拉低它，推挽会短路；
 *      写 1 = 关断输出管 = 释放（靠上拉变高），IDR 仍能读到真实电平，
 *      所以读写都不用来回切输入/输出方向。
 *   2) SCL 也开漏 —— SMBus 允许从机做「时钟延展」把 SCL 拉低，
 *      推挽输出会和它对着拉，既是电气冲突也检测不到延展。
 *      开漏后主机只负责「放手」，从机松开时线自己升到高电平。
 *
 * ⚠️ 前提：板上 SCL 必须有上拉电阻（战舰 V4 的 24C02 两线都带 4.7k 上拉）。
 *    如果换成没有 SCL 上拉的板子，会把本项改成 0x2（推挽，SCL 由主机独占驱动），
 *    代价是失去时钟延展支持 —— 此时 i2c_bit_recover() 会在启动时报 I2C_ERR_BUS。
 *
 * 选 2MHz 而不是 50MHz：I2C 只有 100kHz，慢档边沿更缓、EMI 和过冲更小。
 */
#ifndef I2C_SCL_CFG_NIBBLE
#define I2C_SCL_CFG_NIBBLE  0x6u
#endif
#ifndef I2C_SDA_CFG_NIBBLE
#define I2C_SDA_CFG_NIBBLE  0x6u
#endif

#endif /* DRIVERS_I2C_CONFIG_H__ */
