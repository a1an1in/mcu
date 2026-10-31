# FatFs 文件系统：分层、配置与用法

本工程在 SD 卡驱动之上接入了 **FatFs R0.14b**，并保证**与操作系统无关**
（不 include 任何 RTOS 头、不依赖 `get_fattime()` / 动态内存 / 同步对象）。
本文记录分层、配置取舍、块设备接口，以及上板时要注意的地方。

对应代码：

```text
modules/msh/fs_cmd.c          msh 命令（fs list/mount/ls/cat/write/mkfs/df）
modules/fs/diskio.c           FatFs 的 disk 层：drive ↔ 块设备
modules/fs/fatfs/ff.c         FatFs 本体（第三方，原样保留）
modules/fs/fatfs/ffconf.h     本工程的裁剪配置（重点看这个）
drivers/block/blkdev.c        512B 块设备接口 + 静态注册表
drivers/sd/sd_blkdev.c        SD 卡 → 块设备的适配
tests/fs/test_fatfs.c         256KB RAM 盘上的 host 单测
```

---

## 1. 分层与数据流

```text
        msh / 应用
            │  f_open / f_read / f_write / f_mkfs …
            ▼
   modules/fs/fatfs/ff.c          ← FatFs：FAT12/16/32 解析、目录、簇分配
            │  disk_read / disk_write / disk_ioctl
            ▼
   modules/fs/diskio.c            ← 只做映射：pdrv → blkdev_get(pdrv)
            │  blkdev_read / blkdev_write / blkdev_init
            ▼
   drivers/block/blkdev.c         ← 设备模型 + 注册表（不认识具体硬件）
            │  struct blkdev_ops
            ├────────────────────────────┐
            ▼                            ▼
   drivers/sd/sd_blkdev.c        （以后）SPI Flash / RAM 盘
            │  sd_read_blocks …
            ▼
   drivers/sd/sd.c → sdio.h → stm32f1_sdio.c
```

每一层只依赖下一层，**没有一层需要知道上面是谁**：

- FatFs 不知道底下是 SD 卡还是 RAM 盘，只知道「扇区 0..N-1 能读写」；
- `blkdev` 不知道上面是 FatFs，也可以是裸块读写或别的文件系统；
- SD 驱动不知道有文件系统，仍然是 `sd_read_blocks()` 的卡协议层。

---

## 2. 为什么能做到「与 OS 无关」

FatFs 本身是纯 C，但默认配置会把 OS 依赖带进来。本工程在
`modules/fs/fatfs/ffconf.h` 里关掉了这四条：

| 配置 | 本工程 | 打开后需要什么 |
|---|---|---|
| `FF_FS_NORTC` | **1** | `0` 时 FatFs 调 `get_fattime()` 写时间戳；默认实现要读 RTC（RT-Thread 里就是 `rt_device_*`） |
| `FF_USE_LFN` | **0** | 非 0 要加 `ffunicode.c`；`= 3` 还要实现 `ff_memalloc/ff_memfree`（`ffsystem.c`），`= 2` 吃栈 |
| `FF_FS_REENTRANT` | **0** | `1` 要实现 `ff_cre_syncobj/ff_req_grant/ff_rel_grant/ff_del_syncobj`，`FF_SYNC_t` 就是 RTOS 的信号量类型 |
| `FF_FS_EXFAT` | **0** | 要求 `FF_USE_LFN >= 1`，且代码从 C89 变 C99+ |

另外两条也是刻意选的：

- `FF_VOLUMES = 1`：一个逻辑卷，对应一个块设备；扩容只改这个数字和注册表。
- `FF_FS_LOCK = 0`：不维护文件打开表，省代码；代价是**调用方自己保证**
  不对打开中的文件做删除/重命名。本工程只有一个 `fs` 命令入口，天然串行。

所以整个 `modules/fs` 里没有任何 `rt_*` / `osal_*` 调用，换 RTOS 时一行不用改。

---

## 3. 块设备接口（drivers/block）

