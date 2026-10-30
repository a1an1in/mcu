/*
 * flash_cmd.c - 板载 NOR FLASH 的 msh 命令 <modules/msh/>
 *
 * 只调 w25q128.h 的公开接口，不碰寄存器、不碰 SPI 时序：是哪颗片子、
 * 挂在哪条总线、片选是哪个脚，全由 bsp/<board>/board.c + w25q128_config.h 决定。
 *
 *   flash info                  读 JEDEC ID / 容量 / 状态寄存器
 *   flash read  <addr> [len]    读一段并打印（默认 16，最多 64）
 *   flash erase <addr>          擦除 addr 所在扇区（4KB 对齐）
 *   flash write <addr> <hex...> 写一段（**目标区域必须已擦除**，NOR 只能 1→0）
 *   flash test                  自检：擦 0 号扇区 → 写 256 字节 → 读回比对
 *
 * ⚠️ flash test / flash erase 会丢数据，命令描述里已标明。
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "w25q128.h"

#define FLASH_CMD_MAX_LEN   64u

static struct w25q128_dev *flash_handle(void)
{
    struct w25q128_dev *dev = w25q128_get_default();

    if (dev == NULL)
    {
        rt_kprintf("NOR FLASH 未注册（board_flash_init 失败？可看启动日志）\n");
    }
    return dev;
}

static const char *flash_err_str(int err)
{
    switch (err)
    {
    case 0:                 return "OK";
    case W25Q_ERR_RANGE:    return "地址越界";
    case W25Q_ERR_ID:       return "ID 不认识 / 器件不应答";
    case W25Q_ERR_TIMEOUT:  return "等 BUSY 超时";
    case W25Q_ERR_ALIGN:    return "地址未按 4KB 对齐";
    case SPI_ERR_PARAM:     return "参数非法";
    case SPI_ERR_NOMEM:     return "消息段数超限";
    case SPI_ERR_TIMEOUT:   return "SPI 等 TXE/RXNE 超时";
    case SPI_ERR_BUSY:      return "SPI 总线忙";
    default:                return "总线错误";
    }
}

static void flash_usage(void)
{
    rt_kprintf("用法: flash <子命令> [参数]\n"
               "  flash info                  读 ID / 容量 / 状态\n"
               "  flash read  <addr> [len]    读一段，例: flash read 0 32\n"
               "  flash erase <addr>          擦除 addr 所在扇区（会丢数据）\n"
               "  flash write <addr> <hex...> 写一段（区域必须已擦除）\n"
               "  flash test                  自检（会擦除 0 号扇区）\n");
}

/* ================= info ================= */

static void flash_do_info(int argc, char **argv)
{
    struct w25q128_dev *dev = flash_handle();
    uint8_t sr;
    int ret;

    (void)argc;
    (void)argv;

    if (dev == NULL)
    {
        return;
    }

    /* init 只绑总线不读 ID，这里显式探测一次，好把错误码打出来 */
    ret = w25q128_probe(dev);
    if (ret < 0)
    {
        rt_kprintf("探测失败: %s(%d)\n", flash_err_str(ret), ret);
        rt_kprintf("  排查：片选 PB12 是否被拉低 / 片子是否焊好 / spi list 看总线在不在\n");
        return;
    }

    sr = w25q128_read_status(dev);

    rt_kprintf("JEDEC ID : %06x（厂商 %02x / 类型 %02x / 容量码 %02x）\n",
               (unsigned int)dev->jedec,
               (unsigned int)((dev->jedec >> 16) & 0xFFu),
               (unsigned int)((dev->jedec >> 8) & 0xFFu),
               (unsigned int)(dev->jedec & 0xFFu));
    rt_kprintf("容量     : %u 字节（%u KB）\n",
               (unsigned int)dev->capacity, (unsigned int)(dev->capacity / 1024u));
    rt_kprintf("SR1      : 0x%02x（BUSY=%u WEL=%u）\n",
               (unsigned int)sr,
               (unsigned int)((sr & W25Q_SR1_BUSY) ? 1u : 0u),
               (unsigned int)((sr & W25Q_SR1_WEL) ? 1u : 0u));
}

