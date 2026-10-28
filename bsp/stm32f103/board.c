/*
 * board.c - 板级初始化（STM32F103）
 *
 * 职责：
 *   1) 初始化 UART1（115200-8-N-1），作为 rt_kprintf / osal_printf 的输出通道
 *   2) 配置 SysTick 产生 1ms 系统时钟节拍
 *   3) 初始化 RT-Thread 动态内存堆
 *   4) 实现 rt_hw_console_output（覆盖 RT-Thread 的 weak 空实现）
 *
 * 时钟方案：使用内部高速时钟 HSI（8MHz）作为系统时钟，不做 PLL 超频，
 * 降低对外部晶振的依赖，便于在多种 F103 板卡上跑通。
 */
#include <rtthread.h>

#include "board.h"

/* ================= STM32F1 寄存器（最小子集） ================= */
#define RCC_BASE        0x40021000u
#define RCC_APB2ENR     (*(volatile unsigned long *)(RCC_BASE + 0x18u))
#define RCC_IOPAEN      (0x1u << 2)
#define RCC_USART1EN    (0x1u << 14)

#define GPIOA_BASE      0x40010800u
#define GPIOA_CRH       (*(volatile unsigned long *)(GPIOA_BASE + 0x04u))

#define USART1_BASE     0x40013800u
#define USART1_SR       (*(volatile unsigned long *)(USART1_BASE + 0x00u))
#define USART1_DR       (*(volatile unsigned long *)(USART1_BASE + 0x04u))
#define USART1_BRR      (*(volatile unsigned long *)(USART1_BASE + 0x08u))
#define USART1_CR1      (*(volatile unsigned long *)(USART1_BASE + 0x0Cu))

#define SYSTICK_BASE    0xE000E010u
#define SYSTICK_CTRL    (*(volatile unsigned long *)(SYSTICK_BASE + 0x00u))
#define SYSTICK_LOAD    (*(volatile unsigned long *)(SYSTICK_BASE + 0x04u))
#define SYSTICK_VAL     (*(volatile unsigned long *)(SYSTICK_BASE + 0x08u))
#define SYSTICK_CFLAG_CLKSOURCE (0x1u << 2)
#define SYSTICK_CFLAG_TICKINT   (0x1u << 1)
#define SYSTICK_CFLAG_ENABLE    (0x1u << 0)

#define UART_BAUD       115200u

/* ================= RCC 时钟估算（防乱码：UART/SysTick 与实际时钟解耦） =================
 * 始终按 STM32F1 RCC_CFGR 实际位配置推算系统时钟/总线时钟：
 *   - UART1(BRR)  用 PCLK2(APB2)，保证波特率在任何时钟方案下都精确 = 115200
 *   - SysTick     CLKSOURCE=1 时时钟 = HCLK(AHB)，保证 1ms 节拍精确
 * 头注释默认"HSI 8MHz"，补丁后即使换成 HSE/PLL(72MHz) 也不会出现串口乱码/节拍偏快。 */
#define RCC_CFGR        (*(volatile unsigned long *)(RCC_BASE + 0x04u))

#define HSI_CLOCK_HZ    8000000u   /* 内部高速时钟 */
#define HSE_CLOCK_HZ    8000000u   /* 战舰V4 外部晶振 */

static unsigned long rcc_sysclk_hz(void)
{
    unsigned long cfgr = RCC_CFGR;
    unsigned long src  = (cfgr >> 2) & 0x3u;             /* SW: 0=HSI 1=HSE 2=PLL */
    unsigned long clk;

    if (src == 1u)
    {
        clk = HSE_CLOCK_HZ;
    }
    else if (src == 2u)
    {
        unsigned long pllsrc = (cfgr >> 16) & 0x1u;      /* PLLSRC: 0=HSI/2 1=HSE */
        unsigned long pllxt  = (cfgr >> 17) & 0x1u;      /* PLLXTPRE: 0=HSE 1=HSE/2 */
        unsigned long pllmul = ((cfgr >> 18) & 0xFu) + 2u; /* PLLMUL: ×2..×16 */
        unsigned long in = pllsrc ? (HSE_CLOCK_HZ >> pllxt) : (HSI_CLOCK_HZ / 2u);
        clk = in * pllmul;
    }
    else
    {
        clk = HSI_CLOCK_HZ;                              /* SW=0 或预留值按 HSI */
    }
    return clk;
}

