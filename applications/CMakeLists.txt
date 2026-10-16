# =============================================================================
# 应用层（applications）：主程序与系统级编排
# 只依赖 OSAL 与模块提供的接口，对 RTOS 零依赖。
# =============================================================================

add_library(applications OBJECT main.c)

# include 各模块头（hello.h / led.h 等）
target_include_directories(applications PUBLIC
    ${CMAKE_SOURCE_DIR}/modules/hello
    ${CMAKE_SOURCE_DIR}/modules/led
)

target_link_libraries(applications PRIVATE modules osal)