/* ================= read ================= */

static void flash_do_read(int argc, char **argv)
{
    struct w25q128_dev *dev = flash_handle();
    uint8_t buf[FLASH_CMD_MAX_LEN];
    uint32_t addr;
    int len = 16;
    int ret;
    int i;

    if (dev == NULL)
    {
        return;
    }
    if (argc < 1)
    {
        rt_kprintf("用法: flash read <addr> [len<=%d]\n", (int)sizeof(buf));
        return;
    }

    addr = (uint32_t)strtoul(argv[0], NULL, 0);
    if (argc > 1)
    {
        len = atoi(argv[1]);
    }
    if ((len <= 0) || (len > (int)sizeof(buf)))
    {
        rt_kprintf("len 取值 1~%d\n", (int)sizeof(buf));
        return;
    }

    memset(buf, 0, sizeof(buf));
    ret = w25q128_read(dev, addr, buf, (uint32_t)len);
    if (ret < 0)
    {
        rt_kprintf("读失败: %s(%d)\n", flash_err_str(ret), ret);
        return;
    }

    for (i = 0; i < len; i++)
    {
        if ((i % 16) == 0)
        {
            if (i != 0)
            {
                rt_kprintf("\n");
            }
            rt_kprintf("%06x:", (unsigned int)(addr + (uint32_t)i));
        }
        rt_kprintf(" %02x", buf[i]);
    }
    rt_kprintf("\n");
}

/* ================= erase ================= */

static void flash_do_erase(int argc, char **argv)
{
    struct w25q128_dev *dev = flash_handle();
    uint32_t addr;
    int ret;

    if (dev == NULL)
    {
        return;
    }
    if (argc < 1)
    {
        rt_kprintf("用法: flash erase <addr>（addr 必须 4KB 对齐，会丢数据）\n");
        return;
    }

    addr = (uint32_t)strtoul(argv[0], NULL, 0);
    ret = w25q128_erase_sector(dev, addr);
    if (ret < 0)
    {
        rt_kprintf("擦除失败: %s(%d)\n", flash_err_str(ret), ret);
        return;
    }
    rt_kprintf("已擦除扇区 0x%06x ~ 0x%06x（4KB）\n",
               (unsigned int)addr, (unsigned int)(addr + W25Q_SECTOR_SIZE - 1u));
}

/* ================= write ================= */

static void flash_do_write(int argc, char **argv)
{
    struct w25q128_dev *dev = flash_handle();
    uint8_t buf[FLASH_CMD_MAX_LEN];
    uint32_t addr;
    int n = 0;
    int i;
    int ret;

    if (dev == NULL)
    {
        return;
    }
    if (argc < 2)
    {
        rt_kprintf("用法: flash write <addr> <hex...>\n"
                   "  例: flash write 0 12 34 ab（地址必须先 flash erase）\n");
        return;
    }

    addr = (uint32_t)strtoul(argv[0], NULL, 0);
    for (i = 1; (i < argc) && (n < (int)sizeof(buf)); i++)
    {
        buf[n++] = (uint8_t)strtoul(argv[i], NULL, 16);
    }

    ret = w25q128_write(dev, addr, buf, (uint32_t)n);
    if (ret < 0)
    {
        rt_kprintf("写失败: %s(%d)\n", flash_err_str(ret), ret);
        return;
    }
    rt_kprintf("已写入 %d 字节到 0x%06x\n", n, (unsigned int)addr);
}

/* ================= test ================= */

