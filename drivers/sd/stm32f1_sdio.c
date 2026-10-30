/*
 * stm32f1_sdio.c - STM32F1 SDIO 主机控制器实现 <drivers/sd/>
 *
 * 只做四件事，全部是寄存器操作，不含任何 SD 协议知识：
 *   1) 引脚（PC8~PC12 + PD2）复用推挽、使能 SDIO 时钟、上电；
 *   2) 发一条命令并等响应（短/长/无响应），把 RESP1~4 交出去；
 *   3) 数据阶段：配 DTIMER/DLEN/DCTRL，然后轮询 FIFO 搬字节；
 *   4) 时钟分频：SDIO_CK = HCLK / (CLKDIV + 2)。
 *
 * 时序参考：正点原子《STM32F103 战舰开发指南》第 49 章（实验37 SD卡实验）
 * 的 sdio_sdcard.c。与原版的两点差异：
 *   - 原版在 FIFO 轮询期间关全局中断；这里**不关**。原因是本工程跑在 RT-Thread
 *     里，关 2~3ms 会把 1ms 系统节拍拖掉。代价是极端情况下长中断可能让 FIFO
 *     溢出 —— 而 FIFO 有 128 字节（HCLK 8MHz、CK 1.6MHz 时约 640µs）的余量，
 *     本工程的 ISR 都是几微秒级，够用。
 *   - 原版用全局变量存卡状态；这里状态全在 struct sdio_host / struct sd_card 里。
 *
 * 不含任何 OS 头。
 */
#include <stdint.h>
#include <stddef.h>

#include "sdio.h"
#include "sd_config.h"
#include "stm32f1_sdio.h"

/* ================= 寄存器 ================= */
#define RCC_BASE            (0x40021000u)
#define RCC_AHBENR          (*(volatile uint32_t *)(RCC_BASE + 0x14u))
#define RCC_APB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x18u))

#define RCC_AHBENR_SDIOEN   (1u << 10)

#define GPIO_CRL(base)      (*(volatile uint32_t *)((base) + 0x00u))
#define GPIO_CRH(base)      (*(volatile uint32_t *)((base) + 0x04u))

#define SDIO_BASE           0x40018000u
#define SDIO_POWER          (*(volatile uint32_t *)(SDIO_BASE + 0x00u))
#define SDIO_CLKCR          (*(volatile uint32_t *)(SDIO_BASE + 0x04u))
#define SDIO_ARG            (*(volatile uint32_t *)(SDIO_BASE + 0x08u))
#define SDIO_CMD            (*(volatile uint32_t *)(SDIO_BASE + 0x0Cu))
#define SDIO_RESPCMD        (*(volatile uint32_t *)(SDIO_BASE + 0x10u))
#define SDIO_RESP1          (*(volatile uint32_t *)(SDIO_BASE + 0x14u))
#define SDIO_RESP2          (*(volatile uint32_t *)(SDIO_BASE + 0x18u))
#define SDIO_RESP3          (*(volatile uint32_t *)(SDIO_BASE + 0x1Cu))
#define SDIO_RESP4          (*(volatile uint32_t *)(SDIO_BASE + 0x20u))
#define SDIO_DTIMER         (*(volatile uint32_t *)(SDIO_BASE + 0x24u))
#define SDIO_DLEN           (*(volatile uint32_t *)(SDIO_BASE + 0x28u))
#define SDIO_DCTRL          (*(volatile uint32_t *)(SDIO_BASE + 0x2Cu))
#define SDIO_DCOUNT         (*(volatile uint32_t *)(SDIO_BASE + 0x30u))
#define SDIO_STA            (*(volatile uint32_t *)(SDIO_BASE + 0x34u))
#define SDIO_ICR            (*(volatile uint32_t *)(SDIO_BASE + 0x38u))
#define SDIO_MASK           (*(volatile uint32_t *)(SDIO_BASE + 0x3Cu))
#define SDIO_FIFO           (*(volatile uint32_t *)(SDIO_BASE + 0x80u))

