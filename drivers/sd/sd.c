/*
 * sd.c - SD 卡协议层 <drivers/sd/>
 *
 * 只认识命令和响应，不认识寄存器：所有总线动作通过 sdio.h 的 ops 下发。
 *
 * 初始化序列（SD 2.0 规范，与厂商例程一致）：
 *   CMD0  → 进 idle
 *   CMD8  → 探是不是 2.0 卡（带 0x1AA 校验图样）
 *   CMD55+ACMD41 循环 → 等卡上电完成，顺便拿到 CCS 位（高容量标志）
 *   CMD2  → CID
 *   CMD3  → RCA（卡地址）
 *   CMD9  → CSD（算容量）
 *   CMD7  → 选中卡
 *   CMD55+ACMD6 → 切 4 位总线
 *   最后把时钟提到 SD_TRANSFER_CLOCK_HZ
 *
 * 块读写：
 *   读：CMD16（SDSC 才需要）→ 置数据状态机 → CMD17/CMD18 → 搬数据 → CMD12
 *   写：同上，CMD24/CMD25，写完再等卡内部编程完成
 */
#include <string.h>

#include <osal.h>

#include "sd.h"
#include "sd_config.h"

/* ============================ 命令码 ============================ */
#define SD_CMD_GO_IDLE_STATE        0u
#define SD_CMD_ALL_SEND_CID         2u
#define SD_CMD_SET_REL_ADDR         3u
#define SD_CMD_SEL_DESEL_CARD       7u
#define SD_CMD_SEND_IF_COND         8u
#define SD_CMD_SEND_CSD             9u
#define SD_CMD_STOP_TRANSMISSION    12u
#define SD_CMD_SEND_STATUS          13u
#define SD_CMD_SET_BLOCKLEN         16u
#define SD_CMD_READ_SINGLE_BLOCK    17u
#define SD_CMD_READ_MULT_BLOCK      18u
#define SD_CMD_WRITE_SINGLE_BLOCK   24u
#define SD_CMD_WRITE_MULT_BLOCK     25u
#define SD_CMD_APP_CMD              55u

#define SD_ACMD_SET_BUS_WIDTH       6u
#define SD_ACMD_SD_APP_OP_COND      41u

/* ACMD41 参数：bit31 由卡置位表示就绪；这里给电压窗口 2.7~3.6V */
#define SD_VOLTAGE_WINDOW_SD        0x80100000u
#define SD_CHECK_PATTERN            0x000001AAu
#define SD_HIGH_CAPACITY_FLAG       0x40000000u

/* R1 的错误位在 bit31:20；bit15:0 是状态，不算错 */
#define SD_R1_ERROR_MASK            0xFFF00000u

/* 卡状态字段（R1 的 bit12:9），4 = TRAN（可传输） */
#define SD_R1_STATE_MASK            0x00001E00u
#define SD_R1_STATE_TRAN            0x00000800u

/* ACMD41 轮询次数：卡上电就绪最慢 1 秒（SD 规范），一次 CMD55+ACMD41 在
 * 400kHz 下约 300µs，4000 次 ≈ 1.2s，留一点余量。厂商例程给的是 0xFFFF
 * （最坏 20s），这里按规范收紧。 */
#define SD_MAX_VOLT_TRIAL           4000u

/* 写完等卡内部编程结束：一次 CMD13 在中速下约 20µs，单块写入最坏 250ms，
 * 30000 次 ≈ 0.6s @12MHz（低速时钟下按比例更长）。 */
#define SD_MAX_BUSY_TRIAL           30000u

/* 初始化后是否切 4 位总线。
 *
 * 本板（战舰 V4 + 这张 SanDisk SD64G）实测：1 位读写都正常；切到 4 位后
 * 读会 RXOVERR、写会 TXUNDERR —— 现象是数据通路来不及，指向 DAT1~3
 * （PC9/PC10/PC11）接不上或走线有问题。1 位已稳定可用，默认就用 1 位。
 *
 * 换板/换卡确认 4 位没问题时，把这个宏改成 1 即可，其它代码不用动。 */