static unsigned long rcc_ahb_hz(void)                     /* HCLK */
{
    static const unsigned long hpre_div[16] =
        { 1,1,1,1,1,1,1,1, 2,4,8,16,64,128,256,512 };
    unsigned long div = (RCC_CFGR >> 4) & 0xFu;
    return rcc_sysclk_hz() / hpre_div[div];
}

static unsigned long rcc_pclk2_hz(void)                   /* PCLK2 = APB2 */
{
    static const unsigned long ppre2_div[8] = { 1,1,1,1, 2,4,8,16 };
    unsigned long div = (RCC_CFGR >> 11) & 0x7u;
    return rcc_ahb_hz() / ppre2_div[div];
}

/* ================= UART1（控制台） ================= */
#define NVIC_ISER1      (*(volatile unsigned long *)(0xE000E104u))

/* USART_SR 位定义（RM0008，STM32F1 参考手册）：
 *   bit0=PE  bit1=FE  bit2=NE  bit3=ORE  bit4=IDLE  bit5=RXNE  bit6=TC  bit7=TXE
 * 注意 RXNE 是 bit5 而不是 bit0。写成 bit0 会读不到 DR，RXNE 永不清零，
 * 电平触发的 USART 中断就会无限重入，把 CPU 卡死在 ISR 里（表现为打印完
 * "msh >" 后一按键盘系统就死）。 */
#define USART_SR_PE     (0x1u << 0)
#define USART_SR_FE     (0x1u << 1)
#define USART_SR_NE     (0x1u << 2)
#define USART_SR_ORE    (0x1u << 3)
#define USART_SR_RXNE   (0x1u << 5)
#define USART_SR_TXE    (0x1u << 7)

/* RX 环形缓冲 + 信号量（FINSH shell 输入用） */
#define RX_BUF_SIZE 64
static unsigned char  rx_ring[RX_BUF_SIZE];
static volatile int   rx_head = 0;
static volatile int   rx_tail = 0;
static rt_sem_t       rx_sem  = RT_NULL;

#ifdef RT_USING_DEVICE
/* 串口字符设备：FINSH 在 RT_USING_DEVICE 下只从设备取输入 */
static struct rt_device s_uart1_dev;
#endif

static void uart1_rx_init(void)
{
    rx_sem = rt_sem_create("urx", 0, RT_IPC_FLAG_FIFO);
    USART1_CR1 |= (0x1u << 5);   /* RXNEIE = bit5：开 RX 中断 */
    NVIC_ISER1 = (0x1u << 5);    /* USART1 IRQ=37 → ISER1 bit5 (37-32=5) */
}

void USART1_IRQHandler(void)
{
    unsigned long sr;
    unsigned char c;

    rt_interrupt_enter();

    sr = USART1_SR;

    /* RXNE / ORE / FE / NE / PE 任一置位都必须「先读 SR、再读 DR」把标志清掉。
     * 电平触发的中断只要源不清，就会不停重入，其它线程全部饿死。 */
    if (sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE))
    {
        c = (unsigned char)(USART1_DR & 0xFFu);   /* 读 DR 清 RXNE/ORE/FE/NE/PE */

        if (sr & USART_SR_RXNE)                   /* 只有真正收到数据才入队 */
        {
            int next = (rx_head + 1) % RX_BUF_SIZE;
            if (next != rx_tail)
            {
                rx_ring[rx_head] = c;
                rx_head = next;
#ifdef RT_USING_DEVICE
                /* 通知设备层（FINSH 注册的 rx_indicate 会释放它的信号量） */
                if (s_uart1_dev.rx_indicate != RT_NULL)
                {
                    s_uart1_dev.rx_indicate(&s_uart1_dev, 1);
                }
#endif
                rt_sem_release(rx_sem);           /* 唤醒阻塞的 getchar */
            }
        }
    }

    rt_interrupt_leave();
}

