/*
 * diskio.h - FatFs 的底层磁盘接口 <modules/fs/>
 *
 * FatFs（ff.c）不认识任何硬件：它只按 drive 号调用这里的五个函数。
 * 本工程把 drive 号直接映射到 drivers/block 的注册表下标：
 *
 *     pdrv = 0  →  blkdev_get(0)  （通常是 "sd0"）
 *     pdrv = 1  →  blkdev_get(1)  （以后接 SPI Flash 等）
 *
 * 所以换介质、加介质都不用改这里，只要在 board 里多注册一个块设备。
 *
 * 类型和命令码来自 FatFs R0.14b 的官方 diskio.h（保持二进制/语义一致）。
 */
#ifndef MODULES_FS_DISKIO_H__
#define MODULES_FS_DISKIO_H__

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 磁盘状态 */
typedef BYTE DSTATUS;

/* 函数返回值 */
typedef enum
{
    RES_OK = 0,     /* 成功 */
    RES_ERROR,      /* 读写错误 */
    RES_WRPRT,      /* 写保护 */
    RES_NOTRDY,     /* 介质未就绪 */
    RES_PARERR      /* 参数错误 */
} DRESULT;

/* DSTATUS 的位 */
#define STA_NOINIT      0x01    /* 未初始化 */
#define STA_NODISK      0x02    /* 无介质 */
#define STA_PROTECT     0x04    /* 写保护 */

/* disk_ioctl 命令码（FatFs 用得到的） */
#define CTRL_SYNC           0   /* 等待挂起的写完成 */
#define GET_SECTOR_COUNT    1   /* 取扇区总数（f_mkfs / f_getfree 需要） */
#define GET_SECTOR_SIZE     2   /* 取扇区大小（FF_MIN_SS != FF_MAX_SS 时需要） */
#define GET_BLOCK_SIZE      3   /* 取擦除块大小（f_mkfs 需要，单位：扇区） */
#define CTRL_TRIM           4   /* 通知这些扇区已无用（FF_USE_TRIM 需要） */

/* 下面几条 FatFs 不用，保留是为了和官方 diskio.h 兼容 */
#define CTRL_POWER          5
#define CTRL_LOCK           6
#define CTRL_EJECT          7
#define CTRL_FORMAT         8

/* ============================ 接口 ============================ */

DSTATUS disk_status(BYTE pdrv);
DSTATUS disk_initialize(BYTE pdrv);

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff);

#if FF_MULTI_PARTITION
/* 选择卷对应的 MBR 分区：pt = 0 自动找第一个 FAT 分区，1~4 强制。
 * 在 f_mount() 之前调用；已经挂载的卷需要先 f_mount(NULL, ...) 卸载。 */
void disk_set_partition(BYTE vol, BYTE pt);
BYTE disk_get_partition(BYTE vol);
#endif

#ifdef __cplusplus
}
#endif

#endif /* MODULES_FS_DISKIO_H__ */
