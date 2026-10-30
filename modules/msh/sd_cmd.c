/*
 * sd_cmd.c - SD 卡的 msh 命令 <modules/msh/>
 *
 * 只调 sd.h 的公开接口，不碰 SDIO 寄存器：卡插在哪、时钟多少，全由
 * bsp/<board>/board.c + sd_config.h 决定。
 *
 *   sd info                 识别卡并打印类型 / 容量 / CID
 *   sd read  <lba> [cnt]    读 1~4 个块并打印前 64 字节
 *   sd write <lba> <hex...> 写一个块（其余字节填 0，会覆盖原内容！）
 *   sd test                 在最后一个块上写图样再读回比对（会覆盖该块！）
 *
 * ⚠️ 卡上如果有文件系统，写操作会破坏它。这是调试命令，不是日常接口。
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "sd.h"

#define SD_CMD_MAX_WRITE    64u         /* 一次最多写这么多个字节（其余填 0） */
#define SD_CMD_MAX_BLOCKS   4u          /* sd read 一次最多读几块 */

/* 读多块时整块都落在这里，所以按最大块数分配（4 × 512 = 2KB） */
static uint8_t s_block[SD_BLOCK_SIZE * SD_CMD_MAX_BLOCKS] __attribute__((aligned(4)));

static struct sd_card *sd_handle(void)
{
    struct sd_card *card = sd_get_default();

    if (card == NULL)
    {
        rt_kprintf("SD 未注册（board_sd_init 失败？）\n");
    }
    return card;
}

static const char *sd_err_str(int err)
{
    switch (err)
    {
    case 0:                 return "OK";
    case SD_ERR_IO:         return "传输失败";
    case SD_ERR_PARAM:      return "参数非法";
    case SD_ERR_TIMEOUT:    return "超时";
    case SD_ERR_CRC:        return "CRC 错";
    case SD_ERR_RESP:       return "卡返回错误";
    case SD_ERR_NOCARD:     return "没有卡 / 识别失败";
    case SD_ERR_RANGE:      return "LBA 越界";
    default:                return "未知错误";
    }
}

static const char *sd_type_str(uint8_t type)
{
    switch (type)
    {
    case SD_TYPE_SDSC_V1:   return "SDSC 1.x";
    case SD_TYPE_SDSC_V2:   return "SDSC 2.0";
    case SD_TYPE_SDHC:      return "SDHC/SDXC";
    case SD_TYPE_MMC:       return "MMC";
    default:                return "未知";
    }
}

static void sd_usage(void)
{
    rt_kprintf("用法: sd <子命令> [参数]\n"
               "  sd info                 识别卡并打印信息\n"
               "  sd read  <lba> [cnt]    读块，例: sd read 0 2\n"
               "  sd write <lba> <hex...> 写一个块（会覆盖原内容）\n"
               "  sd test                 末块写入并读回（会覆盖该块）\n");
}

/* ================= info ================= */

static void sd_do_info(int argc, char **argv)
{
    struct sd_card *card = sd_handle();
    struct sd_cid cid;
    int ret;

    (void)argc;
    (void)argv;

    if (card == NULL)
    {
        return;
    }

    ret = sd_init(card, card->host);
    if (ret != 0)
    {
        uint8_t last_cmd = 0;
        uint32_t last_resp[4];

        sd_get_last_cmd(&last_cmd, last_resp);
        rt_kprintf("识别失败: %s(%d)，最后一条命令 CMD%u\n",
                   sd_err_str(ret), ret, (unsigned int)last_cmd);
        if (last_resp[0] != 0u)
        {
            rt_kprintf("  该命令响应: %08x %08x %08x %08x\n",
                       (unsigned int)last_resp[0], (unsigned int)last_resp[1],
                       (unsigned int)last_resp[2], (unsigned int)last_resp[3]);
        }
        rt_kprintf("  排查：卡是否插到底 / 卡座接触；-4 = 响应 CRC 错（多为没插卡或"
                   "线上噪声），-3 = 卡没应答（没插卡 / 引脚不对）\n");
        return;
    }

    rt_kprintf("卡类型   : %s\n", sd_type_str(card->type));
    rt_kprintf("容量     : %u MB（%u 个 512B 块）\n",
               (unsigned int)(card->capacity >> 20), (unsigned int)card->block_count);
    rt_kprintf("RCA      : %u    总线宽度: %u 位\n",
               (unsigned int)card->rca, (unsigned int)card->bus_width);

    if (sd_parse_cid(card->cid_raw, &cid) == 0)
    {
        rt_kprintf("厂商     : 0x%02x  OEM: 0x%04x  产品: %s  版本: 0x%02x\n",
                   (unsigned int)cid.manufacturer, (unsigned int)cid.oem,
                   cid.product, (unsigned int)cid.revision);
        rt_kprintf("序列号   : 0x%08x  生产日期: %u-%02u\n",
                   (unsigned int)cid.serial, (unsigned int)cid.year,
                   (unsigned int)cid.month);
    }
}

/* ================= read ================= */

