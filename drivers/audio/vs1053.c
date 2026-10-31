/*
 * vs1053.c - VS1053B 音频解码器驱动 <drivers/audio/>
 *
 * 只做芯片协议：SCI（寄存器）与 SDI（音频数据）两条通道，全部走 drivers/spi
 * 的 spi_device / spi_sync。片选由 SPI 核心按设备切换：
 *   cmd  : CS = XCS (PF7)
 *   data : CS = XDCS(PF6)
 *
 * 与 CH395 共用 SPI1：互斥由 SPI 核心的总线锁负责（ch395_port.c 也用同一把），
 * 所以这里不需要任何自己的锁。
 *
 * 时序参考：正点原子《STM32F103 战舰开发指南》第 54 章 + VS1053B 数据手册。
 */
#include <string.h>

#include <osal.h>

#include "vs1053.h"
#include "vs1053_config.h"

/* SPI 操作码 */
#define VS1053_OP_WRITE     0x02u
#define VS1053_OP_READ      0x03u

/* VS1053 内部 RAM 地址（GPIO 控制） */
#define VS1053_GPIO_DDR     0xC017u
#define VS1053_GPIO_ODATA   0xC019u

/* 默认音量 0~100 */
#define VS1053_DEFAULT_VOL  80

static struct vs1053 *s_default;

/* ============================ 基础工具 ============================ */

static int vs1053_wait_dreq(struct vs1053 *dev, uint32_t timeout_ms)
{
    uint32_t waited = 0;

    while (gpio_read(VS1053_DREQ_PIN) == 0)
    {
        if (waited >= timeout_ms)
        {
            return VS1053_ERR_TIMEOUT;
        }
        osal_thread_sleep_ms(1);
        waited++;
    }
    return VS1053_OK;
}

int vs1053_ready(struct vs1053 *dev)
{
    if (dev == NULL)
    {
        return 0;
    }
    return (gpio_read(VS1053_DREQ_PIN) != 0) ? 1 : 0;
}

/* ============================ 寄存器访问 ============================ */

int vs1053_write_reg(struct vs1053 *dev, uint8_t reg, uint16_t val)
{
    uint8_t tx[4];
    int ret;

    if ((dev == NULL) || (dev->cmd.ctlr == NULL))
    {
        return VS1053_ERR_PARAM;
    }

    ret = vs1053_wait_dreq(dev, VS1053_DREQ_TIMEOUT_MS);
    if (ret != VS1053_OK)
    {
        return ret;
    }

    tx[0] = VS1053_OP_WRITE;
    tx[1] = reg;
    tx[2] = (uint8_t)(val >> 8);
    tx[3] = (uint8_t)(val & 0xFFu);

    return (spi_write(&dev->cmd, tx, sizeof(tx)) == 0) ? VS1053_OK : VS1053_ERR_IO;
}

int vs1053_read_reg(struct vs1053 *dev, uint8_t reg, uint16_t *val)
{
    uint8_t tx[2];
    uint8_t rx[2];
    int ret;

    if ((dev == NULL) || (val == NULL) || (dev->cmd.ctlr == NULL))
    {
        return VS1053_ERR_PARAM;
    }

    ret = vs1053_wait_dreq(dev, VS1053_DREQ_TIMEOUT_MS);
    if (ret != VS1053_OK)
    {
        return ret;
    }

    tx[0] = VS1053_OP_READ;
    tx[1] = reg;

    if (spi_write_then_read(&dev->cmd, tx, sizeof(tx), rx, sizeof(rx)) != 0)
    {
        return VS1053_ERR_IO;
    }

    *val = ((uint16_t)rx[0] << 8) | rx[1];
    return VS1053_OK;
}

static int vs1053_write_ram(struct vs1053 *dev, uint16_t addr, uint16_t val)
{
    int ret = vs1053_write_reg(dev, VS1053_REG_WRAMADDR, addr);

    if (ret != VS1053_OK)
    {
        return ret;
    }
    return vs1053_write_reg(dev, VS1053_REG_WRAM, val);
}

/* ============================ 数据通道 ============================ */

