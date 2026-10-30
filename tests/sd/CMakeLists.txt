# =============================================================================
# drivers/sd host 单元测试（只测纯逻辑：CSD / CID 解析）
#
# sd.c 里只有 sd_parse_csd / sd_parse_cid 不碰硬件；它们所在的目标文件里
# 还引用了 osal_mutex_*（总线锁），所以链接时补一个假 OSAL 实现。
# =============================================================================

add_executable(test_sd_parse
    test_sd_parse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../spi/fake_osal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/sd/sd.c)

target_include_directories(test_sd_parse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/sd
    ${CMAKE_CURRENT_SOURCE_DIR}/../../osal/include)

add_test(NAME sd_parse COMMAND test_sd_parse)
