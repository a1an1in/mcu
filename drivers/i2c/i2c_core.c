/*
 * i2c_core.c - 通用 I2C 核心逻辑 <drivers/i2c/>
 *
 * 对应 Linux 的 i2c-core-base.c 里那一小块最常用的功能：
 *   - adapter 注册表（i2c_add_adapter / i2c_get_adapter）
 *   - i2c_transfer()：加总线锁 + 按 adapter->retries 重试
 *   - i2c_write / i2c_read（单段收发）
 *   - 寄存器读写（i2c_reg_read / i2c_reg_write，裸 I2C，非 SMBus）
 *   - i2c_probe_device()
 *
 * 本文件不碰任何寄存器，也不含任何 RTOS 头（总线锁走 OSAL）。
 */
#include <osal.h>

#include "i2c.h"

/* 一个项目里挂几条 I2C 总线，够用就行 */
#define I2C_ADAPTER_MAX     4

/* 一次传输（含重试）最长占用总线的时间 */
#define I2C_LOCK_TIMEOUT_MS 1000u

static struct i2c_adapter *s_adapters[I2C_ADAPTER_MAX];
static osal_mutex_t s_lock[I2C_ADAPTER_MAX];

/* ================= 注册表 ================= */

int i2c_add_adapter(struct i2c_adapter *adap)
{
    unsigned int i;

    if (adap == 0 || adap->algo == 0 || adap->algo->master_xfer == 0)
    {
        return I2C_ERR_PARAM;
    }

    /* 重复注册同一个 adapter 视为「取编号」，方便驱动初始化被调用两次 */
    for (i = 0; i < I2C_ADAPTER_MAX; i++)
    {
        if (s_adapters[i] == adap)
        {
            return (int)i;
        }
    }

    for (i = 0; i < I2C_ADAPTER_MAX; i++)
    {
        if (s_adapters[i] == 0)
        {
            /* 顺手把这条总线的互斥量建好：注册发生在板级单线程阶段，
             * 不存在「首次加锁时两个线程各建一把」的竞态 */
            if (s_lock[i] == OSAL_NULL)
            {
                if (osal_mutex_create(&s_lock[i]) != OSAL_OK)
                {
                    return I2C_ERR_IO;
                }
            }
            s_adapters[i] = adap;
            return (int)i;
        }
    }

    return I2C_ERR_IO;                  /* 表满 */
}

struct i2c_adapter *i2c_get_adapter(unsigned int nr)
{
    if (nr >= I2C_ADAPTER_MAX)
    {
        return 0;
    }
    return s_adapters[nr];
}

int i2c_adapter_nr(struct i2c_adapter *adap)
{
    unsigned int i;

    if (adap == 0)
    {
        return I2C_ERR_PARAM;
    }

    for (i = 0; i < I2C_ADAPTER_MAX; i++)
    {
        if (s_adapters[i] == adap)
        {
            return (int)i;
        }
    }
    return I2C_ERR_PARAM;
}

/* ================= 总线锁 =================
 * 每条总线一把互斥量，用 OSAL 的 osal_mutex_* 实现 —— 所以本文件不含任何
 * RTOS 头，换 OS/裸机只换 osal 后端，不需要再有 rtthread_i2c.c 这类适配文件。
 *
 * 锁在 i2c_add_adapter() 里创建：那时是板级单线程阶段，不会出现「两个线程
 * 同时首次加锁、各建一把锁」的竞态；建互斥量只分配对象、不依赖当前线程，
 * 调度器起来之前调用也安全。
 */
int i2c_bus_lock(struct i2c_adapter *adap)
{
    int nr = i2c_adapter_nr(adap);

    if ((nr < 0) || (s_lock[nr] == OSAL_NULL))
    {
        return I2C_ERR_PARAM;
    }
    if (osal_mutex_lock(s_lock[nr], I2C_LOCK_TIMEOUT_MS) != OSAL_OK)
    {
        return I2C_ERR_TIMEOUT;
    }
    return 0;
}

void i2c_bus_unlock(struct i2c_adapter *adap)
{
    int nr = i2c_adapter_nr(adap);

    if ((nr < 0) || (s_lock[nr] == OSAL_NULL))
    {
        return;
    }
    osal_mutex_unlock(s_lock[nr]);
}

/* ================= 传输 ================= */

