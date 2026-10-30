# SD 卡遗留优化：CMD6 高速模式 + DMA → 36MHz

> 状态：**待办**（当前已交付的是 4 位 + 24MHz，读写实测通过）
> 相关代码：`drivers/sd/sd.c`、`drivers/sd/stm32f1_sdio.c`、`drivers/sd/sd_config.h`

## 1. 现在到什么程度

| 项目 | 当前值 |
|---|---|
| 总线宽度 | 4 位（`SD_BUS_WIDTH_4 = 1`） |
| SDIO 时钟 | 24MHz（`SD_TRANSFER_CLOCK_HZ = 24000000`） |
| 理论速率 | 24MHz × 4 位 = **12 MB/s** |
| 实测 | 64GB SanDisk SDHC，MBR 读、末块写读比对均 PASS |

## 2. 为什么卡在 24MHz

两层限制：

| 限制 | 数值 | 说明 |
|---|---|---|
| SD 卡**默认速率模式**（Default Speed） | **25MHz** | 上电识别后卡就工作在这个模式，不需要任何命令切换 |
| STM32F1 SDIO 外设 | 规格 48MHz；HCLK=72MHz 时可达 **36MHz** | `SDIO_CK = HCLK / (CLKDIV + 2)`，CLKDIV=0 → 72/2 = 36MHz |

也就是说：**硬件能给 36MHz，但卡在默认速率模式下只保证 25MHz**。
我们取 24MHz（72/3，正好整除）。想再往上（36MHz，即 18MB/s），必须让卡切到
**高速模式（High Speed，SD 2.0，最高 50MHz）** —— 这需要 CMD6。

另外 36MHz 下 4 位是 18MB/s，FIFO（128 字节）只够 7µs，纯轮询在 72MHz 下
余量已经很薄，**建议同时把数据搬运换成 DMA**。

## 3. 第一步：CMD6 切高速模式

### 3.1 先确认卡支不支持

1. `CMD55 + ACMD51`（SEND_SCR）→ 拿到 SCR，确认 `SD_SPEC` ≥ 2（SD 2.0 才有
   高速模式；本卡是 SDHC，肯定满足）。
2. 也可以直接看 CMD6 的返回状态（见下），不支持时对应位为 0。

### 3.2 发 CMD6（SWITCH_FUNC）

```c
/* arg 编码（SD 物理层规范 4.3.10）：
 *   bit31    mode：1 = 切换（set），0 = 只查询（check）
 *   bit30:28 保留，置 0
 *   bit27:24 function group 1（命令系统/总线速度）→ 填 1
 *   bit23:20 group 2 → 0xF（不关心）
 *   bit19:16 group 3 → 0xF
 *   bit15:12 group 4 → 0xF
 *   bit11:8  group 5 → 0xF
 *   bit7:4   group 6 → 0xF
 *   bit3:0   group 7 → 0xF
 *
 * 切高速模式 = group1 选 function 1：
 */
#define SD_CMD_SWITCH_FUNC   6u
#define SD_SWITCH_SET_HS     0x80FFFFF1u   /* mode=1, group1=1(high speed) */

ret = sd_send_r1(card, SD_CMD_SWITCH_FUNC, SD_SWITCH_SET_HS);
```

要点：

- CMD6 的响应是 **R1**，但**随后卡还会在 DAT 线上回 64 字节的 switch status**
  （和 CMD17 一样是数据块），必须先 `data_start(读, 64, 64)` 再发命令，否则
  这 64 字节会丢失甚至把状态机搞乱。
- 返回的 status 里，`status[16]` 起是 group1 的当前/可选项：
  第 13 个字节（offset 13）低 4 位是 group1 当前选中的 function，
  我们要它等于 **1**。
- 切换成功后**至少 8 个时钟**才能发下一条命令（SD 规范要求）。

### 3.3 切完再提时钟

```c
if (高速切换成功) {
    sd_set_clock(card, 36000000u);   /* HCLK 72MHz → CLKDIV=0 → 36MHz */
    card->high_speed = 1;
}
```

