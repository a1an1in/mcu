/*
 * test_fatfs.c - 块设备 + FatFs 的 host 单测（不碰任何硬件）
 *
 * 用一块 256KB 的 RAM 盘当介质：
 *   drivers/block/blkdev.c   注册表、init 惰性初始化、越界检查
 *   modules/fs/diskio.c      drive ↔ blkdev 映射、ioctl
 *   modules/fs/fatfs/ff.c    FatFs 本体（R0.14b，本工程裁剪配置）
 *
 * 覆盖：
 *   - 注册表：查名字/下标、重名拒绝、空参数
 *   - 未 init 时读写必须报错，init 后容量/扇区大小正确
 *   - 越界读写拒绝
 *   - f_mkfs / f_mount / f_getfree
 *   - 写文件 → 关闭 → 重开读回；跨扇区（1KB）数据完整性
 *   - 卸载后重新挂载，内容仍在（持久性）
 *   - 目录遍历、f_unlink
 *   - disk_ioctl 的 GET_SECTOR_COUNT / GET_SECTOR_SIZE / GET_BLOCK_SIZE
 */
#include <stdio.h>
#include <string.h>

#include "blkdev.h"
#include "diskio.h"
#include "ff.h"

#define RAM_SECTORS     512u                        /* 512 × 512B = 256KB */
#define RAM_BYTES       (RAM_SECTORS * BLKDEV_SECTOR_SIZE)

