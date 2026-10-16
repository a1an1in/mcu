# 如何新增一个业务模块

1. 在 `modules/` 下新建 `xxx/`，实现 `xxx.c`（内部只 `#include <osal.h>`）与 `xxx.h`。
2. 在 `modules/CMakeLists.txt` 的 `add_library(modules OBJECT ...)` 中加入源文件。
3. 在 `applications/main.c` 中调用模块的初始化函数。

示例：

```c
/* modules/xxx/xxx.c */
#include <osal.h>
void xxx_start(void) {
    osal_thread_t t;
    osal_thread_create(&t, "xxx", xxx_entry, NULL, NULL, 2048, 15);
}
```

> 说明：业务模块只依赖 `osal.h` 与 `drivers/*`，与具体 RTOS 无关，
> 因此切换 RTOS 后端时业务代码零改动。