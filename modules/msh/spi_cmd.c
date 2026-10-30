/*
 * spi_cmd.c - SPI 的 msh 命令 <modules/msh/>
 *
 * 只调 spi.h 的公开接口，不碰寄存器：是哪一路 SPI、引脚在哪，全由
 * bsp/<board>/board.c + spi_config.h 决定。
 *
 * 命令只导出一个 `spi`，其余动作都是子命令：
 *   spi list                     列出已注册的总线
 *   spi write <bus> <hex...>     单段发送（无片选，直接在总线上发）
 *   spi read  <bus> <n>          单段接收（发 0xFF 拉出 n 个字节）
 *   spi loop  <bus>              回环自检：把 MOSI 与 MISO 短接后跑
 *
 * 为什么要「无片选」的调试设备：spi list/read/write 是在不知道板上焊了什么
 * 器件时用的 —— 配合逻辑分析仪看波形，或者 MOSI/MISO 短接做回环。
 * 真有器件的场景应该自己建 struct spi_device 带 cs_pin，走 spi_write_then_read。
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "spi.h"

#define SPI_CMD_MAX_LEN     32u

static void spi_usage(void)
{
    rt_kprintf("用法: spi <子命令> [参数]\n"
               "  spi list                 列出已注册的 SPI 总线\n"
               "  spi write <bus> <hex...> 发一段字节，例: spi write 0 9f 00\n"
               "  spi read  <bus> <n>      收 n 字节并打印，例: spi read 0 8\n"
               "  spi loop  <bus>          回环自检（需把 MOSI 与 MISO 短接）\n");
}

static const char *spi_err_str(int err)
{
    switch (err)
    {
    case 0:              return "OK";
    case SPI_ERR_IO:     return "IO 错误";
    case SPI_ERR_PARAM:  return "参数非法";
    case SPI_ERR_NOMEM:  return "消息段数超限";
    case SPI_ERR_TIMEOUT: return "等 TXE/RXNE 超时";
    case SPI_ERR_BUSY:   return "总线忙";
    default:             return "未知错误";
    }
}

/* 取总线 + 造一个「不控片选」的临时设备：调试用，不假设板上有哪颗器件 */
static struct spi_device *spi_cmd_dev(unsigned int bus)
{
    static struct spi_device dev;

    dev.ctlr = spi_get_controller(bus);
    if (dev.ctlr == NULL)
    {
        rt_kprintf("spi%u: 未注册（先看 spi list）\n", bus);
        return NULL;
    }

    dev.max_speed_hz = 0u;          /* 0 = 走控制器最慢档，调试安全 */
    dev.mode = SPI_MODE_0;
    dev.bits_per_word = 8u;
    dev.cs_pin = SPI_CS_NONE;       /* 不拉任何片选 */

    return &dev;
}

/* ================= 子命令：list ================= */

static void spi_do_list(int argc, char **argv)
{
    unsigned int i;
    int found = 0;

    (void)argc;
    (void)argv;

    for (i = 0; i < SPI_MAX_CONTROLLERS; i++)
    {
        struct spi_controller *ctlr = spi_get_controller(i);

        if (ctlr == NULL)
        {
            continue;
        }
        rt_kprintf("spi%u: %s  最高 SCLK %u Hz\n",
                   i, (ctlr->name != NULL) ? ctlr->name : "-",
                   (unsigned int)ctlr->max_speed_hz);
        found = 1;
    }

    if (found == 0)
    {
        rt_kprintf("没有已注册的 SPI 总线（board_spi_init 是否成功？）\n");
    }
}

/* ================= 子命令：write ================= */

static void spi_do_write(int argc, char **argv)
{
    struct spi_device *dev;
    unsigned char buf[SPI_CMD_MAX_LEN];
    unsigned int bus;
    int n = 0;
    int i;
    int ret;

    if (argc < 2)
    {
        rt_kprintf("用法: spi write <bus> <hex...>\n    例: spi write 0 9f 00 00\n");
        return;
    }

    bus = (unsigned int)strtoul(argv[0], NULL, 0);
    for (i = 1; (i < argc) && (n < (int)SPI_CMD_MAX_LEN); i++)
    {
        buf[n++] = (unsigned char)strtoul(argv[i], NULL, 16);
    }

    dev = spi_cmd_dev(bus);
    if (dev == NULL)
    {
        return;
    }

    ret = spi_write(dev, buf, (uint16_t)n);
    if (ret != 0)
    {
        rt_kprintf("spi%u: 发送失败 %s(%d)\n", bus, spi_err_str(ret), ret);
        return;
    }
    rt_kprintf("spi%u: 已发送 %d 字节\n", bus, n);
}

