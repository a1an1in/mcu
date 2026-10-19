# GPIO 驱动：三种后端选择与测试

`module`（如 `led.c`）只依赖统一接口 `gpio.h`，具体实现通过 `-DGPIO_BACKEND=` 在构建期切换，
切换时业务模块**零改动**：

| 后端 | 底层所用驱动 | `-DGPIO_BACKEND=` | 运行前提 |
|------|-------------|------------------|---------|
| 自研寄存器 | `stm32f1_gpio.c`（纯寄存器，不依赖 OS/HAL） | `stm32f1`（默认） | 即开即用 |
| RT-Thread 自带 | `rtthread_gpio.c` → RT-Thread `rt_pin_*`（官方 `drv_gpio`） | `rtthread` | RT-Thread 需启用 PIN 设备驱动并注册 |
| Zephyr 自带 | `zephyr_gpio.c` → Zephyr `gpio_pin_*`（device-tree 匹配） | `zephyr` | 需 Zephyr 构建体系与板级 dts |

`zephyr_gpio.c` / `rtthread_gpio.c` 的 include 名与对应 OS 官方一致，真机上用 OS 自身头编译；
本工程 `tests/gpio/` 用**桩头**替换 OS 头，使这两个适配实现能在 host 上真实编译并运行断言。

```bash
python3 tools/devops.py build --host   # 编译固件 + 编译 host 单测程序（仅编译，不运行）
python3 tools/devops.py test           # 只运行已编译的 host 单测
```

> 说明：自研 `stm32f1_gpio.c` 直接访问寄存器地址，host 上运行会段错误，故仅做编译冒烟
> （`-fsyntax-only`）；ROM 运行时测试需接入对应 OS 驱动栈/工具链或真机。