static void sd_do_read(int argc, char **argv)
{
    struct sd_card *card = sd_handle();
    uint32_t lba;
    int cnt = 1;
    int ret;
    int i;

    if (card == NULL)
    {
        return;
    }
    if (argc < 1)
    {
        rt_kprintf("用法: sd read <lba> [cnt]\n    例: sd read 0 1\n");
        return;
    }

    lba = (uint32_t)strtoul(argv[0], NULL, 0);
    if (argc > 1)
    {
        cnt = atoi(argv[1]);
    }
    if ((cnt <= 0) || (cnt > (int)SD_CMD_MAX_BLOCKS))
    {
        rt_kprintf("cnt 取值 1~%d（一次最多读 %d 块）\n",
                   (int)SD_CMD_MAX_BLOCKS, (int)SD_CMD_MAX_BLOCKS);
        return;
    }

    ret = sd_read_blocks(card, lba, s_block, (uint32_t)cnt);
    if (ret != 0)
    {
        rt_kprintf("读失败: %s(%d)\n", sd_err_str(ret), ret);
        return;
    }

    rt_kprintf("LBA %u:", (unsigned int)lba);
    for (i = 0; i < 64; i++)
    {
        if ((i % 16) == 0)
        {
            rt_kprintf("\n  %04x:", (unsigned int)(lba * 512u + (uint32_t)i));
        }
        rt_kprintf(" %02x", s_block[i]);
    }
    rt_kprintf("\n");
}

/* ================= write ================= */

static void sd_do_write(int argc, char **argv)
{
    struct sd_card *card = sd_handle();
    uint32_t lba;
    int n = 0;
    int i;
    int ret;

    if (card == NULL)
    {
        return;
    }
    if (argc < 2)
    {
        rt_kprintf("用法: sd write <lba> <hex...>（其余字节填 0，会覆盖整块）\n");
        return;
    }

    lba = (uint32_t)strtoul(argv[0], NULL, 0);
    memset(s_block, 0, sizeof(s_block));
    for (i = 1; (i < argc) && (n < (int)SD_CMD_MAX_WRITE); i++)
    {
        s_block[n++] = (uint8_t)strtoul(argv[i], NULL, 16);
    }

    ret = sd_write_blocks(card, lba, s_block, 1u);
    if (ret != 0)
    {
        rt_kprintf("写失败: %s(%d)\n", sd_err_str(ret), ret);
        return;
    }
    rt_kprintf("已写入 LBA %u（前 %d 字节有数据，其余为 0）\n", (unsigned int)lba, n);
}

/* ================= test ================= */

static void sd_do_test(int argc, char **argv)
{
    static uint8_t rb[SD_BLOCK_SIZE] __attribute__((aligned(4)));
    struct sd_card *card = sd_handle();
    uint32_t lba;
    unsigned int i;
    int ret;

    (void)argc;
    (void)argv;

    if (card == NULL)
    {
        return;
    }

    ret = sd_init(card, card->host);
    if (ret != 0)
    {
        rt_kprintf("识别失败: %s(%d)\n", sd_err_str(ret), ret);
        return;
    }

    lba = card->block_count - 1u;       /* 最后一个块：最不容易撞上文件系统数据 */

    for (i = 0; i < SD_BLOCK_SIZE; i++)
    {
        s_block[i] = (uint8_t)(0x5Au ^ (i * 7u + 1u));
    }

    rt_kprintf("写入末块 LBA %u（%u 字节）…\n", (unsigned int)lba, (unsigned int)SD_BLOCK_SIZE);
    ret = sd_write_blocks(card, lba, s_block, 1u);
    if (ret != 0)
    {
        rt_kprintf("  写失败: %s(%d)\n", sd_err_str(ret), ret);
        return;
    }

    memset(rb, 0, sizeof(rb));
    ret = sd_read_blocks(card, lba, rb, 1u);
    if (ret != 0)
    {
        rt_kprintf("  读失败: %s(%d)\n", sd_err_str(ret), ret);
        return;
    }

    for (i = 0; i < SD_BLOCK_SIZE; i++)
    {
        if (rb[i] != s_block[i])
        {
            rt_kprintf("  FAIL 偏移 %u 期望 %02x 实际 %02x\n",
                       i, s_block[i], rb[i]);
            rt_kprintf("sd test: FAIL\n");
            return;
        }
    }

    rt_kprintf("sd test: PASS（末块 %u 已被覆盖）\n", (unsigned int)lba);
}

/* ================= 入口 ================= */

static void sd(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        sd_usage();
        return;
    }

    sub = argv[1];
    if (strcmp(sub, "info") == 0)
    {
        sd_do_info(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "read") == 0)
    {
        sd_do_read(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "write") == 0)
    {
        sd_do_write(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "test") == 0)
    {
        sd_do_test(argc - 2, &argv[2]);
    }
    else if (strcmp(sub, "help") == 0 || strcmp(sub, "?") == 0)
    {
        sd_usage();
    }
    else
    {
        rt_kprintf("未知子命令: %s\n", sub);
        sd_usage();
    }
}
MSH_CMD_EXPORT(sd, sd <info|read|write|test>: SD 卡调试);