int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
    int ret;
    uint16_t tries;

    if (adap == 0 || adap->algo == 0 || adap->algo->master_xfer == 0 ||
        msgs == 0 || num <= 0)
    {
        return I2C_ERR_PARAM;
    }

    /* 未注册的 adapter（PARAM）或等不到锁（TIMEOUT）都直接返回，不碰硬件 */
    ret = i2c_bus_lock(adap);
    if (ret != 0)
    {
        return ret;
    }

    /* 首次 + retries 次重试。只对「没拿到 num 条成功」重试，
     * 因为 NACK（比如 EEPROM 正在写内部）是最常见的可恢复失败。 */
    ret = adap->algo->master_xfer(adap, msgs, num);
    for (tries = 0; (ret != num) && (tries < adap->retries); tries++)
    {
        ret = adap->algo->master_xfer(adap, msgs, num);
    }

    i2c_bus_unlock(adap);

    if (ret == num)
    {
        return num;
    }
    return (ret < 0) ? ret : I2C_ERR_IO;
}

int i2c_write(struct i2c_adapter *adap, uint16_t addr,
              const uint8_t *buf, uint16_t len)
{
    struct i2c_msg msg;
    int ret;

    if (buf == 0 || len == 0)
    {
        return I2C_ERR_PARAM;
    }

    msg.addr  = addr;
    msg.flags = 0;
    msg.len   = len;
    msg.buf   = (uint8_t *)buf;

    ret = i2c_transfer(adap, &msg, 1);
    return (ret == 1) ? (int)len : ret;     /* 与 Linux 一致：返回字节数 */
}

int i2c_read(struct i2c_adapter *adap, uint16_t addr,
             uint8_t *buf, uint16_t len)
{
    struct i2c_msg msg;
    int ret;

    if (buf == 0 || len == 0)
    {
        return I2C_ERR_PARAM;
    }

    msg.addr  = addr;
    msg.flags = I2C_M_RD;
    msg.len   = len;
    msg.buf   = buf;

    ret = i2c_transfer(adap, &msg, 1);
    return (ret == 1) ? (int)len : ret;
}

/* ================= 寄存器读写（裸 I2C）=================
 * 注意：这不是 SMBus 的 Block Read —— 数据前没有计数字节。
 */

int i2c_reg_read(struct i2c_adapter *adap, uint16_t addr, uint8_t reg,
                 uint8_t *buf, uint16_t len)
{
    uint8_t reg_copy = reg;
    struct i2c_msg msgs[2];
    int ret;

    if (buf == 0 || len == 0)
    {
        return I2C_ERR_PARAM;
    }

    /* 第一段写寄存器地址，第二段重复起始位后按读方向收 len 个字节 */
    msgs[0].addr  = addr;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg_copy;

    msgs[1].addr  = addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = len;
    msgs[1].buf   = buf;

    ret = i2c_transfer(adap, msgs, 2);
    if (ret < 0)
    {
        return ret;
    }
    return (ret == 2) ? (int)len : I2C_ERR_IO;
}

int i2c_reg_write(struct i2c_adapter *adap, uint16_t addr, uint8_t reg,
                  const uint8_t *buf, uint16_t len)
{
    uint8_t reg_copy = reg;
    struct i2c_msg msgs[2];
    int ret;

    if (buf == 0 || len == 0)
    {
        return I2C_ERR_PARAM;
    }

    /* 两段拼成一次连续写：先寄存器地址，再用 I2C_M_NOSTART 接着发数据。
     * 这样调用方不用自己拼 [reg, data...] 缓冲区，也不会受栈上临时数组大小限制。 */
    msgs[0].addr  = addr;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg_copy;

    msgs[1].addr  = addr;
    msgs[1].flags = I2C_M_NOSTART;
    msgs[1].len   = len;
    msgs[1].buf   = (uint8_t *)buf;

    ret = i2c_transfer(adap, msgs, 2);
    if (ret < 0)
    {
        return ret;
    }
    return (ret == 2) ? 0 : I2C_ERR_IO;
}

/* ================= 地址探测 ================= */

int i2c_probe_device(struct i2c_adapter *adap, uint16_t addr)
{
    struct i2c_msg msg;
    int ret;

    if (addr > 0x7Fu)
    {
        return I2C_ERR_PARAM;
    }

    /* len = 0：只发地址字节 + 停止位，看从机是否回 ACK。
     * 用读方向可以避免像 EEPROM 那样「收到写地址就去改内部指针」。
     * 这就是 SMBus 的 Quick Command 读方向，正式走协议请用 smbus.h。 */
    msg.addr  = addr;
    msg.flags = I2C_M_RD;
    msg.len   = 0;
    msg.buf   = 0;

    ret = i2c_transfer(adap, &msg, 1);
    if (ret < 0)
    {
        return ret;
    }
    return (ret == 1) ? 0 : I2C_ERR_IO;
}
