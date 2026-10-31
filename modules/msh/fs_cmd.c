/*
 * fs_cmd.c - 文件系统（FatFs）的 msh 命令 <modules/msh/>
 *
 * 只用 FatFs 的公开 API + 块设备注册表，不碰 SDIO/SPI 寄存器：
 * 底下是 SD 卡还是别的介质，由 board 注册的块设备决定（drive 0 = blkdev 0）。
 *
 *   fs dev             列出已注册的块设备（大小 / 是否就绪）
 *   fs part            打印 MBR 分区表
 *   fs mount [pt]      挂载 0: 号卷（pt=1~4 指定分区，省略=自动）
 *   fs umount          卸载
 *   fs mkfs [pt]       格式化（省略 pt = 重写整盘分区表！会清数据）
 *   fs ls [path]       列目录，默认当前目录
 *   fs cd [path]       切换当前目录（省略=根目录）
 *   fs pwd             显示当前目录
 *   fs cat <path>      打印文本文件（最多 4KB）
 *   fs write <path> <text...>  写文本文件（覆盖），例: fs write /a.txt hello
 *   fs mkdir <path>    建目录
 *   fs rm [-r] <path>  删除文件/空目录；-r 递归删除整棵目录
 *   fs df              容量与剩余空间
 *
 * 路径写法：可以写 "/dir/file.txt"（自动补成 "0:/dir/file.txt"），
 * 也可以写完整的 "0:/dir/file.txt"。文件名按 8.3 短名（FF_USE_LFN = 0）。
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "blkdev.h"
#include "diskio.h"
#include "ff.h"
#include "fs_util.h"
#include "music.h"
#include "recorder.h"

#define FS_DRIVE            "0:"
#define FS_WORK_SIZE        4096        /* f_mkfs 的工作缓冲，4KB 足够 */
#define FS_IO_SIZE          512         /* 单次读写块（也当递归删除的路径缓冲） */
#define FS_CAT_CHUNK        128         /* cat 每次打印的字节数：受 RT_CONSOLEBUF_SIZE 限制 */
#define FS_CAT_MAX          4096        /* cat 最多打这么多，防止刷屏 */
#define FS_WRITE_MAX        511         /* 一次写入的文本上限 */

static FATFS s_fs;
static uint8_t s_work[FS_WORK_SIZE] __attribute__((aligned(4)));
static char s_io[FS_IO_SIZE] __attribute__((aligned(4)));
static uint8_t s_mounted;

static void fs_do_umount(void);

/* ============================ 小工具 ============================ */

static const char *fr_str(FRESULT fr)
{
    switch (fr)
    {
    case FR_OK:                 return "OK";
    case FR_DISK_ERR:           return "底层读写失败";
    case FR_INT_ERR:            return "内部错误（断言）";
    case FR_NOT_READY:          return "介质未就绪（没插卡 / 没注册块设备）";
    case FR_NO_FILE:            return "找不到文件或路径";
    case FR_NO_PATH:            return "找不到路径";
    case FR_INVALID_NAME:       return "文件名非法";
    case FR_DENIED:             return "拒绝访问（只读 / 目录满 / 文件已打开）";
    case FR_EXIST:              return "已存在同名对象";
    case FR_INVALID_OBJECT:     return "对象无效";
    case FR_WRITE_PROTECTED:    return "写保护";
    case FR_INVALID_DRIVE:      return "盘符无效";
    case FR_NOT_ENABLED:        return "卷未挂载";
    case FR_NO_FILESYSTEM:      return "没有文件系统（先 fs mkfs）";
    case FR_MKFS_ABORTED:       return "格式化中止";
    case FR_TIMEOUT:            return "超时";
    case FR_LOCKED:             return "对象被锁定";
    case FR_NOT_ENOUGH_CORE:    return "内存不足";
    case FR_TOO_MANY_OPEN_FILES:return "打开的文件太多";
    case FR_INVALID_PARAMETER:  return "参数非法";
    default:                    return "未知错误";
    }
}

