/*----------------------------------------------------------------------------/
/  FatFs 配置（FatFs R0.14b，本工程裁剪版）
/
/  这个文件是本工程自己写的，只保留当前需求用得到的特性。改任何一项之前，
/  先看下面的说明：FatFs 的宏不是「开得越多越好」，有些会额外要求实现
/  回调函数（从而把 OS 依赖带进来）。
/
/  本工程的约束：**与操作系统无关**。
/  所以特意关掉了 LFN（需要 ffunicode.c + 工作缓冲）、exFAT、RTC 时间戳
/  （需要 get_fattime()）、可重入（需要 ff_req_grant 等一组 OS 回调）。
/  需要这些特性时，在 ffconf.h 打开并补对应实现即可，ff.c 一行不用改。
/----------------------------------------------------------------------------*/

#define FFCONF_DEF	86631	/* 必须与 ff.h 的 FF_DEFINED 一致（R0.14b） */

/*---------------------------------------------------------------------------/
/ 功能裁剪
/----------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
/* 0：读写（本工程要写文件）；1：只读，会裁掉 f_write/f_sync/f_unlink/... */

#define FF_FS_MINIMIZE	0
/* 0：保留全部基础 API（f_stat/f_getfree/f_unlink/f_mkdir/f_opendir/...）
/  1~3：逐级裁掉不常用 API。本工程要用 ls/rm/df，所以不裁。 */

#define FF_USE_FIND		0
/* f_findfirst/f_findnext（按通配符找文件）。目录遍历用 f_opendir/f_readdir 就够。 */

#define FF_USE_MKFS		1
/* f_mkfs（格式化）。msh 的 `fs mkfs` 要用。 */

#define FF_USE_FASTSEEK	0
/* f_lseek 的快速定位（FIL 里会多一个 CLMT 表，占 RAM）。本工程顺序读写为主，关掉。 */

#define FF_USE_EXPAND	0
/* f_expand（预分配连续簇）。用不上。 */

#define FF_USE_CHMOD	0
/* f_chmod/f_utime（改属性/时间）。用不上。 */

#define FF_USE_LABEL	0
/* f_getlabel/f_setlabel（卷标）。用不上。 */

#define FF_USE_FORWARD	0
/* f_forward（直接把文件数据转发给流）。用不上。 */

#define FF_USE_STRFUNC	0
/* f_gets/f_putc/f_puts/f_printf。本工程用 f_read/f_write，
/  不引入这套字符串 I/O（打开它会连带 FF_PRINT_LLI/FF_PRINT_FLOAT 等）。 */

/*---------------------------------------------------------------------------/
/ 语言与文件名
/----------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437
/* OEM 代码页。关掉 LFN 后只影响 8.3 短名的字符解释，437（ASCII）足够。
/  要用中文短名再改 936（DBCS）。 */

#define FF_USE_LFN		0
/* 0：只支持 8.3 短名（如 readme.txt、firmware.bin）。
/
/  ⚠️ 打开 LFN（1/2/3）必须同时把 ffunicode.c 加进编译，并且：
/     1 = 静态工作缓冲（占 BSS 约 FF_MAX_LFN*2 字节，非线程安全）
/     2 = 栈上缓冲（吃栈）
/     3 = 堆上缓冲（要求实现 ff_memalloc/ff_memfree，见 ffsystem.c）
/  本工程为了「纯 OS 无关 + 省 RAM」先用短名。 */

#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	0
#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
/* FF_USE_LFN = 0 时以上四项都不生效，保持默认值即可。 */

#define FF_FS_RPATH		2
/* 2：启用相对路径 + f_chdir/f_chdrive + f_getcwd。
/  本工程需要 `fs cd`，所以不能再是 0：
/    0 = 不支持相对路径，所有路径都相对根
/    1 = f_chdir/f_chdrive
/    2 = 再加 f_getcwd（`fs pwd` 要用）
/  注意：路径不以 '/' 开头时，FatFs 从「当前目录」(fs->cdir) 开始解析。 */

