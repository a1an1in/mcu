# 串口一开就进 ROM / 板子像"死机"——原因与解决

> 适用：正点原子 战舰 STM32F103 V4（`WARSHIP_DNF103P`），板载 CH340 串口。

## 一句话结论

**不是固件的问题，是 BOOT0 引脚在"复位那一瞬间"的电平被改掉了。**
这块板的 BOOT0 除了跳线帽，还并联着 CH340 的"一键下载"电路——串口软件（MobaXterm / PuTTY / SSCOM…）打开端口时会自动拉 DTR/RTS，这两个信号经两个三极管把 BOOT0 拉高，于是芯片复位后跑去执行 **ROM 里的出厂 bootloader**，而不是你烧进 Flash 的程序。

---

## 1. STM32 从哪儿启动，只看 BOOT0/BOOT1

复位后第 4 个 SYSCLK，芯片采样这两个引脚：

| BOOT0 | BOOT1 | 启动位置 | 你看到的现象 |
|---|---|---|---|
| **0** | x | Flash `0x0800_0000` | 跑你的程序，串口有启动日志 |
| **1** | 0 | 系统存储器（ROM）`0x1FFF_F000` | 跑出厂 bootloader，**串口无任何日志** |

所以：**跟你烧进去的是谁的 bin、固件怎么写，完全无关**，只跟复位那一刻 BOOT0 是高还是低有关。

用调试器 `reset halt` 可以一眼区分：

- 进 Flash：`PC` = 你的 `Reset_Handler`（本工程 `0x080012f0` 附近），`0x00000000` 处是**你的**向量表。
- 进 ROM：`PC` = `0x1FFF_F0xx`，`0x00000000` 处是 ROM 向量表 `200001fc 1ffff021`。

## 2. 这块板的特殊之处：BOOT0 被 CH340 接管了

原理图 `POWER&USB_USART` 页：

```
CH340C (U14)  DTR# / RTS#  ──►  Q2(S8050) / Q3(S8550)  ──►  R49(1K)  ──►  BOOT0
                                                            │
                                          Q3 导通 = 把 BOOT0 拉到 VCC3.3
```

这就是所谓的**一键下载**：上位机操作 DTR/RTS 就能让 MCU 进 ROM，配合串口助手烧录程序。

另外注意 `R49` 只有 **1K**，而 BOOT 排针那条路是 `R11` **10K**——所以"用跳线帽把 BOOT0 接到 GND"是压不住它的，必须动 BOOT0 网络本身。

## 3. 【常见误解】"我没用一键下载啊" —— 它不是功能，是常接的一根线

这是最容易误会的地方：

- **它不是"你点了一键下载才接通"的开关，而是永远并联在 BOOT0 上的一条硬件电路。** 只要 `Q3` 一导通，BOOT0 就被拉高——不管你在下载、在看日志、还是什么都没干。
- **让 `Q3` 导通的是 DTR / RTS**。这两个是串口的**控制线**（不是数据线）。**串口软件一打开端口就会自动设置它们**，且多数软件的默认值就是拉高——界面上不显示，你也感觉不到。
- 所以"我没动跳线"并不代表 BOOT0 没被改：**是软件替你拨了开关。**

"一键下载软件" 和 "普通串口终端" 的区别：

| | 怎么用 DTR/RTS | 结果 |
|---|---|---|
| **下载软件**（FlyMcu 等） | 有个**时序**：先按住复位 → 拉 BOOT0 → 放开复位 | 主动进 ROM 等下载 |
| **普通串口终端**（MobaXterm / PuTTY / SSCOM…） | 打开端口时**静静地**把 DTR/RTS 拉高 | **它并不下载**，但 BOOT0 已经变高 → 下次复位就进 ROM |

**实证**：用 MobaXterm 只是"开着串口看日志"，一个下载动作都没做，板子照样卡死没日志。可见**根本不需要"用一键下载"**。

## 4. DTR / RTS 到底怎么影响 RESET 和 BOOT0

### 4.1 先澄清：软件里的 "1" ≠ 引脚上的 "1"

原理图上写的是 **`DTR#` / `RTS#`**，`#` 表示**低电平有效**。

- 软件里"断言"（`DtrEnable=$true` / 勾选 DTR）→ 芯片把该引脚拉到 **低电平（0V）**。
- 软件里"取消"（`$false`）→ 引脚回到 **高电平（3.3V）**。

