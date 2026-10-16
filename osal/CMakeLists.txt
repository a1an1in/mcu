# =============================================================================
# OS 抽象层 (OSAL)
#   - include/    统一接口头文件（与 RTOS 无关）
#   - port/<os>/  各 RTOS 的适配实现（由顶层 CMakeLists 中的 RTOS_TYPE 选择）
# 应用层只依赖本库 + include 头，不直接接触 RTOS，实现可扩展性。
# =============================================================================

set(OSAL_PORT_DIR ${CMAKE_SOURCE_DIR}/osal/port/${RTOS_TYPE})

set(OSAL_SRCS
    ${OSAL_PORT_DIR}/osal_thread.c
    ${OSAL_PORT_DIR}/osal_sem.c
    ${OSAL_PORT_DIR}/osal_mutex.c
    ${OSAL_PORT_DIR}/osal_queue.c
    ${OSAL_PORT_DIR}/osal_timer.c
    ${OSAL_PORT_DIR}/osal_memory.c
    ${OSAL_PORT_DIR}/osal_print.c
)

add_library(osal OBJECT ${OSAL_SRCS})

# 对外头文件目录（传播给 modules/applications）
target_include_directories(osal PUBLIC ${CMAKE_SOURCE_DIR}/osal/include)

# 使 osal 编译时能 #include <rtthread.h> 等内核头（依赖 rtt_kernel 的 PUBLIC include）
target_link_libraries(osal PRIVATE ${RTOS_KERNEL_TARGET})