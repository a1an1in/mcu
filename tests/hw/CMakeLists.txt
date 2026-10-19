# =============================================================================
# 业务模块 host 单元测试（由 tests/CMakeLists.txt add_subdirectory 引入）
#
# modules/led 与 modules/key 本身只依赖 <osal.h> + "gpio.h"，
# 因此 host 上只要给出这两个接口的「假实现」，整条链路就能真机之外真实跑起来：
#
#   fake_gpio.c  实现 gpio.h（电平表：可注入外部电平、可读出模块写出的电平）
#   fake_osal.c  实现用到的 OSAL（不起真线程，由测试手动调 key_poll_once() 推进采样）
#
# 覆盖链路：KEY1(PE3) 按下 → key_is_down() 消抖 → led_set(LED_DS0) → PB5 输出低（点亮）
# =============================================================================

add_executable(test_key_led
    test_key_led.c
    fake_gpio.c
    fake_osal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/led/led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/key/key.c)

target_include_directories(test_key_led PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../osal/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers/gpio
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/led
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/key)

add_test(NAME hw_key_led COMMAND test_key_led)
