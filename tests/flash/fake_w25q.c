/*
 * fake_w25q.c - host 单测用的 SPI NOR FLASH 假器件
 *
 * 提供两样东西：
 *   1) 一个假的 struct spi_controller（transfer_one / set_cs），按 25Q 的
 *      命令时序解析字节流；
 *   2) 一个 16MB 的 NOR 存储模型，刻意实现真实的 NOR 语义：
 *        - 写入只能 1→0（mem &= data）
 *        - 页编程跨页会回卷到页首（256 字节）
 *        - 扇区擦除变回 0xFF
 *        - 写/擦之前必须有 WRITE_ENABLE，否则忽略
 *        - 写/擦之后 BUSY 置位若干次轮询才落下
 *
 * 这些「不友好」的行为正是驱动最容易写错的地方：如果驱动不按页切分、
 * 不发写使能、不等 BUSY，测试会立刻失败。
 */
#include <string.h>

#include "spi.h"
#include "w25q128.h"    /* 只为 W25Q_SR1_BUSY / W25Q_SR1_WEL 两个状态位定义 */

#define FAKE_CAPACITY   (16u * 1024u * 1024u)
#define FAKE_PAGE_SIZE  256u
#define FAKE_SECTOR     4096u

static uint8_t s_mem[FAKE_CAPACITY];

/* ---------------- 器件状态 ---------------- */

enum
{
    ST_CMD = 0,
    ST_JEDEC,
    ST_SR1,
    ST_ADDR_RD,
    ST_ADDR_WR,
    ST_ADDR_ERASE,
    ST_READ,
    ST_WRITE,
    ST_DONE
};

static int s_state;
static uint32_t s_addr;
static int s_addr_cnt;
static uint32_t s_page_base;
static int s_jedec_idx;
static int s_wel;
static int s_busy_polls;
static uint8_t s_id[3] = { 0xEFu, 0x40u, 0x18u };   /* W25Q128 = 16MB */

/* 观测计数 */
static int s_erase_ops;
static int s_program_ops;
static int s_sr1_reads;

void fake_flash_reset(void)
{
    memset(s_mem, 0xFF, sizeof(s_mem));
    s_state = ST_CMD;
    s_addr = 0;
    s_addr_cnt = 0;
    s_page_base = 0;
    s_jedec_idx = 0;
    s_wel = 0;
    s_busy_polls = 0;
    s_id[0] = 0xEFu;
    s_id[1] = 0x40u;
    s_id[2] = 0x18u;
    s_erase_ops = 0;
    s_program_ops = 0;
    s_sr1_reads = 0;
}

void fake_flash_set_id(uint8_t manuf, uint8_t type, uint8_t cap)
{
    s_id[0] = manuf;
    s_id[1] = type;
    s_id[2] = cap;
}

uint8_t *fake_flash_mem(void)
{
    return s_mem;
}

int fake_flash_erase_ops(void)
{
    return s_erase_ops;
}

int fake_flash_program_ops(void)
{
    return s_program_ops;
}

int fake_flash_sr1_reads(void)
{
    return s_sr1_reads;
}

static uint8_t fake_sr1(void)
{
    s_sr1_reads++;

    /* 每次读都让 BUSY 计数减一：模拟「擦除要轮询几次才落」 */
    if (s_busy_polls > 0)
    {
        s_busy_polls--;
        return (uint8_t)(W25Q_SR1_BUSY | (s_wel ? W25Q_SR1_WEL : 0));
    }
    return (uint8_t)(s_wel ? W25Q_SR1_WEL : 0);
}

static void fake_erase_sector(uint32_t addr)
{
    uint32_t base = addr & ~(FAKE_SECTOR - 1u);

    if (s_wel == 0)
    {
        return;                 /* 没写使能，器件忽略（真实行为） */
    }

    memset(&s_mem[base], 0xFF, FAKE_SECTOR);
    s_wel = 0;
    s_busy_polls = 8;
    s_erase_ops++;
}

/* ---------------- 字节级状态机 ---------------- */

