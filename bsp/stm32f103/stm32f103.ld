/*
 * stm32f103.ld - STM32F103 链接脚本（Cortex-M3）
 *
 * 内存布局请与 bsp/board.h 保持一致：
 *   FLASH 64KB @ 0x08000000，SRAM 20KB @ 0x20000000
 * 若你的芯片容量不同，请同步修改两处宏。
 */
ENTRY(Reset_Handler)

MEMORY
{
    ROM (rx)  : ORIGIN = 0x08000000, LENGTH = 64K
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

/* 栈顶 = RAM 末尾（向下生长） */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* 中断向量表与启动代码 */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > ROM

    /* 代码与只读数据 */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
    } > ROM

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);

        /* 当前未启用 FINSH / RT_USING_COMPONENTS_INIT，无需这些段；
         * 保留占位，便于后续启用组件初始化时无需改动链接脚本。 */
        __rt_init_start = .;
        KEEP(*(SORT(.rti_fn*)))
        __rt_init_end = .;
        __fsymtab_start = .;
        KEEP(*(FSymTab))
        __fsymtab_end = .;
        __vsymtab_start = .;
        KEEP(*(VSymTab))
        __vsymtab_end = .;
        . = ALIGN(4);
    } > ROM

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > ROM
    __exidx_end = .;

    /* 初始化数据：加载镜像在 FLASH，运行时拷贝到 RAM */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        *(.gnu.linkonce.d*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > ROM

    /* 零初始化段 */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end = .;      /* 供 bsp/board.h 的 HEAP_BEGIN（系统堆起点）使用 */
    } > RAM

    . = ALIGN(4);
    _end = .;
}