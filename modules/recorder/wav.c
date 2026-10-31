/*
 * wav.c - WAV(PCM) 文件头工具 <modules/recorder/>
 *
 * WAV 头是 44 字节的小端结构：
 *   0  "RIFF" | 4  file_size-8 | 8  "WAVE"
 *   12 "fmt " | 16 16 | 20 1(PCM) | 22 channels | 24 rate
 *   28 byte_rate | 32 block_align | 34 bits | 36 "data" | 40 data_size
 */
#include <string.h>

#include "wav.h"

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

void wav_header_init(uint8_t hdr[WAV_HEADER_SIZE], uint32_t sample_rate,
                     uint16_t channels, uint16_t bits_per_sample)
{
    uint16_t block_align;
    uint32_t byte_rate;

    if (hdr == NULL)
    {
        return;
    }

    block_align = (uint16_t)(channels * (bits_per_sample / 8u));
    byte_rate = sample_rate * (uint32_t)block_align;

    memcpy(&hdr[0], "RIFF", 4);
    put_le32(&hdr[4], 36u);                 /* 36 + data_size，先按 0 算 */
    memcpy(&hdr[8], "WAVE", 4);

    memcpy(&hdr[12], "fmt ", 4);
    put_le32(&hdr[16], 16u);                /* fmt chunk size */
    put_le16(&hdr[20], 1u);                 /* PCM */
    put_le16(&hdr[22], channels);
    put_le32(&hdr[24], sample_rate);
    put_le32(&hdr[28], byte_rate);
    put_le16(&hdr[32], block_align);
    put_le16(&hdr[34], bits_per_sample);

    memcpy(&hdr[36], "data", 4);
    put_le32(&hdr[40], 0u);                 /* 录音结束后回填 */
}

void wav_header_set_data_size(uint8_t hdr[WAV_HEADER_SIZE], uint32_t data_bytes)
{
    if (hdr == NULL)
    {
        return;
    }
    put_le32(&hdr[40], data_bytes);
    put_le32(&hdr[4], data_bytes + 36u);
}