static void flash_do_test(int argc, char **argv)
{
    struct w25q128_dev *dev = flash_handle();
    uint8_t w[256];
    uint8_t r[32];
    int ret;
    unsigned int i;

    (void)argc;
    (void)argv;

    if (dev == NULL)
    {
        return;
    }

    /* 1) 擦 0 号扇区（0x0000~0x0FFF），后面两次写都落在这个扇区里 */
    rt_kprintf("步骤 1: 擦除 0 号扇区（0x000000~0x000FFF）\n");
    ret = w25q128_erase_sector(dev, 0u);
    if (ret < 0)
    {
        rt_kprintf("  擦除失败: %s(%d)\n", flash_err_str(ret), ret);
        return;
    }

    /* 2) 从 0x0F5 起写 20 字节：故意跨 0x0FF/0x100 页边界。
     *    这是最容易写错的地方 —— 不切页的话后 9 个字节会回卷到 0x000。 */
    rt_kprintf("步骤 2: 0x0F5 起写 20 字节（跨页边界 0x0FF / 0x100）\n");
    for (i = 0; i < 20u; i++)
    {
        w[i] = (uint8_t)(0x3Cu + i);
    }
    ret = w25q128_write(dev, 0xF5u, w, 20u);
    if (ret < 0)
    {
        rt_kprintf("  写失败: %s(%d)\n", flash_err_str(ret), ret);
        return;
    }

    memset(r, 0, sizeof(r));
    ret = w25q128_read(dev, 0xF5u, r, 20u);
    if (ret < 0)
    {
        rt_kprintf("  读失败: %s(%d)\n", flash_err_str(ret), ret);
        return;
    }
    for (i = 0; i < 20u; i++)
    {
        if (r[i] != (uint8_t)(0x3Cu + i))
        {
            rt_kprintf("  FAIL 0x%03X 期望 %02x 实际 %02x\n",
                       0xF5u + i, (uint8_t)(0x3Cu + i), r[i]);
            rt_kprintf("flash test: FAIL\n");
            return;
        }
    }

    /* 3) 从 0x200 起写一整页 256 字节（页对齐的常规用法） */
    rt_kprintf("步骤 3: 0x200 起写 256 字节（一整页）\n");
    for (i = 0; i < sizeof(w); i++)
    {
        w[i] = (uint8_t)(0xA5u ^ (i * 7u + 1u));
    }
    ret = w25q128_write(dev, 0x200u, w, (uint32_t)sizeof(w));
    if (ret < 0)
    {
        rt_kprintf("  写失败: %s(%d)\n", flash_err_str(ret), ret);
        return;
    }

    memset(r, 0, sizeof(r));
    ret = w25q128_read(dev, 0x200u, r, (uint32_t)sizeof(r));
    if (ret < 0)
    {
        rt_kprintf("  读失败: %s(%d)\n", flash_err_str(ret), ret);
        return;
    }
    for (i = 0; i < sizeof(r); i++)
    {
        if (r[i] != (uint8_t)(0xA5u ^ (i * 7u + 1u)))
        {
            rt_kprintf("  FAIL 0x%03X 期望 %02x 实际 %02x\n",
                       0x200u + i, (uint8_t)(0xA5u ^ (i * 7u + 1u)), r[i]);
            rt_kprintf("flash test: FAIL\n");
            return;
        }
    }

    rt_kprintf("flash test: PASS（0 号扇区已被覆盖）\n");
}

/* ================= 入口 ================= */

static void flash(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        flash_usage();
        return;
    }

    sub = argv[1];
    if (strcmp(sub, "info") == 0)
    {
        flash_do_info(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "read") == 0)
    {
        flash_do_read(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "erase") == 0)
    {
        flash_do_erase(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "write") == 0)
    {
        flash_do_write(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "test") == 0)
    {
        flash_do_test(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "help") == 0 || strcmp(sub, "?") == 0)
    {
        flash_usage();
    }
    else
    {
        rt_kprintf("未知子命令: %s\n", sub);
        flash_usage();
    }
}
MSH_CMD_EXPORT(flash, flash <info|read|erase|write|test>: 板载 NOR FLASH 调试);
