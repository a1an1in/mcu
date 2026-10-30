/*
 * blkdev.h - 512 字节块设备接口与注册表 <drivers/block/>
 *
 * 目的：让「文件系统」不用关心底下是 SD 卡、SPI Flash 还是 RAM 盘。
 * 上层（modules/fs 的 diskio）只认这张表；新介质只要实现 struct blkdev_ops
 * 并调一次 blkdev_register()。
 *
 * ── 分层 ─────────────────────────────────────────────────────────────
 *   blkdev.h / blkdev.c   本文件：设备模型 + 注册表（OS 无关，纯 C）
 *   drivers/sd/sd_blkdev.c  SD 卡 → 块设备的适配（sd.h 的薄壳）
 *   modules/fs/diskio.c    块设备 → FatFs 的胶水
 *
 * ── 约定 ─────────────────────────────────────────────────────────────
 *   1) 扇区固定 512 字节（FatFs 的 FF_MIN_SS = FF_MAX_SS = 512）。
 *   2) lba 是**块地址**，不是字节地址。
 *   3) 注册只登记、不做任何 I/O：board 初始化阶段（调度器未启动）可以安全调用。
 *      真正和介质通信在 blkdev_init()。
 *   4) 本层不加锁。多线程访问同一设备由调用方串行化（目前只有 msh/文件系统
 *      一侧在用，天然串行）。
 *
 * ── 用法 ─────────────────────────────────────────────────────────────
 *     static const struct blkdev_ops ram_ops = { ram_init, ram_read, ram_write };
 *     struct blkdev *dev = blkdev_register("ram0", &ram_ops, &my_ram);
 *
 *     blkdev_init(dev);                       // 可能失败：没卡 / 介质不响应
 *     blkdev_read(dev, 0, buf, 1);            // 读 0 号扇区
 *     blkdev_write(dev, 0, buf, 1);
 */
#ifndef DRIVERS_BLOCK_BLKDEV_H__
#define DRIVERS_BLOCK_BLKDEV_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLKDEV_SECTOR_SIZE      512u
#define BLKDEV_MAX              4       /* 注册表容量，够 SD + Flash + RAM 盘 */

/* 返回值：0 成功，负值失败（与 sd.h / spi.h 的风格一致） */
#define BLKDEV_OK               0
#define BLKDEV_ERR_PARAM        (-1)    /* 空指针 / 块数为 0 / LBA 越界 */
#define BLKDEV_ERR_IO           (-2)    /* 介质读写失败 */
#define BLKDEV_ERR_NOINIT       (-3)    /* 还没 blkdev_init() 或初始化失败 */
#define BLKDEV_ERR_NODEV        (-4)    /* 注册表里没有这个设备 */
#define BLKDEV_ERR_FULL         (-5)    /* 注册表已满 */
#define BLKDEV_ERR_DUP          (-6)    /* 同名设备已存在 */

struct blkdev;

/* 介质实现。init 可以为 NULL（RAM 盘之类不需要），但必须在里面把
 * dev->sector_count 填好，否则 blkdev_init() 会认为介质不可用。 */
struct blkdev_ops
{
    int (*init)(struct blkdev *dev);
    int (*read)(struct blkdev *dev, uint32_t lba, void *buf, uint32_t count);
    int (*write)(struct blkdev *dev, uint32_t lba, const void *buf, uint32_t count);
};

struct blkdev
{
    const char *name;                   /* 注册名，如 "sd0"（最长 15 字符） */
    const struct blkdev_ops *ops;
    void *ctx;                          /* 介质私有数据（sd_card / ram 描述符…） */
    uint32_t sector_count;              /* 介质容量（512B 块数），init 时填 */
    uint16_t sector_size;               /* 固定 512 */
    uint8_t inited;
    uint8_t reserved;
};

/* ============================ 注册表 ============================ */

/* 注册一个块设备。name 必须是长期有效的字符串（不要传栈上缓冲）。
 * 成功返回设备句柄，失败返回 NULL，失败原因写 *err（可传 NULL）。 */
struct blkdev *blkdev_register(const char *name, const struct blkdev_ops *ops,
                               void *ctx, int *err);

/* 按名字 / 下标查设备（下标 = diskio 的 pdrv / FatFs 的盘符号）。 */
struct blkdev *blkdev_find(const char *name);
struct blkdev *blkdev_get(int index);

/* 已注册设备个数（board 里想把所有设备打印出来时用）。 */
int blkdev_count(void);

/* ============================ 访问 ============================ */

/* 让介质就绪：调一次 ops->init（已经就绪则直接返回 0）。
 * ⚠️ 可能和硬件通信，必须在调度器启动后调用（理由同 sd_init，见 sd.h）。 */
int blkdev_init(struct blkdev *dev);

/* 读写 count 个 512 字节块。buf 对齐要求由具体介质决定（SD 要求 4 字节对齐）。
 * 未 init 或 LBA 越界直接返回错误，不会去碰硬件。 */
int blkdev_read(struct blkdev *dev, uint32_t lba, void *buf, uint32_t count);
int blkdev_write(struct blkdev *dev, uint32_t lba, const void *buf, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_BLOCK_BLKDEV_H__ */
