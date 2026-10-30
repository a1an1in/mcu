/*
 * blkdev.c - 512 字节块设备注册表 <drivers/block/>
 *
 * 只做三件事：登记设备、按名字/下标查、转发读写并做边界检查。
 * 不碰寄存器、不依赖 OS、不分配内存（注册表是静态数组）。
 */
#include <string.h>

#include "blkdev.h"

#define BLKDEV_NAME_MAX     16

static struct blkdev s_devs[BLKDEV_MAX];
static int s_dev_count;

/* ============================ 注册表 ============================ */

struct blkdev *blkdev_register(const char *name, const struct blkdev_ops *ops,
                               void *ctx, int *err)
{
    struct blkdev *dev;

    if (err != NULL)
    {
        *err = BLKDEV_OK;
    }

    if ((name == NULL) || (name[0] == '\0') || (ops == NULL) ||
        (ops->read == NULL) || (ops->write == NULL) ||
        (strlen(name) >= BLKDEV_NAME_MAX))
    {
        if (err != NULL) { *err = BLKDEV_ERR_PARAM; }
        return NULL;
    }

    if (blkdev_find(name) != NULL)
    {
        if (err != NULL) { *err = BLKDEV_ERR_DUP; }
        return NULL;
    }

    if (s_dev_count >= (int)BLKDEV_MAX)
    {
        if (err != NULL) { *err = BLKDEV_ERR_FULL; }
        return NULL;
    }

    dev = &s_devs[s_dev_count++];
    memset(dev, 0, sizeof(*dev));
    dev->name = name;
    dev->ops = ops;
    dev->ctx = ctx;
    dev->sector_size = (uint16_t)BLKDEV_SECTOR_SIZE;

    return dev;
}

struct blkdev *blkdev_find(const char *name)
{
    int i;

    if (name == NULL)
    {
        return NULL;
    }

    for (i = 0; i < s_dev_count; i++)
    {
        if (strcmp(s_devs[i].name, name) == 0)
        {
            return &s_devs[i];
        }
    }
    return NULL;
}

struct blkdev *blkdev_get(int index)
{
    if ((index < 0) || (index >= s_dev_count))
    {
        return NULL;
    }
    return &s_devs[index];
}

int blkdev_count(void)
{
    return s_dev_count;
}

/* ============================ 访问 ============================ */

int blkdev_init(struct blkdev *dev)
{
    int ret;

    if ((dev == NULL) || (dev->ops == NULL))
    {
        return BLKDEV_ERR_PARAM;
    }

    if (dev->inited != 0u)
    {
        return BLKDEV_OK;
    }

    if (dev->sector_size == 0u)
    {
        dev->sector_size = (uint16_t)BLKDEV_SECTOR_SIZE;
    }

    if (dev->ops->init != NULL)
    {
        ret = dev->ops->init(dev);
        if (ret != 0)
        {
            return ret;
        }
    }

    /* 介质必须报出容量，否则 FatFs 无法建卷（GET_SECTOR_COUNT）。 */
    if (dev->sector_count == 0u)
    {
        return BLKDEV_ERR_IO;
    }

    dev->inited = 1u;
    return BLKDEV_OK;
}

static int blkdev_check(const struct blkdev *dev, uint32_t lba, uint32_t count)
{
    if ((dev == NULL) || (dev->ops == NULL))
    {
        return BLKDEV_ERR_PARAM;
    }
    if (dev->inited == 0u)
    {
        return BLKDEV_ERR_NOINIT;
    }
    if (count == 0u)
    {
        return BLKDEV_ERR_PARAM;
    }
    if ((lba >= dev->sector_count) || (count > (dev->sector_count - lba)))
    {
        return BLKDEV_ERR_PARAM;
    }
    return BLKDEV_OK;
}

int blkdev_read(struct blkdev *dev, uint32_t lba, void *buf, uint32_t count)
{
    int ret = blkdev_check(dev, lba, count);

    if (ret != BLKDEV_OK)
    {
        return ret;
    }
    if (buf == NULL)
    {
        return BLKDEV_ERR_PARAM;
    }
    return dev->ops->read(dev, lba, buf, count);
}

int blkdev_write(struct blkdev *dev, uint32_t lba, const void *buf, uint32_t count)
{
    int ret = blkdev_check(dev, lba, count);

    if (ret != BLKDEV_OK)
    {
        return ret;
    }
    if (buf == NULL)
    {
        return BLKDEV_ERR_PARAM;
    }
    return dev->ops->write(dev, lba, buf, count);
}