/*---------------------------------------------------------------------------/
/ 卷与扇区
/----------------------------------------------------------------------------*/

#define FF_VOLUMES		1
/* 逻辑卷数 = drive 数。本工程 drive N ↔ 第 N 个块设备（blkdev_get(N)）。
/  以后要同时挂 SD + SPI Flash，把这里改成 2。 */

#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"sd"
/* 0：路径写成 "0:/file" 这种数字盘符。 */

#define FF_MULTI_PARTITION	1
/* 1：允许指定挂载哪个分区（VolToPart[vol].pt，0=自动找第一个 FAT 分区，1~4=强制）。
/  本工程卷 0 对应物理盘 0（sd0），pt 可以在运行时改：
/      disk_set_partition(0, 2) → 下一次 f_mount("0:") 挂第 2 个分区
/  msh 里就是 `fs part` 看分区表、`fs mount 2` 挂第 2 个分区。
/  0 仍然自动搜索，和单分区时的行为一致。 */

#define FF_MIN_SS		512
#define FF_MAX_SS		512
/* 扇区大小固定 512（块设备接口 blkdev 也是 512）。
/  两者相等时 FatFs 不要求 disk_ioctl 实现 GET_SECTOR_SIZE。 */

#define FF_LBA64		0
/* 64 位 LBA。SDXC 单分区最大 2TB，32 位 LBA 在 512B 扇区下可寻址 2TB，够用。 */

#define FF_MIN_GPT		0x10000000
/* FF_LBA64 = 0 时不生效。 */

#define FF_USE_TRIM		0
/* ATA TRIM（需要 disk_ioctl 的 CTRL_TRIM）。SD 卡擦除靠卡内部管理，不需要。 */

/*---------------------------------------------------------------------------/
/ 系统相关
/----------------------------------------------------------------------------*/

#define FF_FS_TINY		0
/* 0：每个 FIL 自带一个扇区缓冲（512B），读写时不必互斥同一个缓冲。
/  1：所有 FIL 共用 FATFS 里的一个扇区缓冲，省 RAM 但同一卷上的并发访问会互相踩。 */

#define FF_FS_EXFAT		0
/* exFAT 需要 LFN，且会让代码从 C89 变成 C99+。本工程用 FAT16/FAT32。 */

#define FF_FS_NORTC		1
/* 1：不读 RTC，所有时间戳固定为 FF_NORTC_*。
/  ⚠️ 这是「OS 无关」的关键之一：FF_FS_NORTC = 0 时 FatFs 会调用
/     get_fattime()，默认实现要读 RTC（在 RT-Thread 里就是 rt_device 那一套）。
/  以后要真实时间戳：置 0 并提供一个返回 FAT 时间格式的 get_fattime()。 */

#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026

#define FF_FS_NOFSINFO	0
/* 0：信任 FAT32 的 FSINFO（空闲簇数/下次分配位置），f_getfree 会快很多。
/  如果卡上的 FSINFO 不可靠（某些工具格式化后会写错），改成 1 强制全表扫描。 */

#define FF_FS_LOCK		0
/* 文件锁（防止同一文件被重复打开/删除）。0：不查，代码最小。
/  ⚠️ 不查锁的前提是「调用方自己保证不对打开中的对象做删除/重命名」。
/     本工程由 modules/fs 的单一入口串行化访问。 */

#define FF_FS_REENTRANT	0
/* 0：FatFs 内部不做互斥，重入/线程安全由调用方负责。
/  1：需要实现 ff_cre_syncobj/ff_del_syncobj/ff_req_grant/ff_rel_grant
/     和 FF_SYNC_t —— 那就是把 OS 依赖塞进来了，本工程不这么做。 */

#define FF_FS_TIMEOUT	1000

/*--- End of configuration options ---*/