/* 路径规范化统一在 modules/fs/fs_util.c 的 fs_build_path()，这里只做转发 */
static int fs_path(const char *in, char *out)
{
    return fs_build_path(in, out, FS_PATH_MAX);
}

static void fs_usage(void)
{
    /* 注意：rt_kprintf 单次输出受 RT_CONSOLEBUF_SIZE 限制（超出会截断），
     * 所以这里**一行一次**打印，不要拼成一个几百字节的大字符串。 */
    rt_kprintf("用法: fs <子命令> [参数]\n");
    rt_kprintf("  fs dev              列出块设备\n");
    rt_kprintf("  fs part             打印 MBR 分区表\n");
    rt_kprintf("  fs mount [pt]       挂载 0:（pt=1~4 指定分区，省略=自动）\n");
    rt_kprintf("  fs umount           卸载\n");
    rt_kprintf("  fs mkfs [pt]        格式化（省略 pt=重写整盘分区表）\n");
    rt_kprintf("  fs ls [path]        列目录，默认当前目录\n");
    rt_kprintf("  fs cd [path]        切换目录，省略=根目录\n");
    rt_kprintf("  fs pwd              显示当前目录\n");
    rt_kprintf("  fs cat <path>       打印文本文件\n");
    rt_kprintf("  fs write <path> <text...>  写文本文件（覆盖）\n");
    rt_kprintf("  fs mkdir <path>     建目录\n");
    rt_kprintf("  fs rm [-r] <path>   删除文件/空目录；-r 递归删除\n");
    rt_kprintf("  fs df               容量 / 剩余空间\n");
}

/* 解析可选的 pt 参数：返回 0~4，或 -1（没给/非法） */
static int fs_parse_pt(const char *arg)
{
    long v;
    char *end = NULL;

    if (arg == NULL)
    {
        return -1;
    }

    v = strtol(arg, &end, 10);
    if ((end == arg) || (*end != '\0') || (v < 0) || (v > 4))
    {
        return -1;
    }
    return (int)v;
}

/* ============================ part ============================ */

static uint32_t fs_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 打印 MBR 分区表。用途：确认卡上到底有几个分区、哪个是 FAT——
 * 挂载只认 FAT/FAT32，exFAT / NTFS / ext4 这里能看出来但挂不上。 */
static void fs_do_part(void)
{
    struct blkdev *dev = blkdev_get(0);
    const uint8_t *mbr = (const uint8_t *)s_io;
    int i;
    int found = 0;

    if (dev == NULL)
    {
        rt_kprintf("没有块设备 0（board 里没注册成功？）\n");
        return;
    }
    if (blkdev_init(dev) != 0)
    {
        rt_kprintf("块设备初始化失败（卡识别没过？先用 sd info 看看）\n");
        return;
    }
    if (blkdev_read(dev, 0, s_io, 1) != 0)
    {
        rt_kprintf("读 MBR 失败\n");
        return;
    }

    if ((mbr[510] != 0x55u) || (mbr[511] != 0xAAu))
    {
        rt_kprintf("扇区 0 没有 MBR 签名：可能是整盘 FAT（super floppy），直接 fs mount 即可\n");
        return;
    }

    rt_kprintf("分区  类型        起始 LBA      扇区数        大小        文件系统\n");
    for (i = 0; i < 4; i++)
    {
        const uint8_t *pte = &mbr[446 + (i * 16)];
        uint8_t type = pte[4];
        uint32_t start = fs_le32(&pte[8]);
        uint32_t count = fs_le32(&pte[12]);
        uint32_t mb = count / 2048u;
        const char *name;

        if ((type == 0u) || (count == 0u))
        {
            continue;
        }

        switch (type)
        {
        case 0x01:  name = "FAT12"; break;
        case 0x04:  name = "FAT16 <32M"; break;
        case 0x06:  name = "FAT16"; break;
        case 0x0B:  name = "FAT32"; break;
        case 0x0C:  name = "FAT32 LBA"; break;
        case 0x07:  name = "exFAT/NTFS"; break;
        case 0x83:  name = "Linux"; break;
        case 0xEE:  name = "GPT 保护分区"; break;
        default:    name = "其他"; break;
        }

        rt_kprintf("%-4d  0x%02x        %-13lu %-13lu %-10lu %s\n",
                   i + 1, type, (unsigned long)start,
                   (unsigned long)count, (unsigned long)mb, name);
        found++;
    }

    if (found == 0)
    {
        rt_kprintf("（没有有效分区项）\n");
    }
}