⚠️ 有些卡/山寨卡不支持高速模式。**不支持就必须退回 24MHz**，否则数据会
CRC 错。所以这里要有回退分支，并把结果反映到 `sd info` 的输出里。

## 4. 第二步：数据搬运换成 DMA

### 4.1 为什么

现在是纯轮询：`while (!(STA & RXFIFOHF))` 一次读 8 个字。24MHz 下 12MB/s
还能靠 72MHz CPU 硬扛，36MHz（18MB/s）时 FIFO 只够 7µs，一个中断就溢出。

而且现在为了不被抢占，**整个数据阶段是关中断的**（`sdio_data_irq_lock()`），
单块还要关 ~250µs。DMA 之后这段临界区可以彻底去掉。

### 4.2 配置要点（STM32F1）

- SDIO 在 **DMA2** 上（RX / TX 各一个通道，具体通道号查 RM0008 的
  「DMA 请求映射」表，不同子型号可能不同，接线前先核对）。
- 内存→外设（写卡）/ 外设→内存（读卡），数据宽度 **32 位**，地址自增，
  外设地址固定为 `SDIO_FIFO`（0x40018080）。
- 传输长度 = 字节数 / 4（DMA 以字为单位）。
- 启动顺序：配好 DMA → `DCTRL.DMAEN = 1` → 发 CMD17/18/24/25 → 等
  `DATAEND`（或 DMA 传输完成 + `DATAEND`）。
- 传完之后清 `DMA` 的 TC 标志、关通道、`SDIO_ICR` 清标志。

### 4.3 改完之后

- `stm32f1_sdio_data_move()` 可以简化成「启动 DMA + 轮询完成」，或者直接
  用中断/信号量（但驱动层不想引入 OS，轮询完成标志即可）。
- 去掉 `sdio_data_irq_lock()/unlock()` 的整段临界区 —— 这是 DMA 带来的
  最大收益：**不再长时间关中断**。
- 多块读写可以一次 DMA 搬完（现在也是，只是靠 CPU 搬）。

## 5. 验收标准

1. `sd clock 36000` + `sd read 0 1` + `sd test` 全部通过；
2. `sd info` 能显示当前是「高速模式 / 36MHz」；
3. 连续 `sd test` 20 次不掉链子（切高速后时序更容易出问题）；
4. 传输期间系统节拍正常（可以用一个每 100ms 打印的线程观察，验证没有长时间
   关中断）；
5. 把 `ping` 一起跑着，确认 SD 大流量时网络不丢包（DMA 不再关中断，
   这一点应该明显变好）。

## 6. 风险与回退

| 风险 | 处理 |
|---|---|
| 卡不支持高速模式 | CMD6 查询后回退 24MHz，`sd info` 里标出来 |
| 36MHz 下数据 CRC 错 | 先确认 CMD6 真的切成功了；再检查 DMA 对齐/长度 |
| DMA 通道选错 | 用 `sd read` 立刻能看出来（读不到数据/超时） |
| 改坏了 | 把 `SD_TRANSFER_CLOCK_HZ` 调回 24MHz、`SD_BUS_WIDTH_4` 保持 1，就是现在这版 |

## 7. 顺带可以做的小优化

- `sd read` 现在一次最多 4 块（2KB，msh 的静态缓冲）。DMA 之后可以放大到
  64 块（32KB），做成真正的吞吐测试。
- 加一个 `sd speed` 命令：连续读 N 块并计时，直接给出 MB/s。
- `sd info` 里补上「当前时钟 / 是否高速模式」。

## 8. 参考资料

- SD 物理层简化规范 4.3.10（Switch Function）、4.7.4（High Speed）
- RM0008：SDIO 章节（DCTRL / DMAEN / FIFO）、DMA 请求映射表
- 正点原子《STM32F103 战舰开发指南》第 49 章（只有轮询实现，没有 CMD6/DMA）
