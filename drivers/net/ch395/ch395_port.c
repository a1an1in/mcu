/*
 * ch395_port.c - CH395Q 传输层（STM32F103 寄存器级，不依赖 HAL）
 *
 * SPI1 配置：主机、8 位、模式 3（CPOL=1/CPHA=1，与正点原子驱动一致）、
 *           软件 NSS、MSB first；分频按 PCLK2 现算。
 *
 * 关于分频：SPI1 挂在 APB2 上，SPI_CK = PCLK2 / 2^(BR+1)，代码取「不超过
 * CH395_SPI_MAX_HZ（18MHz，手册上限 30MHz 留余量）」的最快档 ——
 * HSI 8MHz 时选 /2 = 4MHz，HSE+PLL 72MHz 时选 /4 = 18MHz，换主频不用改这里。
 * CH395 支持 mode0/mode3，官方 EVT 用 mode3。
 *
 * 关于总线共享：PA5/PA6/PA7 同时接 VS1053。初始化时把 VS1053 按住复位
 * （PE6=0）并把 XCS 拉高（PF7=1），否则它可能在 CH395 读数据时驱动 MISO。
 *
 * 关于互斥：erx（lwIP 收包）和 etx（lwIP 发包）是两个线程，都会用 SPI。
 * 一个命令事务从 CS 拉低到拉高必须在同一线程内完成，所以互斥量放在
 * cs_low/cs_high 里，天然按「命令」粒度加锁。
 */
#include <rtthread.h>

#include "ch395.h"
#include "spi.h"

/* ============================ 寄存器定义 ============================ */
#define RCC_BASE        0x40021000u
#define RCC_CFGR        (*(volatile uint32_t *)(RCC_BASE + 0x04u))
#define RCC_APB2ENR     (*(volatile uint32_t *)(RCC_BASE + 0x18u))

#define AFIO_BASE       0x40010000u

#define GPIOA_BASE      0x40010800u
#define GPIOD_BASE      0x40011400u
#define GPIOE_BASE      0x40011800u
#define GPIOF_BASE      0x40011C00u
#define GPIOG_BASE      0x40012000u

#define SPI1_BASE       0x40013000u

#define GPIO_CRL(b)     (*(volatile uint32_t *)((b) + 0x00u))
#define GPIO_CRH(b)     (*(volatile uint32_t *)((b) + 0x04u))
#define GPIO_ODR(b)     (*(volatile uint32_t *)((b) + 0x0Cu))
#define GPIO_BSRR(b)    (*(volatile uint32_t *)((b) + 0x10u))

#define SPI_CR1(b)      (*(volatile uint32_t *)((b) + 0x00u))
#define SPI_SR(b)       (*(volatile uint32_t *)((b) + 0x08u))
#define SPI_DR(b)       (*(volatile uint32_t *)((b) + 0x0Cu))

/* RCC_APB2ENR 位 */
#define RCC_AFIOEN      (1u << 0)
#define RCC_IOPAEN      (1u << 2)
#define RCC_IOPDEN      (1u << 5)
#define RCC_IOPEEN      (1u << 6)
#define RCC_IOPFEN      (1u << 7)
#define RCC_IOPGEN      (1u << 8)
#define RCC_SPI1EN      (1u << 12)

/* SPI_SR 位 */
#define SPI_SR_RXNE     (1u << 0)
#define SPI_SR_TXE      (1u << 1)

/* SPI_CR1 位 */
#define SPI_CR1_CPHA    (1u << 0)
#define SPI_CR1_CPOL    (1u << 1)
/* CH395 的 SPI 时钟上限：手册标 30MHz，取 18MHz 留余量 */
#define CH395_SPI_MAX_HZ    18000000u

#define SPI_CR1_MSTR    (1u << 2)
#define SPI_CR1_BR_DIV2 (0u << 3)
#define SPI_CR1_SPE     (1u << 6)
#define SPI_CR1_SSI     (1u << 8)
#define SPI_CR1_SSM     (1u << 9)

