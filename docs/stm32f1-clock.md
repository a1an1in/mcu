# STM32F1 系统时钟：从 HSI 8MHz 到 72MHz

本文记录本工程**为什么、怎么**把系统时钟从复位默认的 HSI 8MHz 切到
HSE 8MHz × PLL9 = 72MHz，以及切换过程中真实踩到的两个坑。

对应代码：`bsp/stm32f103/board.c` 的 `system_clock_init()`，在
`rt_hw_board_init()` 的**第一步**调用。

---

## 1. 时钟树速览

```
          ┌──────────┐
 HSI 8MHz │          │
 （内部）  │          │
          │          ├──(SW)──► SYSCLK ──(HPRE)──► HCLK ──┬──(PPRE1)──► PCLK1 (APB1)
 HSE 8MHz │          │                                     │
 （外部晶振）└───┬──────┘                                     └──(PPRE2)──► PCLK2 (APB2)
                │
        (PLLSRC / PLLXTPRE / PLLMUL)
                ▼
              PLL ──────────────► 也是 SW 的一个可选输入
```

本工程的取值：

| 项目 | 值 | 由谁决定 |
|---|---|---|
| SYSCLK | **72MHz** | HSE 8MHz × PLLMUL(×9) |
| HCLK (AHB) | 72MHz | HPRE = /1 |
| PCLK1 (APB1) | **36MHz** | PPRE1 = /2（F103 的 APB1 上限就是 36MHz） |
| PCLK2 (APB2) | 72MHz | PPRE2 = /1 |
| ADCCLK | 12MHz | ADCPRE = /6（不用 ADC，但按规范设） |
| Flash 等待 | **2 个等待周期** | 48MHz < SYSCLK ≤ 72MHz |

> 为什么 APB1 一定要 /2：APB1 上挂着 I2C、USART2~5、SPI2/3、CAN 等，
> F103 的 APB1 最高只能 36MHz。72MHz 不分频会把这些外设跑超频。
> 而 APB2（GPIO、SPI1、USART1、ADC、SDIO 的控制逻辑）允许 72MHz。

---

## 2. 三个寄存器

### 2.1 `RCC_CR`（0x40021000）—— 振荡器开关与就绪标志

| 位 | 名称 | 含义 |
|---|---|---|
| 0 | HSION | 内部 8MHz 振荡器开 |
| 1 | HSIRDY | 内部振荡器稳定（**只读**） |
| 15:8 | HSICAL | 出厂校准值（**只读**） |
| 7:3 | HSITRIM | 内部振荡器微调 |
| 16 | HSEON | 外部晶振开（**写 1 启动**） |
| 17 | HSERDY | 外部晶振稳定（**只读**，等它变 1） |
| 18 | HSEBYP | 旁路模式（外部直接给时钟、不用晶振时才置位） |
| 19 | CSSON | 时钟安全系统（晶振失效自动切回 HSI，本工程未开） |
| 24 | PLLON | PLL 开 |
| 25 | PLLRDY | PLL 锁定（**只读**，等它变 1） |

规律：**每一位“使能”都有一个对应的“就绪”位，写使能后必须轮询就绪位**，
否则后续操作会用到还没稳定的时钟。

### 2.2 `RCC_CFGR`（0x40021004）—— 分频、PLL 倍频、时钟源选择

| 位 | 名称 | 含义 |
|---|---|---|
| 1:0 | SW | 系统时钟源：00=HSI，01=HSE，10=PLL |
| 3:2 | SWS | **当前实际生效**的时钟源（只读，与 SW 对应） |
| 7:4 | HPRE | AHB 分频 |
| 10:8 | PPRE1 | APB1 分频 |
| 13:11 | PPRE2 | APB2 分频 |
| 15:14 | ADCPRE | ADC 分频：00=/2 01=/4 10=/6 11=/8 |
| 16 | PLLSRC | PLL 输入：0=HSI/2，1=HSE |
| 17 | PLLXTPRE | HSE 再分频：0=HSE，1=HSE/2 |
| 21:18 | PLLMUL | 倍频系数：0000=×2 … 0111=×9 … 1110=×16 |
| 22 | USBPRE | USB 分频（本工程不用 USB） |
| 26:24 | MCO | 时钟输出引脚（调试用，可把 SYSCLK 输出到 PA8 用示波器看） |