static uint8_t s_ram[RAM_BYTES] __attribute__((aligned(4)));
static uint8_t s_file[2048] __attribute__((aligned(4)));
static uint8_t s_work[4096] __attribute__((aligned(4)));
static FATFS s_fs;

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
        {                                                                 \
            printf("FAIL %d  %s\n", __LINE__, #cond);                     \
            failures++;                                                   \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        long long _a = (long long)(a);                                    \
        long long _b = (long long)(b);                                    \
        if (_a != _b)                                                     \
        {                                                                 \
            printf("FAIL %d  %s=%lld 期望 %lld\n", __LINE__, #a, _a, _b); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* ============================ RAM 盘后端 ============================ */

static int ram_init(struct blkdev *dev)
{
    dev->sector_count = RAM_SECTORS;
    dev->sector_size = (uint16_t)BLKDEV_SECTOR_SIZE;
    return 0;
}

static int ram_read(struct blkdev *dev, uint32_t lba, void *buf, uint32_t count)
{
    (void)dev;
    if ((buf == NULL) || (lba >= RAM_SECTORS) || (count > (RAM_SECTORS - lba)))
    {
        return -1;
    }
    memcpy(buf, &s_ram[lba * BLKDEV_SECTOR_SIZE], count * BLKDEV_SECTOR_SIZE);
    return 0;
}

static int ram_write(struct blkdev *dev, uint32_t lba, const void *buf, uint32_t count)
{
    (void)dev;
    if ((buf == NULL) || (lba >= RAM_SECTORS) || (count > (RAM_SECTORS - lba)))
    {
        return -1;
    }
    memcpy(&s_ram[lba * BLKDEV_SECTOR_SIZE], buf, count * BLKDEV_SECTOR_SIZE);
    return 0;
}

static const struct blkdev_ops ram_ops = { ram_init, ram_read, ram_write };

/* ============================ 用例 ============================ */

static void test_registry(struct blkdev *dev)
{
    int err = 0;
    struct blkdev *dup;

    CHECK(dev != NULL);
    CHECK_EQ(blkdev_count(), 1);
    CHECK(blkdev_find("ram0") == dev);
    CHECK(blkdev_get(0) == dev);
    CHECK(blkdev_get(1) == NULL);
    CHECK(blkdev_find("nope") == NULL);
    CHECK(blkdev_find(NULL) == NULL);

    /* 重名必须拒绝 */
    dup = blkdev_register("ram0", &ram_ops, NULL, &err);
    CHECK(dup == NULL);
    CHECK_EQ(err, BLKDEV_ERR_DUP);
    CHECK_EQ(blkdev_count(), 1);

    /* 缺 read/write 的 ops 必须拒绝 */
    {
        static const struct blkdev_ops bad_ops = { 0, 0, 0 };
        err = 0;
        CHECK(blkdev_register("bad", &bad_ops, NULL, &err) == NULL);
        CHECK_EQ(err, BLKDEV_ERR_PARAM);
    }

    /* 未 init 时不许读写 */
    CHECK_EQ(dev->inited, 0);
    CHECK_EQ(blkdev_read(dev, 0, s_file, 1), BLKDEV_ERR_NOINIT);
    CHECK_EQ(blkdev_write(dev, 0, s_file, 1), BLKDEV_ERR_NOINIT);
}

static void test_blkdev_io(struct blkdev *dev)
{
    uint32_t i;

    CHECK_EQ(blkdev_init(dev), BLKDEV_OK);
    CHECK_EQ(blkdev_init(dev), BLKDEV_OK);          /* 幂等 */
    CHECK_EQ(dev->inited, 1);
    CHECK_EQ(dev->sector_count, RAM_SECTORS);
    CHECK_EQ(dev->sector_size, 512);

    /* 单块写读比对 */
    for (i = 0; i < BLKDEV_SECTOR_SIZE; i++)
    {
        s_file[i] = (uint8_t)(i * 7u + 3u);
    }
    CHECK_EQ(blkdev_write(dev, 3, s_file, 1), BLKDEV_OK);
    memset(s_file, 0, BLKDEV_SECTOR_SIZE);
    CHECK_EQ(blkdev_read(dev, 3, s_file, 1), BLKDEV_OK);
    for (i = 0; i < BLKDEV_SECTOR_SIZE; i++)
    {
        if (s_file[i] != (uint8_t)(i * 7u + 3u))
        {
            break;
        }
    }
    CHECK_EQ(i, BLKDEV_SECTOR_SIZE);

    /* 越界：末块之后 / 跨越末块 */
    CHECK_EQ(blkdev_read(dev, RAM_SECTORS, s_file, 1), BLKDEV_ERR_PARAM);
    CHECK_EQ(blkdev_read(dev, RAM_SECTORS - 1u, s_file, 2), BLKDEV_ERR_PARAM);
    CHECK_EQ(blkdev_write(dev, 0, s_file, RAM_SECTORS + 1u), BLKDEV_ERR_PARAM);
    CHECK_EQ(blkdev_read(dev, 0, s_file, 0), BLKDEV_ERR_PARAM);

    /* 填充介质非 0 内容，后面 mkfs 必须能覆盖掉 */
    memset(s_ram, 0xA5, sizeof(s_ram));
}

static void test_diskio(void)
{
    LBA_t count = 0;
    WORD ssize = 0;
    DWORD bsize = 0;

    CHECK_EQ(disk_status(0), 0);
    CHECK_EQ(disk_status(1), (DSTATUS)(STA_NOINIT | STA_NODISK));
    CHECK_EQ(disk_initialize(0), 0);
    CHECK_EQ(disk_initialize(1), (DSTATUS)(STA_NOINIT | STA_NODISK));

    CHECK_EQ(disk_ioctl(0, CTRL_SYNC, NULL), RES_OK);
    CHECK_EQ(disk_ioctl(0, GET_SECTOR_COUNT, &count), RES_OK);
    CHECK_EQ(count, RAM_SECTORS);
    CHECK_EQ(disk_ioctl(0, GET_SECTOR_SIZE, &ssize), RES_OK);
    CHECK_EQ(ssize, 512);
    CHECK_EQ(disk_ioctl(0, GET_BLOCK_SIZE, &bsize), RES_OK);
    CHECK_EQ(bsize, 1);
    CHECK_EQ(disk_ioctl(0, CTRL_POWER, NULL), RES_PARERR);
    CHECK_EQ(disk_ioctl(1, GET_SECTOR_COUNT, &count), RES_NOTRDY);
    CHECK_EQ(disk_read(1, s_file, 0, 1), RES_NOTRDY);
}

static void test_format_and_write(void)
{
    FRESULT fr;
    FIL fil;
    UINT bw = 0;
    UINT br = 0;
    FILINFO fno;
    DIR dir;
    uint32_t i;
    uint32_t total_kb = 0;
    DWORD free_clusters = 0;
    FATFS *pfs = NULL;

    /* ---- 格式化 + 挂载 ---- */
    fr = f_mkfs("0:", NULL, s_work, sizeof(s_work));
    CHECK_EQ(fr, FR_OK);
    if (fr != FR_OK)
    {
        printf("  格式化失败 FRESULT=%d\n", (int)fr);
        return;
    }

    fr = f_mount(&s_fs, "0:", 1);
    CHECK_EQ(fr, FR_OK);
    if (fr != FR_OK)
    {
        printf("  挂载失败 FRESULT=%d\n", (int)fr);
        return;
    }

    fr = f_getfree("0:", &free_clusters, &pfs);
    CHECK_EQ(fr, FR_OK);
    if (fr != FR_OK)
    {
        printf("  f_getfree 失败 FRESULT=%d\n", (int)fr);
        return;
    }
    CHECK(pfs != NULL);
    CHECK(free_clusters > 0);
    total_kb = (pfs->n_fatent - 2u) * pfs->csize / 2u;
    /* f_mkfs 默认会建分区表（卷从第 63 扇区起），所以卷容量比整盘略小 */
    CHECK(total_kb > (RAM_SECTORS / 4u));
    CHECK(total_kb < (RAM_SECTORS / 2u));
    printf("  卷容量 %u KB，每簇 %u 扇区，空闲 %lu 簇，FAT 类型 %u\n",
           (unsigned)total_kb, (unsigned)pfs->csize,
           (unsigned long)free_clusters, (unsigned)pfs->fs_type);

    /* ---- 写一个小文本文件 ---- */
    fr = f_open(&fil, "0:/hello.txt", FA_CREATE_ALWAYS | FA_WRITE);
    CHECK_EQ(fr, FR_OK);
    CHECK_EQ(f_write(&fil, "hello fatfs", 11, &bw), FR_OK);
    CHECK_EQ(bw, 11);
    CHECK_EQ(f_close(&fil), FR_OK);

    fr = f_stat("0:/hello.txt", &fno);
    CHECK_EQ(fr, FR_OK);
    CHECK_EQ(fno.fsize, 11);
    CHECK_EQ(fno.fattrib & AM_DIR, 0);

    /* ---- 重开读回 ---- */
    memset(s_file, 0, sizeof(s_file));
    fr = f_open(&fil, "0:/hello.txt", FA_READ);
    CHECK_EQ(fr, FR_OK);
    CHECK_EQ(f_read(&fil, s_file, sizeof(s_file), &br), FR_OK);
    CHECK_EQ(br, 11);
    CHECK(memcmp(s_file, "hello fatfs", 11) == 0);
    CHECK_EQ(f_close(&fil), FR_OK);

    /* ---- 跨扇区的 2KB 文件（走 disk_read 多块路径） ---- */
    for (i = 0; i < sizeof(s_file); i++)
    {
        s_file[i] = (uint8_t)(i * 31u + 7u);
    }
    fr = f_open(&fil, "0:/blob.bin", FA_CREATE_ALWAYS | FA_WRITE);
    CHECK_EQ(fr, FR_OK);
    CHECK_EQ(f_write(&fil, s_file, sizeof(s_file), &bw), FR_OK);
    CHECK_EQ(bw, sizeof(s_file));
    CHECK_EQ(f_close(&fil), FR_OK);

    memset(s_file, 0, sizeof(s_file));
    fr = f_open(&fil, "0:/blob.bin", FA_READ);
    CHECK_EQ(fr, FR_OK);
    CHECK_EQ(f_read(&fil, s_file, sizeof(s_file), &br), FR_OK);
    CHECK_EQ(br, sizeof(s_file));
    for (i = 0; i < sizeof(s_file); i++)
    {
        if (s_file[i] != (uint8_t)(i * 31u + 7u))
        {
            break;
        }
    }
    CHECK_EQ(i, sizeof(s_file));
    CHECK_EQ(f_close(&fil), FR_OK);

    /* ---- 子目录 ---- */
    CHECK_EQ(f_mkdir("0:/dir"), FR_OK);
    fr = f_open(&fil, "0:/dir/a.bin", FA_CREATE_ALWAYS | FA_WRITE);
    CHECK_EQ(fr, FR_OK);
    CHECK_EQ(f_write(&fil, "abc", 3, &bw), FR_OK);
    CHECK_EQ(f_close(&fil), FR_OK);

    /* ---- 遍历根目录：hello.txt / blob.bin / dir ---- */
    {
        int entries = 0;
        int saw_dir = 0;
        int saw_hello = 0;

        CHECK_EQ(f_opendir(&dir, "0:/"), FR_OK);
        for (;;)
        {
            fr = f_readdir(&dir, &fno);
            if ((fr != FR_OK) || (fno.fname[0] == '\0'))
            {
                break;
            }
            entries++;
            if ((fno.fattrib & AM_DIR) &&
                ((strcmp(fno.fname, "dir") == 0) || (strcmp(fno.fname, "DIR") == 0)))
            {
                saw_dir = 1;
            }
            if (strcmp(fno.fname, "HELLO.TXT") == 0 ||
                strcmp(fno.fname, "hello.txt") == 0)
            {
                saw_hello = 1;
            }
        }
        CHECK_EQ(fr, FR_OK);
        CHECK_EQ(f_closedir(&dir), FR_OK);
        CHECK_EQ(entries, 3);
        CHECK(saw_dir);
        CHECK(saw_hello);
    }
}

static void test_remount_and_remove(void)
{
    FIL fil;
    FRESULT fr;
    UINT br = 0;
    FILINFO fno;

    /* 卸载再挂载：内容必须还在（说明 disk_write 真的落到了介质） */
    CHECK_EQ(f_mount(NULL, "0:", 0), FR_OK);
    CHECK_EQ(f_mount(&s_fs, "0:", 1), FR_OK);

    fr = f_open(&fil, "0:/hello.txt", FA_READ);
    CHECK_EQ(fr, FR_OK);
    memset(s_file, 0, sizeof(s_file));
    CHECK_EQ(f_read(&fil, s_file, sizeof(s_file), &br), FR_OK);
    CHECK_EQ(br, 11);
    CHECK(memcmp(s_file, "hello fatfs", 11) == 0);
    CHECK_EQ(f_close(&fil), FR_OK);

    /* 删除后 stat 必须报 FR_NO_FILE */
    CHECK_EQ(f_unlink("0:/hello.txt"), FR_OK);
    CHECK_EQ(f_stat("0:/hello.txt", &fno), FR_NO_FILE);

    /* 删除不存在的文件 */
    CHECK_EQ(f_unlink("0:/hello.txt"), FR_NO_FILE);
}

static void test_unformatted(void)
{
    /* 抹掉引导扇区 → 挂载应报 FR_NO_FILESYSTEM，而不是崩掉 */
    memset(s_ram, 0, BLKDEV_SECTOR_SIZE);
    CHECK_EQ(f_mount(NULL, "0:", 0), FR_OK);
    CHECK_EQ(f_mount(&s_fs, "0:", 1), FR_NO_FILESYSTEM);
    /* 恢复：重新格式化，顺便验证 mkfs 能从垃圾里恢复 */
    CHECK_EQ(f_mkfs("0:", NULL, s_work, sizeof(s_work)), FR_OK);
    CHECK_EQ(f_mount(&s_fs, "0:", 1), FR_OK);
}

int main(void)
{
    struct blkdev *dev;
    int err = 0;

    dev = blkdev_register("ram0", &ram_ops, NULL, &err);
    CHECK_EQ(err, BLKDEV_OK);

    test_registry(dev);
    test_blkdev_io(dev);
    test_diskio();
    test_format_and_write();
    test_remount_and_remove();
    test_unformatted();

    CHECK_EQ(f_mount(NULL, "0:", 0), FR_OK);

    if (failures == 0)
    {
        printf("PASS: 块设备 + FatFs 全部通过（格式化/读写/跨扇区/重挂载/目录/删除）\n");
        return 0;
    }

    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
