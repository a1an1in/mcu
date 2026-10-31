/*
 * vs1053.h - VS1053B 音频解码器接口 <drivers/audio/>
 *
 * 分层：
 *   vs1053.h / vs1053.c       本文件：SCI 命令、SDI 数据、音量/音调/测试
 *   vs1053_config.h           板级事实（SPI1 的 CS/DREQ/RST 引脚、速率）
 *   drivers/spi/              字节搬运与总线互斥（VS1053 与 CH395 共用 SPI1）
 *
 * 本层不碰寄存器、不依赖 OS，也不认识文件系统：调用方把音频数据按块喂进来。
 * 播放文件是 modules/music 的事（FatFs + 本驱动）。
 *
 * ── 用法 ─────────────────────────────────────────────────────────────
 *     struct vs1053 *dev = vs1053_get_default();
 *
 *     vs1053_init(dev);                       // 复位、软复位、设音量/喇叭
 *     vs1053_cancel(dev);                     // 切歌前清掉上一首（可省）
 *     vs1053_reset_decode_time(dev);
 *     while (read(buf, n) > 0) {              // 任意来源（文件/网络）
 *         vs1053_write_data(dev, buf, n);     // 内部按 32B 切、等 DREQ
 *         if (stop) break;
 *     }
 *     vs1053_cancel(dev);
 *
 * ── 时序要点 ─────────────────────────────────────────────────────────
 *   - SCI 命令（读/写寄存器）必须在 DREQ 为高时进行；
 *   - SDI 数据一次最多 32 字节，DREQ 低时必须停下等；
 *   - 复位后先等 DREQ 变高，再做软复位（SM_RESET），否则芯片不理你；
 *   - 切歌/停止用 SM_CANCEL + 喂 0，再软复位，能避免爆音。
 */
#ifndef DRIVERS_AUDIO_VS1053_H__
#define DRIVERS_AUDIO_VS1053_H__

#include <stdint.h>

#include "spi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vs1053;      /* 前向声明：默认实例接口在结构体定义之前 */

/* 返回值 */
#define VS1053_OK           0
#define VS1053_ERR_PARAM    (-1)
#define VS1053_ERR_IO       (-2)    /* SPI 传输失败 */
#define VS1053_ERR_TIMEOUT  (-3)    /* DREQ 一直不高等 */
#define VS1053_ERR_NODEV    (-4)    /* 没 bind / 没初始化 */

/* SCI 寄存器地址 */
#define VS1053_REG_MODE         0x00u
#define VS1053_REG_STATUS       0x01u
#define VS1053_REG_BASS         0x02u
#define VS1053_REG_CLOCKF       0x03u
#define VS1053_REG_DECODE_TIME  0x04u
#define VS1053_REG_AUDATA       0x05u
#define VS1053_REG_WRAM         0x06u
#define VS1053_REG_WRAMADDR     0x07u
#define VS1053_REG_HDAT0        0x08u
#define VS1053_REG_HDAT1        0x09u
#define VS1053_REG_AIADDR       0x0Au
#define VS1053_REG_VOL          0x0Bu
#define VS1053_REG_AICTRL0      0x0Cu
#define VS1053_REG_AICTRL1      0x0Du
#define VS1053_REG_AICTRL2      0x0Eu
#define VS1053_REG_AICTRL3      0x0Fu

/* MODE 位 */
#define VS1053_SM_RESET         0x0004u
#define VS1053_SM_CANCEL        0x0008u
#define VS1053_SM_TESTS         0x0020u
#define VS1053_SM_LINE_IN       0x0000u
#define VS1053_SM_SDINEW        0x0800u
#define VS1053_SM_ADPCM         0x1000u     /* 录音模式（0x1804 = RESET|SDINEW|ADPCM） */

/* 默认实例（board 里 bind 一次，msh / module 直接取） */
void vs1053_set_default(struct vs1053 *dev);
struct vs1053 *vs1053_get_default(void);

struct vs1053
{
    struct spi_device cmd;      /* 命令口：CS = XCS */
    struct spi_device data;     /* 数据口：CS = XDCS */
    uint8_t ready;              /* init 成功过 */
    uint8_t started;            /* 已经送过音频数据（决定要不要 cancel） */
    uint8_t recording;          /* 正在录音（SCI_MODE 已切到 ADPCM/PCM 模式） */
    uint8_t volume;             /* 0~100，用户音量 */
};

/* 绑定到一条 SPI 总线（只填结构体，不做 I/O，可在 board 初始化阶段调用）。 */
int vs1053_bind(struct vs1053 *dev, struct spi_controller *ctlr);

/* 上电初始化：配引脚、硬复位、软复位、设默认音量/音调、打开板载喇叭。
 * 会做 SPI 通信和毫秒延时，必须在调度器启动后调用。重复调用直接返回 0。 */
int vs1053_init(struct vs1053 *dev);

/* 读 / 写 SCI 寄存器（内部等 DREQ）。 */
int vs1053_write_reg(struct vs1053 *dev, uint8_t reg, uint16_t val);
int vs1053_read_reg(struct vs1053 *dev, uint8_t reg, uint16_t *val);

/* 音量 0~100（0 静音，100 最大）。内部映射到 VS1053 的 0~250 档。 */
int vs1053_set_volume(struct vs1053 *dev, int vol);
int vs1053_get_volume(struct vs1053 *dev);

/* 板载功放 HT6872 开关（VS1053 GPIO4）：on != 0 打开。 */
int vs1053_set_speaker(struct vs1053 *dev, int on);

/* 高低音（参数含义见 vs1053.c 注释）。默认 6/15/10/15。 */
int vs1053_set_tone(struct vs1053 *dev, uint8_t bfreq, uint8_t bass,
                    uint8_t tfreq, uint8_t treble);

/* 送一段音频数据（自动按 32 字节切块、等 DREQ）。返回 0 / VS1053_ERR_*。 */
int vs1053_write_data(struct vs1053 *dev, const void *buf, uint16_t len);

/* DREQ 是否已就绪（1 = 可以再送数据）。 */
int vs1053_ready(struct vs1053 *dev);

/* 停止当前解码：SM_CANCEL + 喂 0，最后软复位。切歌 / 停止时调用。 */
int vs1053_cancel(struct vs1053 *dev);

/* 解码时间（秒）与当前码率（kbps）。 */
uint16_t vs1053_decode_time(struct vs1053 *dev);
void vs1053_reset_decode_time(struct vs1053 *dev);
uint16_t vs1053_bitrate(struct vs1053 *dev);

/* ============================ 录音 ============================
 * 线性 PCM（16 位、单声道），采样率一般 8000。
 * gain: 0 = 自动 AGC；1~15 → 1024*gain（1024 = 1 倍）。
 * 录音期间输出被关闭；停止后会重新做一次完整初始化，恢复播放配置。
 */
int vs1053_record_start(struct vs1053 *dev, uint32_t sample_rate, uint16_t gain);
int vs1053_record_stop(struct vs1053 *dev);

/* 从录音 FIFO 读 len 字节（len 必须为偶数）。
 * 内部等 HDAT1 至少有 len/2 个 16 位字，超时返回 VS1053_ERR_TIMEOUT。 */
int vs1053_record_read(struct vs1053 *dev, void *buf, uint16_t len);

/* 录音 FIFO 里当前可读的 16 位字数（调试/状态用）。 */
int vs1053_record_level(struct vs1053 *dev);

/* 正弦测试（喇叭“嘀——”两声），用于确认硬件通路。会重置换芯片。 */
int vs1053_sine_test(struct vs1053 *dev);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_AUDIO_VS1053_H__ */
