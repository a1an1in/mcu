# =============================================================================
# 应用层（applications）：主程序与系统级编排
# 只依赖 OSAL 与模块提供的接口，对 RTOS 零依赖。
#
# ⚠️ 装配方式分两种（本文件只服务于「裸 CMake 模式」）：
#   1) 裸 CMake 后端（RT-Thread / 未来 FreeRTOS）：由根 CMakeLists.txt
#      add_subdirectory(applications) 调用本文件 → add_library(applications OBJECT main.c)。
#   2) Zephyr（独立构建宿主）：**不调用本 CMakeLists.txt**，而是由
#      applications/zephyr/CMakeLists.txt 用 target_sources 直接把 ../main.c 编入
#      Zephyr 的 app 目标。两侧共用 main.c 同一份源码。
# =============================================================================

add_library(applications OBJECT main.c)

# include 各模块头（hello.h / led.h / key.h 等）
target_include_directories(applications PUBLIC
    ${CMAKE_SOURCE_DIR}/modules/hello
    ${CMAKE_SOURCE_DIR}/modules/led
    ${CMAKE_SOURCE_DIR}/modules/key
)

target_link_libraries(applications PRIVATE modules osal)