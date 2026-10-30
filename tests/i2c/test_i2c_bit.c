/*
 * test_i2c_bit.c - i2c_bit 时序状态机 + i2c_core 寄存器读写的 host 单测
 *
 * 为什么要有这一层测试：I2C 的错误大多不体现在返回值上。之前踩过的坑就是
 * i2c_bit_xfer() 对 I2C_M_NOSTART 的 msg 仍然发了一次地址字节，于是
 * 「字地址 + 数据」被整体错位一格、末尾丢一个字节 —— 写调用照样返回成功，
 * 只有把数据读回来才发现全歪了。这里用「假总线 + 假从机」把线上的电平
 * 解成字节序列，直接断言总线上一共发了什么，这类问题就再也跑不掉。
 *
 * 被测代码是真实的 i2c_bit.c / i2c_core.c（不 stub），只把引脚原语换成
 * 一个会记录电平的假实现；延时回调置空，host 上直接跑。
 */
#include <stdio.h>
#include <string.h>

#include "i2c.h"

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
        {                                                                 \
            printf("FAIL %d  %s\n", __LINE__, #cond);                     \
            failures++;                                                   \
        }                                                                 \
    } while (0)

/* ===========================================================================
 * 假总线：线电平 = 主机输出 AND 从机输出（I2C 是开漏 + 上拉，线与）
 * 主机侧只需 set_sda/set_scl；从机侧在这里实现 ACK 与读数据驱动。
 * ===========================================================================*/
#define TEST_ADDR       0x50u
#define WIRE_MAX        64

static int m_sda = 1;               /* 主机输出：1 = 释放（靠上拉变高） */
static int m_scl = 1;
static int s_sda = 1;               /* 从机输出：1 = 释放 */

static int s_present = 1;           /* 0 = 器件不存在（任何地址都 NACK） */

/* 从机侧状态机 */
static int s_expect_addr;           /* 下一个字节是地址字节 */
static int s_reading;               /* 当前事务是读方向（地址 R 位 = 1） */
static int s_bit_idx;               /* 当前字节已收/发几位（0..8，8 = 进入 ACK 槽） */
static int s_shift;
static int s_txpos;
static uint8_t s_tx[16];            /* 从机要吐出的数据 */
static int s_txn;

/* 线上解码结果（断言用） */
static uint8_t w_bytes[WIRE_MAX];   /* 每个字节的 8 位数据 */
static int w_acks[WIRE_MAX];        /* 每个字节第 9 拍的电平：0 = ACK，1 = NACK */
static int w_n;
static int w_ackn;
static int w_starts;
static int w_stops;

static int line_sda(void)
{
    return (m_sda && s_sda) ? 1 : 0;
}

static void bus_reset(void)
{
    m_sda = 1;
    m_scl = 1;
    s_sda = 1;

    s_expect_addr = 0;
    s_reading = 0;
    s_bit_idx = 0;
    s_shift = 0;
    s_txpos = 0;
    s_txn = 0;

    w_n = 0;
    w_ackn = 0;
    w_starts = 0;
    w_stops = 0;
}

/* SCL 上升沿 = 采样一拍 */
static void slave_on_rise(void)
{
    int bit = line_sda();

    if (s_bit_idx < 8)
    {
        s_shift = (s_shift << 1) | bit;
        s_bit_idx++;
        return;
    }

    /* 第 9 拍：ACK / NACK */
    if (w_n < WIRE_MAX)
    {
        w_bytes[w_n++] = (uint8_t)(s_shift & 0xFF);
    }
    if (w_ackn < WIRE_MAX)
    {
        w_acks[w_ackn++] = bit;
    }

    if (s_expect_addr != 0)
    {
        s_expect_addr = 0;
        s_reading = (s_shift & 0x01) ? 1 : 0;   /* bit0 = R/W */
        s_txpos = 0;
    }
    else if (s_reading != 0)
    {
        s_txpos++;
    }

    s_bit_idx = 0;
    s_shift = 0;
}

/* SCL 下降沿 = 从机准备下一拍要驱动的电平 */
static void slave_on_fall(void)
{
    if (s_reading != 0)
    {
        if (s_bit_idx < 8)
        {
            uint8_t v = (s_txpos < s_txn) ? s_tx[s_txpos] : 0xFFu;
            s_sda = (v >> (7 - s_bit_idx)) & 0x01;
        }
        else
        {
            s_sda = 1;                  /* ACK 槽让给主机驱动 */
        }
        return;
    }

    if (s_present == 0)
    {
        s_sda = 1;                      /* 器件不存在：永远不回 ACK */
    }
    else if (s_bit_idx == 8)
    {
        s_sda = 0;                      /* 拉低 = ACK */
    }
    else
    {
        s_sda = 1;
    }
}

/* ---- 引脚原语（i2c_bit_ops 回调） ---- */
static void fake_set_sda(void *ctx, int level)
{
    (void)ctx;

    if (m_scl != 0)
    {
        /* SCL 高电平期间 SDA 跳变 = START / STOP */
        if (m_sda != 0 && level == 0)
        {
            w_starts++;
            s_expect_addr = 1;
            s_bit_idx = 0;
            s_shift = 0;
            s_txpos = 0;
        }
        else if (m_sda == 0 && level != 0)
        {
            w_stops++;
        }
    }
    m_sda = (level != 0) ? 1 : 0;
}

static void fake_set_scl(void *ctx, int level)
{
    int old = m_scl;

    (void)ctx;
    m_scl = (level != 0) ? 1 : 0;

    if (old == 0 && m_scl != 0)
    {
        slave_on_rise();
    }
    else if (old != 0 && m_scl == 0)
    {
        slave_on_fall();
    }
}

