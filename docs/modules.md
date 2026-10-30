# 如何新增一个业务模块

1. 在 `modules/` 下新建 `xxx/`，实现 `xxx.c`（内部只 `#include <osal.h>` 与 `drivers/*` 的接口头）与 `xxx.h`。
2. 在 `modules/CMakeLists.txt` 的 `add_library(modules OBJECT ...)` 中加入源文件。
3. 在 `applications/CMakeLists.txt` 里把 `modules/xxx` 加进 `target_include_directories(applications ...)`。
4. **如果该模块要与 Zephyr 后端共存**（`applications/zephyr/` 复用同一份 `applications/main.c`），
   还要在 `applications/zephyr/CMakeLists.txt` 的 `target_sources(app ...)` 与
   `zephyr_include_directories(...)` 里加上该模块 —— 否则切到 Zephyr 会因为找不到符号/头文件而链接失败。
5. 在 `applications/main.c` 中调用模块的初始化函数。
6. 给模块补一个 host 单测（见 `tests/hw/`）：模块只依赖 OSAL + 驱动接口，所以用假实现
   （`fake_gpio.c` / `fake_osal.c` 那一套）就能在宿主机上把逻辑真实跑一遍。

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
>
> 现成的两个例子：`modules/led`（板载 LED，低电平点亮）与 `modules/key`
> （按键，内部上/下拉 + 20ms 消抖，采样推进放在 `key_poll_once()` 便于 host 测试）。
>
> 第三方协议模块的范例见 `modules/fs`（FatFs）——它同样不碰 OS，靠
> `drivers/block` 的块设备接口和 host 单测（RAM 盘）做到可离线验证，
> 详见 `docs/fatfs.md`。
