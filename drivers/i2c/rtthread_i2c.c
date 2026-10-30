/*
 * rtthread_i2c.c - I2C 的 RT-Thread 适配 <drivers/i2c/>
 *
 * 只做 OS 相关的事，不碰任何寄存器：
 *   1) 用互斥量实现 i2c_bus_lock / i2c_bus_unlock，
 *      覆盖 i2c_core.c 里的 weak 空实现（链接时强符号优先）
 *   2) 注册 msh 调试命令：i2c_scan / i2c_read / i2c_write
 *
 * 互斥量惰性创建：首次加锁时创建，避免 BSP 再额外调用一次初始化函数。
 */
#include <rtthread.h>
#include <stdlib.h>

#include "i2c.h"
#include "smbus.h"

static struct rt_mutex *s_i2c_mutex = RT_NULL;

/* 单位：系统节拍。1s 足够覆盖一次最长传输（EEPROm 页写也就几 ms） */
#define I2C_LOCK_TIMEOUT_MS     1000

int i2c_bus_lock(struct i2c_adapter *adap)
{
    (void)adap;

    if (s_i2c_mutex == RT_NULL)
    {
        s_i2c_mutex = rt_mutex_create("i2cbus", RT_IPC_FLAG_PRIO);
        if (s_i2c_mutex == RT_NULL)
        {
            return I2C_ERR_IO;          /* 堆不够，退化为不保护 */
        }
    }

    if (rt_mutex_take(s_i2c_mutex, rt_tick_from_millisecond(I2C_LOCK_TIMEOUT_MS)) != RT_EOK)
    {
        return I2C_ERR_TIMEOUT;
    }
    return 0;
}

void i2c_bus_unlock(struct i2c_adapter *adap)
{
    (void)adap;

    if (s_i2c_mutex != RT_NULL)
    {
        (void)rt_mutex_release(s_i2c_mutex);
    }
}

/* ================= msh 调试命令 ================= */

static void i2c_scan(int argc, char **argv)
{
    struct i2c_adapter *adap;
    int nr = 0;
    int addr;
    int found = 0;

    if (argc > 1)
    {
        nr = atoi(argv[1]);
    }

    adap = i2c_get_adapter((unsigned int)nr);
    if (adap == RT_NULL)
    {
        rt_kprintf("i2c%d: 未注册\n", nr);
        return;
    }

    rt_kprintf("scan %s (7 位地址 0x08~0x77):\n", adap->name);

    for (addr = 0x08; addr <= 0x77; addr++)
    {
        if (i2c_probe_device(adap, (uint16_t)addr) == 0)
        {
            rt_kprintf("  0x%02x  ACK\n", addr);
            found++;
        }
    }

    rt_kprintf("共 %d 个器件\n", found);
}
MSH_CMD_EXPORT(i2c_scan, i2c_scan [bus]: 扫描 I2C 总线上的从机地址);

/* i2c_read <addr> <reg> [len] —— 读寄存器（默认 1 字节）
 * 函数名加 _cmd_ 前缀避免和 i2c.h 的公开接口 i2c_read()/i2c_write() 撞名，
 * 命令名用 MSH_CMD_EXPORT_ALIAS 保持为 i2c_read / i2c_write。 */