分频编码（HPRE / PPRE1 / PPRE2 通用）：

| 编码 | 分频 |
|---|---|
| 0xx | /1 |
| 100 | /2 |
| 101 | /4 |
| 110 | /8 |
| 111 | /16 |

（HPRE 还多两档：1100=/64、1101=/128、1110=/256、1111=/512）

### 2.3 `FLASH_ACR`（0x40022000）—— Flash 等待周期

| 位 | 名称 | 含义 |
|---|---|---|
| 2:0 | LATENCY | 等待周期数：0=0WS，1=1WS，2=2WS |
| 3 | HLFCYA | 半周期访问（**只能在低频、且不与预取同时开**） |
| 4 | PRFTBE | 预取缓冲使能 |
| 5 | PRFTBS | 预取状态（只读） |

对应关系（RM0008）：

| SYSCLK | 等待周期 |
|---|---|
| 0 ~ 24MHz | 0 |
| 24 ~ 48MHz | 1 |
| 48 ~ 72MHz | **2** |

**Flash 的读取速度跟不上 72MHz 的 CPU**，所以必须让 CPU 在取指时等 2 个
周期。这一条没设，就是“时钟一切过去立刻跑飞”。

---

## 3. 切换顺序（每一步都有原因）

```c
FLASH_ACR = LATENCY_2 | PRFTBE;      /* ① 先加等待周期 + 开预取 */
RCC_CR   |= HSEON;                   /* ② 开外部晶振 */
while (!HSERDY) ...                  /*    等它稳定 */
RCC_CFGR  = 分频 + PLLSRC + PLLMUL;  /* ③ 配分频和 PLL 参数（PLL 还关着） */
RCC_CR   |= PLLON;                   /* ④ 开 PLL */
while (!PLLRDY) ...                  /*    等锁定 */
RCC_CFGR  = (CFGR & ~0x3) | 0x2;     /* ⑤ SW = PLL */
while (SWS != 0x2) ...               /*    等切换真正生效 */
```

为什么是这个顺序：

- **① 必须最先**：72MHz 的取指速度超过 Flash 的读取能力，等待周期没设好，
  一旦提速 CPU 读到的指令就是错的。
- **③ 必须在 ④ 之前**：PLL 一旦锁定，它的倍频/分频参数就**不能再改**
  （F1 要求改 PLLMUL/PLLSRC 时 PLLON=0）。所以先把参数写好，再开 PLL。
- **③ 里的分频也要在提速前配好**：APB1 一旦超过 36MHz，挂在上面的外设
  （I2C/USART2/CAN…）行为就不可预期。
- **⑤ 之后要等 SWS**：SW 只是“请求”，SWS 才是“已经切过去”。不等 SWS
  就继续跑，后续代码可能还在旧时钟下执行。

---

## 4. 代码走读

`bsp/stm32f103/board.c`：

```c
static int system_clock_init(void)
{
    /* ① Flash：LATENCY=2 + 预取。注意不能加 HLFCYA（见第 6 节坑 1） */
    FLASH_ACR = FLASH_ACR_LATENCY_2 | FLASH_ACR_PREFETCH;

    /* ② HSE 起振，失败就关掉返回 -1，让系统继续跑 HSI 8MHz */
    RCC_CR |= (1u << 16);
    timeout = 0x40000u;
    while ((RCC_CR & (1u << 17)) == 0u) {
        if (--timeout == 0u) { RCC_CR &= ~(1u << 16); return -1; }
    }

    /* ③ AHB /1、APB1 /2、APB2 /1、ADC /6、PLL 源=HSE、倍频 ×9 */
    RCC_CFGR = (RCC_CFGR & ~0x003FFFF0u) |
               (0x4u << 8) | (0x2u << 14) | (1u << 16) | (0x7u << 18);

    /* ④ PLL 开 + 等锁定 */
    RCC_CR |= (1u << 24);
    timeout = 0x40000u;
    while ((RCC_CR & (1u << 25)) == 0u) {
        if (--timeout == 0u) { RCC_CR &= ~(1u << 24); return -1; }
    }

    /* ⑤ 切到 PLL + 等 SWS */
    RCC_CFGR = (RCC_CFGR & ~0x3u) | 0x2u;
    timeout = 0x40000u;
    while (((RCC_CFGR >> 2) & 0x3u) != 0x2u) {
        if (--timeout == 0u) return -1;
    }
    return 0;
}
```

