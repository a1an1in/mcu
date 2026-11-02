# ADC 驱动：接口 adc.h + STM32F1 实现（ADC1 + TIM3 TRGO + DMA1）
add_library(adc OBJECT stm32f1_adc.c)

target_include_directories(adc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
