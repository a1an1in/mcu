# ATK 出厂固件「按 RESET 重启」验证记录

> 目标：确认在 **DAP + 串口 + 电源都接好** 的前提下，烧录正点原子出厂固化
> `atk_f103.bin`（无屏幕验收工程）后，按 RESET 键能从主 Flash 正常重启。
>
> 记录时间：2026-10-18（linux 宿主，WSL）

---

## 1. 硬件 / 连接状态（实测）

| 项 | 状态 |
|----|------|
| 调试器 | Microchip ATK-FS-HID-CMSIS-DAP（`lsusb` ID `04d8:00df`，CMSIS-DAP FW 2.0.0，SWD） |
| DAP 虚拟串口 | `/dev/ttyACM0`（**不是** UART1，仅调试用） |
| UART1 (CH340) | 本 Linux/WSL 侧未枚举出 `/dev/ttyUSB0`（串口在 Windows 侧，故本次未抓 UART 打印） |
| 芯片 | STM32F103（SWD DPIDR `0x1ba01477`，device id `0x10036414`，Flash 512 KiB） |
| 固件 | `tools/atk_f103.bin`（95,072 字节，对应资料盘 `…\1，入门资料\…\验收工程(无屏幕版)\Output\atk_f103.hex` 转 bin） |

---

## 2. 烧录

```bash
python3 tools/devops.py flash --flasher dap --bin tools/atk_f103.bin
```

输出（关键行）：

```
** Programming Started **
Info : flash size = 512 KiB
Warn : Adding extra erase range, 0x08017360 .. 0x080177ff
** Programming Finished **
** Verify Started **
** Verified OK **
```

结论：写入主 Flash 基址 `0x08000000`，写 + 校验通过。

---

## 3. 复位验证

```bash
openocd -f tools/openocd-stm32f1-dap.cfg \
  -c "gdb_port 0" -c "adapter speed 1000" \
  -c "init" -c "reset halt" -c "reg pc" -c "reg sp" \
  -c "mdw 0x08000000 6" -c "shutdown"
```

复位后读数：

```
pc  (/32): 0x08000248        ← ATK Reset_Handler（0x08000249 的 Thumb 地址）
sp  (/32): 0x2000d338        ← 向量表[0] 的初始 SP
0x08000000: 2000d338 08000249 08005be7 080058c3 08005be3 08001a35
```

解读：

- 复位后 PC 落在 **主 Flash 的复位入口 `0x08000248`**，SP 加载为向量表初始栈指针 → 芯片从主 Flash 启动，而非 ROM BootLoader。
- 向量表全部指向 `0x0800xxxx`，无任何 `0x1FFFxxxx`（系统内存/ROM 自举）地址。
- 两次独立会话复位结果一致（均 `0x08000248`）。

---

## 4. 结论

| 验证项 | 结果 |
|--------|------|
| `atk_f103.bin` 写入并校验 | ✅ `Verified OK` |
| 复位后从主 Flash 进入 ATK 复位入口 | ✅ PC=`0x08000248`、SP=`0x2000d338` |
| 是否进 ROM BootLoader（`0x1FFFxxxx`） | ❌ 未出现 |

即：**DAP + 串口（Windows 侧）+ 电源都接好时，烧录 `atk_f103.bin` 后按 RESET 能从主 Flash 正常重启。**

---

## 5. 说明 / 边界

1. **物理 RESET 键为远程无法直接按下**；本记录用同一颗 DAP 的 debug 复位（`reset halt`）作为
   复位代理，并已在前序 A/B 中验证：该 DAP 复位对 ATK 与本工程 rt-thread 固件均落回主 Flash
   复位入口（行为一致）。
2. **UART1 打印本次未抓取**：Linux/WSL 侧仅有 DAP 虚拟串口 `/dev/ttyACM0`，板载 CH340 的
   UART1（`/dev/ttyUSB0`）在 Windows 侧。若要确认 ATK「无屏幕验收版」的串口输出（115200），
   请在 Windows 侧观展串口助手。
3. 与「我的 bin 按 reset 进 bootloader」对照：本工程 rt-thread 固件
   （`build/stm32f103/rt-thread/stm32f103.bin`）在同等条件下同样落回主 Flash 复位入口
   （PC=`0x080012f0`），因此该问题不在固件，而在 **BOOT0 启动档 / 刷包通道**（见
   `docs/hello-world-issue-and-fix.md` 的 P4/P6）。

---

## 6. 原始日志位置（本次）

- 烧录日志：`/tmp/atk_flash.log`
---

## 7. 重要修正（物理 RESET vs DAP reset 的区别 —— 2026-10-18 追记）