```c
struct blkdev_ops {
    int (*init)(struct blkdev *dev);                                   /* 可为 NULL */
    int (*read)(struct blkdev *dev, uint32_t lba, void *buf, uint32_t count);
    int (*write)(struct blkdev *dev, uint32_t lba, const void *buf, uint32_t count);
};

struct blkdev {
    const char *name;              /* "sd0" */
    const struct blkdev_ops *ops;
    void *ctx;                     /* 介质私有数据（sd_card / RAM 盘描述符） */
    uint32_t sector_count;         /* 512B 块数，init 里填 */
    uint16_t sector_size;          /* 固定 512 */
    uint8_t inited;
};
```

规则：

| 规则 | 原因 |
|---|---|
| 扇区固定 **512 字节** | FatFs 配置 `FF_MIN_SS = FF_MAX_SS = 512`，块设备层就不必再协商大小 |
| `lba` 是**块地址**，不是字节地址 | 与 SD/ATA/FatFs 一致，避免每层各乘一次 512 |
| `blkdev_register()` **不做任何 I/O** | board 初始化时调度器还没起来；SD 的 `sd_init()` 会拿 `osal_mutex`，此时调用会死 |
| 真正通信放在 `blkdev_init()` | 第一次 `f_mount` 时触发，线程上下文里安全 |
| 本层**不加锁** | 目前只有文件系统一个使用者；将来多线程访问同一设备，在 `blkdev_read/write` 外层加锁即可 |

注册表是 `static struct blkdev s_devs[BLKDEV_MAX]`，不分配内存。`BLKDEV_MAX = 4`，
足够 SD + SPI Flash + RAM 盘。

### 新增一种介质要写什么

以 SPI Flash 为例，只需要一个 `xxx_blkdev.c`：

```c
static int flash_init(struct blkdev *dev)
{
    dev->sector_count = 16u * 1024u * 1024u / 512u;   /* 16MB */
    return 0;
}

static int flash_read(struct blkdev *dev, uint32_t lba, void *buf, uint32_t count)
{
    return w25q128_read(dev->ctx, lba * 512u, buf, count * 512u);
}

static const struct blkdev_ops flash_ops = { flash_init, flash_read, flash_write };
blkdev_register("flash0", &flash_ops, &s_flash, NULL);
```

**注意**：NOR Flash 的擦除单位是 4KB，块设备接口没有「擦除」概念，
写之前要自己保证区域已擦除（可以在 `write` 里做读-改-写），并把
`disk_ioctl(GET_BLOCK_SIZE)` 报成 8（4KB / 512）——目前 `diskio.c` 固定报 1，
接 Flash 时需要按介质区分。

### SD 适配层（sd_blkdev.c）做了什么

1. `init`：调 `sd_init(card, card->host)`，把 `card->block_count` 填进
   `dev->sector_count`。识别失败就直接返回错误 → FatFs 得到 `FR_NOT_READY`。
2. `read/write`：直接转发 `sd_read_blocks/sd_write_blocks`。
3. **对齐跳板**：SDIO FIFO 按 32 位搬，`sd_read_blocks` 要求缓冲 4 字节对齐。
   FatFs 自己的扇区缓冲是对齐的，但 `f_read` 整扇区访问时会**直接把用户缓冲
   传下来**（栈上的 `char[]` 未必对齐），所以这里用 512B 静态跳板按扇区中转。

---

## 4. FatFs 配置取舍（ffconf.h）