所以"**DTR = 1**"在物理上是"**`DTR#` 引脚 = 0V**"。看电路时别被这个"1"骗了。

### 4.2 真实拓扑（原理图 `POWER&USB_USART` 放大确认）

```
               DTR# (13) ──[R47 1K]──► Q2 基极
 VCC3.3 ─[R46 10K]─ nX ─[D8 1N4148]─ RESET      (nX 被拉低 → RESET 被拉低)
                     │
                   Q2 集电极        Q2 = S8050 (NPN)
                   Q2 发射极 ── nY

          RTS# (14) ──┬── nY
                      └──[R48 1K]──► Q3 基极
 VCC3.3 ─ Q3 发射极 ; Q3 集电极 ─[R49 1K]─ BOOT0   (Q3 导通 → BOOT0 被拉高)
                     Q3 = S8550 (PNP)
```

关键：**两个管子是交叉耦合的** —— `RTS#` 同时接到 **Q2 的发射极**和 **Q3 的基极**；`DTR#` 接 **Q2 的基极**。

### 4.3 四种组合（软件值 ↔ 引脚电平 ↔ 结果）

| 软件 DTR | 软件 RTS | `DTR#` 引脚 | `RTS#` 引脚 | Q2 | Q3 | RESET | BOOT0 | 结果 |
|---|---|---|---|---|---|---|---|---|
| 0 | 0 | 高 | 高 | 截止 | 截止 | 释放 | 0 | ✅ 从 Flash 启动 |
| 1 | 0 | **低** | 高 | 截止 | 截止 | 释放 | 0 | ✅ 从 Flash 启动 |
| 0 | 1 | 高 | **低** | **导通** | **导通** | **被拉低** | 1 | ❌ MCU 一直复位（卡死） |
| 1 | 1 | **低** | **低** | 截止 | **导通** | 释放 | **1** | ❌ 复位后进 ROM |

**规律（记住这一条就够）：**

- **BOOT0 只由 `RTS` 决定**：`RTS` 被断言（`RTS#`=低）→ Q3 导通 → **BOOT0=1**；`RTS` 不断言 → BOOT0 恒为 0。
- **`DTR` 只决定"RESET 要不要被按住"**：`RTS` 断言而 `DTR` 不断言时，Q2 导通 → RESET 被拉低 → MCU 卡死。
- 所以：**只要 `RTS` 不被拉起，就永远进不了 ROM。**

### 4.4 为什么 DTR/RTS 会"自己"变成 1

不是你设的，是**串口软件 / 系统的默认行为**：

- Windows 打开串口时按 DCB 初始化，默认就是 `DTR_CONTROL_ENABLE` + `RTS_CONTROL_ENABLE`（都断言）。
- 终端软件（MobaXterm / PuTTY 等）打开端口时也主动把这两根线拉起来。
- 历史原因：DTR/RTS 源自 RS-232 的调制解调器握手线（DTR = Data Terminal Ready，RTS = Request To Send）。PC 一开端口就"告诉对方：我准备好了"。

### 4.5 影响的时机与范围

- **只在"复位 / 上电那一刻"起作用**（BOOT0 是复位时采样的启动选项）：
  - 串口开着但一直不复位 → **当前运行不受影响**，照样能看日志；
  - 一旦 **按 RESET 或重新上电** → 立刻按上表生效。
  - 这也解释了为什么"开着串口还能看到日志、一按复位就再没动静了"。
- **DTR/RTS 是控制线，不是数据线** → 不影响串口**收发数据**本身。
- **不会损坏芯片**：Q3 导通时 `R49`(1K) 上约 3.3mA；RESET 被按住只是不运行。
- **关掉串口程序后会自动恢复**（BOOT0 回 0），但"已经进 ROM 的那一次复位"不会自己跳回来，**需要再复位一次**。

## 5. 实测对照（本机 COM4，STM32F103）

| 打开串口时的 DTR/RTS | 复位后落点 | 串口输出 |
|---|---|---|
| `DTR=0 RTS=0`（安全） | **Flash** | 完整 RT-Thread 横幅 + `heartbeat` |
| `DTR=1 RTS=1`（踩坑） | **ROM**（`PC=0x1ffff3b6`） | 无 |
| `DTR=0 RTS=1` | RESET 被拉低，MCU 卡住 | 无 |

**交叉验证（排除固件差异）**：同一套动作下切换两张 bin——

