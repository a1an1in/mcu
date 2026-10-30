/*
 * sd.h - SD 卡驱动接口（协议层） <drivers/sd/>
 *
 * 分层：
 *   sd.h / sd.c        卡协议：上电识别、CID/CSD、块读写（本文件）
 *   sdio.h             主机控制器接口（SoC 无关）
 *   stm32f1_sdio.c     STM32F1 SDIO 寄存器实现
 *
 * 本层不碰寄存器、不依赖 OS：换 MCU 只换 sdio.h 的实现。
 *
 * ── 用法 ─────────────────────────────────────────────────────────────
 *     struct sd_card card;
 *
 *     if (sd_init(&card, sdio_get_host()) == 0)
 *     {
 *         sd_read_blocks(&card, 0, buf, 1);    // 读 0 号扇区（512 字节）
 *     }
 *
 * ── 关于「什么时候能调 init」────────────────────────────────────────
 *   sd_init() 会和卡通信（CMD0/CMD8/ACMD41…），**必须在内核调度器启动后**
 *   调用（main / 线程 / msh 命令里）。板级的 rt_hw_board_init 阶段只能建
 *   主机对象（stm32f1_sdio_init 只配寄存器和引脚，不发命令），不能识别卡。
 *   为方便使用，sd_read_blocks / sd_write_blocks 发现卡还没识别会自动补一次
 *   sd_init()。
 *
 * 时序参考：正点原子《STM32F103 战舰开发指南》第 49 章 SD 卡实验。
 */
#ifndef DRIVERS_SD_H__
#define DRIVERS_SD_H__

#include <stdint.h>

#include "sdio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_BLOCK_SIZE       512u

/* ============================ 错误码 ============================ */
#define SD_ERR_IO           (-1)    /* 传输失败 */
#define SD_ERR_PARAM        (-2)    /* 参数非法（空指针 / 块数 0） */
#define SD_ERR_TIMEOUT      (-3)    /* 命令或数据超时 */
#define SD_ERR_CRC          (-4)    /* 命令/数据 CRC 错 */
#define SD_ERR_RESP         (-5)    /* 响应里的错误位（R1 的 bit31:20） */
#define SD_ERR_NOCARD       (-6)    /* 上电识别失败：没插卡 / 卡不响应 */
#define SD_ERR_RANGE        (-7)    /* LBA 越界 */

/* ============================ 卡类型 ============================ */
typedef enum
{
    SD_TYPE_UNKNOWN = 0,
    SD_TYPE_SDSC_V1,        /* 标准容量 SD 1.x（≤2GB，字节寻址） */
    SD_TYPE_SDSC_V2,        /* 标准容量 SD 2.0（≤2GB，字节寻址） */
    SD_TYPE_SDHC,           /* 高容量 SDHC/SDXC（块寻址） */
    SD_TYPE_MMC,            /* MMC（不支持 4 位总线） */
} sd_type_t;

/* ============================ CID（卡标识） ============================ */
struct sd_cid {
    uint8_t  manufacturer;      /* 制造商 ID */
    uint16_t oem;               /* OEM/应用 ID */
    char     product[6];        /* 产品名，5 字符 + '\0' */
    uint8_t  revision;          /* 修订号 */
    uint32_t serial;            /* 序列号 */
    uint16_t year;              /* 生产年份 */
    uint8_t  month;             /* 生产月份 */
};

/* ============================ 卡句柄 ============================ */
struct sd_card {
    struct sdio_host *host;     /* 主机控制器 */
    uint16_t rca;               /* 相对卡地址（CMD3 分配） */
    uint8_t  type;              /* sd_type_t */
    uint8_t  bus_width;         /* 1 或 4 */
    uint8_t  inited;            /* sd_init() 成功过 */
    uint8_t  reserved;
    uint64_t capacity;          /* 字节数（SDXC 会超过 4GB，所以用 64 位） */
    uint32_t block_count;       /* 512 字节块数 */
    uint16_t block_size;        /* 单块字节数，固定 512 */
    uint8_t  cid_raw[16];       /* CID 原始 16 字节（大端顺序） */
    uint8_t  csd_raw[16];       /* CSD 原始 16 字节 */
};

/* ============================ 操作 ============================ */

/* 上电识别并初始化卡：CMD0 → CMD8 → ACMD41 → CMD2/3/9 → CMD7 → ACMD6。
 * 成功后切到 SD_TRANSFER_CLOCK_HZ 并在 SD 卡上开 4 位总线。
 * 已初始化过则直接返回 0。失败返回负的 SD_ERR_*。 */
int sd_init(struct sd_card *card, struct sdio_host *host);

/* 读 / 写 count 个 512 字节块。lba 是**块地址**（不是字节地址），驱动内部
 * 会按卡类型换算。
 * buf 必须 **4 字节对齐**（SDIO FIFO 是按字搬的，未对齐会直接异常），
 * 不满足时返回 SD_ERR_PARAM。
 * 返回 0，或负的 SD_ERR_*。 */
int sd_read_blocks(struct sd_card *card, uint32_t lba, void *buf, uint32_t count);
int sd_write_blocks(struct sd_card *card, uint32_t lba, const void *buf, uint32_t count);

/* ============================ 纯函数（可单测） ============================ */

/* 从 CSD 算容量与块大小。返回 0 / SD_ERR_RESP（结构版本不支持）。 */
int sd_parse_csd(const uint8_t csd[16], uint64_t *capacity, uint32_t *block_size);

/* 从 CID 解析厂商 / 产品名 / 序列号 / 日期。返回 0 / SD_ERR_PARAM。 */
int sd_parse_cid(const uint8_t cid[16], struct sd_cid *info);

/* 运行时改 SDIO 时钟（调试用）。hz 会被 SoC 层取成不超过它的最接近分频。
 * 返回 0 / SD_ERR_*。用于扫「这块板/这张卡能跑到多少」。 */
int sd_set_clock(struct sd_card *card, uint32_t hz);

/* ============================ 默认实例 ============================ */
void sd_set_default(struct sd_card *card);
struct sd_card *sd_get_default(void);

/* ============================ 调试 ============================
 * 最近一次发出的命令和它的响应（只用于把「哪条命令失败了」打出来）。
 * 初始化失败时用得上：CRC 错 / 超时是哪个 CMD 出的，一眼能看出来。
 */
void sd_get_last_cmd(uint8_t *cmd, uint32_t resp[4]);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_SD_H__ */
