/*
 * test_sd_parse.c - SD 驱动的纯逻辑单测（CSD / CID 解析）
 *
 * SD 卡协议里最容易算错的是「从 CSD 反推容量」：CSD v1.0 要用
 * (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN，v2.0 是 (C_SIZE+1) * 512KB，
 * 字段还都是跨字节拼接的。这部分不碰硬件，正好用真实卡片的 CSD 抓包来钉住。
 *
 * 覆盖：
 *   - SDHC 8GB（CSD v2.0）容量与块大小
 *   - SDSC 1GB（CSD v1.0）容量与块大小
 *   - 不认识的 CSD 结构版本要报错
 *   - CID 的厂商/产品名/序列号/生产日期解析
 */
#include <stdio.h>
#include <string.h>

#include "sd.h"

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

int main(void)
{
    uint64_t capacity;
    uint32_t block_size;

    /* ---- 1) SDHC 8GB：CSD v2.0，C_SIZE = 0x3FFF ---- */
    {
        static const uint8_t csd[16] =
        {
            0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00, 0x00,
            0x3F, 0xFF, 0x80, 0x0A, 0x40, 0x00, 0x00, 0x00,
        };

        capacity = 0;
        block_size = 0;
        CHECK_EQ(sd_parse_csd(csd, &capacity, &block_size), 0);
        CHECK_EQ(capacity, 8ULL * 1024ULL * 1024ULL * 1024ULL);   /* 8GB */
        CHECK_EQ(block_size, 512);
    }

    /* ---- 2) SDSC 1GB：CSD v1.0，READ_BL_LEN=9、C_SIZE=0xFFF、C_SIZE_MULT=7 ---- */
    {
        static const uint8_t csd[16] =
        {
            0x00, 0x2D, 0x00, 0x32, 0x1B, 0x59, 0x03, 0xFF,
            0xC0, 0x03, 0x80, 0x0A, 0x40, 0x00, 0x00, 0x00,
        };

        capacity = 0;
        block_size = 0;
        CHECK_EQ(sd_parse_csd(csd, &capacity, &block_size), 0);
        CHECK_EQ(capacity, 1024ULL * 1024ULL * 1024ULL);          /* 1GB */
        CHECK_EQ(block_size, 512);
    }

    /* ---- 3) 不认识的 CSD 结构（v3.0，structure=2）要报错 ---- */
    {
        static const uint8_t csd[16] = { 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

        capacity = 0;
        block_size = 0;
        CHECK_EQ(sd_parse_csd(csd, &capacity, &block_size), SD_ERR_RESP);
    }

    /* ---- 4) 参数检查 ---- */
    CHECK_EQ(sd_parse_csd(NULL, &capacity, &block_size), SD_ERR_PARAM);
    CHECK_EQ(sd_parse_cid(NULL, NULL), SD_ERR_PARAM);

    /* ---- 5) CID：厂商 0x03 / OEM "SD" / 产品 "SU08G" / 2015-06 ---- */
    {
        static const uint8_t cid[16] =
        {
            0x03,               /* ManufacturerID */
            0x53, 0x44,         /* OEM: "SD" */
            'S', 'U', '0', '8', 'G',
            0x80,               /* 版本 */
            0x12, 0x34, 0x56, 0x78,
            0x00,               /* 年份高 4 位 */
            0xF6,               /* 年份低 4 位 = 0xF，月份 = 6 */
            0x5A,               /* CRC */
        };
        struct sd_cid info;

        memset(&info, 0, sizeof(info));
        CHECK_EQ(sd_parse_cid(cid, &info), 0);
        CHECK_EQ(info.manufacturer, 0x03);
        CHECK_EQ(info.oem, 0x5344);
        CHECK(strcmp(info.product, "SU08G") == 0);
        CHECK_EQ(info.revision, 0x80);
        CHECK_EQ(info.serial, 0x12345678u);
        CHECK_EQ(info.year, 2015);
        CHECK_EQ(info.month, 6);
    }

    if (failures == 0)
    {
        printf("PASS: SD 解析全部通过（CSD v1/v2 容量、坏结构拒绝、CID 字段）\n");
        return 0;
    }

    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
