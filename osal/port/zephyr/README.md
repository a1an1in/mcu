# OSAL → Zephyr 适配层（已实现）

本目录把 `osal/include/` 声明的统一接口映射到 **Zephyr 4.x 内核 API**，使
`applications/` 与 `modules/` 在 Zephyr 下**零改动**运行。与 `osal/port/rt-thread/`
一一对应，共 7 个文件。

| 文件            | OSAL 接口        | Zephyr 底层实现                          | 需要的 Kconfig            |
|-----------------|------------------|-------------------------------------------|---------------------------|
| `osal_thread.c` | 线程             | `k_thread_create`+`k_thread_stack_alloc`+`k_thread_abort`+`k_yield`+`k_msleep` | `CONFIG_THREAD_STACK_ALLOCATOR=y` |
| `osal_sem.c`    | 信号量           | `k_sem_init`/`k_sem_give`/`k_sem_take`（k_malloc 动态） | `CONFIG_SYSTEM_HEAP_SIZE>0` |
| `osal_mutex.c`  | 互斥量           | `k_mutex_init`/`k_mutex_lock`/`k_mutex_unlock`         | 同上                    |
| `osal_queue.c`  | 消息队列         | `k_msgq_alloc_init`/`k_msgq_put`/`k_msgq_get`/`k_msgq_cleanup` | 同上                 |
| `osal_timer.c`  | 定时器           | `k_timer_init`/`k_timer_start|stop`（运行时 ctx 记回调/周期） | —                      |
| `osal_memory.c` | 动态内存         | `k_malloc`/`k_free`                         | `CONFIG_SYSTEM_HEAP_SIZE>0` |
| `osal_print.c`  | 打印             | `vprintk`（→ console 后端）                 | —                        |

## 语义对照

- **优先级**：OSAL 数值越小优先级越高；Zephyr 抢占优先级为非负值、0 最高。
  适配层做 `K_PRIO_PREEMPT(min(prio, CONFIG_NUM_PREEMPT_PRIORITIES-1))` 直接映射。
- **线程栈**：OSAL 允许 `stack=NULL`（OS 自管），Zephyr 通过 `k_thread_stack_alloc`
  动态分配；`k_thread_delete` 时用 `CONTAINER_OF` 从 `struct k_thread` 还原包装结构，
  正确归还栈并释放。
- **超时**：`OSAL_NO_WAIT/OSAL_WAIT_FOREVER` 分别映射到 `K_NO_WAIT/K_FOREVER`，否则
  `K_MSEC(ms)`。

## 说明

- Zephyr 是**构建系统宿主**，因此本 port 不参与顶层 `CMakeLists.txt` 的
  `add_subdirectory`，而是被 `applications/zephyr/`（`--rtos zephyr` 的应用入口）以
  `target_sources` 方式编译。用法见 `applications/zephyr/README.md`。