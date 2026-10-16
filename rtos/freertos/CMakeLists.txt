# =============================================================================
# FreeRTOS 后端（预留）
# 未来接入步骤（详见 README）：
#   1. git submodule add https://github.com/FreeRTOS/FreeRTOS-Kernel rtos/freertos
#   2. 在此添加 add_library(rtthread_kernel ...) 等价目标 rtt_kernel，编译
#      FreeRTOS-Kernel 源码，并实现 osal/port/freertos 适配层。
#   3. 顶层 CMakeLists.txt 中的 RTOS=freertos 分支会调用本文件。
# 当前仅占位，接入前会触发 FATAL_ERROR，避免误用。
# =============================================================================
message(FATAL_ERROR "FreeRTOS 后端尚未接入，请按 README 操作。")