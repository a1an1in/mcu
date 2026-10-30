# =============================================================================
# drivers/flash host 单元测试
#
# 被测代码是真实的 w25q128.c + spi_core.c；底座是：
#   fake_w25q.c    16MB NOR 模型（写只能 1→0、页编程回卷、擦除 0xFF、BUSY 轮询）
#   ../spi/fake_osal.c   spi_core 的总线锁走 OSAL，这里复用同一个假实现
# =============================================================================

add_executable(test_w25q128
    test_w25q128.c
    fake_w25q.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../spi/fake_osal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/flash/w25q128.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/spi/spi_core.c)

target_include_directories(test_w25q128 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/flash
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/spi
    ${CMAKE_CURRENT_SOURCE_DIR}/../../osal/include)

add_test(NAME flash_w25q128 COMMAND test_w25q128)