#ifndef SD_BUS_WIDTH_4
#define SD_BUS_WIDTH_4              1
#endif

/* 识别阶段依次尝试的时钟：从规范上限往下退。
 * 400kHz 是 SD 规范允许的最高识别速率，不是必须；卡座接触不良时降速最有效。 */
static const uint32_t s_init_clocks[] = { SD_INIT_CLOCK_HZ, 200000u, 100000u };

static struct sd_card *s_default;

/* 最近一次命令与响应，只用于失败时定位是哪条命令出的问题 */
static uint8_t s_last_cmd;
static uint32_t s_last_resp[4];

/* 总线互斥量：一次块读写期间必须独占（并发读写会把卡的状态机搞乱）。
 * 用 OSAL 的互斥量而不是 rt_mutex —— 本组件因此不含任何 RTOS 头。
 * 惰性创建：第一次调用一定发生在 main / msh 线程里。 */
static osal_mutex_t s_lock;

static int sd_lock(void)
{
    if (s_lock == OSAL_NULL)
    {
        if (osal_mutex_create(&s_lock) != OSAL_OK)
        {
            return SD_ERR_IO;
        }
    }
    return (osal_mutex_lock(s_lock, 1000u) == OSAL_OK) ? 0 : SD_ERR_TIMEOUT;
}

static void sd_unlock(void)
{
    if (s_lock != OSAL_NULL)
    {
        osal_mutex_unlock(s_lock);
    }
}

/* ============================ 小工具 ============================ */

/* 4 个响应字 → 16 字节（RESP1 是最高 32 位） */
static void sd_resp_to_bytes(const uint32_t resp[4], uint8_t out[16])
{
    unsigned int i;

    for (i = 0u; i < 16u; i++)
    {
        out[i] = (uint8_t)(resp[i / 4u] >> (24u - 8u * (i % 4u)));
    }
}

/* 发命令，把 SoC 层的错误码翻译成本层的 */
static int sd_send(struct sd_card *card, uint8_t cmd, uint32_t arg,
                   uint8_t resp_type, uint32_t resp[4])
{
    uint32_t local[4];
    int ret;

    s_last_cmd = cmd;
    s_last_resp[0] = 0u;
    s_last_resp[1] = 0u;
    s_last_resp[2] = 0u;
    s_last_resp[3] = 0u;

    ret = card->host->ops->send_cmd(card->host, cmd, arg, resp_type,
                                    (resp != NULL) ? resp : local);

    /* 不管成功失败都把响应记下来：CRC 错时这几个字最能说明问题 */
    s_last_resp[0] = ((resp != NULL) ? resp : local)[0];
    s_last_resp[1] = ((resp != NULL) ? resp : local)[1];
    s_last_resp[2] = ((resp != NULL) ? resp : local)[2];
    s_last_resp[3] = ((resp != NULL) ? resp : local)[3];

    switch (ret)
    {
    case 0:     return 0;
    case -2:    return SD_ERR_TIMEOUT;
    case -3:    return SD_ERR_CRC;
    default:    return SD_ERR_IO;
    }
}

/* 发命令并检查 R1 里的错误位 */
static int sd_send_r1(struct sd_card *card, uint8_t cmd, uint32_t arg)
{
    uint32_t resp[4];
    int ret = sd_send(card, cmd, arg, SDIO_RESP_SHORT, resp);

    if (ret != 0)
    {
        return ret;
    }
    if ((resp[0] & SD_R1_ERROR_MASK) != 0u)
    {
        return SD_ERR_RESP;
    }
    return 0;
}

/* 等卡内部编程结束（写操作之后用）：轮询 CMD13 直到状态回到 TRAN */
static int sd_wait_idle(struct sd_card *card)
{
    uint32_t trial;
    uint32_t resp[4];

    for (trial = 0u; trial < SD_MAX_BUSY_TRIAL; trial++)
    {
        if (sd_send(card, SD_CMD_SEND_STATUS, (uint32_t)card->rca << 16,
                    SDIO_RESP_SHORT, resp) != 0)
        {
            return SD_ERR_IO;
        }
        if ((resp[0] & SD_R1_STATE_MASK) == SD_R1_STATE_TRAN)
        {
            return 0;
        }
    }
    return SD_ERR_TIMEOUT;
}