/* ============================ mount / umount ============================ */

static void fs_do_mount(const char *arg)
{
    int pt = fs_parse_pt(arg);
    FRESULT fr;

    if ((arg != NULL) && (pt < 0))
    {
        rt_kprintf("分区号必须是 0~4（0=自动）\n");
        return;
    }
    if (pt < 0)
    {
        pt = (int)disk_get_partition(0);    /* 没给就沿用上次的选择 */
    }

    if ((music_is_playing() != 0) || (recorder_is_recording() != 0))
    {
        rt_kprintf("正在播放/录音，先 mplayer stop / record stop 再重新挂载\n");
        return;
    }

    if (s_mounted != 0u)
    {
        fs_do_umount();
    }

    disk_set_partition(0, (BYTE)pt);

    fr = f_mount(&s_fs, FS_DRIVE, 1);
    if (fr == FR_OK)
    {
        s_mounted = 1u;
        rt_kprintf("已挂载 %s（分区 %d%s）\n", FS_DRIVE, pt, (pt == 0) ? "，自动" : "");
    }
    else
    {
        s_mounted = 0u;
        rt_kprintf("挂载失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        rt_kprintf("  用 fs part 看分区表：分区类型必须是 FAT，exFAT/NTFS 挂不上\n");
    }
}

static void fs_do_dev(void)
{
    int i;

    if (blkdev_count() == 0)
    {
        rt_kprintf("没有注册任何块设备（board 里 blkdev_register 没成功？）\n");
        return;
    }

    rt_kprintf("下标 名字            扇区数      容量       状态\n");
    for (i = 0; i < blkdev_count(); i++)
    {
        struct blkdev *dev = blkdev_get(i);

        rt_kprintf("%-4d %-15s %-11lu %-10lu %s\n",
                   i, dev->name,
                   (unsigned long)dev->sector_count,
                   (unsigned long)((dev->sector_count / 2048u)),   /* 单位 MB */
                   (dev->inited != 0u) ? "已就绪" : "未初始化");
    }
}

static void fs_do_umount(void)
{
    if (s_mounted == 0u)
    {
        rt_kprintf("本来就没挂载\n");
        return;
    }
    if ((music_is_playing() != 0) || (recorder_is_recording() != 0))
    {
        rt_kprintf("正在播放/录音，先 mplayer stop / record stop 再卸载\n");
        return;
    }
    (void)f_mount(NULL, FS_DRIVE, 0);
    s_mounted = 0u;
    rt_kprintf("已卸载 %s\n", FS_DRIVE);
}

/* ============================ mkfs ============================ */

static void fs_do_mkfs(const char *arg)
{
    FRESULT fr;
    int pt = fs_parse_pt(arg);

    if ((arg != NULL) && (pt < 0))
    {
        rt_kprintf("分区号必须是 0~4（0=重写分区表）\n");
        return;
    }
    if (pt < 0)
    {
        pt = 0;                             /* 默认：重写整盘分区表 */
    }

    if ((music_is_playing() != 0) || (recorder_is_recording() != 0))
    {
        rt_kprintf("正在播放/录音，先 mplayer stop / record stop 再格式化\n");
        return;
    }

    if (s_mounted != 0u)
    {
        fs_do_umount();
    }

    disk_set_partition(0, (BYTE)pt);

    if (pt == 0)
    {
        rt_kprintf("正在格式化 %s（重写分区表，整盘数据都会丢）…\n", FS_DRIVE);
    }
    else
    {
        rt_kprintf("正在格式化 %s 的第 %d 分区（只动这个分区）…\n", FS_DRIVE, pt);
    }

    fr = f_mkfs(FS_DRIVE, NULL, s_work, sizeof(s_work));
    if (fr != FR_OK)
    {
        rt_kprintf("格式化失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }
    rt_kprintf("格式化完成，接着 fs mount %s\n", (pt == 0) ? "" : arg);
}

/* ============================ ls ============================ */

static void fs_do_ls(const char *path)
{
    DIR dir;
    FILINFO fno;
    FRESULT fr;
    char p[FS_PATH_MAX];
    unsigned int files = 0;
    unsigned long bytes = 0;

    if (fs_path((path == NULL) ? "" : path, p) != 0)
    {
        rt_kprintf("路径太长\n");
        return;
    }

    fr = f_opendir(&dir, p);
    if (fr != FR_OK)
    {
        rt_kprintf("打开目录失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }

    rt_kprintf("属性       大小  名字\n");
    for (;;)
    {
        fr = f_readdir(&dir, &fno);
        if ((fr != FR_OK) || (fno.fname[0] == '\0'))
        {
            break;
        }
        rt_kprintf("%c%c%c%c %9lu  %s%s\n",
                   (fno.fattrib & AM_DIR) ? 'd' : '-',
                   (fno.fattrib & AM_RDO) ? 'r' : '-',
                   (fno.fattrib & AM_HID) ? 'h' : '-',
                   (fno.fattrib & AM_SYS) ? 's' : '-',
                   (unsigned long)fno.fsize,
                   fno.fname,
                   (fno.fattrib & AM_DIR) ? "/" : "");
        files++;
        bytes += fno.fsize;
    }
    (void)f_closedir(&dir);

    if (fr != FR_OK)
    {
        rt_kprintf("读目录失败: %s\n", fr_str(fr));
        return;
    }
    rt_kprintf("共 %u 项，%lu 字节\n", files, bytes);
}

/* ============================ cat ============================ */

static void fs_do_cat(const char *path)
{
    FIL fil;
    FRESULT fr;
    UINT got;
    unsigned long total = 0;
    char p[FS_PATH_MAX];

    if (path == NULL)
    {
        rt_kprintf("用法: fs cat <path>\n");
        return;
    }
    if (fs_path(path, p) != 0)
    {
        rt_kprintf("路径太长\n");
        return;
    }

    fr = f_open(&fil, p, FA_READ);
    if (fr != FR_OK)
    {
        rt_kprintf("打开失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }

    for (;;)
    {
        fr = f_read(&fil, s_io, FS_CAT_CHUNK, &got);
        if ((fr != FR_OK) || (got == 0u))
        {
            break;
        }
        s_io[got] = '\0';
        rt_kprintf("%s", s_io);

        total += got;
        if (total >= FS_CAT_MAX)
        {
            rt_kprintf("\n…（超过 %d 字节，后面省略）\n", FS_CAT_MAX);
            break;
        }
    }
    rt_kprintf("\n");
    (void)f_close(&fil);

    if (fr != FR_OK)
    {
        rt_kprintf("读失败: %s\n", fr_str(fr));
    }
}

/* ============================ write / rm ============================ */

static void fs_do_write(const char *path, int argc, char **argv)
{
    FIL fil;
    FRESULT fr;
    UINT written;
    char p[FS_PATH_MAX];
    int i;
    size_t len = 0;

    if (path == NULL)
    {
        rt_kprintf("用法: fs write <path> <text...>\n");
        return;
    }
    if (fs_path(path, p) != 0)
    {
        rt_kprintf("路径太长\n");
        return;
    }

    /* 把剩下的参数用空格拼成一行文本 */
    s_io[0] = '\0';
    for (i = 0; i < argc; i++)
    {
        size_t n = strlen(argv[i]);

        if ((len + n + 2u) > FS_WRITE_MAX)
        {
            rt_kprintf("文本太长（最多 %d 字节）\n", FS_WRITE_MAX);
            return;
        }
        if (i != 0)
        {
            s_io[len++] = ' ';
        }
        memcpy(&s_io[len], argv[i], n);
        len += n;
        s_io[len] = '\0';
    }

    fr = f_open(&fil, p, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        rt_kprintf("创建失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }

    fr = f_write(&fil, s_io, (UINT)len, &written);
    (void)f_close(&fil);

    if ((fr != FR_OK) || (written != len))
    {
        rt_kprintf("写入失败: %s (写入 %u/%u)\n", fr_str(fr), written, (unsigned)len);
        return;
    }
    rt_kprintf("写入 %s，%u 字节\n", p, written);
}

static void fs_do_pwd(void)
{
    char cwd[FS_PATH_MAX];
    FRESULT fr = f_getcwd(cwd, sizeof(cwd));

    if (fr != FR_OK)
    {
        rt_kprintf("取当前目录失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }
    rt_kprintf("%s\n", cwd);
}

static void fs_do_cd(const char *path)
{
    char p[FS_PATH_MAX];
    FRESULT fr;

    if (fs_path((path == NULL) ? "/" : path, p) != 0)
    {
        rt_kprintf("路径太长\n");
        return;
    }

    fr = f_chdir(p);
    if (fr != FR_OK)
    {
        rt_kprintf("切换目录失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }
    fs_do_pwd();
}

static void fs_do_mkdir(const char *path)
{
    FRESULT fr;
    char p[FS_PATH_MAX];

    if (path == NULL)
    {
        rt_kprintf("用法: fs mkdir <path>\n");
        return;
    }
    if (fs_path(path, p) != 0)
    {
        rt_kprintf("路径太长\n");
        return;
    }

    fr = f_mkdir(p);
    if (fr != FR_OK)
    {
        rt_kprintf("建目录失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }
    rt_kprintf("已创建目录 %s\n", p);
}

static void fs_do_rm(const char *path, int recursive)
{
    FRESULT fr;
    char p[FS_PATH_MAX];
    FILINFO fno;

    if (path == NULL)
    {
        rt_kprintf("用法: fs rm [-r] <path>\n");
        return;
    }
    if (fs_path(path, p) != 0)
    {
        rt_kprintf("路径太长\n");
        return;
    }

    if (recursive != 0)
    {
        /* 文件 / 目录通吃：文件直接删，目录先清空再删（fs_util.c） */
        fr = fs_remove_tree(p, s_io, sizeof(s_io));
    }
    else
    {
        fr = f_unlink(p);

        /* f_unlink 对「非空目录」和「只读」都回 FR_DENIED，这里区分一下 */
        if ((fr == FR_DENIED) && (f_stat(p, &fno) == FR_OK) &&
            ((fno.fattrib & AM_DIR) != 0u))
        {
            rt_kprintf("%s 是非空目录，用 fs rm -r %s\n", p, p);
            return;
        }
    }

    if (fr != FR_OK)
    {
        rt_kprintf("删除失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }
    rt_kprintf("已删除 %s\n", p);
}

/* ============================ df ============================ */

static void fs_do_df(void)
{
    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    FRESULT fr = f_getfree(FS_DRIVE, &free_clusters, &fs);
    DWORD total_kb;
    DWORD free_kb;

    if (fr != FR_OK)
    {
        rt_kprintf("取容量失败: %s (FRESULT=%d)\n", fr_str(fr), (int)fr);
        return;
    }

    /* n_fatent = 簇数 + 2；每簇 csize 个 512B 扇区 → KB = 簇数 * csize / 2 */
    total_kb = (fs->n_fatent - 2u) * (DWORD)fs->csize / 2u;
    free_kb = free_clusters * (DWORD)fs->csize / 2u;

    rt_kprintf("卷 %s  总 %lu KB（%lu MB）  剩余 %lu KB（%lu MB）\n",
               FS_DRIVE,
               (unsigned long)total_kb, (unsigned long)(total_kb / 1024u),
               (unsigned long)free_kb, (unsigned long)(free_kb / 1024u));
    rt_kprintf("每簇 %u 扇区（%u 字节），FAT 类型 %s\n",
               (unsigned)fs->csize, (unsigned)(fs->csize * 512u),
               (fs->fs_type == FS_FAT32) ? "FAT32" :
               (fs->fs_type == FS_FAT16) ? "FAT16" :
               (fs->fs_type == FS_FAT12) ? "FAT12" : "其他");
}

/* ============================ 入口 ============================ */

static int fs(int argc, char **argv)
{
    const char *sub;

    if (argc < 2)
    {
        fs_usage();
        return 0;
    }

    sub = argv[1];

    if (strcmp(sub, "dev") == 0)
    {
        fs_do_dev();
        return 0;
    }

    if (strcmp(sub, "part") == 0)
    {
        fs_do_part();
        return 0;
    }

    if (strcmp(sub, "mkfs") == 0)
    {
        fs_do_mkfs((argc > 2) ? argv[2] : NULL);
        return 0;
    }

    /* 下面这些都要有已挂载的卷 */
    if (strcmp(sub, "mount") == 0)
    {
        fs_do_mount((argc > 2) ? argv[2] : NULL);
        return 0;
    }
    if (strcmp(sub, "umount") == 0)
    {
        fs_do_umount();
        return 0;
    }

    if (s_mounted == 0u)
    {
        rt_kprintf("卷 %s 还没挂载，先 fs mount\n", FS_DRIVE);
        return 0;
    }

    if (strcmp(sub, "ls") == 0)
    {
        fs_do_ls((argc > 2) ? argv[2] : NULL);
    }
    else if (strcmp(sub, "cd") == 0)
    {
        fs_do_cd((argc > 2) ? argv[2] : NULL);
    }
    else if (strcmp(sub, "pwd") == 0)
    {
        fs_do_pwd();
    }
    else if (strcmp(sub, "cat") == 0)
    {
        fs_do_cat((argc > 2) ? argv[2] : NULL);
    }
    else if (strcmp(sub, "write") == 0)
    {
        fs_do_write((argc > 2) ? argv[2] : NULL, argc - 3, &argv[3]);
    }
    else if (strcmp(sub, "mkdir") == 0)
    {
        fs_do_mkdir((argc > 2) ? argv[2] : NULL);
    }
    else if (strcmp(sub, "rm") == 0)
    {
        const char *p = (argc > 2) ? argv[2] : NULL;
        int recursive = 0;

        /* 支持 -r / -rf / -fr / -f（-f 只是兼容 Unix 习惯，这里没有实际作用） */
        if ((p != NULL) && (p[0] == '-') && (p[1] != '\0'))
        {
            recursive = (strchr(p, 'r') != NULL) ? 1 : 0;
            p = (argc > 3) ? argv[3] : NULL;
        }
        fs_do_rm(p, recursive);
    }
    else if (strcmp(sub, "df") == 0)
    {
        fs_do_df();
    }
    else if ((strcmp(sub, "help") == 0) || (strcmp(sub, "?") == 0))
    {
        fs_usage();
    }
    else
    {
        rt_kprintf("未知子命令: %s\n", sub);
        fs_usage();
    }
    return 0;
}
MSH_CMD_EXPORT(fs, fs <dev|part|mount|mkfs|ls|cd|pwd|cat|write|mkdir|rm|df>: 文件系统);
