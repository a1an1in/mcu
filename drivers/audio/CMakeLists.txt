# =============================================================================
# 音频组件：VS1053B 解码器（SPI1，与 CH395 共用总线）
#
#   vs1053.h        芯片接口：SCI 寄存器 / SDI 数据 / 音量 / 切歌 / 正弦测试
#   vs1053.c        芯片协议（OS 无关，用 drivers/spi 的 spi_device）
#   vs1053_config.h 板级事实（XCS/XDCS/DREQ/RST 引脚、SPI 速率）
#
# 文件系统与播放线程在 modules/music，不在本组件里 —— 本组件只认「字节流」。
# =============================================================================

add_library(audio OBJECT
    vs1053.c
)

target_include_directories(audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/drivers/spi
    ${CMAKE_SOURCE_DIR}/drivers/gpio
)

# 毫秒延时走 OSAL（不引 RTOS 头）
target_link_libraries(audio PRIVATE osal)