static int sd_init_locked(struct sd_card *card, struct sdio_host *host);

/* 调用前必须已持有总线锁 */
static int sd_ensure_locked(struct sd_card *card)
{
    if (card->inited != 0u)
    {
        return 0;
    }
    return sd_init_locked(card, card->host);
}

/* ============================ 初始化 ============================ */

static int sd_init_locked(struct sd_card *card, struct sdio_host *host)
{
    uint32_t resp[4];
    uint32_t count;
    uint32_t i;
    uint32_t arg;
    uint32_t csd_block_size = SD_BLOCK_SIZE;
    int v2 = 0;
    int ret;

    if ((card == NULL) || (host == NULL) || (host->ops == NULL))
    {
        return SD_ERR_PARAM;
    }
    if (card->inited != 0u)
    {
        return 0;
    }

    memset(card, 0, sizeof(*card));
    card->host = host;
    card->block_size = SD_BLOCK_SIZE;

    /* ── 上电识别阶段 ────────────────────────────────────────────────
     * SD 规范要求这一段 ≤400kHz。但实际板子上 400kHz 有时偏快：卡座接触
     * 电阻、走线长短、卡本身的驱动能力都会让响应 CRC 抖动。所以这里依次
     * 降速重试（400k → 200k → 100k），哪一档能稳定收到 CMD8 的响应就用它
     * 继续后面的初始化 —— 不用手动改宏、也不用重新烧固件。 */
    ret = SD_ERR_NOCARD;
    for (i = 0u; i < (uint32_t)(sizeof(s_init_clocks) / sizeof(s_init_clocks[0])); i++)
    {
        uint32_t trial;

        if (host->ops->set_clock != NULL)
        {
            (void)host->ops->set_clock(host, s_init_clocks[i]);
        }

        /* CMD0：上电后最多试 74 次（卡内部要 74 个以上时钟才认命令） */
        for (trial = 0u; trial < 74u; trial++)
        {
            if (sd_send(card, SD_CMD_GO_IDLE_STATE, 0u, SDIO_RESP_NONE, NULL) == 0)
            {
                break;
            }
        }

        /* CMD8：带 0x1AA 校验图样，是第一条「带响应」的命令。
         * 能收到且 CRC 正确，就说明这一档时钟可用。 */
        ret = sd_send(card, SD_CMD_SEND_IF_COND, SD_CHECK_PATTERN,
                      SDIO_RESP_SHORT, resp);
        if (ret == 0)
        {
            if ((resp[0] & 0x00000FFFu) == (SD_CHECK_PATTERN & 0x00000FFFu))
            {
                v2 = 1;
            }
            break;
        }
    }

    if (ret != 0)
    {
        /* 三档都不行：-3 超时基本是没卡 / 没接触好；
         * -4 CRC 说明有响应但帧不对（接触电阻大、卡不兼容、线太长）。 */
        return ret;
    }

    /* CMD55 + ACMD41：等卡上电完成 */
    for (count = 0u; count < SD_MAX_VOLT_TRIAL; count++)
    {
        ret = sd_send_r1(card, SD_CMD_APP_CMD, 0u);
        if (ret != 0)
        {
            return SD_ERR_NOCARD;       /* 不响应 ACMD 的按非 SD 卡处理 */
        }

        arg = SD_VOLTAGE_WINDOW_SD | (v2 != 0 ? SD_HIGH_CAPACITY_FLAG : 0u);
        /* ACMD41 的响应是 R3（OCR），按规范没有 CRC7 —— 必须忽略 CRC 错，
         * 否则这条命令永远「失败」。见 sdio.h 的 SDIO_RESP_NO_CRC。 */
        ret = sd_send(card, SD_ACMD_SD_APP_OP_COND, arg,
                      SDIO_RESP_SHORT | SDIO_RESP_NO_CRC, resp);
        if (ret != 0)
        {
            return ret;
        }
        if ((resp[0] & 0x80000000u) != 0u)
        {
            break;
        }
    }
    if (count >= SD_MAX_VOLT_TRIAL)
    {
        return SD_ERR_NOCARD;
    }

    if (v2 == 0)
    {
        card->type = (uint8_t)SD_TYPE_SDSC_V1;
    }
    else if ((resp[0] & SD_HIGH_CAPACITY_FLAG) != 0u)
    {
        card->type = (uint8_t)SD_TYPE_SDHC;
    }
    else
    {
        card->type = (uint8_t)SD_TYPE_SDSC_V2;
    }

    /* CMD2 → CID */
    ret = sd_send(card, SD_CMD_ALL_SEND_CID, 0u, SDIO_RESP_LONG, resp);
    if (ret != 0)
    {
        return ret;
    }
    sd_resp_to_bytes(resp, card->cid_raw);

    /* CMD3 → RCA（R6 的 bit31:16） */
    ret = sd_send(card, SD_CMD_SET_REL_ADDR, 0u, SDIO_RESP_SHORT, resp);
    if (ret != 0)
    {
        return ret;
    }
    card->rca = (uint16_t)(resp[0] >> 16);

    /* CMD9 → CSD（要选中卡之前读；SD 规范允许用 RCA 读） */
    ret = sd_send(card, SD_CMD_SEND_CSD, (uint32_t)card->rca << 16,
                  SDIO_RESP_LONG, resp);
    if (ret != 0)
    {
        return ret;
    }
    sd_resp_to_bytes(resp, card->csd_raw);

    ret = sd_parse_csd(card->csd_raw, &card->capacity, &csd_block_size);
    if (ret != 0)
    {
        return ret;
    }
    /* I/O 一律按 512 字节块走（SD 规范要求所有卡都支持），CSD 里的
     * READ_BL_LEN 只是「最大块长」，不拿它当 I/O 粒度 */
    card->block_size = SD_BLOCK_SIZE;
    card->block_count = (uint32_t)(card->capacity / SD_BLOCK_SIZE);

    /* CMD7 → 选中这张卡 */
    ret = sd_send_r1(card, SD_CMD_SEL_DESEL_CARD, (uint32_t)card->rca << 16);
    if (ret != 0)
    {
        return ret;
    }

    /* CMD55 + ACMD6 → 4 位总线（MMC 不支持） */
    if (SD_BUS_WIDTH_4 != 0 && card->type != (uint8_t)SD_TYPE_MMC)
    {
        ret = sd_send_r1(card, SD_CMD_APP_CMD, (uint32_t)card->rca << 16);
        if (ret == 0)
        {
            ret = sd_send_r1(card, SD_ACMD_SET_BUS_WIDTH, 2u);
        }
        if (ret == 0)
        {
            if (host->ops->set_bus_width != NULL)
            {
                (void)host->ops->set_bus_width(host, 4u);
            }
            card->bus_width = 4u;
        }
        else
        {
            card->bus_width = 1u;       /* 切不过去就退回 1 位，不影响功能 */
        }
    }
    else
    {
        card->bus_width = 1u;
    }

    /* 识别完成，提到传输时钟 */
    if (host->ops->set_clock != NULL)
    {
        (void)host->ops->set_clock(host, SD_TRANSFER_CLOCK_HZ);
    }

    card->inited = 1u;
    return 0;
}

