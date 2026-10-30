/*
 * test_spi_core.c - drivers/spi 的 host 单元测试（只测 core，不碰寄存器）
 *
 * spi_core.c 不认识任何芯片：它只负责「一个 CS 时隙里按顺序跑完所有 transfer」。
 * 于是 host 上给一个假控制器（记录调用、按需填 rx、按需失败），就能把编排逻辑
 * 完整跑一遍：
 *
 *   - 注册表（重复注册 / 越界 / 非法算法）
 *   - spi_message_init / add_tail（含段数超限）
 *   - CS 时隙：一次消息只拉一次片选、退出必释放
 *   - cs_change 的「放 CS → 重新拉 CS」
 *   - tx / rx / 全双工 三种段
 *   - delay_usecs 交给控制器的 delay_us
 *   - 出错时：错误码透传、actual_length 停在出错前、CS 照样释放、锁照样解
 *
 * 引脚、时钟、寄存器都在 stm32f1_spi.c 里，属于上板验证的内容。
 */
#include <stdio.h>
#include <string.h>

#include "spi.h"

/* ---------------- 假控制器 ---------------- */

struct fake_state {
    int setup_calls;
    int transfer_calls;
    int cs_asserts;
    int cs_releases;
    int cs_level;
    int cs_log[16];
    int cs_log_len;
    unsigned int delay_us_total;
    unsigned int last_delay_us;
    unsigned int last_mode;
    unsigned int last_speed;
    unsigned char tx_log[64];
    int tx_log_len;
    int saw_tx_null;
    int fail_on_transfer;       /* >0：第 N 次 transfer_one 返回 SPI_ERR_TIMEOUT */
    int fail_setup;
};

static struct fake_state st;

static void st_reset(void)
{
    memset(&st, 0, sizeof(st));
    st.cs_level = -1;
}

static int fake_setup(struct spi_controller *ctlr, struct spi_device *spi)
{
    (void)ctlr;

    st.setup_calls++;
    st.last_mode = spi->mode;
    st.last_speed = spi->max_speed_hz;
    return (st.fail_setup != 0) ? SPI_ERR_IO : 0;
}

static int fake_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
                             struct spi_transfer *xfer)
{
    (void)ctlr;
    (void)spi;

    st.transfer_calls++;
    if ((st.fail_on_transfer > 0) && (st.transfer_calls >= st.fail_on_transfer))
    {
        return SPI_ERR_TIMEOUT;
    }

    if (xfer->tx_buf != NULL)
    {
        if ((st.tx_log_len + (int)xfer->len) <= (int)sizeof(st.tx_log))
        {
            memcpy(&st.tx_log[st.tx_log_len], xfer->tx_buf, xfer->len);
            st.tx_log_len += (int)xfer->len;
        }
    }
    else
    {
        st.saw_tx_null = 1;
    }

    if (xfer->rx_buf != NULL)
    {
        memset(xfer->rx_buf, 0x5A, xfer->len);
    }

    return 0;
}

static void fake_set_cs(struct spi_controller *ctlr, struct spi_device *spi, int active)
{
    (void)ctlr;
    (void)spi;

    if (active != 0)
    {
        st.cs_asserts++;
        st.cs_level = 1;
    }
    else
    {
        st.cs_releases++;
        st.cs_level = 0;
    }

    if (st.cs_log_len < (int)(sizeof(st.cs_log) / sizeof(st.cs_log[0])))
    {
        st.cs_log[st.cs_log_len++] = (active != 0) ? 1 : 0;
    }
}

static void fake_delay_us(void *ctx, unsigned int us)
{
    (void)ctx;

    st.delay_us_total += us;
    st.last_delay_us = us;
}

static const struct spi_algorithm fake_algo =
{
    .setup = fake_setup,
    .transfer_one = fake_transfer_one,
    .set_cs = fake_set_cs,
};

static struct spi_controller fake_ctlr =
{
    .name = "fake",
    .algo = &fake_algo,
    .algo_data = NULL,
    .max_speed_hz = 1000000u,
    .delay_us = fake_delay_us,
    .delay_ctx = NULL,
};

