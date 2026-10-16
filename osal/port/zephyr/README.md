# OSAL → Zephyr 适配层（预留）

## 接入步骤
1. 在 `rtos/zephyr` 挂 Zephyr submodule，并完善其 `CMakeLists.txt`（顶层的
   `RTOS=zephyr` 分支已预留）。
2. 在本目录实现以下文件（遵循 `osal/include/` 中声明的接口语义）：
   - `osal_thread.c`   → 映射至 `k_thread_spawn` / `k_thread_abort` / `k_msleep` 等
   - `osal_sem.c`      → 映射至 K_SEM
   - `osal_mutex.c`    → 映射至 K_MUTEX
   - `osal_queue.c`    → 映射至 K_MSGQ
   - `osal_timer.c`    → 映射至 K_TIMER
   - `osal_memory.c`   → 映射至 `k_malloc` / `k_free`
3. `osal/CMakeLists.txt` 会依据 `RTOS_TYPE` 自动选择本目录，无需改动应用层。