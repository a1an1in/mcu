/*
 * eeprom.c - 字节级 EEPROM 访问的通用核心 <drivers/eeprom/>
 *
 * 本文件**不认识任何器件**：没有 at24c02 的头、没有页大小、没有地址宽度、
 * 也没有「长度必须是 16 位」这种实现细节。只有三件事：
 *
 *   1) 参数与越界校验 —— 只写一遍，所有后端共享；
 *   2) 把请求分发给 dev->ops；
 *   3) 零长度短路 —— 上层不必自己判断 len == 0。
 *
 * 新增器件时**不用改本文件**：在器件自己的 .c 里加一个 ops 表 + 一个
 * register 函数即可（照抄 at24c02.c 末尾那段 at24c02_register()）。
 * 核心层与器件之间唯一的契约是 struct eeprom_ops 的两个函数。
 *
 * 本文件不含任何 OS 头，两个 RTOS 后端都能编。
 */
#include <stddef.h>
#include <stdint.h>

#include "eeprom.h"

int eeprom_register(struct eeprom_dev *dev, const struct eeprom_ops *ops,
                      void *ctx, uint32_t capacity)
{
    if (dev == NULL || ops == NULL || ops->read == NULL || ops->write == NULL)
    {
        return EEPROM_ERR_PARAM;
    }
    if (capacity == 0u)
    {
        return EEPROM_ERR_PARAM;
    }

    dev->ops = ops;
    dev->ctx = ctx;
    dev->capacity = capacity;
    return EEPROM_OK;
}

int eeprom_dev_valid(const struct eeprom_dev *dev)
{
    if (dev == NULL || dev->ops == NULL ||
        dev->ops->read == NULL || dev->ops->write == NULL)
    {
        return 0;
    }
    return (dev->capacity > 0u) ? 1 : 0;
}

/* addr + len 是否落在 [0, capacity] 内（允许正好写到尾部）。
 * 用减法而不是加法比较，避免 addr + len 在 32 位上回绕。 */
static int eeprom_range_ok(const struct eeprom_dev *dev, uint32_t addr, size_t len)
{
    if (addr > dev->capacity)
    {
        return 0;
    }
    return (len <= (size_t)(dev->capacity - addr)) ? 1 : 0;
}

int eeprom_read(struct eeprom_dev *dev, uint32_t addr, void *buf, size_t len)
{
    if (eeprom_dev_valid(dev) == 0 || buf == NULL)
    {
        return EEPROM_ERR_PARAM;
    }
    if (eeprom_range_ok(dev, addr, len) == 0)
    {
        return EEPROM_ERR_RANGE;
    }
    if (len == 0u)
    {
        return EEPROM_OK;           /* 空读：不打扰硬件 */
    }

    return dev->ops->read(dev, addr, buf, len);
}

int eeprom_write(struct eeprom_dev *dev, uint32_t addr, const void *buf, size_t len)
{
    if (eeprom_dev_valid(dev) == 0 || buf == NULL)
    {
        return EEPROM_ERR_PARAM;
    }
    if (eeprom_range_ok(dev, addr, len) == 0)
    {
        return EEPROM_ERR_RANGE;
    }
    if (len == 0u)
    {
        return EEPROM_OK;           /* 空写：不打扰硬件 */
    }

    return dev->ops->write(dev, addr, buf, len);
}
