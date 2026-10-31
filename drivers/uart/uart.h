/*
 * uart.h - 通用串口驱动接口 <drivers/uart/>
 *
 * 与 MCU 解耦：上层只依赖本接口，具体芯片实现见 stm32f1_uart.c。
 *
 * 设计要点：总线时钟由调用方注入（uart_hw_init 的 pclk_hz 参数），
 * 驱动内部不写死主频 —— 换 HSI/HSE/PLL 只改 BSP，驱动不用动。
 *
 * 初始化分两阶段，因为 rt_hw_board_init() 期间堆还没就绪：
 *   阶段 1  uart_hw_init()        —— 纯寄存器配置，可在 heap 之前调用
 *   阶段 2  uart_console_attach() —— 建信号量、开接收中断、注册设备，必须在
 *                                    rt_system_heap_init() 之后调用
 */
#ifndef DRIVERS_UART_H__
#define DRIVERS_UART_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 阶段 1：串口硬件初始化（引脚 / 波特率 / 收发使能）。
 * 只写寄存器，不申请任何资源，可以在堆初始化之前调用。
 *
 * @param baud     波特率，例如 115200
 * @param pclk_hz  该串口所在总线的时钟频率
 *                 （STM32F1 上 USART1 挂在 APB2，即 PCLK2）
 * @return 0 成功，负值失败
 */
int uart_hw_init(unsigned int baud, unsigned long pclk_hz);

/**
 * 阶段 2：接收中断 + 字符设备注册 + 设为控制台。
 * 内部会 rt_sem_create()，因此必须在 rt_system_heap_init() 之后调用。
 *
 * @return 0 成功，负值失败
 */
int uart_console_attach(void);

/** 阻塞发送一个字符（'\n' 自动补 '\r'） */
void uart_putc(char c);

/** 阻塞发送一个字符串 */
void uart_puts(const char *s);

/**
 * 读一个字符。
 * @return 0~255 读到的字节；尚未 attach 时返回 -1。
 *         缓冲区为空且已 attach 时阻塞等待接收中断。
 */
int uart_getchar(void);

/**
 * 往接收环形缓冲注入字节（线程上下文）。
 *
 * 用途：把板载按键映射成终端的转义序列（如 ↑ = ESC [ A），让 msh 的
 * 行编辑/历史记录功能像收到键盘输入一样工作。返回实际写入的字节数
 * （缓冲满时可能小于 len）。中断已关闭保护，可与接收中断并发调用。
 */
int uart_inject_rx(const void *buf, unsigned int len);

#ifdef __cplusplus
}
#endif

#endif /* DRIVERS_UART_H__ */