int sd_init(struct sd_card *card, struct sdio_host *host)
{
    int ret;

    if ((card == NULL) || (host == NULL))
    {
        return SD_ERR_PARAM;
    }

    ret = sd_lock();
    if (ret != 0)
    {
        return ret;
    }

    ret = sd_init_locked(card, host);
    sd_unlock();
    return ret;
}

/* ============================ 块读写 ============================ */

int sd_read_blocks(struct sd_card *card, uint32_t lba, void *buf, uint32_t count)
{
    uint32_t addr;
    uint32_t bytes;
    int ret;

    if (card == NULL || buf == NULL || count == 0u)
    {
        return SD_ERR_PARAM;
    }
    if (((uintptr_t)buf & 3u) != 0u)
    {
        return SD_ERR_PARAM;        /* SDIO FIFO ???????? 4 ???? */
    }

    ret = sd_lock();
    if (ret != 0)
    {
        return ret;
    }

    ret = sd_ensure_locked(card);
    if (ret == 0)
    {
        if ((lba >= card->block_count) || (count > (card->block_count - lba)))
        {
            ret = SD_ERR_RANGE;
        }
    }
    if (ret != 0)
    {
        sd_unlock();
        return ret;
    }

    bytes = count * SD_BLOCK_SIZE;
    /* SDHC/SDXC 用块地址；SDSC 用字节地址 */
    addr = (card->type == (uint8_t)SD_TYPE_SDHC) ? lba : (lba * SD_BLOCK_SIZE);

    if (card->type != (uint8_t)SD_TYPE_SDHC)
    {
        ret = sd_send_r1(card, SD_CMD_SET_BLOCKLEN, SD_BLOCK_SIZE);
    }

    if (ret == 0)
    {
        ret = (card->host->ops->data_start(card->host, 1, NULL, bytes, SD_BLOCK_SIZE) == 0)
              ? 0 : SD_ERR_IO;
    }

    if (ret == 0)
    {
        ret = sd_send_r1(card, (count > 1u) ? SD_CMD_READ_MULT_BLOCK
                                            : SD_CMD_READ_SINGLE_BLOCK, addr);
    }

    if (ret == 0)
    {
        ret = (card->host->ops->data_move(card->host, 1, buf, bytes) == 0)
              ? 0 : SD_ERR_IO;
    }

    if (count > 1u)
    {
        /* 多块读要显式停止；失败也发，别把总线留在数据传输态 */
        (void)sd_send(card, SD_CMD_STOP_TRANSMISSION, 0u, SDIO_RESP_SHORT, NULL);
    }

    sd_unlock();
    return ret;
}