/* SDIO_CLKCR 位 */
#define CLKCR_CLKDIV_MASK   (0xFFu << 0)
#define CLKCR_CLKEN         (1u << 8)
#define CLKCR_PWRSAV        (1u << 9)
#define CLKCR_WIDBUS_4      (1u << 11)

/* SDIO_CMD 位 */
#define CMD_WAITRESP_NONE   (0u << 6)
#define CMD_WAITRESP_SHORT  (1u << 6)
#define CMD_WAITRESP_LONG   (3u << 6)
#define CMD_CPSMEN          (1u << 10)

/* SDIO_STA / ICR 位（只列用到的） */
#define STA_CCRCFAIL        (1u << 0)
#define STA_DCRCFAIL        (1u << 1)
#define STA_CTIMEOUT        (1u << 2)
#define STA_DTOUT           (1u << 3)
#define STA_TXUNDERR        (1u << 4)
#define STA_RXOVERR         (1u << 5)
#define STA_CMDREND         (1u << 6)
#define STA_CMDSENT         (1u << 7)
#define STA_DATAEND         (1u << 8)
#define STA_STBITERR        (1u << 9)
#define STA_RXDAVL          (1u << 21)
#define STA_TXDAVL          (1u << 20)
#define STA_RXFIFOE         (1u << 19)
#define STA_TXFIFOE         (1u << 18)
#define STA_RXFIFOF         (1u << 17)
#define STA_TXFIFOF         (1u << 16)
#define STA_RXFIFOHF        (1u << 15)
#define STA_TXFIFOHE        (1u << 14)

#define SDIO_ICR_ALL        0x1FE00FFFu

/* 命令 / 数据轮询上限。HCLK 8MHz 下约几百毫秒，只是防死循环。 */
#define SDIO_CMD_SPIN       2000000u
#define SDIO_DATA_SPIN      8000000u

/* ================= 私有数据 ================= */

struct stm32f1_sdio_priv {
    uint32_t hclk_hz;
};

/* ???????????? data_start ????send_cmd ??????? */
static void sdio_data_irq_unlock(void);

static struct stm32f1_sdio_priv s_priv;
static struct sdio_host s_host;
static int s_inited;

/* GPIOA..GPIOG 基址，下标即端口编号 0=A … 6=G */
static const uint32_t s_gpio_base[7] =
{
    0x40010800u, 0x40010C00u, 0x40011000u, 0x40011400u,
    0x40011800u, 0x40011C00u, 0x40012000u,
};

/* 复用推挽输出 50MHz：CNF=10, MODE=11 → 0xB */
static void pin_af_pp(uint8_t port, uint8_t pin)
{
    volatile uint32_t *cr;
    uint32_t shift;

    if ((port > 6u) || (pin > 15u))
    {
        return;
    }

    RCC_APB2ENR |= (1u << (2u + port));

    cr = (pin < 8u) ? &GPIO_CRL(s_gpio_base[port]) : &GPIO_CRH(s_gpio_base[port]);
    shift = 4u * (pin & 7u);
    *cr = (*cr & ~(0xFu << shift)) | (0xBu << shift);
}

/* ================= 时钟 ================= */

static int stm32f1_sdio_set_clock(struct sdio_host *h, uint32_t hz)
{
    struct stm32f1_sdio_priv *p = (struct stm32f1_sdio_priv *)h->priv;
    uint32_t div;
    uint32_t clkdiv;

    if (hz == 0u)
    {
        return -1;
    }

    /* SDIO_CK = HCLK / (CLKDIV + 2)，向上取整保证不超过请求值 */
    div = (p->hclk_hz + hz - 1u) / hz;
    if (div < 2u)
    {
        div = 2u;
    }
    clkdiv = div - 2u;
    if (clkdiv > 0xFFu)
    {
        clkdiv = 0xFFu;
    }

    SDIO_CLKCR = (SDIO_CLKCR & ~CLKCR_CLKDIV_MASK) | clkdiv;
    return 0;
}

/* ================= 命令 ================= */