int vs1053_write_data(struct vs1053 *dev, const void *buf, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    if ((dev == NULL) || (buf == NULL) || (dev->data.ctlr == NULL))
    {
        return VS1053_ERR_PARAM;
    }

    while (len > 0u)
    {
        uint16_t n = (len > 32u) ? 32u : len;
        int ret = vs1053_wait_dreq(dev, VS1053_DREQ_TIMEOUT_MS);

        if (ret != VS1053_OK)
        {
            return ret;
        }
        if (spi_write(&dev->data, p, n) != 0)
        {
            return VS1053_ERR_IO;
        }

        dev->started = 1u;
        p += n;
        len = (uint16_t)(len - n);
    }
    return VS1053_OK;
}

/* ============================ 复位 ============================ */

static int vs1053_hard_reset(struct vs1053 *dev)
{
    gpio_write(VS1053_RST_PIN, 0);          /* 进复位 */
    osal_thread_sleep_ms(20);
    gpio_write(VS1053_RST_PIN, 1);          /* 释放复位 */
    osal_thread_sleep_ms(20);

    if (vs1053_wait_dreq(dev, 300u) != VS1053_OK)
    {
        return VS1053_ERR_TIMEOUT;
    }
    return VS1053_OK;
}

static int vs1053_soft_reset(struct vs1053 *dev)
{
    uint16_t v = 0;
    int i;

    /* SM_RESET | SM_SDINEW，等芯片清掉 SM_RESET（读回 0x0800 左右）。
     * 每次写之间至少 1.35ms，这里给 2ms。 */
    for (i = 0; i < 60; i++)
    {
        if (vs1053_write_reg(dev, VS1053_REG_MODE,
                             VS1053_SM_RESET | VS1053_SM_SDINEW) != VS1053_OK)
        {
            return VS1053_ERR_IO;
        }
        osal_thread_sleep_ms(2);

        if ((vs1053_read_reg(dev, VS1053_REG_MODE, &v) == VS1053_OK) &&
            ((v & VS1053_SM_SDINEW) != 0u) &&
            ((v & VS1053_SM_RESET) == 0u))
        {
            break;
        }
    }
    if (i >= 60)
    {
        return VS1053_ERR_TIMEOUT;
    }

    /* 3 倍频 + 1.5xADD（12.288MHz 晶振 → 内部 55.296MHz） */
    for (i = 0; i < 20; i++)
    {
        if (vs1053_write_reg(dev, VS1053_REG_CLOCKF, 0x9800u) != VS1053_OK)
        {
            return VS1053_ERR_IO;
        }
        if ((vs1053_read_reg(dev, VS1053_REG_CLOCKF, &v) == VS1053_OK) &&
            (v == 0x9800u))
        {
            break;
        }
    }
    osal_thread_sleep_ms(10);
    return VS1053_OK;
}

/* ============================ 音量 / 音调 / 喇叭 ============================ */

int vs1053_set_volume(struct vs1053 *dev, int vol)
{
    uint16_t raw;
    uint16_t reg;

    if (dev == NULL)
    {
        return VS1053_ERR_PARAM;
    }

    if (vol < 0)
    {
        vol = 0;
    }
    if (vol > 100)
    {
        vol = 100;
    }

    /* 用户 0~100 → VS1053 衰减档 0~250（值越大越响，寄存器 254-raw） */
    raw = (uint16_t)((vol * 250) / 100);
    reg = (uint16_t)(((254u - raw) << 8) | (254u - raw));

    dev->volume = (uint8_t)vol;
    return vs1053_write_reg(dev, VS1053_REG_VOL, reg);
}

int vs1053_get_volume(struct vs1053 *dev)
{
    return (dev != NULL) ? (int)dev->volume : -1;
}

int vs1053_set_speaker(struct vs1053 *dev, int on)
{
    int ret;

    /* VS1053 的 GPIO4 控制板载 HT6872 功放：先设方向，再设电平 */
    ret = vs1053_write_ram(dev, VS1053_GPIO_DDR, 1u << 4);
    if (ret != VS1053_OK)
    {
        return ret;
    }
    return vs1053_write_ram(dev, VS1053_GPIO_ODATA, (uint16_t)((on != 0) ? (1u << 4) : 0u));
}