/* ================= 子命令：read ================= */

static void spi_do_read(int argc, char **argv)
{
    struct spi_device *dev;
    unsigned char buf[SPI_CMD_MAX_LEN];
    unsigned int bus;
    int n;
    int i;
    int ret;

    if (argc < 2)
    {
        rt_kprintf("用法: spi read <bus> <n>\n    例: spi read 0 8\n");
        return;
    }

    bus = (unsigned int)strtoul(argv[0], NULL, 0);
    n = atoi(argv[1]);
    if ((n <= 0) || (n > (int)SPI_CMD_MAX_LEN))
    {
        rt_kprintf("n 取值 1~%d\n", (int)SPI_CMD_MAX_LEN);
        return;
    }

    dev = spi_cmd_dev(bus);
    if (dev == NULL)
    {
        return;
    }

    memset(buf, 0, sizeof(buf));
    ret = spi_read(dev, buf, (uint16_t)n);
    if (ret != 0)
    {
        rt_kprintf("spi%u: 接收失败 %s(%d)\n", bus, spi_err_str(ret), ret);
        return;
    }

    rt_kprintf("spi%u:", bus);
    for (i = 0; i < n; i++)
    {
        rt_kprintf(" %02x", buf[i]);
    }
    rt_kprintf("\n");
}

/* ================= 子命令：loop（回环自检） ================= */

static void spi_do_loop(int argc, char **argv)
{
    static const unsigned char pat[] = { 0x00u, 0xFFu, 0xA5u, 0x5Au, 0x01u, 0x80u };
    struct spi_device *dev;
    unsigned char rx[sizeof(pat)];
    unsigned int bus;
    int ret;
    unsigned int i;

    if (argc < 1)
    {
        rt_kprintf("用法: spi loop <bus>（先把 MOSI 与 MISO 短接）\n");
        return;
    }

    bus = (unsigned int)strtoul(argv[0], NULL, 0);
    dev = spi_cmd_dev(bus);
    if (dev == NULL)
    {
        return;
    }

    memset(rx, 0, sizeof(rx));
    ret = spi_write_then_read(dev, pat, (uint16_t)sizeof(pat), rx, (uint16_t)sizeof(rx));
    if (ret != 0)
    {
        rt_kprintf("spi%u: 回环失败 %s(%d)\n", bus, spi_err_str(ret), ret);
        return;
    }

    for (i = 0; i < sizeof(pat); i++)
    {
        if (rx[i] != pat[i])
        {
            rt_kprintf("spi%u: FAIL 第 %u 字节 期望 %02x 实际 %02x\n",
                       bus, i, pat[i], rx[i]);
            rt_kprintf("  排查：MOSI 与 MISO 是否真的短接？片上是否还有别的器件在驱动 MISO？\n");
            return;
        }
    }

    rt_kprintf("spi%u: 回环 PASS（%u 字节原样返回）\n", bus, (unsigned int)sizeof(pat));
}

/* ================= 入口：spi <子命令> [参数] =================
 * 只导出这一个命令，动作全走子命令。命令名 = 函数名 = `spi`。
 */
static void spi(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        spi_usage();
        return;
    }

    sub = argv[1];
    if (strcmp(sub, "list") == 0)
    {
        spi_do_list(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "write") == 0)
    {
        spi_do_write(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "read") == 0)
    {
        spi_do_read(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "loop") == 0)
    {
        spi_do_loop(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "help") == 0 || strcmp(sub, "?") == 0)
    {
        spi_usage();
    }
    else
    {
        rt_kprintf("未知子命令: %s\n", sub);
        spi_usage();
    }
}
MSH_CMD_EXPORT(spi, spi <list|write|read|loop>: SPI 总线调试);
