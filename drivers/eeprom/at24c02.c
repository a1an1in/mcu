/*
 * at24c02.c - AT24C02 EEPROM 驱动实现 <drivers/eeprom/>
 *
 * 全部建立在 i2c.h 之上，不碰寄存器、不依赖 OS。
 *
 * ── 为什么只有两个传输形状 ─────────────────────────────────────────────
 * 24C02 的字地址是 1 个字节，正好对上 i2c.h 的寄存器读写：
 *
 *   写：START [addr|W] [word] [d0]...[dn] STOP
 *       = i2c_reg_write(adap, addr, word, buf, len)
 *
 *   读：START [addr|W] [word] Sr [addr|R] [d0]...[dn] STOP
 *       = i2c_reg_read (adap, addr, word, buf, len)
 *
 * 所以本文件不重复拼 msg，只负责 24C02 特有的两件事：按页切分、等写周期。
 *
 * ── read / write 就是本器件的 eeprom_ops ──────────────────────────────
 * 两个函数的签名与 eeprom.h 的 struct eeprom_ops 逐项一致（首参
 * struct eeprom_dev *），于是注册时**不需要任何包装函数**：
 *
 *     const struct eeprom_ops at24c02_eeprom_ops = {
 *         .read = at24c02_read, .write = at24c02_write,
 *     };
 *
 * 器件自己的 struct at24c02_dev * 放在 dev->ctx 里，函数入口取出来并校验
 * adap —— 等价 Linux 里 file_operations 用 filp->private_data 取私有数据。
 *
 * 本文件不含任何 OS 头。
 */
#include "at24c02.h"
#include "at24c02_config.h"

/* ops 收到的是注册后的 eeprom_dev，器件私有句柄在 dev->ctx 里 */
static struct at24c02_dev *at24c02_chip_of(struct eeprom_dev *dev)
{
    return (dev == 0) ? 0 : (struct at24c02_dev *)dev->ctx;
}

/* addr + len 是否落在 [0, AT24C02_CAPACITY] 内。
 * 先比 addr 再相减，避免 addr > capacity 时的无符号下溢。 */
static int at24c02_range_ok(uint32_t addr, size_t len)
{
    if (addr > AT24C02_CAPACITY)
    {
        return 0;
    }
    return (len <= (size_t)(AT24C02_CAPACITY - addr)) ? 1 : 0;
}

int at24c02_wait_ready(struct at24c02_dev *dev)
{
    unsigned int i;

    if (dev == 0 || dev->adap == 0)
    {
        return I2C_ERR_PARAM;
    }

    /* 写周期内器件对所有地址都回 NACK；能收到 ACK 就说明写完了。
     * 每次探测都是一次完整的 START + 地址 + STOP，本身就要百来微秒，
     * 所以这个循环天然带延时 —— 不需要 OS 的 sleep，驱动保持 OS 无关。 */
    for (i = 0; i < AT24C02_ACK_POLL_MAX; i++)
    {
        if (i2c_probe_device(dev->adap, dev->addr) == 0)
        {
            return 0;
        }
    }

    return I2C_ERR_TIMEOUT;
}

int at24c02_read(struct eeprom_dev *dev, uint32_t addr, void *buf, size_t len)
{
    struct at24c02_dev *chip = at24c02_chip_of(dev);
    int ret;

    if (chip == 0 || chip->adap == 0 || buf == 0 || len == 0u)
    {
        return EEPROM_ERR_PARAM;
    }
    /* 与其让器件在 0xFF 之后回卷到 0x00 读出一堆意外数据，不如直接报错 */
    if (at24c02_range_ok(addr, len) == 0)
    {
        return EEPROM_ERR_RANGE;
    }

    /* 一次给字地址，之后器件内部地址自动递增 —— 顺序读 */
    ret = i2c_reg_read(chip->adap, chip->addr, (uint8_t)addr, (uint8_t *)buf,
                       (uint16_t)len);
    return (ret == (int)len) ? EEPROM_OK : EEPROM_ERR_IO;
}

int at24c02_write(struct eeprom_dev *dev, uint32_t addr, const void *buf, size_t len)
{
    struct at24c02_dev *chip = at24c02_chip_of(dev);
    uint32_t pos = addr;
    size_t left = len;
    const uint8_t *p = (const uint8_t *)buf;

    if (chip == 0 || chip->adap == 0 || buf == 0 || len == 0u)
    {
        return EEPROM_ERR_PARAM;
    }
    if (at24c02_range_ok(addr, len) == 0)
    {
        return EEPROM_ERR_RANGE;
    }

    while (left > 0u)
    {
        /* 本页还剩几个字节可写：从 pos 到本页末尾。
         * 例：pos=0x05 → 0x05..0x07 共 3 字节；pos=0x08 → 8 字节。 */
        uint32_t room  = AT24C02_PAGE_SIZE - (pos % AT24C02_PAGE_SIZE);
        uint32_t chunk = (left < room) ? (uint32_t)left : room;
        int ret;

        ret = i2c_reg_write(chip->adap, chip->addr, (uint8_t)pos, p,
                            (uint16_t)chunk);
        if (ret < 0)
        {
            return EEPROM_ERR_IO;
        }

        /* 每页写完必须等内部写周期结束，否则下一页的写命令会被器件忽略 */
        if (at24c02_wait_ready(chip) < 0)
        {
            return EEPROM_ERR_IO;
        }

        pos  += chunk;
        p    += chunk;
        left -= chunk;
    }

    return EEPROM_OK;
}

/* 本器件的驱动 ops：直接指向上面两个函数，没有任何转形状的壳 */
const struct eeprom_ops at24c02_eeprom_ops = {
    .read  = at24c02_read,
    .write = at24c02_write,
};

int at24c02_register(struct eeprom_dev *dev, struct at24c02_dev *chip,
                     uint32_t capacity)
{
    if (dev == 0 || chip == 0 || chip->adap == 0)
    {
        return EEPROM_ERR_PARAM;
    }
    /* 容量可以小于整片（当分区用），但不能超过 24C02 的物理容量 */
    if (capacity == 0u || capacity > AT24C02_CAPACITY)
    {
        return EEPROM_ERR_PARAM;
    }

    return eeprom_register(dev, &at24c02_eeprom_ops, chip, capacity);
}