调用点（`rt_hw_board_init` 的最开头）：

```c
clock_ok = system_clock_init();

uart_hw_init(UART_BAUD, rcc_pclk2_hz());   /* 波特率按切换后的 PCLK2 算 */

if (clock_ok != 0) {
    rt_kprintf("[clock] HSE/PLL 未就绪，退回 HSI %lu Hz\n", rcc_sysclk_hz());
} else {
    rt_kprintf("[clock] SYSCLK %lu Hz（HCLK %lu / PCLK1 %lu / PCLK2 %lu）\n", ...);
}
```

**回退策略**：HSE 或 PLL 超时就返回 -1，此时 CFGR 还是 HSI 的配置，系统照常
以 8MHz 运行。因为所有驱动都是运行时读 `RCC_CFGR` 推算频率（下一节），
所以**一行都不用改**。

---

## 5. 各驱动怎么自动跟上

board.c 里有三个推算函数，全部**运行时读 RCC_CFGR**，不写死：

```c
rcc_sysclk_hz()   /* 读 SWS 判断当前时钟源；PLL 时按 PLLSRC/PLLXTPRE/PLLMUL 算 */
rcc_ahb_hz()      /* 再按 HPRE 分频 */
rcc_pclk1_hz()    /* AHB 再按 PPRE1（APB1） */
rcc_pclk2_hz()    /* AHB 再按 PPRE2（APB2） */
```

注入方式：

| 驱动 | 注入的参数 | 影响 |
|---|---|---|
| UART | `uart_hw_init(115200, rcc_pclk2_hz())` | 波特率 BRR = PCLK2 / baud |
| SysTick | `rcc_ahb_hz() / RT_TICK_PER_SECOND` | 1ms 节拍 |
| I2C bit-bang | `stm32f1_i2c_gpio_init(rcc_ahb_hz())` | DWT 把周期数换算成 µs |
| SPI (25Q128) | `stm32f1_spi_init(bus, pclk, hclk, …)` | 波特率分频 BR |
| SDIO | `stm32f1_sdio_init(rcc_ahb_hz())` | CLKDIV = HCLK/CK − 2 |
| CH395 SPI1 | 驱动内部读 `RCC_CFGR` 现算 | 见下 |

**唯一需要特别注意的是“写死分频”的地方**。CH395 原来把 SPI1 固定成
`fPCLK2/2`，8MHz 时是 4MHz 没问题，但 72MHz 时就成了 36MHz，超过 CH395
的 ~30MHz 上限。现在改成 `ch395_spi_br()` 按 PCLK2 现算（取不超过 18MHz
的最快档）：8MHz 时仍是 /2，72MHz 时自动变成 /4 = 18MHz。

> 经验：**驱动里凡是写死“分频系数”的地方，换主频都会坏**。
> 应该写“我要多少 Hz”，由驱动按实际总线频率算分频。

---

## 6. 两个真实踩坑

### 坑 1：`FLASH_ACR` 的半周期访问和预取互斥

最初我按“多开点总没坏处”的想法写成：

```c
FLASH_ACR = LATENCY_2 | HLFCYA | PRFTBE;   /* ❌ */
```

结果时钟一切到 72MHz，**PC 直接跳到 0x00000000**（取指拿了垃圾）。
用调试器读回来：`RCC_CFGR` 明明是 72MHz、PLL 也锁定了，就是跑飞。

