/*
 * startup_stm32f10x_md.S - STM32F103 (Cortex-M3, Medium density) 启动文件
 *
 * 使用 GNU as 语法。入口为 Reset_Handler，完成 .data/.bss 初始化后
 * 跳转到 RT-Thread 的 rtthread_startup()（由 components.c 提供）。
 *
 * 注意：
 *   - PendSV_Handler / HardFault_Handler 由 RT-Thread libcpu/context_gcc.S 提供
 *   - SysTick_Handler 在 bsp/board.c 中实现
 *   - 其余中断使用弱默认处理（死循环），可按需在工程中强定义覆盖
 */
    .syntax unified
    .cpu    cortex-m3
    .thumb

    .section .isr_vector, "a", %progbits
    .align 2
    .globl g_pfnVectors
g_pfnVectors:
    .word   _estack                       /* 初始栈顶 */
    .word   Reset_Handler
    .word   NMI_Handler
    .word   HardFault_Handler             /* RT-Thread 提供 */
    .word   MemManage_Handler
    .word   BusFault_Handler
    .word   UsageFault_Handler
    .word   0, 0, 0, 0                    /* Reserved */
    .word   SVC_Handler
    .word   DebugMon_Handler
    .word   0                             /* Reserved */
    .word   PendSV_Handler                /* RT-Thread 提供 */
    .word   SysTick_Handler               /* bsp/board.c 提供 */
    /* 外设中断（位于向量表偏移 +0x40 起），当前全部使用弱默认处理 */
    .rept   60
    .word   Default_Handler
    .endr

    .section .text.Reset_Handler, "ax", %progbits
    .align 2
    .thumb_func
    .globl Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    ldr   sp, =_estack

    /* 将向量表基址（VTOR）显式重定向到主 Flash（0x08000000）。
     * BOOT0=1 时硬件把 0x00000000 映射到 ROM（系统内存），若不在此设置 VTOR，
     * 后续首个中断（SysTick/PendSV）以及 RT-Thread 调度器启动时的 restore-MSP
     * 都会从 ROM 向量表取值，把 CPU 拽回 ROM BootLoader。
     * 此处最早设置后，中断/异常向量与初始栈顶均改从 Flash 向量表读取，
     * 固件即可摆脱对 BOOT0 启动档的依赖，稳定运行。 */
    ldr   r0, =0xE000ED08        /* SCB->VTOR */
    ldr   r1, =0x08000000        /* 主 Flash 基址 */
    str   r1, [r0]

    /* 拷贝 .data 到 RAM */
    ldr   r0, =_sidata
    ldr   r1, =_sdata
    ldr   r2, =_edata
.Lcopy_data:
    cmp   r1, r2
    bge   .Lcopy_done
    ldr   r3, [r0], #4
    str   r3, [r1], #4
    b     .Lcopy_data
.Lcopy_done:

    /* 清零 .bss */
    ldr   r1, =_sbss
    ldr   r2, =_ebss
    movs  r3, #0
.Lzero_bss:
    cmp   r1, r2
    bge   .Lzero_done
    str   r3, [r1], #4
    b     .Lzero_bss
.Lzero_done:

    /* 启动 RT-Thread */
    bl    rtthread_startup
    b     .                              /* 不应返回 */

    .size Reset_Handler, .-Reset_Handler

/* ---------------- 弱默认中断处理 ---------------- */
    .section .text.Default_Handler, "ax", %progbits
    .align 2
    .thumb_func
    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    b     .
    .size Default_Handler, .-Default_Handler

/* 为每个未使用的异常定义弱处理入口 */
    .macro weak_irq name
    .section .text.\name, "ax", %progbits
    .align 2
    .thumb_func
    .weak \name
    .type \name, %function
\name:
    b     .
    .endm

    weak_irq NMI_Handler
    weak_irq MemManage_Handler
    weak_irq BusFault_Handler
    weak_irq UsageFault_Handler
    weak_irq SVC_Handler
    weak_irq DebugMon_Handler
    .end