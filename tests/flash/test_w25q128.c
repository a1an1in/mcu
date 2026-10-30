/*
 * test_w25q128.c - drivers/flash 的 host 单元测试
 *
 * 被测对象是真实的 w25q128.c 与 spi_core.c，底座是 fake_w25q.c 里的假 25Q：
 * 16MB NOR 模型，实现了「写只能 1→0」「页编程跨页回卷」「擦除变 0xFF」
 * 「写/擦前必须有 WRITE_ENABLE」「BUSY 要轮询几次才落」。
 *
 * 重点覆盖三类真机上很难看清的错误：
 *   1) 页编程跨页回卷 —— 从 0x0F5 写 20 字节，不切页的话后 9 字节会落回
 *      页首，把 0x000 那里的数据覆盖掉；
 *   2) 擦除粒度 —— 读-改-写必须保住同扇区里没动过的字节；
 *   3) 时序 —— 写使能有没有发、BUSY 有没有等。
 */
#include <stdio.h>
#include <string.h>

#include "spi.h"
#include "w25q128.h"

/* ---------------- fake_w25q.c 提供的接口 ---------------- */
struct spi_controller *fake_flash_controller(void);
void     fake_flash_reset(void);
void     fake_flash_set_id(uint8_t manuf, uint8_t type, uint8_t cap);
uint8_t *fake_flash_mem(void);
int      fake_flash_erase_ops(void);
int      fake_flash_program_ops(void);
int      fake_flash_sr1_reads(void);

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
        long _a = (long)(a);                                              \
        long _b = (long)(b);                                              \
        if (_a != _b)                                                     \
        {                                                                 \
            printf("FAIL %d  %s=%ld 期望 %ld\n", __LINE__, #a, _a, _b);   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static struct w25q128_dev dev;

static int all_ff(const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        if (p[i] != 0xFFu)
        {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    uint8_t buf[256];
    uint8_t marker[8];
    static uint8_t scratch[W25Q_SECTOR_SIZE];   /* write_erase 用的 4KB 临时区 */
    int ret;
    int i;

    /* ---- 1) 初始化：只绑总线，不碰硬件 ---- */
    fake_flash_reset();
    CHECK_EQ(spi_add_controller(fake_flash_controller()), 0);

    CHECK_EQ(w25q128_init(&dev, fake_flash_controller(), SPI_CS_PIN(1u, 12u)), 0);
    CHECK(dev.spi.cs_pin == SPI_CS_PIN(1u, 12u));
    CHECK_EQ(dev.spi.mode, SPI_MODE_3);
    /* init 一个字节的总线操作都不能有：它跑在调度器起来之前，
     * 一旦加锁就会写坏线程控制块（板上表现为 IMPRECISERR 死机） */
    CHECK_EQ(dev.capacity, 0u);
    CHECK_EQ(fake_flash_sr1_reads(), 0);

    /* ---- 2) 探测：读 JEDEC ID、算容量 ---- */
    CHECK_EQ(w25q128_probe(&dev), 0);
    CHECK_EQ(dev.jedec, 0xEF4018);
    CHECK_EQ(dev.capacity, 16u * 1024u * 1024u);

    /* ---- 3) ID 不认识 / 不支持的容量都要拒绝 ---- */
    fake_flash_reset();
    fake_flash_set_id(0x00u, 0x00u, 0x00u);            /* 没接器件，MISO 全 0 */
    CHECK_EQ(w25q128_init(&dev, fake_flash_controller(), SPI_CS_PIN(1u, 12u)), 0);
    CHECK_EQ(w25q128_probe(&dev), W25Q_ERR_ID);

    fake_flash_reset();
    fake_flash_set_id(0xEFu, 0x40u, 0x19u);            /* 32MB：要 4 字节地址 */
    CHECK_EQ(w25q128_init(&dev, fake_flash_controller(), SPI_CS_PIN(1u, 12u)), 0);
    CHECK_EQ(w25q128_probe(&dev), W25Q_ERR_ID);

    /* 恢复正常片子 */
    fake_flash_reset();
    CHECK_EQ(w25q128_init(&dev, fake_flash_controller(), SPI_CS_PIN(1u, 12u)), 0);
    CHECK_EQ(w25q128_probe(&dev), 0);

    /* ---- 3) 擦除前读到的是 0xFF ---- */
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(w25q128_read(&dev, 0u, buf, 16u), 0);
    CHECK(all_ff(buf, 16));

    /* ---- 4) 扇区擦除 ---- */
    CHECK_EQ(w25q128_erase_sector(&dev, 0u), 0);
    CHECK_EQ(fake_flash_erase_ops(), 1);
    CHECK_EQ(w25q128_erase_sector(&dev, 0x100u), W25Q_ERR_ALIGN);   /* 没对齐 */
    CHECK_EQ(w25q128_erase_sector(&dev, dev.capacity), W25Q_ERR_RANGE);

    /* ---- 5) 页编程跨页：0x0F5 写 20 字节，不能回卷到 0x000 ---- */
    for (i = 0; i < 8; i++)
    {
        marker[i] = (uint8_t)(0xA0u + i);
    }
    CHECK_EQ(w25q128_write(&dev, 0u, marker, 8u), 0);

    for (i = 0; i < 20; i++)
    {
        buf[i] = (uint8_t)(0x10u + i);
    }
    CHECK_EQ(w25q128_write(&dev, 0xF5u, buf, 20u), 0);

    memset(buf, 0, sizeof(buf));
    CHECK_EQ(w25q128_read(&dev, 0u, buf, 8u), 0);
    CHECK(memcmp(buf, marker, 8) == 0);                 /* 页首没被回卷覆盖 */
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(w25q128_read(&dev, 0xF5u, buf, 20u), 0);
    for (i = 0; i < 20; i++)
    {
        CHECK_EQ(buf[i], (uint8_t)(0x10u + i));         /* 跨页的两段都写对了 */
    }

    /* ---- 6) 整页写 ---- */
    for (i = 0; i < 256; i++)
    {
        buf[i] = (uint8_t)(i ^ 0x5Au);
    }
    CHECK_EQ(w25q128_write(&dev, 0x200u, buf, 256u), 0);
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(w25q128_read(&dev, 0x200u, buf, 256u), 0);
    for (i = 0; i < 256; i++)
    {
        CHECK_EQ(buf[i], (uint8_t)(i ^ 0x5Au));
    }

    /* ---- 7) 越界检查 ---- */
    CHECK_EQ(w25q128_read(&dev, dev.capacity - 1u, buf, 2u), W25Q_ERR_RANGE);
    CHECK_EQ(w25q128_read(&dev, dev.capacity, buf, 1u), W25Q_ERR_RANGE);
    CHECK_EQ(w25q128_write(&dev, dev.capacity, buf, 1u), W25Q_ERR_RANGE);
    CHECK_EQ(w25q128_read(NULL, 0u, buf, 1u), SPI_ERR_PARAM);
    CHECK_EQ(w25q128_write(&dev, 0u, NULL, 1u), SPI_ERR_PARAM);

    /* ---- 8) 读-改-写：跨扇区，且保住没动过的字节 ---- */
    fake_flash_reset();
    CHECK_EQ(w25q128_init(&dev, fake_flash_controller(), SPI_CS_PIN(1u, 12u)), 0);

    /* 先在 0x4000（4 号扇区）和 0x5000（5 号扇区）各写一点东西 */
    for (i = 0; i < 8; i++)
    {
        marker[i] = (uint8_t)(0x30u + i);
    }
    CHECK_EQ(w25q128_write(&dev, 0x4000u, marker, 8u), 0);
    CHECK_EQ(w25q128_write(&dev, 0x5000u, marker, 8u), 0);

    /* 从 0x4FFE 写 4 字节：正好跨 4 号扇区末尾和 5 号扇区开头 */
    {
        const uint8_t cross[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };

        CHECK_EQ(w25q128_write_erase(&dev, 0x4FFEu, cross, 4u, scratch), 0);

        memset(buf, 0, sizeof(buf));
        CHECK_EQ(w25q128_read(&dev, 0x4FFEu, buf, 4u), 0);
        CHECK(memcmp(buf, cross, 4) == 0);

        /* 同扇区里原来那 8 个字节必须还在 */
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(w25q128_read(&dev, 0x4000u, buf, 8u), 0);
        CHECK(memcmp(buf, marker, 8) == 0);

        /* 5 号扇区的头 2 个字节被跨扇区写覆盖了，其余 6 个必须原样保留 */
        memset(buf, 0, sizeof(buf));
        CHECK_EQ(w25q128_read(&dev, 0x5000u, buf, 8u), 0);
        CHECK_EQ(buf[0], 0xBEu);
        CHECK_EQ(buf[1], 0xEFu);
        CHECK(memcmp(buf + 2, marker + 2, 6) == 0);
    }

    /* ---- 9) 内容没变时 write_erase 不该擦除（省寿命） ---- */
    {
        const uint8_t cross[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
        int before = fake_flash_erase_ops();

        CHECK_EQ(w25q128_write_erase(&dev, 0x4FFEu, cross, 4u, scratch), 0);
        CHECK_EQ(fake_flash_erase_ops(), before);
    }

    /* ---- 10) 写使能与 BUSY 轮询确实发生过 ---- */
    CHECK(fake_flash_program_ops() > 0);
    CHECK(fake_flash_sr1_reads() > 0);

    /* ---- 11) 原始 NOR 语义：没擦就写会与旧值相与 ---- */
    fake_flash_reset();
    CHECK_EQ(w25q128_init(&dev, fake_flash_controller(), SPI_CS_PIN(1u, 12u)), 0);
    {
        const uint8_t v1 = 0xF0u, v2 = 0x0Fu;

        CHECK_EQ(w25q128_write(&dev, 0u, &v1, 1u), 0);
        CHECK_EQ(w25q128_write(&dev, 0u, &v2, 1u), 0);   /* 不擦直接改 */
        memset(buf, 0, 1);
        CHECK_EQ(w25q128_read(&dev, 0u, buf, 1u), 0);
        CHECK_EQ(buf[0], 0x00u);                        /* 0xF0 & 0x0F = 0 */
    }

    /* ---- 12) 整扇区分块读：空扇区读出来必须全是 0xFF ----
     * 对应 main.c 里 flash_selftest() 的「扫空扇区」，防止分块读在某个
     * 边界上返回错数据（那会让自测误判「扇区非空」而白白跳过）。 */
    {
        uint8_t chunk[64];
        uint32_t base = dev.capacity - W25Q_SECTOR_SIZE;
        int off;
        int bad = -1;

        for (off = 0; off < (int)W25Q_SECTOR_SIZE; off += (int)sizeof(chunk))
        {
            CHECK_EQ(w25q128_read(&dev, base + (uint32_t)off, chunk, sizeof(chunk)), 0);
            for (i = 0; i < (int)sizeof(chunk); i++)
            {
                if (chunk[i] != 0xFFu)
                {
                    bad = off + i;
                    break;
                }
            }
            if (bad >= 0)
            {
                break;
            }
        }
        CHECK_EQ(bad, -1);
    }

    if (failures == 0)
    {
        printf("PASS: w25q128 全部断言通过"
               "（JEDEC/容量、扇区擦除、页编程跨页、整页写、越界、读-改-写跨扇区、"
               "写使能与 BUSY 轮询）\n");
        return 0;
    }

    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