int sd_write_blocks(struct sd_card *card, uint32_t lba, const void *buf, uint32_t count)
{
    uint32_t addr;
    uint32_t bytes;
    int ret;

    if (card == NULL || buf == NULL || count == 0u)
    {
        return SD_ERR_PARAM;
    }
    if (((uintptr_t)buf & 3u) != 0u)
    {
        return SD_ERR_PARAM;        /* SDIO FIFO ???????? 4 ???? */
    }

    ret = sd_lock();
    if (ret != 0)
    {
        return ret;
    }

    ret = sd_ensure_locked(card);
    if (ret == 0)
    {
        if ((lba >= card->block_count) || (count > (card->block_count - lba)))
        {
            ret = SD_ERR_RANGE;
        }
    }
    if (ret != 0)
    {
        sd_unlock();
        return ret;
    }

    bytes = count * SD_BLOCK_SIZE;
    addr = (card->type == (uint8_t)SD_TYPE_SDHC) ? lba : (lba * SD_BLOCK_SIZE);

    if (card->type != (uint8_t)SD_TYPE_SDHC)
    {
        ret = sd_send_r1(card, SD_CMD_SET_BLOCKLEN, SD_BLOCK_SIZE);
    }

    /* 顺序（与厂商例程一致）：先发写命令，等它接收（R1），再武装数据通路
     * 并把 FIFO 预填满。
     *
     * 反过来（DTEN 提前置位）的话，数据通路在命令之前就开始把 FIFO 里的数据
     * 当时钟打出去，卡收到的是错位的数据 —— 实测「512 字节全发完但卡回 DCRCFAIL」。 */
    if (ret == 0)
    {
        ret = sd_send_r1(card, (count > 1u) ? SD_CMD_WRITE_MULT_BLOCK
                                            : SD_CMD_WRITE_SINGLE_BLOCK, addr);
    }

    if (ret == 0)
    {
        ret = (card->host->ops->data_start(card->host, 0, buf, bytes, SD_BLOCK_SIZE) == 0)
              ? 0 : SD_ERR_IO;
    }

    if (ret == 0)
    {
        ret = (card->host->ops->data_move(card->host, 0, (void *)buf, bytes) == 0)
              ? 0 : SD_ERR_IO;
    }

    if (count > 1u)
    {
        (void)sd_send(card, SD_CMD_STOP_TRANSMISSION, 0u, SDIO_RESP_SHORT, NULL);
    }

    if (ret == 0)
    {
        /* 数据进了卡的缓冲区，还要等它真正写进 flash；不等的话紧接着读会拿到旧数据 */
        ret = sd_wait_idle(card);
    }

    sd_unlock();
    return ret;
}