int vs1053_set_tone(struct vs1053 *dev, uint8_t bfreq, uint8_t bass,
                    uint8_t tfreq, uint8_t treble)
{
    uint16_t v;
    int8_t t = (int8_t)treble - 8;      /* 0~15 → -8~7 */

    v  = (uint16_t)((uint8_t)t & 0x0Fu);    /* 高音增益 */
    v <<= 4;
    v |= (uint16_t)(tfreq & 0x0Fu);         /* 高音下限频率 */
    v <<= 4;
    v |= (uint16_t)(bass & 0x0Fu);          /* 低音增益 */
    v <<= 4;
    v |= (uint16_t)(bfreq & 0x0Fu);         /* 低音上限频率 */

    return vs1053_write_reg(dev, VS1053_REG_BASS, v);
}

/* ============================ 初始化 ============================ */

int vs1053_bind(struct vs1053 *dev, struct spi_controller *ctlr)
{
    if ((dev == NULL) || (ctlr == NULL))
    {
        return VS1053_ERR_PARAM;
    }

    dev->cmd.ctlr = ctlr;
    dev->cmd.max_speed_hz = VS1053_SPI_CMD_HZ;
    dev->cmd.mode = SPI_MODE_3;
    dev->cmd.bits_per_word = 8;
    dev->cmd.cs_pin = VS1053_XCS_PIN;

    dev->data.ctlr = ctlr;
    dev->data.max_speed_hz = VS1053_SPI_DATA_HZ;
    dev->data.mode = SPI_MODE_3;
    dev->data.bits_per_word = 8;
    dev->data.cs_pin = VS1053_XDCS_PIN;

    dev->ready = 0;
    dev->started = 0;
    dev->volume = VS1053_DEFAULT_VOL;
    return VS1053_OK;
}

int vs1053_init(struct vs1053 *dev)
{
    int ret;

    if ((dev == NULL) || (dev->cmd.ctlr == NULL))
    {
        return VS1053_ERR_NODEV;
    }
    if (dev->ready != 0u)
    {
        return VS1053_OK;
    }

    /* 引脚：RST 输出（先拉低把芯片按住），DREQ 上拉输入。
     * XCS / XDCS 与 GPIO_PIN 编码相同，先配成输出高，避免第一次传输前浮空。 */
    gpio_write(VS1053_RST_PIN, 0);
    gpio_output_init(VS1053_RST_PIN);
    gpio_input_init(VS1053_DREQ_PIN, GPIO_IN_PULLUP);

    gpio_write(VS1053_XCS_PIN, 1);
    gpio_write(VS1053_XDCS_PIN, 1);
    gpio_output_init(VS1053_XCS_PIN);
    gpio_output_init(VS1053_XDCS_PIN);

    osal_thread_sleep_ms(20);

    ret = vs1053_hard_reset(dev);
    if (ret != VS1053_OK)
    {
        return ret;
    }

    ret = vs1053_soft_reset(dev);
    if (ret != VS1053_OK)
    {
        return ret;
    }

    /* 默认参数：音量、平直音调（实际用 6/15/10/15 的默认增强）、开喇叭 */
    (void)vs1053_set_volume(dev, VS1053_DEFAULT_VOL);
    (void)vs1053_set_tone(dev, 6u, 15u, 10u, 15u);
    (void)vs1053_set_speaker(dev, 1);

    dev->ready = 1u;
    return VS1053_OK;
}

/* ============================ 切歌 / 停止 ============================ */

