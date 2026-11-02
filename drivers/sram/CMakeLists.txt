# 外部 SRAM 驱动：接口 sram.h + STM32F1 FSMC 实现
add_library(sram OBJECT stm32f1_fsmc_sram.c)

target_include_directories(sram PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