/* ============================ CSD / CID 解析 ============================ */

int sd_parse_csd(const uint8_t csd[16], uint64_t *capacity, uint32_t *block_size)
{
    uint8_t version;

    if (csd == NULL || capacity == NULL || block_size == NULL)
    {
        return SD_ERR_PARAM;
    }

    version = (uint8_t)(csd[0] >> 6);

    if (version == 0u)
    {
        /* CSD v1.0：SDSC。capacity = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN */
        uint32_t read_bl_len = (uint32_t)(csd[5] & 0x0Fu);
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10) |
                          ((uint32_t)csd[7] << 2) |
                          ((uint32_t)csd[8] >> 6);
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03u) << 1) |
                               ((uint32_t)csd[10] >> 7);
        uint32_t blk = 1u << read_bl_len;

        *block_size = blk;
        *capacity = (uint64_t)(c_size + 1u) *
                    (uint64_t)(1u << (c_size_mult + 2u)) *
                    (uint64_t)blk;
        return 0;
    }

    if (version == 1u)
    {
        /* CSD v2.0：SDHC/SDXC。capacity = (C_SIZE+1) * 512KB */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3Fu) << 16) |
                          ((uint32_t)csd[8] << 8) |
                          (uint32_t)csd[9];

        *block_size = SD_BLOCK_SIZE;
        *capacity = (uint64_t)(c_size + 1u) * 512u * 1024u;
        return 0;
    }

    return SD_ERR_RESP;     /* 3.0 以后的 CSD 结构，本驱动不支持 */
}

int sd_parse_cid(const uint8_t cid[16], struct sd_cid *info)
{
    unsigned int i;

    if (cid == NULL || info == NULL)
    {
        return SD_ERR_PARAM;
    }

    memset(info, 0, sizeof(*info));

    info->manufacturer = cid[0];
    info->oem = (uint16_t)(((uint16_t)cid[1] << 8) | cid[2]);
    for (i = 0u; i < 5u; i++)
    {
        info->product[i] = (char)cid[3u + i];
    }
    info->product[5] = '\0';
    info->revision = cid[8];
    info->serial = ((uint32_t)cid[9] << 24) | ((uint32_t)cid[10] << 16) |
                   ((uint32_t)cid[11] << 8) | (uint32_t)cid[12];
    /* 生产日期：byte13 低 4 位是年份高 4 位，byte14 高 4 位是年份低 4 位、
     * 低 4 位是月份 */
    info->year = (uint16_t)(2000u + (uint16_t)(((cid[13] & 0x0Fu) << 4) |
                                               (cid[14] >> 4)));
    info->month = (uint8_t)(cid[14] & 0x0Fu);
    return 0;
}

int sd_set_clock(struct sd_card *card, uint32_t hz)
{
    if ((card == NULL) || (card->host == NULL) || (card->host->ops == NULL) ||
        (card->host->ops->set_clock == NULL))
    {
        return SD_ERR_PARAM;
    }
    return (card->host->ops->set_clock(card->host, hz) == 0) ? 0 : SD_ERR_IO;
}

/* ============================ 默认实例 ============================ */

void sd_set_default(struct sd_card *card)
{
    s_default = card;
}

struct sd_card *sd_get_default(void)
{
    return s_default;
}

void sd_get_last_cmd(uint8_t *cmd, uint32_t resp[4])
{
    if (cmd != NULL)
    {
        *cmd = s_last_cmd;
    }
    if (resp != NULL)
    {
        resp[0] = s_last_resp[0];
        resp[1] = s_last_resp[1];
        resp[2] = s_last_resp[2];
        resp[3] = s_last_resp[3];
    }
}