static int stm32f1_sdio_send_cmd(struct sdio_host *h, uint8_t cmd, uint32_t arg,
                                 uint8_t resp_type, uint32_t resp[4])
{
    uint32_t tmp;
    uint32_t spin = SDIO_CMD_SPIN;
    uint8_t rtype = (uint8_t)(resp_type & 0x03u);
    int no_crc = ((resp_type & SDIO_RESP_NO_CRC) != 0u) ? 1 : 0;

    (void)h;

    /* 清掉上一次的完成/错误标志，否则会把旧 CMDREND 当成这次的结果 */
    SDIO_ICR = SDIO_ICR_ALL;

    SDIO_ARG = arg;
    tmp = (uint32_t)(cmd & 0x3Fu);
    if (rtype == SDIO_RESP_SHORT)
    {
        tmp |= CMD_WAITRESP_SHORT;
    }
    else if (rtype == SDIO_RESP_LONG)
    {
        tmp |= CMD_WAITRESP_LONG;
    }
    else
    {
        tmp |= CMD_WAITRESP_NONE;
    }
    tmp |= CMD_CPSMEN;
    SDIO_CMD = tmp;

    if (rtype == SDIO_RESP_NONE)
    {
        while ((SDIO_STA & (STA_CMDSENT | STA_CTIMEOUT)) == 0u)
        {
            if (--spin == 0u)
            {
                sdio_data_irq_unlock();
                return -1;
            }
        }
        if ((SDIO_STA & STA_CTIMEOUT) != 0u)
        {
            SDIO_ICR = SDIO_ICR_ALL;
            sdio_data_irq_unlock();
            return -2;
        }
        SDIO_ICR = STA_CMDSENT;
        return 0;
    }

    while ((SDIO_STA & (STA_CMDREND | STA_CCRCFAIL | STA_CTIMEOUT)) == 0u)
    {
        if (--spin == 0u)
        {
            return -1;
        }
    }

    if ((SDIO_STA & STA_CTIMEOUT) != 0u)
    {
        SDIO_ICR = SDIO_ICR_ALL;
        sdio_data_irq_unlock();
        return -2;
    }
    if (((SDIO_STA & STA_CCRCFAIL) != 0u) && (no_crc == 0))
    {
        /* CRC 错也把收到的内容交出去：上层打出来能看出是「错位」还是「全 0」 */
        if (resp != NULL)
        {
            resp[0] = SDIO_RESP1;
            resp[1] = SDIO_RESP2;
            resp[2] = SDIO_RESP3;
            resp[3] = SDIO_RESP4;
        }
        SDIO_ICR = SDIO_ICR_ALL;
        sdio_data_irq_unlock();
        return -3;
    }

    /* 走到这里：要么 CMDREND，要么是「要求忽略 CRC」的 R3 —— 两种情况
     * 响应内容都有效（厂商例程也是这么用的）。 */
    if (resp != NULL)
    {
        resp[0] = SDIO_RESP1;
        resp[1] = SDIO_RESP2;
        resp[2] = SDIO_RESP3;
        resp[3] = SDIO_RESP4;
    }

    SDIO_ICR = SDIO_ICR_ALL;
    return 0;
}

/* ================= 总线宽度 ================= */

static int stm32f1_sdio_set_bus_width(struct sdio_host *h, unsigned int width)
{
    (void)h;

    if (width == 4u)
    {
        SDIO_CLKCR |= CLKCR_WIDBUS_4;
    }
    else
    {
        SDIO_CLKCR &= ~CLKCR_WIDBUS_4;
    }
    return 0;
}

/* ================= 数据阶段 ================= */

/* SDIO 的 FIFO 只有 32 个字（128 字节）。卡在命令响应之后立刻就要数据
 * （写）或开始送数据（读），这中间只要被别的线程抢走一次，硬件就置
 * TXUNDERR / RXOVERR —— 512 字节搬到一半中止。
 * 所以从 data_start（配好数据状态机）到 data_move 结束，全程关中断。
 * 单块 512B 约 256µs，最多 4 块约 1ms，对 1ms 节拍是可接受的代价。 */
static uint32_t s_data_primask;
static int s_data_irq_off;