/* 引脚号（都按 GPIO 端口内的编号） */
#define CH395_CS_PIN    9u      /* PG9 */
#define CH395_INT_PIN   6u      /* PG6 */
#define CH395_RST_PIN   7u      /* PD7 */
#define CH395_SCK_PIN   5u      /* PA5 */
#define CH395_MISO_PIN  6u      /* PA6 */
#define CH395_MOSI_PIN  7u      /* PA7 */
#define VS_XCS_PIN      7u      /* PF7：VS1053 片选，拉高 */
#define VS_RST_PIN      6u      /* PE6：VS1053 复位，拉低 */

/* 内核主频（MHz），只用于微秒忙等。board.c 用 HSI 8MHz。 */
#define CH395_CPU_MHZ   8u

static struct spi_controller *s_spi1;   /* SPI1 的通用控制器：借它的总线锁 */

/* ============================ 内部工具 ============================ */
/* 配一个引脚的 CRL/CRH 四位域。mode_cnf 直接写 RM0008 的 MODE/CNF 组合值：
 *   0x3 = 推挽输出 50MHz   0x8 = 上/下拉输入   0xB = 复用推挽 50MHz
 *   0x4 = 浮空输入 */
static void ch395_pin_config(uint32_t base, unsigned pin, unsigned mode_cnf)
{
    volatile uint32_t *cfg;
    unsigned shift;

    cfg   = (pin < 8u) ? &GPIO_CRL(base) : &GPIO_CRH(base);
    shift = (pin & 0x07u) * 4u;

    *cfg = (*cfg & ~(0xFu << shift)) | (((uint32_t)mode_cnf & 0xFu) << shift);
}

/* ============================ 传输层接口 ============================ */
/* 读 RCC_CFGR 现算 PCLK2，再选一个不超过 CH395_SPI_MAX_HZ 的最快分频。
 * 不依赖 BSP 传参：换主频（HSI 8MHz / HSE+PLL 72MHz）都不用改这里。 */
static uint32_t ch395_spi_br(void)
{
    static const uint8_t hpre_div[16] = { 1,1,1,1,1,1,1,1, 2,4,8,16,64,128,256,512 };
    static const uint8_t ppre2_div[8] = { 1,1,1,1, 2,4,8,16 };
    uint32_t cfgr = RCC_CFGR;
    uint32_t clk;
    uint32_t br;

    switch ((cfgr >> 2) & 0x3u)             /* SWS：当前实际时钟源 */
    {
    case 1u:                                /* HSE 直连 */
        clk = 8000000u;
        break;
    case 2u:                                /* PLL */
    {
        uint32_t src   = (cfgr >> 16) & 0x1u;
        uint32_t xtpre = (cfgr >> 17) & 0x1u;
        uint32_t mul   = ((cfgr >> 18) & 0xFu) + 2u;
        uint32_t in    = src ? (8000000u >> xtpre) : 4000000u;   /* HSI/2 */
        clk = in * mul;
        break;
    }
    default:                                /* HSI */
        clk = 8000000u;
        break;
    }

    clk /= hpre_div[(cfgr >> 4) & 0xFu];    /* AHB  */
    clk /= ppre2_div[(cfgr >> 11) & 0x7u];  /* APB2 */

    for (br = 0u; br < 7u; br++)
    {
        if ((clk >> (br + 1u)) <= CH395_SPI_MAX_HZ)
        {
            return br;
        }
    }
    return 7u;                              /* 最慢档兜底 */
}

