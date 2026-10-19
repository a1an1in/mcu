# GPIO 驱动：统一接口、三种后端与 host 单测

`module`（如 `led.c` / `key.c`）只依赖统一接口 `gpio.h`，具体实现通过 `-DGPIO_BACKEND=` 在构建期切换，
切换时业务模块**零改动**：

| 后端 | 底层所用驱动 | `-DGPIO_BACKEND=` | 运行前提 |
|------|-------------|------------------|---------|
| 自研寄存器 | `stm32f1_gpio.c`（纯寄存器，不依赖 OS/HAL） | `stm32f1`（默认） | 即开即用 |
| RT-Thread 自带 | `rtthread_gpio.c` → RT-Thread `rt_pin_*`（官方 `drv_gpio`） | `rtthread` | RT-Thread 需启用 PIN 设备驱动并注册 |
| Zephyr 自带 | `zephyr_gpio.c` → Zephyr `gpio_pin_*`（device-tree 匹配） | `zephyr` | 需 Zephyr 构建体系与板级 dts |

## 统一接口 `gpio.h`

| 接口 | 用途 |
|------|------|
| `gpio_init()` | 使能各端口时钟 |
| `gpio_output_init(pin)` | 配置为推挽输出 |
| `gpio_input_init(pin, pull)` | 配置为输入并选内部上/下拉，`pull` ∈ `GPIO_IN_FLOAT` / `GPIO_IN_PULLUP` / `GPIO_IN_PULLDOWN`（按键等输入场景） |
| `gpio_write(pin, val)` | 输出电平（非 0 为高） |
| `gpio_read(pin)` | 读引脚电平（非 0 为高） |

引脚编码统一为 `GPIO_PIN(端口, 引脚号)`，即 `(端口 << 8) | 引脚号`。

> 注意 `gpio_input_init()` 的上拉枚举刻意用 `GPIO_IN_*` 而不是 `GPIO_PULL_*`：
> Zephyr 已把 `GPIO_PULL_UP` / `GPIO_PULL_DOWN` 用作宏，`zephyr_gpio.c` 又必须同时包含
> Zephyr 自己的 `gpio.h`，重名会直接编译失败。host 单测能在提交前拦住这类问题。
>
> STM32F1 的细节：输入模式下没有独立的上/下拉寄存器位，方向由 **ODR** 决定
> （CNF=10 输入，ODR=1 上拉 / ODR=0 下拉），所以实现里是「先写 CRL/CRH，再写 BSRR 选方向」。

## host 单测

`tests/` 下分两组，全部用宿主 gcc 编译运行，不需要真硬件：

| 目录 | 覆盖内容 |
|------|---------|
| `tests/gpio/` | 三个后端与 `gpio.h` 的接口一致性。RT-Thread / Zephyr 用**桩头**替换 OS 官方头（include 名保持一致），使适配实现能在 host 上真实编译并运行断言；自研 `stm32f1_gpio.c` 直接访问寄存器地址，host 上运行会段错误，故只做 `-fsyntax-only` 编译冒烟。 |
| `tests/hw/` | 业务模块的**行为**。用 `fake_gpio.c`（一张电平表，可注入外部电平、可读出模块写出的电平）与 `fake_osal.c`（不起真线程）把真机同款 `modules/led`、`modules/key` 编进来跑，断言「按键 → LED」整条链路：KEY0 按住→DS0 亮、KEY1 按住→DS1 亮、KEY2 按一下产生**恰好一个**按下边沿（切换键最怕抖动变连击），以及消抖、边沿读后清除、低电平点亮极性。 |

```bash
python3 tools/devops.py build --host   # 编译固件 + 编译 host 单测程序（仅编译，不运行）
python3 tools/devops.py test           # 只运行已编译的 host 单测（ctest）
```

> 真机行为（引脚是否真的有电平、按键手感）仍需烧到板子上验证；host 单测保证的是
> 「上层逻辑与极性没写反」，这两层互补。
