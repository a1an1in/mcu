# =============================================================================
# NOR FLASH 器件组件：W25Q128 / NM25Q128（SPI，16MB / 256B 页 / 4KB 扇区）
#
# 分层（详见 docs/drivers.md 的四维模型）：
#   w25q128.h          接口，芯片 / OS 无关
#   w25q128.c          器件协议：读、按页写、扇区/整片擦除、JEDEC ID、BUSY 轮询
#   w25q128_config.h   板级事实：片选引脚、SPI 模式、速率上限、轮询上限
#
# 没有 SoC 层：总线已被 spi 组件抽象成 spi_message / spi_sync，本器件不碰寄存器。
# 没有 OS 层：等 BUSY 用「反复读状态寄存器」轮询，不依赖任何 OS 延时。
# =============================================================================

add_library(flash OBJECT
    w25q128.c
)

# 组件自身需要找到同目录的接口头 w25q128.h / 板级事实 w25q128_config.h
target_include_directories(flash PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 器件协议建立在 spi.h 之上（只用到头文件与 spi_sync 等符号）
target_link_libraries(flash PRIVATE spi)
