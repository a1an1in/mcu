# =============================================================================
# drivers/spi host 单元测试（只测 core，不碰寄存器）
#
# spi_core.c 只依赖 spi.h + <osal.h>（总线锁），所以 host 上：
#   - 直接把 spi_core.c 编进来；
#   - 用一个假控制器提供 transfer_one / setup / set_cs；
#   - 用 fake_osal.c 提供 osal_mutex_*（统计加解锁次数，验证不泄漏）。
# =============================================================================

add_executable(test_spi_core
    test_spi_core.c
    fake_osal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/spi/spi_core.c)

target_include_directories(test_spi_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/spi
    ${CMAKE_CURRENT_SOURCE_DIR}/../../osal/include)

add_test(NAME spi_core COMMAND test_spi_core)