原因：`HLFCYA`（半周期访问）只用于**低频**，且 RM0008 明确它不能和预取
同时使能。两者都置位后 Flash 访问时序是坏的。

正确写法只有 `LATENCY_2 | PRFTBE`。

### 坑 2：串口初始化之前打印会死循环

```c
if (system_clock_init() != 0) {
    rt_kprintf("[clock] 退回 HSI...\n");   /* ❌ 此时 USART1 还没配 */
}
uart_hw_init(...);
```

`rt_kprintf` → `rt_hw_console_output` → `uart_puts` → 里面有
`while (!(SR & TXE))`。**USART1 的时钟还没使能**，寄存器读回来恒为 0，
TXE 永远不置位 → 死循环。

表现就是“改了时钟之后板子起不来了”，而且完全没有串口输出，看起来像时钟
配置本身有问题。

正确做法：**先初始化串口，再打印时钟状态**（用 `clock_ok` 暂存结果）。

---

## 7. 想换成别的主频怎么改

改 `system_clock_init()` 里的两处：分频/倍频字段和 `FLASH_ACR`。

| 目标 SYSCLK | PLL 配置 | 等待周期 | 备注 |
|---|---|---|---|
| 8MHz | 不切，保持 HSI | 0 | 直接不调用 `system_clock_init()` 即可 |
| 24MHz | HSE ×3（PLLMUL=1） | 0 | APB1 可以 /1（24MHz < 36MHz） |
| 48MHz | HSE ×6（PLLMUL=4） | 1 | APB1 必须 /2（24MHz） |
| **72MHz** | HSE ×9（PLLMUL=7） | **2** | APB1 必须 /2（36MHz） |

改的时候注意：

1. `PLLMUL` 编码 = 倍数 − 2（×9 → 0b0111 = 7）。
2. APB1 永远 ≤ 36MHz。
3. Flash 等待周期按上表跟着改，**先改它再提速**。
4. 改完顺手确认依赖主频的东西：`SD_TRANSFER_CLOCK_HZ`（SDIO ≤ 48MHz）、
   `CH395_SPI_MAX_HZ`（≤ 30MHz）、`SPI` 请求速率。

---

## 8. 怎么验证时钟真的切过去了

**方法一：看启动日志**（本工程已加）

```
[clock] SYSCLK 72000000 Hz（HCLK 72000000 / PCLK1 36000000 / PCLK2 72000000）
```

**方法二：直接读寄存器**（DAP / OpenOCD）

```
mdw 0x40021004 1     # RCC_CFGR
```

期望 `0x001D840A`：

| 字段 | 值 | 含义 |
|---|---|---|
| SW / SWS | 0b10 / 0b10 | 系统时钟 = PLL，且已生效 |
| HPRE | 0 | AHB /1 |
| PPRE1 | 4 | APB1 /2 |
| PPRE2 | 0 | APB2 /1 |
| ADCPRE | 2 | ADC /6 |
| PLLSRC | 1 | PLL 输入 = HSE |
| PLLMUL | 7 | ×9 |

**方法三：MCO 输出**（要示波器）
把 `RCC_CFGR[26:24]` 设成 `100`（SYSCLK），PA8 上就能量到 72MHz 方波。
这是最硬的验证，但需要改代码。

---

## 9. 故障对照表

| 现象 | 最可能的原因 |
|---|---|
| 完全没有串口输出，像死机 | 在 `uart_hw_init()` 之前打印（坑 2）；或 Flash 等待周期没设 |
| 能启动，但很快 HardFault / PC=0 | `FLASH_ACR` 配错（坑 1）；或 APB1 超过 36MHz |
| 串口乱码 | UART 初始化时用的 PCLK2 与实际时钟不一致（应该用切换后的值） |
| 某些外设时好时坏 | 某处写死了分频系数，实际总线频率变了但分频没跟着变 |
| HSE 起振超时（日志提示退回 HSI） | 晶振没焊/坏了；或 `HSEBYP` 该置位（外部有源晶振时） |
| 系统能跑但 SysTick 不准 | SysTick LOAD 没按新的 HCLK 重算 |