| 条件 | 厂家 `atk_f103.bin` | 本工程 `stm32f103.bin` |
|---|---|---|
| COM4 关闭 | 进 Flash（`PC=0x08000248`） | 进 Flash（`PC=0x080012f0`） |
| `DTR=1 RTS=1` | **进 ROM** | **进 ROM** |

两张 bin 随条件**同步**变化 ⇒ "厂家 bin 不进 ROM"只是当时串口线状态恰好不同，并非固件差异。

**这些实验里没有任何"下载"动作**，全程只是「打开 COM4 → 设两个属性 → 复位」，结果就天差地别。

## 6. 解决办法

### A. 软件规避（不改硬件，推荐日常使用）

打开串口时让 **DTR 和 RTS 都不断言**。本仓库提供零依赖脚本：

```powershell
# 正常监视
powershell -ExecutionPolicy Bypass -File "C:\Users\a1an1in\workspace\mcu\tools\serial-mon.ps1" -Port COM4

# 顺便存日志
powershell -ExecutionPolicy Bypass -File "C:\Users\a1an1in\workspace\mcu\tools\serial-mon.ps1" -Port COM4 -Log uart.log

# 复现"踩坑"状态（复位后串口一片空白）
powershell -ExecutionPolicy Bypass -File "C:\Users\a1an1in\workspace\mcu\tools\serial-mon.ps1" -Port COM4 -Bad
```

核心就两行：`DtrEnable = $false` / `RtsEnable = $false`。

> 其实按 §4.3 的规律，**关键是 `RtsEnable = $false`** —— 只要 RTS 不断言，BOOT0 恒为 0，就进不了 ROM。两个都设 `$false` 最保险。

其它工具：

| 工具 | 做法 |
|---|---|
| Python (pyserial) | `ser.dtr = False`；`ser.rts = False`（需 `pip install pyserial`） |
| SSCOM / 串口助手 | 取消 **DTR、RTS** 勾选 |
| **MobaXterm** | ❌ v22.1 没有 DTR/RTS 开关（只有 端口/波特率/数据位/校验/停止位/流控），流控设 `None` 也没用 → 本板串口**别用它** |
| PuTTY | ❌ 默认断言 DTR/RTS，别用 |
| WSL | 需先 `usbipd` 把 CH340 挂进 WSL，或用上面的 PowerShell 版 |

### B. 硬件根治（一劳永逸，想用任何串口软件时）

**拆掉 `R49`（1K，位于 Q3 与 BOOT0 之间），或剪断它到 BOOT0 的那段走线。**

之后 USB 串口再也拉不高 BOOT0，MobaXterm / SSCOM / PuTTY 随便用。

- 代价：失去"一键下载"功能（本项目用 DAP 烧录，不受影响）。
- 替代做法：飞线把 BOOT0 网络直接短到 GND（**不要**经过 R11 那 10K，要接 BOOT0 网络本身，例如 R49 靠 BOOT0 一侧的焊盘或 MCU 的 BOOT0 脚）。

### C. 已经卡在 ROM 了怎么救

把**所有占用 COM4 的程序**（脚本 / MobaXterm / 串口助手）全部关掉，再按板子 **RESET** —— 就回 Flash 正常启动了。**不需要调试器。**

## 7. 三个容易踩的坑

- **"LED 不亮"不能说明板子没跑。** 本工程启动横幅走的是 UART1（COM4），不是 LED；LED 只在应用主动点亮时才亮。当前 `applications/main.c` 里挂了一个硬件自测线程（`led_init()` + `key_init()`：KEY0/KEY1 按住点亮对应 LED，KEY2 切换两灯交替闪，见 `docs/gpio.md`），它跑起来才会亮。判断死活仍请看串口有没有启动横幅。
- **串口中文全变成 `?`。** 固件打印的是 **UTF-8**，而 `System.IO.Ports.SerialPort` 的 `Encoding` 默认是 **ASCII**，会把每个 ≥0x80 的字节替换成 `?`（一个汉字 3 字节 → 3 个 `?`）。用 `tools/serial-mon.ps1` 时已默认 `-Charset utf8`；自己写代码记得 `sp.Encoding = [Text.Encoding]::UTF8`。
- 排错顺序：先看串口有没有启动横幅 → 没有就先怀疑 BOOT0 / 串口工具的 DTR/RTS → 最后才怀疑固件。
