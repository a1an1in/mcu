/*
 * diskio.c - 块设备 → FatFs 的胶水 <modules/fs/>
 *
 * 五个函数全部是「查表 + 转发」：pdrv 是 FatFs 的盘符号，直接当
 * blkdev_get() 的下标用。没有寄存器、没有 OS、没有缓冲。
 *
 * 为什么这么薄：FatFs 需要的「介质能力」只有读、写、扇区数、擦除块大小，
 * 这些 blkdev 都已经表达了；把映射留在这一层，drivers 和 FatFs 互不认识。
 */
#include <stddef.h>

#include "diskio.h"
#include "blkdev.h"

static struct blkdev *disk_dev(BYTE pdrv)
{
    return blkdev_get((int)pdrv);
}

DSTATUS disk_status(BYTE pdrv)
{
    struct blkdev *dev = disk_dev(pdrv);

    if (dev == NULL)
    {
        return (DSTATUS)(STA_NOINIT | STA_NODISK);
    }
    return (dev->inited != 0u) ? 0u : (DSTATUS)STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    struct blkdev *dev = disk_dev(pdrv);

    if (dev == NULL)
    {
        return (DSTATUS)(STA_NOINIT | STA_NODISK);
    }

    /* 真正和介质通信的地方（SD：识别卡、读容量）。
     * 失败就是没插卡 / 卡不响应，FatFs 会返回 FR_NOT_READY。 */
    if (blkdev_init(dev) != BLKDEV_OK)
    {
        return (DSTATUS)STA_NOINIT;
    }
    return 0u;
}

static DRESULT disk_result(int ret)
{
    switch (ret)
    {
    case BLKDEV_OK:             return RES_OK;
    case BLKDEV_ERR_PARAM:      return RES_PARERR;
    case BLKDEV_ERR_NOINIT:     return RES_NOTRDY;
    default:                    return RES_ERROR;
    }
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    struct blkdev *dev = disk_dev(pdrv);

    if (dev == NULL)
    {
        return RES_NOTRDY;
    }
    return disk_result(blkdev_read(dev, (uint32_t)sector, buff, (uint32_t)count));
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    struct blkdev *dev = disk_dev(pdrv);

    if (dev == NULL)
    {
        return RES_NOTRDY;
    }
    return disk_result(blkdev_write(dev, (uint32_t)sector, buff, (uint32_t)count));
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    struct blkdev *dev = disk_dev(pdrv);

    if (dev == NULL)
    {
        return RES_NOTRDY;
    }

    switch (cmd)
    {
    case CTRL_SYNC:
        /* 块设备没有写缓存（sd_write_blocks 返回时卡已编程完成），无事可做。 */
        return RES_OK;

    case GET_SECTOR_COUNT:
        if ((buff == NULL) || (dev->sector_count == 0u))
        {
            return RES_PARERR;
        }
        *(LBA_t *)buff = (LBA_t)dev->sector_count;
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (buff == NULL)
        {
            return RES_PARERR;
        }
        *(WORD *)buff = (WORD)BLKDEV_SECTOR_SIZE;
        return RES_OK;

    case GET_BLOCK_SIZE:
        /* 擦除块大小。SD 卡内部自己管擦除，按 1 个扇区报即可；
         * SPI NOR 以后要报 8（4KB / 512）来让 f_mkfs 对齐。 */
        if (buff == NULL)
        {
            return RES_PARERR;
        }
        *(DWORD *)buff = 1u;
        return RES_OK;

    default:
        return RES_PARERR;
    }
}