/* FINSH shell 读字节：缓冲空就阻塞等信号量（中断 release）
 * 这才是正确的中断驱动模式——shell 线程 sleep 在信号量上，不抢 CPU */
signed char rt_hw_console_getchar(void)
{
    /* 先试非阻塞读缓冲 */
    if (rx_tail != rx_head)
    {
        signed char c = (signed char)rx_ring[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
        return c;
    }
    /* 缓冲空 → 阻塞等中断 release 信号量 */
    rt_sem_take(rx_sem, RT_WAITING_FOREVER);
    /* 被唤醒了，再读一次 */
    signed char c = (signed char)rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return c;
}

static void uart1_init(void)
{
    unsigned long brr;

    /* 使能 GPIOA、USART1 时钟 */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);

    /* PA9 = TX：推挽 + 复用输出，50MHz（CRH 引脚9，CNF=10, MODE=11 → 0b1011） */
    GPIOA_CRH = (GPIOA_CRH & ~(0x0Fu << 4)) | (0x0Bu << 4);

    /* PA10 = RX：浮空输入（CRH 引脚10，CNF=01, MODE=00 → 0b0100） */
    GPIOA_CRH = (GPIOA_CRH & ~(0x0Fu << 8)) | (0x04u << 8);

    /* 波特率 */
    brr = (rcc_pclk2_hz() + (UART_BAUD / 2u)) / UART_BAUD;
    USART1_BRR = (unsigned long)brr;

    /* UE=13, TE=3, RE=2（RXNEIE 在 uart1_rx_init 里单独开） */
    USART1_CR1 = (0x1u << 13) | (0x1u << 3) | (0x1u << 2);
}

static void uart1_putc(char c)
{
    if (c == '\n')
    {
        while ((USART1_SR & USART_SR_TXE) == 0u) { }
        USART1_DR = (unsigned long)'\r';
    }
    while ((USART1_SR & USART_SR_TXE) == 0u) { }
    USART1_DR = (unsigned long)c;
}

/* 覆盖 RT-Thread 的 weak 空实现：所有内核输出最终汇入此函数 */
void rt_hw_console_output(const char *str)
{
    while (*str != '\0')
    {
        uart1_putc(*str++);
    }
}

#ifdef RT_USING_DEVICE
/* ================= UART1 设备封装 =================
 * 打开 RT_USING_DEVICE 后 finsh_getchar() 只从 shell->device 读数据
 * （components/finsh/shell.c），而 shell 线程启动时会拿
 * rt_console_get_device() 作为输入设备。板级此前只实现了
 * rt_hw_console_output/getchar，没有注册任何设备，于是 shell->device
 * 恒为 NULL —— 表现就是「能打印、打不进命令」。
 * 这里把已有环形缓冲包成标准字符设备，并设为控制台。 */
static rt_err_t uart1dev_init(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

static rt_err_t uart1dev_open(rt_device_t dev, rt_uint16_t oflag)
{
    (void)dev;
    (void)oflag;
    return RT_EOK;
}

static rt_err_t uart1dev_close(rt_device_t dev)
{
    (void)dev;
    return RT_EOK;
}

/* 非阻塞读：缓冲区空就返回 0，由 finsh 的信号量负责等待 */
static rt_ssize_t uart1dev_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_size_t n = 0;
    char *p = (char *)buffer;

    (void)dev;
    (void)pos;

    while (n < size && rx_tail != rx_head)
    {
        p[n++] = (char)rx_ring[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    }

    return n;
}

static rt_ssize_t uart1dev_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    const char *p = (const char *)buffer;
    rt_size_t i;

    (void)dev;
    (void)pos;

    for (i = 0; i < size; i++)
    {
        uart1_putc(p[i]);
    }

    return size;
}

static rt_err_t uart1dev_control(rt_device_t dev, int cmd, void *args)
{
    (void)dev;
    (void)cmd;
    (void)args;
    return RT_EOK;
}

