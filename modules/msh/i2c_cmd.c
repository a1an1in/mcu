/*
 * i2c_cmd.c - I2C 调试的 msh 命令 <modules/msh/>
 *
 * 与其他 msh 命令一样，这里是「给 shell 用的适配」：
 *   - 只调 i2c.h / smbus.h 的接口，不碰寄存器、不认识 STM32；
 *   - 所以 drivers/i2c/ 里不再有任何 msh 命令，换 RTOS 只影响本目录是否参与编译。
 *
 * 命令只导出一个 `i2c`，其余动作都是子命令：
 *   i2c scan [bus]               扫描总线上的从机地址（默认 bus=0）
 *   i2c read <addr> <reg> [len]  读器件寄存器（len 1~16，默认 1）
 *   i2c write <addr> <reg> <val> 写一个寄存器
 *   i2c smbtest                  SMBus 协议层自检（PEC 校验值）
 *   i2c help                     打印用法
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "i2c.h"
#include "smbus.h"

static void i2c_usage(void)
{
    rt_kprintf("用法: i2c <子命令> [参数]\n"
               "  i2c scan [bus]               扫描总线上的从机地址（默认 bus=0）\n"
               "  i2c read <addr> <reg> [len]  读器件寄存器（len 1~16，默认 1）\n"
               "  i2c write <addr> <reg> <val> 写一个寄存器\n"
               "  i2c smbtest                  SMBus 协议层自检（PEC 校验值）\n"
               "  例: i2c scan    i2c read 0x50 0x00 4    i2c write 0x50 0x00 0x5A\n");
}

/* ================= 子命令：scan ================= */

static void i2c_do_scan(int argc, char **argv)
{
    struct i2c_adapter *adap;
    int nr = 0;
    int addr;
    int found = 0;

    if (argc >= 1)
    {
        nr = atoi(argv[0]);
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

/* ================= 子命令：read ================= */

static void i2c_do_read(int argc, char **argv)
{
    struct i2c_adapter *adap;
    uint8_t buf[16];
    uint16_t addr;
    uint8_t reg;
    int len = 1;
    int ret;
    int i;

    if (argc < 2)
    {
        rt_kprintf("用法: i2c read <addr> <reg> [len<=16]\n");
        rt_kprintf("  例: i2c read 0x50 0x00 4   (AT24C02 前 4 字节)\n");
        return;
    }

    adap = i2c_get_adapter(0);
    if (adap == RT_NULL)
    {
        rt_kprintf("i2c0: 未注册\n");
        return;
    }

    addr = (uint16_t)strtoul(argv[0], RT_NULL, 0);
    reg  = (uint8_t)strtoul(argv[1], RT_NULL, 0);
    if (argc > 2)
    {
        len = atoi(argv[2]);
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

/* ================= 子命令：write ================= */

static void i2c_do_write(int argc, char **argv)
{
    struct i2c_adapter *adap;
    uint16_t addr;
    uint8_t reg;
    uint8_t val;
    int ret;

    if (argc < 3)
    {
        rt_kprintf("用法: i2c write <addr> <reg> <val>\n");
        rt_kprintf("  例: i2c write 0x50 0x00 0x5A\n");
        return;
    }

    adap = i2c_get_adapter(0);
    if (adap == RT_NULL)
    {
        rt_kprintf("i2c0: 未注册\n");
        return;
    }

    addr = (uint16_t)strtoul(argv[0], RT_NULL, 0);
    reg  = (uint8_t)strtoul(argv[1], RT_NULL, 0);
    val  = (uint8_t)strtoul(argv[2], RT_NULL, 0);

    ret = i2c_reg_write(adap, addr, reg, &val, 1);
    if (ret < 0)
    {
        rt_kprintf("写失败: %d\n", ret);
        return;
    }
    rt_kprintf("[0x%02x] reg 0x%02x <= 0x%02x\n", addr, reg, val);
}

/* ================= 子命令：smbtest =================
 * 板上如果没有真正的 SMBus 器件（只有 24C02 这种普通 I2C EEPROM），
 * 至少可以用标准校验值确认 PEC 算法本身是对的。
 */
static void i2c_do_smbtest(int argc, char **argv)
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

/* ================= 入口：i2c <子命令> [参数] =================
 * 只导出这一个命令，动作全部走子命令，避免在 msh 里刷出一堆 i2c_xxx。
 * 子处理函数收到的是「去掉 i2c 和子命令之后」的参数（argv[0] 即第一个参数），
 * 函数名用 i2c_do_* 是为了不和 i2c.h 的公开接口 i2c_read/i2c_write 撞名。
 *
 * 命令名 = 函数名 = `i2c`（MSH_CMD_EXPORT 拿符号名当命令名），
 * 不是 i2c_cmd —— msh 里敲的是 `i2c scan` / `i2c read 0x50 0x00 8`。
 */
static void i2c(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        i2c_usage();
        return;
    }

    sub = argv[1];
    if (strcmp(sub, "scan") == 0)
    {
        i2c_do_scan(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "read") == 0)
    {
        i2c_do_read(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "write") == 0)
    {
        i2c_do_write(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "smbtest") == 0)
    {
        i2c_do_smbtest(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "help") == 0 || strcmp(sub, "?") == 0)
    {
        i2c_usage();
    }
    else
    {
        rt_kprintf("未知子命令: %s\n", sub);
        i2c_usage();
    }
}
MSH_CMD_EXPORT(i2c, i2c <scan|read|write|smbtest>: I2C 总线调试);