static uint8_t fake_io(uint8_t in)
{
    switch (s_state)
    {
    case ST_CMD:
        s_addr = 0;
        s_addr_cnt = 0;
        s_jedec_idx = 0;

        switch (in)
        {
        case 0x06u:                 /* WRITE ENABLE */
            s_wel = 1;
            s_state = ST_DONE;
            break;
        case 0x9Fu:                 /* JEDEC ID */
            s_state = ST_JEDEC;
            break;
        case 0x05u:                 /* READ SR1 */
            s_state = ST_SR1;
            break;
        case 0x03u:                 /* READ DATA */
            s_state = ST_ADDR_RD;
            break;
        case 0x02u:                 /* PAGE PROGRAM */
            s_state = ST_ADDR_WR;
            break;
        case 0x20u:                 /* SECTOR ERASE */
            s_state = ST_ADDR_ERASE;
            break;
        default:
            s_state = ST_DONE;      /* 不认识的命令：忽略后续字节 */
            break;
        }
        return 0xFFu;

    case ST_JEDEC:
        return (s_jedec_idx < 3) ? s_id[s_jedec_idx++] : 0xFFu;

    case ST_SR1:
        return fake_sr1();

    case ST_ADDR_RD:
    case ST_ADDR_WR:
    case ST_ADDR_ERASE:
        s_addr = (s_addr << 8) | in;
        s_addr_cnt++;
        if (s_addr_cnt == 3)
        {
            if (s_state == ST_ADDR_RD)
            {
                s_state = ST_READ;
            }
            else if (s_state == ST_ADDR_WR)
            {
                s_page_base = s_addr & ~(FAKE_PAGE_SIZE - 1u);
                s_state = ST_WRITE;
                if (s_wel != 0)
                {
                    s_program_ops++;
                }
            }
            else
            {
                fake_erase_sector(s_addr);
                s_state = ST_DONE;
            }
        }
        return 0xFFu;

    case ST_READ:
    {
        uint8_t v = s_mem[s_addr % FAKE_CAPACITY];

        s_addr++;
        return v;
    }

    case ST_WRITE:
        if (s_wel != 0)
        {
            /* NOR 只能 1→0；跨页时地址在页内回卷 —— 与真实器件一致 */
            uint32_t a = s_page_base | (s_addr & (FAKE_PAGE_SIZE - 1u));

            s_mem[a % FAKE_CAPACITY] &= in;
            s_addr++;
            if ((s_addr & (FAKE_PAGE_SIZE - 1u)) == 0u)
            {
                /* 一整页写完，写周期开始 */
                s_wel = 0;
                s_busy_polls = 3;
            }
        }
        return 0xFFu;

    default:
        return 0xFFu;
    }
}

/* ---------------- 假控制器 ---------------- */

static int fake_setup(struct spi_controller *ctlr, struct spi_device *spi)
{
    (void)ctlr;
    (void)spi;
    return 0;
}

static int fake_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
                             struct spi_transfer *xfer)
{
    const uint8_t *tx = (const uint8_t *)xfer->tx_buf;
    uint8_t *rx = (uint8_t *)xfer->rx_buf;
    uint16_t i;

    (void)ctlr;
    (void)spi;

    for (i = 0; i < xfer->len; i++)
    {
        uint8_t out = (tx != NULL) ? tx[i] : 0xFFu;
        uint8_t in = fake_io(out);

        if (rx != NULL)
        {
            rx[i] = in;
        }
    }
    return 0;
}

static void fake_set_cs(struct spi_controller *ctlr, struct spi_device *spi, int active)
{
    (void)ctlr;
    (void)spi;

    if (active != 0)
    {
        s_state = ST_CMD;       /* 每次拉低片选，命令从头开始 */
        s_jedec_idx = 0;
    }
    else
    {
        /* CS 释放：页编程在这里「提交」，与真实器件一致 */
        if ((s_state == ST_WRITE) && (s_wel != 0))
        {
            s_wel = 0;
            s_busy_polls = 3;
            s_state = ST_DONE;
        }
        s_state = ST_CMD;
    }
}

static const struct spi_algorithm fake_algo =
{
    .setup = fake_setup,
    .transfer_one = fake_transfer_one,
    .set_cs = fake_set_cs,
};

static struct spi_controller fake_ctlr =
{
    .name = "fake-nor",
    .algo = &fake_algo,
    .algo_data = NULL,
    .max_speed_hz = 18000000u,
    .delay_us = NULL,
    .delay_ctx = NULL,
};

struct spi_controller *fake_flash_controller(void)
{
    return &fake_ctlr;
}