int vs1053_cancel(struct vs1053 *dev)
{
    uint16_t mode = 0;
    uint8_t zero[32];
    int i;

    if ((dev == NULL) || (dev->ready == 0u))
    {
        return VS1053_ERR_NODEV;
    }
    if (dev->started == 0u)
    {
        return VS1053_OK;               /* 还没放过东西，不用清 */
    }

    memset(zero, 0, sizeof(zero));

    if (vs1053_read_reg(dev, VS1053_REG_MODE, &mode) != VS1053_OK)
    {
        return VS1053_ERR_IO;
    }
    mode |= VS1053_SM_CANCEL | 0x0002u; /* SM_CANCEL | SM_LAYER12 */
    if (vs1053_write_reg(dev, VS1053_REG_MODE, mode) != VS1053_OK)
    {
        return VS1053_ERR_IO;
    }

    /* 喂 0，直到 SM_CANCEL 被芯片清掉（最多 2KB） */
    for (i = 0; i < 64; i++)
    {
        if (vs1053_write_data(dev, zero, sizeof(zero)) != VS1053_OK)
        {
            break;
        }
        if ((vs1053_read_reg(dev, VS1053_REG_MODE, &mode) == VS1053_OK) &&
            ((mode & VS1053_SM_CANCEL) == 0u))
        {
            break;
        }
    }

    /* 再喂一点静音，把内部缓冲冲干净 */
    for (i = 0; i < 8; i++)
    {
        (void)vs1053_write_data(dev, zero, sizeof(zero));
    }

    dev->started = 0u;
    return vs1053_soft_reset(dev);
}

/* ============================ 状态 ============================ */

void vs1053_reset_decode_time(struct vs1053 *dev)
{
    if (dev == NULL)
    {
        return;
    }
    (void)vs1053_write_reg(dev, VS1053_REG_DECODE_TIME, 0);
    (void)vs1053_write_reg(dev, VS1053_REG_DECODE_TIME, 0);
}

uint16_t vs1053_decode_time(struct vs1053 *dev)
{
    uint16_t v = 0;

    if (vs1053_read_reg(dev, VS1053_REG_DECODE_TIME, &v) != VS1053_OK)
    {
        return 0;
    }
    return v;
}

/* MP3 码率表：head1 的版本/层决定用哪一行（与正点原子驱动一致） */
static const uint16_t s_bitrate[2][16] =
{
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
};

uint16_t vs1053_bitrate(struct vs1053 *dev)
{
    uint16_t h0 = 0;
    uint16_t h1 = 0;

    if ((vs1053_read_reg(dev, VS1053_REG_HDAT0, &h0) != VS1053_OK) ||
        (vs1053_read_reg(dev, VS1053_REG_HDAT1, &h1) != VS1053_OK))
    {
        return 0;
    }

    switch (h1)
    {
    case 0x4D54:    /* MIDI */
    case 0x4154:    /* AAC ADTS */
    case 0x4144:    /* AAC ADIF */
    case 0x4D34:    /* M4A */
    case 0x4F67:    /* OGG */
    case 0x574D:    /* WMA */
        return (uint16_t)(h0 / 125u);

    case 0x7665:    /* WAV */
        return (h0 == 0xFFFFu) ? 1411u : (uint16_t)((h0 * 8u) / 1000u);

    case 0x664C:    /* FLAC（经验公式） */
        return (uint16_t)((h0 * 32u) / 1000u);

    default:        /* MP3 */
        h1 >>= 3;
        h1 &= 0x03u;
        return s_bitrate[(h1 == 3u) ? 1u : 0u][h0 >> 12];
    }
}

/* ============================ 录音 ============================ */

/* VS1053 的 WAV 录音有 bug，这颗 patch 修正它（正点原子第 55 章原样搬过来）。
 * 格式是 [寄存器, 个数, 数据...]，个数最高位 = 1 表示 RLE。 */
static const uint16_t s_wav_plugin[40] =
{
    0x0007, 0x0001, 0x8010, 0x0006, 0x001c, 0x3e12, 0xb817, 0x3e14,
    0xf812, 0x3e01, 0xb811, 0x0007, 0x9717, 0x0020, 0xffd2, 0x0030,
    0x11d1, 0x3111, 0x8024, 0x3704, 0xc024, 0x3b81, 0x8024, 0x3101,
    0x8024, 0x3b81, 0x8024, 0x3f04, 0xc024, 0x2808, 0x4800, 0x36f1,
    0x9811, 0x0007, 0x0001, 0x8028, 0x0006, 0x0002, 0x2a00, 0x040e,
};

