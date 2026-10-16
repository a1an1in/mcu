# OSAL → FreeRTOS 适配层（预留）

## 接入步骤
1. 在 `rtos/freertos` 挂 FreeRTOS-Kernel submodule，并完善其 `CMakeLists.txt`
   （顶层的 `RTOS=freertos` 分支已预留）。
2. 在本目录实现以下文件（遵循 `osal/include/` 中声明的接口语义）：
   - `osal_thread.c`   → 映射至 `xTaskCreate` / `vTaskDelete` / `vTaskDelay` 等
   - `osal_sem.c`      → 映射至 FreeRTOS 互斥信号量 API
   - `osal_mutex.c`    → 映射至 `xSemaphoreCreateMutex` 等
   - `osal_queue.c`    → 映射至 `xQueueCreate` / `xQueueSend` / `xQueueReceive`
   - `osal_timer.c`    → 映射至 `xTimerCreate` 等
   - `osal_memory.c`   → 映射至 `pvPortMalloc` / `vPortFree`
3. `osal/CMakeLists.txt` 会依据 `RTOS_TYPE` 自动选择本目录，无需改动应用层。