> 用户按了板上**物理 RESET 键**后，直接 `halt` 抓取真实 PC（本次不再用 DAP `reset`）：

```
pc (/32): 0x1ffff3b6     ← 落在 ROM BootLoader（系统内存 0x1FFF_F000 区）
sp (/32): 0x200000c4
0x08000000: 20010000 080012f1   ← 主 Flash 里的 rt-thread 固件仍完好
0x1ffff000: 200001fc 1ffff021   ← ROM 自举向量表
```

结论与机理：

1. **物理 RESET 键 → PC = `0x1FFFF3B6` → 真的进了 ROM BootLoader。**
   按 F103 启动表，进系统内存 ⟺ **BOOT0=1、BOOT1=0**。→ 当前 BOOT0 跳线在 **1**。
2. 之前 §3 用 DAP `reset halt` 测出落在 Flash，是一种**误判**。原因是 `stm32f1x.cfg`
   第 77 行 `cortex_m reset_config sysresetreq`：CMSIS-DAP 下 OpenOCD 的 `reset` 走
   **软复位 SYSRESETREQ**，**不会重新采样 BOOT0**，沿用上一次上电锁存的 Flash 启动档，
   所以把 BOOT0=1 的问题掩盖住了；物理 RESET 拉 NRST 会重新采样 BOOT0，才会暴露真实启动档。
3. 因此：**判断“复位到底进 Flash 还是进 ROM”，必须以物理 RESET（或断电上电）为准，
   不能用 DAP `reset` 的 PC 落点来代替。**

修复：把 **BOOT0 拨到 0（GND）**，再按物理 RESET / 断电上电，PC 应落回
`0x080012f0`（rt-thread Reset_Handler），串口出 RT-Thread banner + heartbeat。

（本追记同时适用于 ATK 与 rt-thread 固件——问题在 BOOT0 启动档，不在固件内容。）
- 复位日志：`/tmp/atk_reset.log`

---

## 8. “打印 banner 后又回 ROM” —— VTOR 未设置的机理（2026-10-18 追记）

> 用户补充现象：复位后**有时能打印 RT-Thread banner，随后又异常进入 ROM**。
> 这是对 §7 的深化：`BOOT0=1` 已确认，本节解释“为何还能打 banner”以及“打完为何回 ROM”。

- 机理（四步）：
  1. 复位 + `BOOT0=1` → 硬件把 `0x00000000` 映射到 ROM，取 ROM 向量表进 ROM 自举
     （即 §7 的 `PC=0x1FFFF3B6`、`SP=0x200001fc`）。
  2. ROM 自举读到 Flash `0x08000000` 向量表[0]=`0x20010000`（合法栈顶）→ **透传跳转**
     到 Flash 应用 → rt-thread 打印 banner（所以“能打 banner”）。
  3. `rt_system_scheduler_start()` → `rt_hw_context_switch_to()` 里 “restore MSP” 段
     （`rtos/rt-thread/libcpu/arm/cortex-m3/context_gcc.S:154-159`）执行
     `LDR r0,=SCB_VTOR; LDR r0,[r0]; LDR r0,[r0]; MSR msp,r0` —— 读 `[0x00000000]`
     （`BOOT0=1` 时=ROM 向量表[0]=`0x200001fc`）覆写 MSP。
  4. 随后 PendSV/SysTick 向量也取自 ROM 向量表 → CPU 跳回 ROM。

- 二进制铁证（`atk_f103.bin` vs `build/stm32f103/rt-thread/stm32f103.bin`）：
  | 项 | 我的 RT-Thread | ATK 验收工程 |
  |----|---------------|-------------|
  | 引用 `0xE000ED08`(小端 `08 ED 00 E0`) 次数 | 1（读，restore MSP） | 0 |
  | 是否写 `SCB->VTOR` | 否 | 否 |

  即：**ATK 也从不设置 VTOR**（其 HAL `SystemInit` 的 `SCB->VTOR=FLASH_BASE` 被
  `/* #define USER_VECT_TAB_ADDRESS */` 注释，`main.c` 未调 `sys_nvic_set_vector_table()`）。
  “进 ROM”是 `BOOT0=1` 的通用后果，非本工程独有缺陷；RT-Thread 因中断驱动而更快更干脆地回 ROM。

- 修复：
  1. **根本**：`BOOT0` 接 GND（以万用表实测 BOOT0 对 GND = 0V 为准）。
  2. **防御加固（已实施 ✅）**：`Reset_Handler` 最早处 `SCB->VTOR = 0x08000000`，
     即使 `BOOT0=1` 透传场景也能持续运行（详见 `hello-world-issue-and-fix.md` §7.5）。