static void vs1053_load_patch(struct vs1053 *dev, const uint16_t *patch, uint16_t len)
{
    uint16_t i = 0;
    uint16_t addr;
    uint16_t n;
    uint16_t val;

    while ((i + 1u) < len)
    {
        addr = patch[i++];
        n = patch[i++];

        if ((n & 0x8000u) != 0u)          /* RLE：同一个值重复 n 次 */
        {
            n &= 0x7FFFu;
            if (i >= len)
            {
                break;
            }
            val = patch[i++];
            while (n-- != 0u)
            {
                (void)vs1053_write_reg(dev, (uint8_t)addr, val);
            }
        }
        else                              /* 连续 n 个值 */
        {
            while (n-- != 0u)
            {
                if (i >= len)
                {
                    return;
                }
                val = patch[i++];
                (void)vs1053_write_reg(dev, (uint8_t)addr, val);
            }
        }
    }
}

int vs1053_record_start(struct vs1053 *dev, uint32_t sample_rate, uint16_t gain)
{
    int ret;

    if ((dev == NULL) || (dev->cmd.ctlr == NULL))
    {
        return VS1053_ERR_NODEV;
    }
    if (dev->ready == 0u)
    {
        ret = vs1053_init(dev);
        if (ret != VS1053_OK)
        {
            return ret;
        }
    }
    if (dev->started != 0u)
    {
        (void)vs1053_cancel(dev);         /* 正在放歌：先停干净 */
    }

    (void)vs1053_set_speaker(dev, 0);     /* 录音时关掉功放，避免啸叫 */

    /* 线性 PCM、MIC 输入、左声道单声道（与第 55 章一致） */
    ret = vs1053_write_reg(dev, VS1053_REG_BASS, 0x0000u);
    if (ret == VS1053_OK) ret = vs1053_write_reg(dev, VS1053_REG_AICTRL0, (uint16_t)sample_rate);
    if (ret == VS1053_OK) ret = vs1053_write_reg(dev, VS1053_REG_AICTRL1, gain);
    if (ret == VS1053_OK) ret = vs1053_write_reg(dev, VS1053_REG_AICTRL2, 0x0000u);
    if (ret == VS1053_OK) ret = vs1053_write_reg(dev, VS1053_REG_AICTRL3, 6u);
    if (ret == VS1053_OK) ret = vs1053_write_reg(dev, VS1053_REG_CLOCKF, 0x2000u);
    if (ret == VS1053_OK)
    {
        /* SM_ADPCM=1：进入录音；SM_RESET 必须一起写 */
        ret = vs1053_write_reg(dev, VS1053_REG_MODE,
                               VS1053_SM_RESET | VS1053_SM_SDINEW | VS1053_SM_ADPCM);
    }
    if (ret != VS1053_OK)
    {
        return ret;
    }

    osal_thread_sleep_ms(5);              /* 至少 1.35ms */
    vs1053_load_patch(dev, s_wav_plugin, (uint16_t)(sizeof(s_wav_plugin) / sizeof(s_wav_plugin[0])));

    dev->recording = 1u;
    dev->started = 0u;
    return VS1053_OK;
}

int vs1053_record_stop(struct vs1053 *dev)
{
    uint8_t volume;
    int ret;

    if (dev == NULL)
    {
        return VS1053_ERR_PARAM;
    }
    if (dev->recording == 0u)
    {
        return VS1053_OK;
    }

    volume = dev->volume;               /* 记住用户音量，init 会重置成默认 80 */

    /* 退出录音：写回普通模式并软复位；随后重新 init 恢复播放侧的时钟/音量 */
    (void)vs1053_write_reg(dev, VS1053_REG_MODE,
                           VS1053_SM_RESET | VS1053_SM_SDINEW);
    osal_thread_sleep_ms(5);

    dev->recording = 0u;
    dev->started = 0u;
    dev->ready = 0u;
    ret = vs1053_init(dev);
    if (ret == VS1053_OK)
    {
        (void)vs1053_set_volume(dev, volume);
    }
    return ret;
}