/* 第二个控制器，用来验证注册表 */
static struct spi_controller fake_ctlr2 =
{
    .name = "fake2",
    .algo = &fake_algo,
};

/* ---------------- 覆盖 OSAL 的计数接口（fake_osal.c） ---------------- */

void fake_osal_reset_counters(void);
int  fake_osal_create_count(void);
int  fake_osal_lock_count(void);
int  fake_osal_unlock_count(void);
void fake_osal_set_fail_lock(int fail);

/* ---------------- 断言 ---------------- */

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

/* 造一个设备：挂在 fake_ctlr 上，CS = PB12，mode 3，2MHz */
static struct spi_device make_dev(void)
{
    struct spi_device dev;

    memset(&dev, 0, sizeof(dev));
    dev.ctlr = &fake_ctlr;
    dev.max_speed_hz = 2000000u;
    dev.mode = SPI_MODE_3;
    dev.bits_per_word = 8u;
    dev.cs_pin = SPI_CS_PIN(1u, 12u);
    return dev;
}

int main(void)
{
    struct spi_device dev;
    struct spi_message msg;
    struct spi_transfer x[SPI_MSG_MAX_XFER + 1];
    unsigned char tx[4] = { 0x01u, 0x02u, 0x03u, 0x04u };
    unsigned char rx[4];
    int ret;

    /* ---- 1) 控制器注册 ---- */
    CHECK_EQ(spi_add_controller(NULL), SPI_ERR_PARAM);

    {
        /* 算法不全的一律拒绝 */
        static const struct spi_algorithm half = { .setup = fake_setup };
        static struct spi_controller bad = { .name = "bad", .algo = &half };

        CHECK_EQ(spi_add_controller(&bad), SPI_ERR_PARAM);
    }

    CHECK_EQ(spi_add_controller(&fake_ctlr), 0);
    CHECK_EQ(spi_add_controller(&fake_ctlr2), 1);
    CHECK_EQ(spi_add_controller(&fake_ctlr), 0);        /* 重复注册返回原编号 */
    CHECK(spi_get_controller(0) == &fake_ctlr);
    CHECK(spi_get_controller(1) == &fake_ctlr2);
    CHECK(spi_get_controller(2) == NULL);
    CHECK(spi_get_controller(SPI_MAX_CONTROLLERS) == NULL);
    CHECK_EQ(spi_controller_nr(&fake_ctlr2), 1);
    CHECK(spi_controller_nr(NULL) < 0);

    dev = make_dev();

    /* ---- 2) message 组装 ---- */
    {
        int i;

        spi_message_init(&msg);
        CHECK_EQ(msg.n, 0);
        CHECK_EQ(msg.actual_length, 0);
        CHECK_EQ(msg.status, 0);

        for (i = 0; i < (int)SPI_MSG_MAX_XFER; i++)
        {
            memset(&x[i], 0, sizeof(x[i]));
            CHECK_EQ(spi_message_add_tail(&x[i], &msg), 0);
            CHECK_EQ(msg.n, i + 1);
        }
        /* 再多一段就必须报错，不能静默丢 */
        memset(&x[SPI_MSG_MAX_XFER], 0, sizeof(x[0]));
        CHECK_EQ(spi_message_add_tail(&x[SPI_MSG_MAX_XFER], &msg), SPI_ERR_NOMEM);
        CHECK_EQ(spi_message_add_tail(NULL, &msg), SPI_ERR_PARAM);
        CHECK_EQ(msg.n, SPI_MSG_MAX_XFER);
    }

    /* ---- 3) 参数校验 ---- */
    spi_message_init(&msg);
    CHECK_EQ(spi_sync(NULL, &msg), SPI_ERR_PARAM);
    CHECK_EQ(spi_sync(&dev, NULL), SPI_ERR_PARAM);
    CHECK_EQ(spi_sync(&dev, &msg), SPI_ERR_PARAM);      /* 没有任何一段 */

    {
        struct spi_device orphan = dev;

        orphan.ctlr = NULL;
        CHECK_EQ(spi_sync(&orphan, &msg), SPI_ERR_PARAM);
    }

    CHECK_EQ(spi_write(&dev, NULL, 4), SPI_ERR_PARAM);
    CHECK_EQ(spi_write(&dev, tx, 0), SPI_ERR_PARAM);
    CHECK_EQ(spi_read(&dev, NULL, 4), SPI_ERR_PARAM);
    CHECK_EQ(spi_write_then_read(&dev, NULL, 0, NULL, 0), SPI_ERR_PARAM);

    /* ---- 4) spi_write：一段发、一次 CS 时隙 ---- */
    st_reset();
    fake_osal_reset_counters();

    CHECK_EQ(spi_write(&dev, tx, sizeof(tx)), 0);
    CHECK_EQ(st.setup_calls, 1);
    CHECK_EQ(st.last_mode, SPI_MODE_3);                 /* setup 拿到的是设备的 mode */
    CHECK_EQ(st.last_speed, 2000000u);
    CHECK_EQ(st.transfer_calls, 1);
    CHECK_EQ(st.tx_log_len, 4);
    CHECK(memcmp(st.tx_log, tx, 4) == 0);
    CHECK_EQ(st.cs_asserts, 1);                         /* 一次消息只拉一次片选 */
    CHECK_EQ(st.cs_releases, 1);
    CHECK_EQ(st.cs_level, 0);                           /* 退出时必须是释放态 */
    CHECK_EQ(fake_osal_lock_count(), 1);
    CHECK_EQ(fake_osal_unlock_count(), 1);

    /* ---- 5) spi_read：只收，tx_buf 为 NULL ---- */
    st_reset();
    memset(rx, 0, sizeof(rx));
    CHECK_EQ(spi_read(&dev, rx, sizeof(rx)), 0);
    CHECK_EQ(st.transfer_calls, 1);
    CHECK(st.saw_tx_null == 1);
    CHECK(rx[0] == 0x5A && rx[3] == 0x5A);
    CHECK_EQ(st.cs_asserts, 1);
    CHECK_EQ(st.cs_releases, 1);

    /* ---- 6) write_then_read：两段、同一个 CS 时隙 ---- */
    st_reset();
    memset(rx, 0, sizeof(rx));
    {
        unsigned char cmd = 0x9Fu;

        CHECK_EQ(spi_write_then_read(&dev, &cmd, 1, rx, 2), 0);
    }
    CHECK_EQ(st.transfer_calls, 2);
    CHECK_EQ(st.tx_log_len, 1);
    CHECK_EQ(st.tx_log[0], 0x9F);
    CHECK_EQ(st.cs_asserts, 1);                         /* ← 关键：CS 不能在两段之间释放 */
    CHECK_EQ(st.cs_releases, 1);
    CHECK(rx[0] == 0x5A && rx[1] == 0x5A);

    /* 只发 / 只收时 write_then_read 退化成单段 */
    st_reset();
    CHECK_EQ(spi_write_then_read(&dev, tx, 2, NULL, 0), 0);
    CHECK_EQ(st.transfer_calls, 1);
    st_reset();
    CHECK_EQ(spi_write_then_read(&dev, NULL, 0, rx, 2), 0);
    CHECK_EQ(st.transfer_calls, 1);
    CHECK(st.saw_tx_null == 1);

    /* ---- 7) delay_usecs 交给控制器的 delay_us ---- */
    st_reset();
    {
        unsigned char cmd = 0x03u;

        spi_message_init(&msg);
        memset(&x[0], 0, sizeof(x[0]));
        x[0].tx_buf = &cmd;
        x[0].len = 1;
        x[0].delay_usecs = 10;
        memset(&x[1], 0, sizeof(x[1]));
        x[1].rx_buf = rx;
        x[1].len = 2;

        CHECK_EQ(spi_message_add_tail(&x[0], &msg), 0);
        CHECK_EQ(spi_message_add_tail(&x[1], &msg), 0);
        CHECK_EQ(spi_sync(&dev, &msg), 0);
    }
    CHECK_EQ(st.delay_us_total, 10);
    CHECK_EQ(msg.actual_length, 3);
    CHECK_EQ(msg.status, 0);
    CHECK(msg.spi == &dev);

    /* ---- 8) cs_change：段之间放开再拉低 ---- */
    st_reset();
    {
        spi_message_init(&msg);
        memset(&x[0], 0, sizeof(x[0]));
        x[0].tx_buf = tx;
        x[0].len = 1;
        x[0].cs_change = 1;
        memset(&x[1], 0, sizeof(x[1]));
        x[1].tx_buf = tx + 1;
        x[1].len = 1;
        (void)spi_message_add_tail(&x[0], &msg);
        (void)spi_message_add_tail(&x[1], &msg);
        CHECK_EQ(spi_sync(&dev, &msg), 0);
    }
    /* 序列：拉低(1) → 第一段 → 放开(0) → 重新拉低(1) → 第二段 → 放开(0) */
    CHECK_EQ(st.cs_log_len, 4);
    CHECK_EQ(st.cs_log[0], 1);
    CHECK_EQ(st.cs_log[1], 0);
    CHECK_EQ(st.cs_log[2], 1);
    CHECK_EQ(st.cs_log[3], 0);

    /* ---- 9) 出错：错误码透传、CS 释放、锁释放、actual_length 停在出错前 ---- */
    st_reset();
    fake_osal_reset_counters();
    st.fail_on_transfer = 2;
    {
        unsigned char a = 0xAAu, b = 0xBBu;

        spi_message_init(&msg);
        memset(&x[0], 0, sizeof(x[0]));
        x[0].tx_buf = &a;
        x[0].len = 1;
        memset(&x[1], 0, sizeof(x[1]));
        x[1].tx_buf = &b;
        x[1].len = 1;
        (void)spi_message_add_tail(&x[0], &msg);
        (void)spi_message_add_tail(&x[1], &msg);

        ret = spi_sync(&dev, &msg);
        CHECK_EQ(ret, SPI_ERR_TIMEOUT);
        CHECK_EQ(msg.status, SPI_ERR_TIMEOUT);
        CHECK_EQ(msg.actual_length, 1);                 /* 只有第一段成功 */
        CHECK_EQ(st.cs_asserts, 1);
        CHECK_EQ(st.cs_releases, 1);                    /* 出错也必须放片选 */
        CHECK_EQ(fake_osal_unlock_count(), 1);                    /* 以及解锁 */
    }

    /* setup 失败：一段都不该跑，但锁要解 */
    st_reset();
    fake_osal_reset_counters();
    st.fail_setup = 1;
    CHECK_EQ(spi_write(&dev, tx, 1), SPI_ERR_IO);
    CHECK_EQ(st.transfer_calls, 0);
    CHECK_EQ(st.cs_asserts, 0);
    CHECK_EQ(fake_osal_unlock_count(), 1);

    /* 加锁失败：直接返回 BUSY，不碰硬件 */
    st_reset();
    fake_osal_reset_counters();
    fake_osal_set_fail_lock(1);
    CHECK_EQ(spi_write(&dev, tx, 1), SPI_ERR_BUSY);
    CHECK_EQ(st.transfer_calls, 0);
    CHECK_EQ(st.cs_asserts, 0);
    CHECK_EQ(fake_osal_unlock_count(), 0);
    fake_osal_set_fail_lock(0);

    /* ---- 10) cs_pin == SPI_CS_NONE 时完全不碰片选 ---- */
    st_reset();
    {
        struct spi_device no_cs = dev;

        no_cs.cs_pin = SPI_CS_NONE;
        CHECK_EQ(spi_write(&no_cs, tx, 1), 0);
        CHECK_EQ(st.cs_asserts, 0);
        CHECK_EQ(st.cs_releases, 0);
    }

    if (failures == 0)
    {
        printf("PASS: spi_core 全部断言通过"
               "（注册表、message 组装、CS 时隙、cs_change、delay、出错路径、锁平衡）\n");
        return 0;
    }

    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