static void i2c_cmd_read(int argc, char **argv)
{
    struct i2c_adapter *adap;
    uint8_t buf[16];
    uint16_t addr;
    uint8_t reg;
    int len = 1;
    int ret;
    int i;

    if (argc < 3)
    {
        rt_kprintf("用法: i2c_read <addr> <reg> [len<=16]\n");
        rt_kprintf("  例: i2c_read 0x50 0x00 4   (AT24C02 前 4 字节)\n");
        return;
    }

    adap = i2c_get_adapter(0);
    if (adap == RT_NULL)
    {
        rt_kprintf("i2c0: 未注册\n");
        return;
    }

    addr = (uint16_t)strtoul(argv[1], RT_NULL, 0);
    reg  = (uint8_t)strtoul(argv[2], RT_NULL, 0);
    if (argc > 3)
    {
        len = atoi(argv[3]);
        if (len <= 0 || len > (int)sizeof(buf))
        {
            rt_kprintf("len 取值 1~%d\n", (int)sizeof(buf));
            return;
        }
    }

    ret = i2c_reg_read(adap, addr, reg, buf, (uint16_t)len);
    if (ret < 0)
    {
        rt_kprintf("读失败: %d\n", ret);
        return;
    }

    rt_kprintf("[0x%02x] reg 0x%02x:", addr, reg);
    for (i = 0; i < len; i++)
    {
        rt_kprintf(" %02x", buf[i]);
    }
    rt_kprintf("\n");
}
MSH_CMD_EXPORT_ALIAS(i2c_cmd_read, i2c_read, i2c_read <addr> <reg> [len]: 读 I2C 器件寄存器);

/* i2c_write <addr> <reg> <val> —— 写一个寄存器 */
static void i2c_cmd_write(int argc, char **argv)
{
    struct i2c_adapter *adap;
    uint16_t addr;
    uint8_t reg;
    uint8_t val;
    int ret;

    if (argc < 4)
    {
        rt_kprintf("用法: i2c_write <addr> <reg> <val>\n");
        rt_kprintf("  例: i2c_write 0x50 0x00 0x5A\n");
        return;
    }

    adap = i2c_get_adapter(0);
    if (adap == RT_NULL)
    {
        rt_kprintf("i2c0: 未注册\n");
        return;
    }

    addr = (uint16_t)strtoul(argv[1], RT_NULL, 0);
    reg  = (uint8_t)strtoul(argv[2], RT_NULL, 0);
    val  = (uint8_t)strtoul(argv[3], RT_NULL, 0);

    ret = i2c_reg_write(adap, addr, reg, &val, 1);
    if (ret < 0)
    {
        rt_kprintf("写失败: %d\n", ret);
        return;
    }
    rt_kprintf("[0x%02x] reg 0x%02x <= 0x%02x\n", addr, reg, val);
}
MSH_CMD_EXPORT_ALIAS(i2c_cmd_write, i2c_write, i2c_write <addr> <reg> <val>: 写 I2C 器件寄存器);

/* ================= SMBus 协议层自检 =================
 * 板上如果没有真正的 SMBus 器件（只有 24C02 这种普通 I2C EEPROM），
 * 至少可以用标准校验值确认 PEC 算法本身是对的。
 */
static void smbus_test(int argc, char **argv)
{
    struct i2c_adapter *adap = i2c_get_adapter(0);
    struct smbus_dev dev;
    uint8_t v = 0;
    int ret;

    (void)argc;
    (void)argv;

    ret = smbus_selftest();
    rt_kprintf("PEC CRC-8 self-test: %s\n", (ret == 0) ? "OK (\"123456789\" -> 0xF4)" : "FAIL");

    if (adap == RT_NULL)
    {
        return;
    }

    /* 统一接口演示：同一个函数靠 cmd / len 选协议。
     * 对 0x50 读地址 0 的 1 个字节 —— AT24C02 也认这种「命令码 + 读」的时序。 */
    smbus_dev_init(&dev, adap, 0x50, 0);

    ret = smbus_read(&dev, 0x00, &v, 1);                /* Read Byte */
    rt_kprintf("smbus_read(0x50, cmd=0x00, len=1): ret=%d data=0x%02x\n", ret, v);

    ret = smbus_read(&dev, SMBUS_NO_CMD, &v, 1);        /* Receive Byte */
    rt_kprintf("smbus_read(0x50, NO_CMD, len=1): ret=%d data=0x%02x\n", ret, v);
}
MSH_CMD_EXPORT(smbus_test, smbus_test: SMBus 协议层自检);