int vs1053_record_read(struct vs1053 *dev, void *buf, uint16_t len)
{
    uint8_t *p = (uint8_t *)buf;
    uint16_t words;
    uint16_t i;
    uint16_t w;
    uint32_t waited = 0;
    int level;

    if ((dev == NULL) || (buf == NULL) || (len == 0u) || ((len & 1u) != 0u))
    {
        return VS1053_ERR_PARAM;
    }
    if (dev->recording == 0u)
    {
        return VS1053_ERR_NODEV;
    }

    words = (uint16_t)(len / 2u);

    for (;;)
    {
        level = vs1053_record_level(dev);
        if (level < 0)
        {
            return VS1053_ERR_IO;
        }
        if ((uint16_t)level >= words)
        {
            break;
        }
        if (waited >= VS1053_DREQ_TIMEOUT_MS)
        {
            return VS1053_ERR_TIMEOUT;
        }
        osal_thread_sleep_ms(1);
        waited++;
    }

    /* HDAT0 是 16 位 FIFO：低字节先出，WAV 的 PCM 是小端 */
    for (i = 0; i < words; i++)
    {
        if (vs1053_read_reg(dev, VS1053_REG_HDAT0, &w) != VS1053_OK)
        {
            return VS1053_ERR_IO;
        }
        p[(2u * i)] = (uint8_t)(w & 0xFFu);
        p[(2u * i) + 1u] = (uint8_t)(w >> 8);
    }
    return VS1053_OK;
}

int vs1053_record_level(struct vs1053 *dev)
{
    uint16_t v = 0;

    if ((dev == NULL) || (dev->recording == 0u))
    {
        return -1;
    }
    if (vs1053_read_reg(dev, VS1053_REG_HDAT1, &v) != VS1053_OK)
    {
        return -1;
    }
    return (int)v;
}

/* ============================ 正弦测试 ============================ */

static int vs1053_sine_enter(struct vs1053 *dev, uint8_t n)
{
    static const uint8_t head[4] = { 0x53u, 0xEFu, 0x6Eu, 0x00u };
    uint8_t cmd[8];

    memcpy(cmd, head, sizeof(head));
    cmd[3] = n;
    cmd[4] = 0;
    cmd[5] = 0;
    cmd[6] = 0;
    cmd[7] = 0;

    return vs1053_write_data(dev, cmd, sizeof(cmd));
}

static int vs1053_sine_exit(struct vs1053 *dev)
{
    static const uint8_t cmd[8] = { 0x45u, 0x78u, 0x69u, 0x74u, 0, 0, 0, 0 };

    return vs1053_write_data(dev, cmd, sizeof(cmd));
}

int vs1053_sine_test(struct vs1053 *dev)
{
    int ret;

    if ((dev == NULL) || (dev->cmd.ctlr == NULL))
    {
        return VS1053_ERR_NODEV;
    }

    if (dev->ready == 0u)
    {
        ret = vs1053_init(dev);
        if (ret != VS1053_OK)
        {
            return ret;
        }
    }

    dev->started = 0u;                      /* 测试不算播放 */

    if (vs1053_hard_reset(dev) != VS1053_OK)
    {
        return VS1053_ERR_TIMEOUT;
    }
    if (vs1053_write_reg(dev, VS1053_REG_VOL, 0x2020u) != VS1053_OK)
    {
        return VS1053_ERR_IO;
    }
    if (vs1053_write_reg(dev, VS1053_REG_MODE,
                         VS1053_SM_SDINEW | VS1053_SM_TESTS) != VS1053_OK)
    {
        return VS1053_ERR_IO;
    }

    if (vs1053_wait_dreq(dev, 300u) != VS1053_OK)
    {
        return VS1053_ERR_TIMEOUT;
    }

    /* 两个频率各响 100ms，确认喇叭/功放通路 */
    (void)vs1053_sine_enter(dev, 0x24u);
    osal_thread_sleep_ms(100);
    (void)vs1053_sine_exit(dev);
    osal_thread_sleep_ms(100);

    (void)vs1053_sine_enter(dev, 0x44u);
    osal_thread_sleep_ms(100);
    (void)vs1053_sine_exit(dev);
    osal_thread_sleep_ms(100);

    /* 测试模式退出后重新初始化解码器 */
    dev->ready = 0u;
    dev->started = 0u;
    ret = vs1053_init(dev);
    return ret;
}

/* ============================ 默认实例 ============================ */

void vs1053_set_default(struct vs1053 *dev)
{
    s_default = dev;
}

struct vs1053 *vs1053_get_default(void)
{
    return s_default;
}