| 宏 | 值 | 说明 |
|---|---|---|
| `FF_FS_READONLY` | 0 | 需要写文件 |
| `FF_FS_MINIMIZE` | 0 | 保留 `ls/rm/df` 要用的 API |
| `FF_USE_MKFS` | 1 | `fs mkfs` |
| `FF_USE_STRFUNC` | 0 | 不用 `f_printf`；写文件走 `f_write` |
| `FF_USE_LABEL` / `FF_USE_CHMOD` / `FF_USE_EXPAND` / `FF_USE_FORWARD` / `FF_USE_FIND` / `FF_USE_FASTSEEK` | 0 | 用不上，省 Flash/RAM |
| `FF_CODE_PAGE` | 437 | 关 LFN 后只影响 8.3 短名的 OEM 解释，ASCII 够用 |
| `FF_USE_LFN` | 0 | 只支持 8.3 短名 |
| `FF_FS_RPATH` | 0 | 一律绝对路径 `0:/...` |
| `FF_VOLUMES` | 1 | drive 0 = 第 0 个块设备 |
| `FF_MULTI_PARTITION` | 1 | 允许指定分区：`VolToPart[0].pt` 运行时可改（0=自动找第一个 FAT 分区，1~4=强制） |
| `FF_MIN_SS` / `FF_MAX_SS` | 512 / 512 | 固定 512，不要求 `GET_SECTOR_SIZE` |
| `FF_LBA64` | 0 | 32 位 LBA 在 512B 扇区下可寻址 2TB |
| `FF_FS_TINY` | 0 | 每个 `FIL` 自带 512B 缓冲，省掉「同卷并发踩缓冲」的坑 |
| `FF_FS_NORTC` | 1 | 时间戳固定为 2026-01-01 |
| `FF_FS_NOFSINFO` | 0 | 信任 FAT32 的 FSINFO，`f_getfree` 不用扫全表 |
| `FF_FS_REENTRANT` | 0 | 线程安全交给调用方 |

### 以后想支持长文件名

1. 把 `modules/fs/fatfs/ffunicode.c` 从 FatFs 发行包加进 `modules/CMakeLists.txt`；
2. `FF_USE_LFN` 改 1（静态缓冲，占 BSS 约 `FF_MAX_LFN * 2` 字节）；
3. `FF_LFN_UNICODE` 按需要改成 2（UTF-8，`f_open("0:/中文.txt")` 可以直接用）。

代价：Flash 约 +15~25KB（码表），RAM +512B。本工程 Flash 还有 350KB 余量，
需要时随时可以开。

---

## 5. 使用（msh 命令）

```text
fs list                 列出块设备（下标 / 名字 / 扇区数 / 容量 / 状态）
fs part                 打印 MBR 分区表（类型 / 起始 LBA / 大小）
fs mount [pt]           挂载 0:（pt=1~4 指定分区，省略=自动找第一个 FAT）
fs umount               卸载
fs mkfs [pt]            格式化（pt 省略=重写整盘分区表；pt=1~4 只格式化该分区）
fs ls [path]            列目录（默认 /）
fs cat <path>           打印文本文件（最多 4KB）
fs write <path> <text…> 写文本文件（覆盖）
fs rm <path>            删除文件
fs df                   总容量 / 剩余空间 / FAT 类型
```

路径可以写 `/dir/a.txt`（自动补成 `0:/dir/a.txt`），也可以写完整 `0:/dir/a.txt`。

典型流程：

```text
msh > fs list
下标 名字            扇区数      容量       状态
0    sd0             124735488   60905      未初始化
msh > fs mount
已挂载 0:
msh > fs ls
属性       大小  名字
d---         0  SYSTEM~1/
----      1234  README.TXT
msh > fs cat /readme.txt
...
```

### 上板必须知道的三个行为

1. **第一次 `fs mount` 才识别卡**。`fs list` 里显示「未初始化」是正常的，
   `fs mount` 会通过 `blkdev_init()` → `sd_init()` 走完整的 CMD0/CMD8/ACMD41。
2. **默认挂载的是卡上的第一个 FAT 分区，不是整张卡**。自动搜索会跳过
   exFAT / NTFS / ext4（FatFs 只认 FAT）。先 `fs part` 看分区表，再用
   `fs mount <pt>` 指定分区：

   ```text
   msh > fs part
   分区  类型        起始 LBA      扇区数        大小        文件系统
   1     0x0b         63            204800        100          FAT32
   2     0x0c         204863        124530625     60805        FAT32 LBA

   msh > fs mount 2
   已挂载 0:（分区 2）
   ```

3. **`fs mkfs` 默认重写整盘分区表**（`pt=0`），原有分区和数据全丢；
   `fs mkfs 2` 只格式化第 2 分区，其它分区不受影响。只在不心疼数据的分区上做。

---

## 6. host 单测

`tests/fs/test_fatfs.c` 用一块 **256KB 的 RAM 盘**当介质，把下面这条链路
完整跑一遍（不需要任何硬件）：