static void sdio_data_irq_lock(void)
{
    if (s_data_irq_off == 0)
    {
        __asm volatile ("mrs %0, primask" : "=r" (s_data_primask));
        __asm volatile ("cpsid i" ::: "memory");
        s_data_irq_off = 1;
    }
}

static void sdio_data_irq_unlock(void)
{
    if (s_data_irq_off != 0)
    {
        __asm volatile ("msr primask, %0" :: "r" (s_data_primask) : "memory");
        s_data_irq_off = 0;
    }
}

static uint32_t s_prefilled_words;   /* data_start 里已经塞进 FIFO 的字数 */

/* 数据阶段准备。
 *
 * 调用时机（和厂商例程一致，读/写不一样）：
 *   读：在发 CMD17/18 **之前**调用 —— 数据状态机要先武装好，否则卡一开始送数据就丢；
 *   写：在发 CMD24/25 **之后**调用 —— 提前武装的话，卡的响应一到、状态机就启动，
 *       而此时 FIFO 还是空的，立刻 TXUNDERR。
 *
 * 写方向在这里把 tx 开头的 32 个字（整个 FIFO）预填进去，再交给 data_move 续，
 * 保证数据阶段一启动就有存货。 */
static int stm32f1_sdio_data_start(struct sdio_host *h, int read, const void *tx,
                                   uint32_t total_bytes, uint32_t blk_size)
{
    uint32_t exponent = 0u;
    uint32_t size = blk_size;

    (void)h;

    if ((total_bytes == 0u) || (blk_size == 0u) || ((blk_size & (blk_size - 1u)) != 0u))
    {
        return -1;
    }

    while ((size > 1u) && (exponent < 0xFu))
    {
        size >>= 1;
        exponent++;
    }

    /* 从这里到 data_move 结束全程关中断：卡在响应之后立刻就索要/发送数据，
     * 中间被别的线程抢走一次就会 TXUNDERR / RXOVERR。 */
    sdio_data_irq_lock();

    SDIO_DTIMER = 0xFFFFFFFFu;
    SDIO_DLEN = total_bytes & 0x01FFFFFFu;

    /* 写方向：命令已经发完、卡正在等数据，这里置 DTEN 并立刻灌 FIFO。
     * 卡在 4MHz 下约 1µs 就开始要第一个字节，所以预填必须紧跟 DTEN，
     * 中间不能有任何可变延时（本函数和调用方都在关中断的临界区里）。 */
    SDIO_DCTRL = (exponent << 4) | ((read != 0) ? ((1u << 1) | 1u) : 1u);
    SDIO_ICR = SDIO_ICR_ALL;

    s_prefilled_words = 0u;

    if ((read == 0) && (tx != NULL))
    {
        const uint32_t *p = (const uint32_t *)tx;
        uint32_t words = total_bytes >> 2;
        uint32_t i;

        for (i = 0u; (i < 32u) && (words > 0u); i++)
        {
            SDIO_FIFO = *p++;
            words--;
        }
        s_prefilled_words = i;
    }

    return 0;
}

