/*
 * stm32f1_uart.c - STM32F103 串口驱动（寄存器级，不依赖 HAL）
 *
 * 从 bsp/stm32f103/board.c 抽出。与 BSP 解耦：
 *   - 不 include board.h，也不引用任何板级符号
 *   - 总线时钟由 uart_hw_init() 的 pclk_hz 入参注入，驱动内不写死主频
 *   只依赖 RT-Thread 内核（rt_device / rt_sem / rt_console_set_device）。
 *
 * 参考 RM0008：
 *   USART1 @ 0x40013800，挂 APB2（PCLK2）
 *   PA9 = TX（复用推挽输出），PA10 = RX（浮空输入）
 */
#include <rtthread.h>

#include "uart.h"

/* ================= 寄存器（STM32F1 最小子集） ================= */
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

/* NVIC 中断使能寄存器组 1（IRQ32..63）。写 1 置位，写 0 无影响 */
#define NVIC_ISER1      (*(volatile unsigned long *)(0xE000E104u))
#define USART1_IRQ_POS  5u          /* USART1 IRQ=37 → ISER1 bit5 (37-32) */

/* USART_SR 位定义（RM0008）：
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

#define USART_CR1_RE    (0x1u << 2)
#define USART_CR1_TE    (0x1u << 3)
#define USART_CR1_RXNEIE (0x1u << 5)
#define USART_CR1_UE    (0x1u << 13)

/* ================= 接收缓冲（FINSH shell 输入用） ================= */
#define RX_BUF_SIZE 64

static unsigned char  rx_ring[RX_BUF_SIZE];
static volatile int   rx_head = 0;
static volatile int   rx_tail = 0;
static rt_sem_t       rx_sem  = RT_NULL;

#ifdef RT_USING_DEVICE
/* 串口字符设备：FINSH 在 RT_USING_DEVICE 下只从设备取输入 */
static struct rt_device s_uart1_dev;
#endif

/* ================= 发送 ================= */
void uart_putc(char c)
{
    if (c == '\n')
    {
        while ((USART1_SR & USART_SR_TXE) == 0u) { }
        USART1_DR = (unsigned long)'\r';
    }
    while ((USART1_SR & USART_SR_TXE) == 0u) { }
    USART1_DR = (unsigned long)c;
}

void uart_puts(const char *s)
{
    if (s == RT_NULL)
    {
        return;
    }

    while (*s != '\0')
    {
        uart_putc(*s++);
    }
}

/* ================= 接收 ================= */
int uart_getchar(void)
{
    int c;

    /* 先试非阻塞读缓冲 */
    if (rx_tail != rx_head)
    {
        c = (int)rx_ring[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
        return c;
    }

    /* 还没 attach（堆尚未就绪、接收中断未开）：不能阻塞，报告无数据 */
    if (rx_sem == RT_NULL)
    {
        return -1;
    }

    /* 缓冲空 → 阻塞等中断 release 信号量 */
    rt_sem_take(rx_sem, RT_WAITING_FOREVER);

    if (rx_tail == rx_head)     /* 被别的读者取走了：等到了信号却没数据 */
    {
        return -1;
    }

    c = (int)rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return c;
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

/* ================= 初始化 ================= */
int uart_hw_init(unsigned int baud, unsigned long pclk_hz)
{
    unsigned long brr;

    if (baud == 0u || pclk_hz == 0u)
    {
        return -1;
    }

    /* 使能 GPIOA、USART1 时钟 */
    RCC_APB2ENR |= (RCC_IOPAEN | RCC_USART1EN);

    /* PA9 = TX：推挽 + 复用输出，50MHz（CRH 引脚9，CNF=10, MODE=11 → 0b1011） */
    GPIOA_CRH = (GPIOA_CRH & ~(0x0Fu << 4)) | (0x0Bu << 4);

    /* PA10 = RX：浮空输入（CRH 引脚10，CNF=01, MODE=00 → 0b0100） */
    GPIOA_CRH = (GPIOA_CRH & ~(0x0Fu << 8)) | (0x04u << 8);

    /* BRR 低 4 位是小数部分，所以整体按定点数四舍五入 */
    brr = (pclk_hz + (baud / 2u)) / baud;
    USART1_BRR = (unsigned long)brr;

    /* UE=13, TE=3, RE=2（RXNEIE 留给 uart_console_attach 单独开） */
    USART1_CR1 = (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);

    return 0;
}

#ifdef RT_USING_DEVICE
/* ================= 字符设备封装 =================
 * 打开 RT_USING_DEVICE 后 finsh_getchar() 只从 shell->device 读数据
 * （components/finsh/shell.c），而 shell 线程启动时会拿
 * rt_console_get_device() 作为输入设备。只实现 rt_hw_console_output/getchar
 * 而不注册设备时，shell->device 恒为 NULL —— 表现就是「能打印、打不进命令」。
 * 这里把接收环形缓冲包成标准字符设备，并设为控制台。 */
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
        uart_putc(p[i]);
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

int uart_console_attach(void)
{
    /* 接收信号量：中断 release，getchar/shell 线程 take */
    if (rx_sem == RT_NULL)
    {
        rx_sem = rt_sem_create("urx", 0, RT_IPC_FLAG_FIFO);
        if (rx_sem == RT_NULL)
        {
            return -1;
        }
    }

    USART1_CR1 |= USART_CR1_RXNEIE;                 /* 开 RX 中断 */
    NVIC_ISER1 |= (0x1u << USART1_IRQ_POS);

#ifdef RT_USING_DEVICE
    uart1_dev_register();
#endif

    return 0;
}
