/*
 * sd_blkdev.c - SD 卡 → 块设备适配 <drivers/sd/>
 *
 * 只做形状转换：blkdev 的 init/read/write 直接落到 sd_init / sd_read_blocks /
 * sd_write_blocks。块大小、容量都在 sd 层算好了（card->block_count）。
 *
 * 对齐：SDIO 的 FIFO 按 32 位搬，sd_read_blocks 要求 buf 4 字节对齐。
 * FatFs 自己的扇区缓冲是对齐的，但 f_read/f_write 在整扇区访问时会**直接把
 * 用户缓冲传下来**，栈上的 char[] 未必对齐。所以这里加一个 512B 的跳板：
 * 不对齐就按扇区过一遍，代价只在少数非对齐调用上。
 */
#include <string.h>

#include "sd_blkdev.h"

static uint8_t s_bounce[SD_BLOCK_SIZE] __attribute__((aligned(4)));

static int sd_blkdev_init(struct blkdev *dev)
{
    struct sd_card *card = (struct sd_card *)dev->ctx;
    int ret;

    if (card == NULL)
    {
        return BLKDEV_ERR_PARAM;
    }
    if (card->host == NULL)
    {
        return BLKDEV_ERR_IO;           /* board 没配 SDIO 主机 */
    }

    ret = sd_init(card, card->host);
    if (ret != 0)
    {
        return ret;                     /* 没卡 / 识别失败 */
    }

    dev->sector_count = card->block_count;
    dev->sector_size = (uint16_t)SD_BLOCK_SIZE;
    return BLKDEV_OK;
}

static int aligned4(const void *p)
{
    return (((uintptr_t)p & 3u) == 0u);
}

static int sd_blkdev_read(struct blkdev *dev, uint32_t lba, void *buf, uint32_t count)
{
    struct sd_card *card = (struct sd_card *)dev->ctx;
    uint32_t i;
    int ret;

    if (aligned4(buf))
    {
        return sd_read_blocks(card, lba, buf, count);
    }

    for (i = 0; i < count; i++)
    {
        ret = sd_read_blocks(card, lba + i, s_bounce, 1);
        if (ret != 0)
        {
            return ret;
        }
        memcpy((uint8_t *)buf + (i * SD_BLOCK_SIZE), s_bounce, SD_BLOCK_SIZE);
    }
    return 0;
}

static int sd_blkdev_write(struct blkdev *dev, uint32_t lba, const void *buf, uint32_t count)
{
    struct sd_card *card = (struct sd_card *)dev->ctx;
    uint32_t i;
    int ret;

    if (aligned4(buf))
    {
        return sd_write_blocks(card, lba, buf, count);
    }

    for (i = 0; i < count; i++)
    {
        memcpy(s_bounce, (const uint8_t *)buf + (i * SD_BLOCK_SIZE), SD_BLOCK_SIZE);
        ret = sd_write_blocks(card, lba + i, s_bounce, 1);
        if (ret != 0)
        {
            return ret;
        }
    }
    return 0;
}

static const struct blkdev_ops s_sd_blkdev_ops =
{
    sd_blkdev_init,
    sd_blkdev_read,
    sd_blkdev_write,
};

struct blkdev *sd_blkdev_register(struct sd_card *card, const char *name)
{
    if (card == NULL)
    {
        return NULL;
    }
    return blkdev_register(name, &s_sd_blkdev_ops, card, NULL);
}