```text
blkdev_register → blkdev_init → diskio → f_mkfs → f_mount
→ f_open/f_write/f_close → 卸载重挂 → f_open/f_read 比对
→ 2KB 跨扇区文件 → f_opendir/f_readdir → f_unlink
→ 抹掉引导扇区 → 确认 FR_NO_FILESYSTEM → 重新格式化
```

覆盖点：

- 注册表：重名拒绝、缺 `read/write` 的 ops 拒绝、按名字/下标查找；
- 未 `init` 时读写必须报错；`init` 幂等；越界（含跨末块）拒绝；
- `disk_ioctl` 的 `GET_SECTOR_COUNT / GET_SECTOR_SIZE / GET_BLOCK_SIZE`；
- 格式化后容量合理（`f_mkfs` 默认建分区，卷比整盘小 63 个扇区）；
- 文件内容、大小、目录项、删除；
- **分区选择**：强制 `pt=1` 挂到同一个卷、不存在的 `pt=3` 报
  `FR_NO_FILESYSTEM`、恢复 `pt=0` 自动搜索；
- 卸载再挂载内容仍在（证明写真的落到了介质，而不是只改了缓存）。

运行：

```bash
python tools/devops.py build --host
python tools/devops.py test        # 10/10 通过，含 fatfs
```

调试时可以直接用 ASan 跑（需要 docker）：

```bash
docker run --rm -v "$PWD:/work" -w /work stm32f103-dev sh -c \
  "gcc -g -O0 -fsanitize=address -Idrivers/block -Imodules/fs -Imodules/fs/fatfs \
   -o /tmp/t tests/fs/test_fatfs.c drivers/block/blkdev.c modules/fs/diskio.c \
   modules/fs/fatfs/ff.c && /tmp/t"
```

---

## 7. 资源占用

接入后（-O2，72MHz）：

| 项目 | 接入前 | 接入后 | 增量 |
|---|---|---|---|
| Flash | 148.9 KB | 163.6 KB | **+14.7 KB**（FatFs 本体 + 胶水 + msh 命令） |
| RAM | 24.4 KB | 30.0 KB | **+5.6 KB**（mkfs 4KB 工作缓冲 + cat 512B + FATFS/FIL） |

RAM 增量主要来自 `fs_cmd.c` 里的静态缓冲：

- `s_work[4096]`：只给 `f_mkfs` 用；
- `s_io[512]`：`cat` / `write` 的读写缓冲；
- `FATFS s_fs`：约 600B。

如果以后 RAM 紧张，可以把 `s_work` 缩到 512（`f_mkfs` 允许，但会慢一些），
或者干脆把 `fs mkfs` 命令裁剪掉。

---

## 8. 已知限制与后续

| 限制 | 影响 | 后续做法 |
|---|---|---|
| 只支持 8.3 短名 | 长文件名 / 中文名不可用 | 打开 `FF_USE_LFN` + 加 `ffunicode.c`（见第 4 节） |
| `FF_VOLUMES = 1` | 只能同时挂一个卷 | 改大并注册多个块设备；路径用 `1:/` |
| 无内部互斥 | 多个线程同时访问同一卷会踩缓冲 | 在 `diskio.c` 或 msh 外层加一把 `osal_mutex` |
| `GET_BLOCK_SIZE` 固定 1 | 接 NOR Flash 时 `f_mkfs` 不会按 4KB 对齐 | 在 `blkdev` 里加 `erase_block` 字段，由 `disk_ioctl` 返回 |
| SD 24MHz | 12MB/s，够用但不是极限 | CMD6 高速模式 + DMA → 36MHz，见 `docs/issues/sd-highspeed-dma-todo.md` |
| `fs cat` 上限 4KB | 大文件只看开头 | 换 `f_lseek` + 分页，或做个 `fs hexdump` |

---

## 9. 参考资料

- FatFs 官方文档（R0.14b）：`ff.h` 头部注释、`ffconf.h` 每项说明
- FatFs 应用笔记：`http://elm-chan.org/fsw/ff/`
- 本工程 SD 协议层：`drivers/sd/sd.h`
- 时钟与主频：`docs/stm32f1-clock.md`