void ch395_port_init(void)
{
    /* SPI1 也被 VS1053 使用：互斥量统一用通用 SPI 层那一把（见 spi_core.c），
     * 避免 CH395 的 rt_mutex 和 VS1053 的 spi_sync 各锁各的。 */
    s_spi1 = spi_get_controller_by_name("stm32f1_spi1");
    if (s_spi1 == NULL)
    {
        rt_kprintf("[ch395] 警告：SPI1 未注册，与 VS1053 的总线互斥失效\n");
    }

    RCC_APB2ENR |= RCC_AFIOEN | RCC_IOPAEN | RCC_IOPDEN |
                   RCC_IOPEEN | RCC_IOPFEN | RCC_IOPGEN | RCC_SPI1EN;

    /* CS 先置高再切输出，避免上电时出现一个低电平毛刺把芯片选中 */
    GPIO_BSRR(GPIOG_BASE) = (1u << CH395_CS_PIN);
    ch395_pin_config(GPIOG_BASE, CH395_CS_PIN, 0x3);

    /* RST 空闲为高（不复位） */
    GPIO_BSRR(GPIOD_BASE) = (1u << CH395_RST_PIN);
    ch395_pin_config(GPIOD_BASE, CH395_RST_PIN, 0x3);

    /* INT：上拉输入（CH395 中断是低有效开漏/推挽，外部无上拉也能读） */
    GPIO_ODR(GPIOG_BASE) |= (1u << CH395_INT_PIN);
    ch395_pin_config(GPIOG_BASE, CH395_INT_PIN, 0x8);

    /* VS1053 隔离：XCS 高（不选中）、RST 低（保持复位，MISO 呈高阻） */
    GPIO_BSRR(GPIOF_BASE) = (1u << VS_XCS_PIN);
    ch395_pin_config(GPIOF_BASE, VS_XCS_PIN, 0x3);
    GPIO_BSRR(GPIOE_BASE) = (1u << (VS_RST_PIN + 16u));
    ch395_pin_config(GPIOE_BASE, VS_RST_PIN, 0x3);

    /* SPI1 引脚：SCK/MOSI 复用推挽，MISO 浮空输入 */
    ch395_pin_config(GPIOA_BASE, CH395_SCK_PIN, 0xB);
    ch395_pin_config(GPIOA_BASE, CH395_MISO_PIN, 0x4);
    ch395_pin_config(GPIOA_BASE, CH395_MOSI_PIN, 0xB);

    /* SPI1：主机 + 模式3 + 软件 NSS + 8 位 + MSB；分频按 PCLK2 现算 */
    SPI_CR1(SPI1_BASE) = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                         SPI_CR1_CPOL | SPI_CR1_CPHA |
                         (ch395_spi_br() << 3) | SPI_CR1_SPE;
}

void ch395_hw_reset(void)
{
    /* 手册要求 RST 低电平至少 10us；这里给足 20ms，保证上电/热复位都可靠 */
    GPIO_BSRR(GPIOD_BASE) = (1u << (CH395_RST_PIN + 16u));
    rt_thread_mdelay(20);
    GPIO_BSRR(GPIOD_BASE) = (1u << CH395_RST_PIN);
    rt_thread_mdelay(100);
}

void ch395_cs_low(void)
{
    (void)spi_bus_lock(s_spi1);
    GPIO_BSRR(GPIOG_BASE) = (1u << (CH395_CS_PIN + 16u));
}

void ch395_cs_high(void)
{
    GPIO_BSRR(GPIOG_BASE) = (1u << CH395_CS_PIN);
    spi_bus_unlock(s_spi1);
}

uint8_t ch395_spi_rw(uint8_t d)
{
    /* 8 位模式下必须用字节访问 DR，否则一次读写 32 位会把帧搞乱 */
    while ((SPI_SR(SPI1_BASE) & SPI_SR_TXE) == 0u)
        ;
    *(volatile uint8_t *)&SPI_DR(SPI1_BASE) = d;

    while ((SPI_SR(SPI1_BASE) & SPI_SR_RXNE) == 0u)
        ;
    return *(volatile uint8_t *)&SPI_DR(SPI1_BASE);
}

void ch395_udelay(uint32_t us)
{
    /* 不占用 SysTick 的粗延时：一次循环约 4~5 个周期，
     * 按 2 倍余量折算成 iterations = us * MHz / 2。
     * CH395 只要求命令字节后间隔不小于 1.5us，这里偏长是安全的。 */
    volatile uint32_t n = us * (CH395_CPU_MHZ / 2u);

    while (n--)
    {
    }
}