static int stm32f1_sdio_data_move(struct sdio_host *h, int read,
                                  void *buf, uint32_t total_bytes)
{
    uint32_t spin = SDIO_DATA_SPIN;
    uint32_t words = total_bytes >> 2;
    uint32_t *p = (uint32_t *)buf;

    (void)h;

    if (read != 0)
    {
        while (words > 0u)
        {
            if ((SDIO_STA & STA_RXFIFOHF) != 0u)
            {
                uint32_t i;

                for (i = 0u; (i < 8u) && (words > 0u); i++)
                {
                    *p++ = SDIO_FIFO;
                    words--;
                }
                spin = SDIO_DATA_SPIN;
            }
            else if ((SDIO_STA & STA_RXDAVL) != 0u)
            {
                *p++ = SDIO_FIFO;
                words--;
                spin = SDIO_DATA_SPIN;
            }
            else if ((SDIO_STA & (STA_DCRCFAIL | STA_DTOUT | STA_RXOVERR |
                                  STA_STBITERR)) != 0u)
            {
                goto fail;
            }
            else if (--spin == 0u)
            {
                goto fail;
            }
        }
    }
    else
    {
        /* 开头 s_prefilled_words 个字已经在 data_start 里进了 FIFO */
        p += s_prefilled_words;
        words -= s_prefilled_words;

        while (words > 0u)
        {
            if ((SDIO_STA & STA_TXFIFOHE) != 0u)
            {
                uint32_t i;

                for (i = 0u; (i < 8u) && (words > 0u); i++)
                {
                    SDIO_FIFO = *p++;
                    words--;
                }
                spin = SDIO_DATA_SPIN;
            }
            else if ((SDIO_STA & (STA_DCRCFAIL | STA_DTOUT | STA_TXUNDERR)) != 0u)
            {
                goto fail;
            }
            else if (--spin == 0u)
            {
                goto fail;
            }
        }
    }

    /* 等最后一个字节真正搬完（DATAEND） */
    spin = SDIO_DATA_SPIN;
    while ((SDIO_STA & (STA_DATAEND | STA_DCRCFAIL | STA_DTOUT |
                        STA_TXUNDERR | STA_RXOVERR)) == 0u)
    {
        if (--spin == 0u)
        {
            goto fail;
        }
    }

    if ((SDIO_STA & (STA_DCRCFAIL | STA_DTOUT | STA_TXUNDERR | STA_RXOVERR)) != 0u)
    {
        goto fail;
    }

    SDIO_ICR = SDIO_ICR_ALL;
    sdio_data_irq_unlock();
    return 0;

fail:
    sdio_data_irq_unlock();
    /* 不在这里清 ICR：把错误标志留在 STA 里，方便用调试器看是哪一类错。
     * 下一次 data_start 会清掉。 */
    return -1;
}

static const struct sdio_host_ops s_ops =
{
    .send_cmd = stm32f1_sdio_send_cmd,
    .set_clock = stm32f1_sdio_set_clock,
    .set_bus_width = stm32f1_sdio_set_bus_width,
    .data_start = stm32f1_sdio_data_start,
    .data_move = stm32f1_sdio_data_move,
};

/* ================= 初始化 ================= */

struct sdio_host *stm32f1_sdio_init(uint32_t hclk_hz)
{
    if (s_inited)
    {
        return &s_host;
    }

    if (hclk_hz == 0u)
    {
        return NULL;
    }

    /* 引脚：D0~D3 / CLK / CMD 全部复用推挽 */
    pin_af_pp(SD_D0_PORT_IDX, SD_D0_PIN);
    pin_af_pp(SD_D1_PORT_IDX, SD_D1_PIN);
    pin_af_pp(SD_D2_PORT_IDX, SD_D2_PIN);
    pin_af_pp(SD_D3_PORT_IDX, SD_D3_PIN);
    pin_af_pp(SD_CLK_PORT_IDX, SD_CLK_PIN);
    pin_af_pp(SD_CMD_PORT_IDX, SD_CMD_PIN);

    RCC_AHBENR |= RCC_AHBENR_SDIOEN;        /* SDIO 挂在 AHB 上 */

    /* 外设复位到已知状态 */
    SDIO_POWER = 0u;
    SDIO_CLKCR = 0u;
    SDIO_ARG = 0u;
    SDIO_CMD = 0u;
    SDIO_DTIMER = 0u;
    SDIO_DLEN = 0u;
    SDIO_DCTRL = 0u;
    SDIO_ICR = SDIO_ICR_ALL;
    SDIO_MASK = 0u;                          /* 纯轮询，不开中断 */

    s_priv.hclk_hz = hclk_hz;
    s_host.ops = &s_ops;
    s_host.priv = &s_priv;

    /* 上电 + 使能卡时钟；初始 1 位总线、非省电、关硬件流控 */
    SDIO_CLKCR = 0u;
    (void)stm32f1_sdio_set_clock(&s_host, SD_INIT_CLOCK_HZ);
    SDIO_POWER = 0x03u;                      /* PWRCTRL = 11：开卡时钟 */
    SDIO_CLKCR |= CLKCR_CLKEN;

    s_inited = 1;
    return &s_host;
}