static void uart1_dev_register(void)
{
    s_uart1_dev.type    = RT_Device_Class_Char;
    s_uart1_dev.init    = uart1dev_init;
    s_uart1_dev.open    = uart1dev_open;
    s_uart1_dev.close   = uart1dev_close;
    s_uart1_dev.read    = uart1dev_read;
    s_uart1_dev.write   = uart1dev_write;
    s_uart1_dev.control = uart1dev_control;

    rt_device_register(&s_uart1_dev, "uart1",
                       RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM);

    /* 设为控制台：rt_kprintf 走本设备，finsh 也会自动把它当输入源 */
    rt_console_set_device("uart1");
}
#endif /* RT_USING_DEVICE */

/* ================= 启动延时（马赛对齐 Zephyr CONFIG_BOOT_DELAY=3000） ================= */
static void boot_delay_ms(unsigned int ms)
{
    /*
     * 忙等延时，用 SysTick 数 1ms 一次（不开 TICKINT，纯查询 COUNTFLAG），
     * 因为 rt_hw_board_init() 早期还没调用 rt_system_timer_init()，
     * 若此时开 SysTick 中断会在第一次 tick 遍历未初始化链表时 HardFault。
     * LOAD 按 HCLK 换算（与串口同源防乱码），1ms ≤ 2^24 计数，精度可靠。
     */
    unsigned long load = rcc_ahb_hz() / 1000u - 1u;

    while (ms-- > 0u)
    {
        SYSTICK_LOAD = load;
        SYSTICK_VAL  = 0u;
        SYSTICK_CTRL = (SYSTICK_CFLAG_CLKSOURCE | SYSTICK_CFLAG_ENABLE);
        while ((SYSTICK_CTRL & 0x10000u) == 0u) { } /* COUNTFLAG */
        SYSTICK_CTRL = 0u; /* 停，留待 systick_init() 重新配置为系统节拍 */
    }
}

/* ================= SysTick（系统节拍） ================= */
static void systick_init(void)
{
    /* 1ms 节拍：LOAD = HCLK/RT_TICK_PER_SECOND - 1（CLKSOURCE=1 时 SysTick 时钟=HCLK） */
    SYSTICK_LOAD = (rcc_ahb_hz() / RT_TICK_PER_SECOND) - 1u;
    SYSTICK_VAL  = 0u;
    SYSTICK_CTRL = (SYSTICK_CFLAG_CLKSOURCE | SYSTICK_CFLAG_TICKINT |
                    SYSTICK_CFLAG_ENABLE);
}

void SysTick_Handler(void)
{
    rt_interrupt_enter();
    rt_tick_increase();
    rt_interrupt_leave();
}

/* ================= 板级入口（由 rtthread_startup 调用） ================= */
void rt_hw_board_init(void)
{
    uart1_init();

    /* 启动延时 3s：与 Zephyr CONFIG_BOOT_DELAY=3000 对齐，给串口工具抓启动日志开窗口 */
    rt_kprintf("[RT-Thread] boot 启动等待 3.0s（对齐 Zephyr CONFIG_BOOT_DELAY=3000）...\n");
    boot_delay_ms(3000u);

    /*
     * 在使能 SysTick 中断之前，必须先初始化内核定时器链表。
     * rtthread_startup() 里 rt_system_timer_init() 位于 rt_hw_board_init() 之后；
     * 若此处过早使能 SysTick，第一次 tick 触发 rt_timer_check() 会遍历
     * 尚未初始化（全 0 / NULL）的 _timer_list，造成 HardFault（INVSTATE）。
     */
    rt_system_timer_init();
    systick_init();

#ifdef RT_USING_HEAP
    rt_system_heap_init(HEAP_BEGIN, HEAP_END);
#endif

    /* FINSH shell UART RX：必须在 heap_init 之后（rt_sem_create 需要 rt_malloc） */
    uart1_rx_init();

#ifdef RT_USING_DEVICE
    uart1_dev_register();
#endif
}