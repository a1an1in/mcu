/*
 * board.h - 板级支持（STM32F103）
 */
#ifndef BSP_BOARD_H__
#define BSP_BOARD_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 本工程以 STM32F103（Blue Pill 常见为 F103C8T6）为目标。
 * 请根据实际芯片的 Flash/SRAM 调整以下宏。 */
#define STM32_FLASH_START       ((unsigned long)0x08000000u)
#define STM32_FLASH_SIZE        (64u * 1024u)        /* 64KB（如为 128KB 请修改） */
#define STM32_FLASH_END         (STM32_FLASH_START + STM32_FLASH_SIZE)

#define STM32_SRAM_START        ((unsigned long)0x20000000u)
#define STM32_SRAM_SIZE         (20u * 1024u)        /* 20KB */
#define STM32_SRAM_END          (STM32_SRAM_START + STM32_SRAM_SIZE)

/* 系统堆：从 .bss 末尾到 SRAM 末尾 */
extern unsigned long __bss_end;
#define HEAP_BEGIN              ((void *)&__bss_end)
#define HEAP_END                ((void *)STM32_SRAM_END)

/* 目标主频（HSI 8MHz，见 board.c 时钟配置） */
#define SYSTEM_CLOCK_HZ         (8000000u)

#ifdef __cplusplus
}
#endif

#endif /* BSP_BOARD_H__ */