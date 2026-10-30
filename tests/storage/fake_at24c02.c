/*
 * fake_at24c02.c - host 单测用的「假 EEPROM 芯片」+ 假 I2C 总线
 *
 * 关键变化：假的层次放在 **i2c 层**，而不是 at24c02 层。
 *   - 用 256 字节 RAM 顶替真片子，实现 i2c_reg_read / i2c_reg_write /
 *     i2c_probe_device；
 *   - 于是真实的 at24c02.c（页切分、ACK 轮询、ops、at24c02_register）在 host 上
 *     真跑一遍，而不是被一个同名的假函数顶掉。
 *
 * 观测接口：
 *   fake_eeprom_write_ops()       i2c_reg_write 调用次数（= 实际页写次数）
 *   fake_eeprom_last_write_addr() 最后一次页写的起始地址
 *                                 （用来断言「正文先写、头部 0x00 最后提交」）
 *
 * 与 tests/hw/fake_gpio.c 是同一个思路：把「器件」换成可控的 RAM，
 * 让上层模块在宿主机上被真实地跑一遍。
 */
#include <string.h>

#include "i2c.h"

#define FAKE_EEPROM_SIZE    256u

static uint8_t s_mem[FAKE_EEPROM_SIZE];
static int s_write_ops;         /* 页写次数，用来验证"没改动就不写" */
static int s_last_write_addr = -1;
static int s_present = 1;       /* 0 = 模拟器件不响应（坏片 / 没焊） */

void fake_eeprom_reset(void)
{
    memset(s_mem, 0xFF, sizeof(s_mem));    /* 全新片子是全 0xFF */
    s_write_ops = 0;
    s_last_write_addr = -1;
    s_present = 1;
}

void fake_eeprom_fill(uint8_t v)
{
    memset(s_mem, v, sizeof(s_mem));
}

uint8_t *fake_eeprom_raw(void)
{
    return s_mem;
}

void fake_eeprom_poke(uint16_t addr, uint8_t val)
{
    s_mem[addr] = val;
}

int fake_eeprom_write_ops(void)
{
    return s_write_ops;
}

int fake_eeprom_last_write_addr(void)
{
    return s_last_write_addr;
}

void fake_eeprom_set_present(int present)
{
    s_present = present;
}

/* ---------------- i2c.h 的最小桩：把总线换成 RAM ----------------
 * 返回值的语义与真 i2c.c 一致：reg_read 成功返回字节数，reg_write 成功返回 0，
 * 失败返回负的 I2C_ERR_*。 */

int i2c_reg_read(struct i2c_adapter *adap, uint16_t addr, uint8_t reg,
                 uint8_t *buf, uint16_t len)
{
    (void)adap;
    (void)addr;

    if (s_present == 0)
    {
        return I2C_ERR_NACK;            /* 器件忙 / 不在场：对所有地址 NACK */
    }
    if (buf == NULL || len == 0u ||
        ((unsigned int)reg + (unsigned int)len) > FAKE_EEPROM_SIZE)
    {
        return I2C_ERR_PARAM;
    }

    memcpy(buf, &s_mem[reg], len);
    return (int)len;
}

int i2c_reg_write(struct i2c_adapter *adap, uint16_t addr, uint8_t reg,
                  const uint8_t *buf, uint16_t len)
{
    (void)adap;
    (void)addr;

    if (s_present == 0)
    {
        return I2C_ERR_NACK;
    }
    if (buf == NULL || len == 0u ||
        ((unsigned int)reg + (unsigned int)len) > FAKE_EEPROM_SIZE)
    {
        return I2C_ERR_PARAM;
    }

    memcpy(&s_mem[reg], buf, len);
    s_write_ops++;
    s_last_write_addr = (int)reg;
    return 0;
}

int i2c_probe_device(struct i2c_adapter *adap, uint16_t addr)
{
    (void)adap;
    (void)addr;

    return (s_present != 0) ? 0 : I2C_ERR_NACK;
}

static struct i2c_adapter s_dummy_adap;

struct i2c_adapter *i2c_get_adapter(unsigned int nr)
{
    return (nr == 0u) ? &s_dummy_adap : NULL;
}
