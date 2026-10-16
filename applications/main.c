/*
 * main.c - 应用主程序（RT-Thread 主线程入口）
 *
 * 只依赖 OSAL 与模块/驱动接口，不直接接触 RT-Thread API，
 * 以验证应用层与 RTOS 的解耦（未来切换 Zephyr/FreeRTOS 时此文件无需修改）。
 */
#include <osal.h>
#include "gpio.h"
#include "hello.h"
#include "led.h"

/* RT-Thread 系统完成初始化后，会在 main 线程中调用 main() */
int main(void)
{
    osal_printf("\n[app] STM32F103 | OSAL backend: rt-thread | 系统启动\n");

    /* 硬件抽象层初始化 */
    gpio_init();

    /* 启动示例模块 */
    led_init();
    hello_start();

    return 0;
}