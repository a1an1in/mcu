/*
 * test_wav.c - WAV 头生成的 host 单测
 *
 * 录音写文件时先落 44 字节头、结束时回填两个长度字段；这两个环节错了
 * PC 上要么放不出声，要么时长显示成 0。这里把字节级布局钉住。
 */
#include <stdio.h>
#include <string.h>

#include "wav.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
        {                                                                 \
            printf("FAIL %d  %s\n", __LINE__, #cond);                     \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int main(void)
{
    uint8_t hdr[WAV_HEADER_SIZE];

    /* ---- 录音默认格式：8kHz / 16bit / 单声道 ---- */
    wav_header_init(hdr, 8000, 1, 16);

    CHECK(memcmp(&hdr[0], "RIFF", 4) == 0);
    CHECK(memcmp(&hdr[8], "WAVE", 4) == 0);
    CHECK(memcmp(&hdr[12], "fmt ", 4) == 0);
    CHECK(memcmp(&hdr[36], "data", 4) == 0);

    CHECK(le32(&hdr[4]) == 36);          /* 还没数据 */
    CHECK(le32(&hdr[16]) == 16);         /* fmt chunk */
    CHECK(le16(&hdr[20]) == 1);          /* PCM */
    CHECK(le16(&hdr[22]) == 1);          /* 单声道 */
    CHECK(le32(&hdr[24]) == 8000);       /* 采样率 */
    CHECK(le32(&hdr[28]) == 16000);      /* 字节率 = 8000*2 */
    CHECK(le16(&hdr[32]) == 2);          /* block align */
    CHECK(le16(&hdr[34]) == 16);         /* 位宽 */
    CHECK(le32(&hdr[40]) == 0);          /* data 长度 */

    /* ---- 回填 16000 字节：data=16000，RIFF=36+16000 ---- */
    wav_header_set_data_size(hdr, 16000);
    CHECK(le32(&hdr[40]) == 16000);
    CHECK(le32(&hdr[4]) == 16036);

    /* ---- 另一个格式：44.1kHz / 立体声 ---- */
    wav_header_init(hdr, 44100, 2, 16);
    CHECK(le16(&hdr[22]) == 2);
    CHECK(le32(&hdr[24]) == 44100);
    CHECK(le32(&hdr[28]) == 176400);     /* 44100 * 4 */
    CHECK(le16(&hdr[32]) == 4);

    /* ---- 空指针不崩 ---- */
    wav_header_init(NULL, 8000, 1, 16);
    wav_header_set_data_size(NULL, 1);

    if (failures == 0)
    {
        printf("PASS: WAV 头字段与长度回填全部正确\n");
        return 0;
    }
    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