static int fake_get_sda(void *ctx)
{
    (void)ctx;
    return line_sda();
}

static int fake_get_scl(void *ctx)
{
    (void)ctx;
    return m_scl;
}

static const struct i2c_bit_ops s_ops = {
    .ctx                = 0,
    .set_sda            = fake_set_sda,
    .set_scl            = fake_set_scl,
    .get_sda            = fake_get_sda,
    .get_scl            = fake_get_scl,
    .delay_us           = 0,        /* 空延时：host 上不等 */
    .half_period_us     = 0,
    .stretch_timeout_us = 0,
};

static struct i2c_adapter s_adap = {
    .name      = "fake",
    .algo      = &i2c_bit_algorithm,
    .algo_data = (void *)&s_ops,
    .retries   = 0,                 /* 测试里不重试，失败要立刻可见 */
};

/* ============================ 用例 ============================ */

/* 寄存器写：线上一共只能是 地址W + 字地址 + 数据，且 ACK 全部为 0。
 * 这条用例就是给「NOSTART 又多发一次地址」那个 bug 钉的钉子。 */
static void test_reg_write(void)
{
    const uint8_t data[3] = { 0x11u, 0x22u, 0x33u };
    int ret;

    bus_reset();
    s_present = 1;
    ret = i2c_reg_write(&s_adap, TEST_ADDR, 0x05u, data, sizeof(data));

    CHECK(ret == 0);
    CHECK(w_starts == 1);
    CHECK(w_stops == 1);
    CHECK(w_n == 5);                            /* 1 地址 + 1 字地址 + 3 数据 */
    CHECK(w_n == 5 && w_bytes[0] == 0xA0u);     /* 0x50 << 1 | W */
    CHECK(w_n == 5 && w_bytes[1] == 0x05u);
    CHECK(w_n == 5 && memcmp(&w_bytes[2], data, sizeof(data)) == 0);
    CHECK(w_ackn == 5 && w_acks[0] == 0 && w_acks[4] == 0);
}

/* 跨页写（一次性 8 字节，模拟 AT24C02 的页内写）：仍然只有一次 START */
static void test_reg_write_block(void)
{
    const uint8_t data[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    int ret;

    bus_reset();
    s_present = 1;
    ret = i2c_reg_write(&s_adap, TEST_ADDR, 0x00u, data, sizeof(data));

    CHECK(ret == 0);
    CHECK(w_starts == 1 && w_stops == 1);
    CHECK(w_n == 10);
    CHECK(w_n == 10 && w_bytes[0] == 0xA0u && w_bytes[1] == 0x00u);
    CHECK(w_n == 10 && memcmp(&w_bytes[2], data, sizeof(data)) == 0);
}

/* 寄存器读：写地址（不发 STOP）→ 重复 START + 地址R → 读数据，末字节回 NACK */
static void test_reg_read(void)
{
    uint8_t buf[2] = { 0u, 0u };
    int ret;

    bus_reset();
    s_present = 1;
    s_tx[0] = 0xDEu;
    s_tx[1] = 0xADu;
    s_txn = 2;

    ret = i2c_reg_read(&s_adap, TEST_ADDR, 0x11u, buf, sizeof(buf));

    CHECK(ret == (int)sizeof(buf));
    CHECK(buf[0] == 0xDEu && buf[1] == 0xADu);
    CHECK(w_starts == 2);                       /* START + 重复 START */
    CHECK(w_stops == 1);
    CHECK(w_n == 5);                            /* 地址W 字地址 地址R 数据 数据 */
    CHECK(w_n == 5 && w_bytes[0] == 0xA0u);
    CHECK(w_n == 5 && w_bytes[1] == 0x11u);
    CHECK(w_n == 5 && w_bytes[2] == 0xA1u);     /* 0x50 << 1 | R */
    /* 最后一个字节必须 NACK(1)，前一个必须 ACK(0) */
    CHECK(w_ackn == 5 && w_acks[3] == 0 && w_acks[4] == 1);
}

/* 地址探测：只发一个读方向地址字节 + STOP */
static void test_probe(void)
{
    bus_reset();
    s_present = 1;

    CHECK(i2c_probe_device(&s_adap, TEST_ADDR) == 0);
    CHECK(w_starts == 1 && w_stops == 1);
    CHECK(w_n == 1 && w_bytes[0] == 0xA1u);
    CHECK(w_ackn == 1 && w_acks[0] == 0);
}

/* 器件不存在：必须报错，而且要把总线放回空闲（否则后续传输全废） */
static void test_nack_releases_bus(void)
{
    const uint8_t b = 0x5Au;
    int ret;

    bus_reset();
    s_present = 0;

    ret = i2c_write(&s_adap, TEST_ADDR, &b, 1u);

    CHECK(ret < 0);
    CHECK(m_scl == 1);              /* SCL 已释放 */
    CHECK(m_sda == 1);              /* SDA 已释放（STOP 收尾） */
    CHECK(w_stops == 1);

    s_present = 1;
}

int main(void)
{
    /* i2c_core 的总线锁按 adapter 编号索引，所以先把假 adapter 注册进来
     * （正常运行时这一步由 stm32f1_i2c_gpio_init() 在板级完成） */
    CHECK(i2c_add_adapter(&s_adap) == 0);

    test_reg_write();
    test_reg_write_block();
    test_reg_read();
    test_probe();
    test_nack_releases_bus();

    if (failures == 0)
    {
        printf("PASS: i2c_bit 时序与 i2c_core 寄存器读写全部断言通过"
               "（NOSTART 不重发地址、重复 START、末字节 NACK、NACK 后释放总线）\n");
        return 0;
    }

    printf("FAIL: %d 处断言失败\n", failures);
    return 1;
}
