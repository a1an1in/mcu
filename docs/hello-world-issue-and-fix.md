# STM32F103「战舰 V4」+ RT-Thread 排障合集

> 目标：让工程在 **STM32F103ZET6（战舰 V4）** 上从“零输出”跑到稳定打印
> `Hello, world!`（UART1、115200、无 HardFault、无乱码）。
>
> 本文档按**遇到问题的先后顺序**逐条记录：现象 → 根因 → 修复 → 验证。
> 既是本次任务的排障手册，也作为后续在这块板/同系列芯片上复现问题的参考。
> 所有关键寄存器值、符号地址、命令均为实测。

---

## 1. 目录 / 问题总览

| # | 阶段 | 问题 | 根因类别 | 状态 |
|---|------|------|----------|------|
| P1 | 构建 | Docker 编译 / 烧录环境 | 工具链/脚本 | ✅ |
| P2 | 启动 | **HardFault** | 堆栈重叠（链接脚本） | ✅ |
| P3 | 串口 | **乱码（约 348 kbaud）** | 时钟频率假设错误（24MHz vs 8MHz） | ✅ |
| P4 | 启动 | 复位后停留 **ROM BootLoader**（哑终端） | `BOOT0` 启动档错误 | ✅ |
| P5 | 输出 | “banner 有 / hello 无”与“全程无日志” | **调试 halt / 复位假象**（非代码 bug） | ✅ |
| P6 | 调试 | OpenOCD+DAP 复位总进 ROM，无法让固件常驻运行 | 调试器/DAP 复位局限 | ✅（已绕行） |

> 核心结论：P2/P3/P4 是**真实代码/接线问题**，已修复并烧录验证；
> P5/P6 是**调试操作与工具链造成的外部假象/局限**，并非固件缺陷。

---

## 2. 环境与关键事实

- 芯片：STM32F103ZET6（战舰 V4 开发板，512 KiB Flash / 64 KiB SRAM）
- RTOS：RT-Thread v5.2.2（工程内置 `rtos/rt-thread`）
- 串口：**UART1**（PA9=TX / PA10=RX，经板载 CH340 → Linux `/dev/ttyUSB0`）
  > ⚠️ **ATK-FS CMSIS-DAP 的虚拟串口 `/dev/ttyACM0` 不是 UART1**。
- 调试器：ATK-FS CMSIS-DAP（OpenOCD `cmsis-dap.cfg` + `transport select swd`）
- 构建/烧录：Docker 工具链；`python3 tools/devops.py build`、`python3 tools/devops.py flash --flasher dap`
- **实际系统时钟：8 MHz HSE → /2 → PLL ×6 = 24 MHz**（见 P3）

### 符号 / 寄存器速查（联调常用，来自 stm32f103.map）

| 符号 / 地址 | 值 | 用途 |
|-------------|----|------|
| 复位向量 `Reset_Handler` | `0x080012F0` | 主 Flash 复位入口 |
| `main` | `0x080012B8` | RT 主线程入口 |
| `main_thread_entry` | `0x080013DA` → `bl main` | 调度后用线程入口 |
| `rt_application_init` | `0x080013F0` | 创建 “main” 线程 |
| `rt_system_scheduler_start` | `0x08005950` | 启动调度器 |
| `rt_show_version` (banner) | `0x08004680` | RT-Thread 启动横幅 |
| `rt_kprintf` | `0x080046E4` | 控制台打印（内核） |
| `rt_hw_console_output` | `0x08001104` | 逐字符送 UART1 |
| `rt_assert_handler` | `0x08004964` | 断言失败回调（会打印） |
| `rt_hw_board_init` | `0x080011E4` | 板级/UART1/堆/时钟估算初始化 |
| `osal_printf` | `0x08001234` | 应用打印（→ rt_kprintf） |
| `HEAP_END` | `0x2000_F000` | 堆上界（= 主栈底） |
| `_estack` / main 栈底 | `0x2001_0000` / `0x2000_F000` | 启动栈边界 |

## 3. 问题详述

### P1 构建 / 烧录环境

- **现象**：宿主机没有可直接用的 `arm-none-eabi` 工具链；直接用 git 的 OpenOCD
  命令烧录时易出“找不到 bin / 目标无法接入”等。
- **根因/处理**：工程规定**一切构建走 Docker 镜像**（`docker/Dockerfile`；
  官方脚本 `python3 tools/devops.py`），烧录用 DAP 配置
  `tools/openocd-stm32f1-dap.cfg`（`interface/cmsis-dap.cfg` + `swd`）。
- **修复/验证**：
  - 构建：`python3 tools/devops.py build`（首次自动 `docker build` 出 `stm32f103-toolchain:latest`）。
  - 烧录：`python3 tools/devops.py flash --flasher dap` → **`Verified OK`**。
  - 宿主机另有 Xilinx 交叉工具链可作备用（非 PATH 内），但工程标准路径是 Docker。

---

### P2 HardFault —— 堆与启动栈重叠

- **现象**：上电 / 首次内存操作即进入 HardFault。
- **根因**：
  - 原配置 `HEAP_END = STM32_SRAM_END = 0x2001_0000`，该地址**恰是启动栈顶 `_estack`**。
  - 堆从 `HEAP_BEGIN` 向上增长，会侵入**启动栈区域**；栈再向下生长即覆盖堆，
    相互踩脏 → SP / 返回地址损坏 → HardFault。
  - map 文件确认 `_estack = 0x2001_0000` 与堆顶完全重合。
- **修复**：
  - `bsp/stm32f103/stm32f103.ld`：
    ```ld
    __stack_size   = 0x00001000;              /* 主栈预留 4KB */
    __stack_bottom = _estack - __stack_size;  /* 0x2000_F000 */
    ```
  - `bsp/stm32f103/board.h`：`HEAP_END ((void *)&__stack_bottom)`（堆止于 `0x2000_F000`）。
- **验证**：map 中确认 `HEAP_END=0x2000_F000` 且不再与 `_estack` 重合；烧录后不再 HardFault。

---

### P3 串口乱码 —— 系统时钟实为 24 MHz

- **现象**：`rt_kprintf` 输出可见但全乱，实测波特率 ≈ 348 kbaud（期望 115200）。
- **根因**：原本按 **8 MHz**（HSI）计算 UART1 BRR 与 SysTick 重载值，实际跑的是 **PLL 24 MHz**。
  - 读 `RCC->CFGR = 0x0010_000A` 解码：
    - SW=2 → 时钟源为 PLL；PLLSRC=0 → PLL 输入 = HSE/2 = 4 MHz；
    - PLLMUL=6 → SYSCLK = 4 MHz × 6 = **24 MHz**；
    - APB2 预分频 = /1 → PCLK2 = 24 MHz；AHB/HCLK = /1 → 24 MHz。
  - 用 8 MHz 算出的 BRR 偏小 → 波特率过高 → 乱码。
- **修复**（`bsp/stm32f103/board.c`）：
  - 新增 `rcc_sysclk_hz()` / `rcc_ahb_hz()` / `rcc_pclk2_hz()`，直接解码 `RCC->CFGR`；
  - UART1：`brr = (rcc_pclk2_hz() + baud/2) / baud;`
  - SysTick：`SYSTICK_LOAD = (rcc_ahb_hz() / RT_TICK_PER_SECOND) - 1;`
- **验证**：反汇编确认 `uart1_init` 调用 `rcc_pclk2_hz`；串口按 **115200** 干净输出。
| `osal_printf` | `0x080011b8` | 应用打印（→ rt_kprintf） |
---

### P4 复位后停留 ROM BootLoader —— BOOT0 启动档错误

- **现象**：上电后长时间无任何输出；调试器读 PC 落在 `0x1FFF_F0xx`（系统内存），
  板上等效为“只进 ST 自举、不跑我们固件”。
- **根因**：`BOOT0` 被置于**系统内存启动**档，复位后从 ROM 引导，固件未执行。
- **修复**：将 `BOOT0` **接地（=0）**，复位从主 Flash（`0x0800_0000`）启动。
- **验证**：复位后先见 RT-Thread banner（= 固件已从 Flash 启动）。
- **⚠️ 遗留（见 §7）**：后续出现“能打 banner、打完又回 ROM”的复现，证明当次
  `BOOT0` 并**未真正稳定接地**（仍为 1）：ROM 自举程序读到有效用户程序后“透传”
  跳进 Flash 应用，故 banner 能出；但向量表基址（VTOR）未指向 Flash，首个中断又把
  CPU 拽回 ROM。→ “接地”与否须以**万用表测 BOOT0 对 GND 电压 = 0V**为准，不能只看 banner。
- **补充**：F103 引脚引导逻辑（BOOT0, BOOT1）——
  | BOOT0 | BOOT1 | 启动来源 |
  |-------|-------|----------|
  | 1 | 0 | 系统内存（ROM 引导） |
  | 1 | 1 | 内嵌 SRAM |
  | 0 | 任意 | **主 Flash**（本工程使用） |

---

### P5 “banner 有 / hello 无”“全程无日志” —— 调试 halt / 复位假象

- **现象链**：固件能出 banner，但看不到 `Hello, world!`；反复用调试器断点/复位后串口一直静默。
- **排查（每项均已验证）**：
  - 反汇编 `rt_application_init`：`rt_thread_create` 失败才走 `rt_assert_handler`（会打印），
    而实际**无断言打印** → main 线程创建成功；
  - 反汇编 `main_thread_entry`：**无条件** `bl main`（`0x08001322 → 0x08001204`）；
  - `main()` 首行**无条件** `osal_printf`；链路 `osal_printf → rt_vsnprintf → rt_kprintf
    → rt_hw_console_output → UART1`，与 banner **同一通路**；
  - OpenOCD **单步执行** Flash `Reset_Handler`（`0x08001238→…→0x08001242→…`）——**Flash 代码可执行**。
- **根因**：对 CPU 执行 `halt`/断点/复位后，处理器被挂起（或停在 ROM BootLoader），
  **固件根本没在运行**，串口自然无输出。→ 这是**调试操作造成的假象**，不是固件缺陷。
- **修复/验证**：**物理断电冷启动**（切断 VDD 数秒再上电）让芯片脱离调试暂停态、从 Flash
  冷启动；随即稳定输出 banner + 每 3 秒 heartbeat。
- **归属**：为避免“错过启动首屏”，把 `Hello, world!` 并入心跳行（见 §4 `applications/main.c` 项）。

---

### P6 OpenOCD + DAP 复位总进 ROM，无法让固件常驻连续运行

- **现象**：任何 `resume`（断点接力、从 Reset_Handler 起跑、`SRST only`、`VTOR` 重定向）
  后，PC 都会落回 ROM BootLoader（`0x1FFF_F020` 附近）或直接 HardFault。
- **根因**：这块 DAP + openocd 的调试复位无法替代物理复位；复位后 `BOOT0=1` 时
  `0x00000000` 被硬件映射到 ROM，而固件**从不把 `SCB->VTOR` 设到 Flash**，于是运行中
  首个中断（SysTick 等）的向量取自 ROM 向量表，被带回 ROM（精确机理见 §7）。
  尝试 `SCB->VTOR`(0xE000ED08)=Flash 重定向会与固件异常处理冲突 → HardFault。
- **处理**：放弃用调试器“连续跑固件”，改以**单步执行**验证 Flash 可执行性 +
  **反汇编**验证 main 通路 + **物理断电重启**做真机确认。
- **经验**：当怀疑“固件没输出”时，不要只依赖调试器状态，**先物理断电重启**排除
  “调试暂停”这一层，再决定是否深入代码。

---

## 4. 改动文件对照

| 文件 | 改动 | 对应问题 |
|------|------|----------|
| `bsp/stm32f103/stm32f103.ld` | 预留主栈 `__stack_size=0x1000`、`__stack_bottom=_estack-0x1000` | P2 |
| `bsp/stm32f103/board.h` | `HEAP_END = &__stack_bottom`（`0x2000_F000`） | P2 |
| `bsp/stm32f103/board.c` | 新增 `rcc_sysclk_hz/ahb/pclk2_hz`，按实际 24 MHz 计算 UART1 BRR 与 SysTick | P3 |
| `bsp/rtconfig.h` | `RT_MAIN_THREAD_STACK_SIZE` 2048 → 4096（防御主线程栈不足） | P5 辅助 |
| `applications/main.c` | heartbeat 每 3 秒一行、含 `Hello, world! … via UART1` | P5 可见性 |

> BOOT0（P4）为硬件接线设置，非代码改动。

## 5. 完整验证流程

```bash
# 1) 构建（Docker）
python3 tools/devops.py build
# 2) 烧录（DAP / SWD）→ 应见 Verified OK
python3 tools/devops.py flash --flasher dap
# 3) 物理断电 2~3s 再上电（清除调试暂停态；BOOT0=0 不变）
# 4) 串口 UART1 / CH340 (ttyUSB0), 115200-8-N-1
#    预期：RT-Thread banner → 每 3s 一行：
#    [app] heartbeat #N : Hello, world! (STM32F103, OSAL backend: rt-thread) via UART1
```

辅助用：
```bash
# 反汇编 / 层段 / 符号
arm-none-eabi-objdump -d build/stm32f103/rt-thread/stm32f103
arm-none-eabi-objdump -h build/stm32f103/rt-thread/stm32f103
arm-none-eabi-size  build/stm32f103/rt-thread/stm32f103
```

## 6. 经验教训 / 排障检查清单

1. **时钟先确认真实值再算波特率/节拍**：直接读 `RCC->CFGR` 解码 PLL，勿凭晶振想当然。
2. **堆与启动栈必须显式隔离**：`HEAP_END` 绝不可等于 `_estack`，链接脚本里预留主栈。
3. **BOOT0 决定复位去向**：跑主 Flash 固件务必 `BOOT0=0`（P4 表格）。
4. **调试器“看到”≠“固件在跑”**：halt/断点会让 CPU 停在暂停态；openocd+DAP 复位还可能
   掉进 ROM BootLoader。**怀疑无输出先物理断电重启**，排除调试暂停这一层，再定位代码。
5. **端口别混淆**：UART1 走 CH340（`ttyUSB0`）；DAP 虚拟串口（`ttyACM0`）不是 UART1。
6. **可信证据优先级**：固件可执行性用“单步执行 Flash 指令”验证，比 `resume` 更抗复位噪声。
7. **区分“真实 bug”与“工具限制假象”**：P2/P3/P4 是真实缺陷，P5/P6 是调试/复位假象——两者解决手段不同。

---

## 7. “打印 banner 后又回 ROM”的完整机理（P7）

> 现象：复位后**有时能打印 RT-Thread banner**，但打完 banner 后**立刻异常进入 ROM**
> （调试器读 PC 落在 `0x1FFF_F0xx`）。等价于“固件跑起来了，但只跑一小段又被打回 ROM”。
>
> 一句话根因：`BOOT0` 实际为 **1**（未真正接地）。复位进 ROM 自举程序，它检测到 Flash
> 有有效用户程序后“透传”跳进用户固件 → 固件跑起来、打出 banner；但固件没设向量表基址
> `SCB->VTOR`，首个中断又把 CPU 拽回 ROM。

### 7.1 四步机理（均有源码 / 二进制证据）

1. 复位 + `BOOT0=1` → 硬件把 `0x00000000` 映射到 ROM，取 ROM 向量表，进 ROM 自举程序
   （对应物理 RESET 后 `PC=0x1FFFF3B6`、`SP=0x200001fc` 的实测）。
2. ROM 自举读到 Flash `0x08000000` 向量表[0]=`0x20010000`（合法栈顶）→ **透传跳转**
   到 Flash `Reset_Handler`（`0x080012F0`）→ `rtthread_startup → … → rt_show_version`
   打印 banner —— 所以“能打 banner”。
3. banner 后紧接 `rt_system_scheduler_start()` → `rt_hw_context_switch_to()`；该函数在
   `rtos/rt-thread/libcpu/arm/cortex-m3/context_gcc.S:154-159` 做 “restore MSP”：
   `LDR r0,=SCB_VTOR; LDR r0,[r0]; LDR r0,[r0]; MSR msp,r0`，即读 `[0x00000000]`
   （`BOOT0=1` 时 = ROM 向量表[0] = `0x200001fc`）覆写 MSP。
4. 随后触发 `PendSV`；`PendSV`/`SysTick` 向量同样取自 `[0]`（ROM 向量表）→ CPU 跳回
   ROM，“异常进入 ROM”坐实。
   - “有时”的由来：ROM 自举是否走到“透传跳转”分支存在时序窗口不确定性，
     故有时打 banner、有时直接静默停在 ROM。

### 7.2 “代码差异”到底存不存在（正面回答）

存在，但**不在“谁设置了 VTOR”**——结论反直觉：

- **ATK 验收工程**：HAL `system_stm32f1xx.c` 里 `SCB->VTOR = FLASH_BASE` 被
  `/* #define USER_VECT_TAB_ADDRESS */` 注释掉；`main.c` 也未调用
  `sys_nvic_set_vector_table()`。二进制 `tools/atk_f103.bin` 全片搜 `0xE000ED08`
  （小端 `08 ED 00 E0`）= **0 次** → **ATK 同样从不设置 VTOR**。
- **本工程（RT-Thread）**：全程不写 `SCB->VTOR`；仅在 `rt_hw_context_switch_to` 里
  **读** VTOR（`08 ED 00 E0` 出现 **1 次**，即 7.1 第 3 步 restore-MSP 那一段）。
- **结论**：“进 ROM”不是本工程独有缺陷，而是 `BOOT0=1` 的通用后果。差别只在
  RT-Thread 是中断驱动 RTOS，调度器一启动就必然踩 ROM 向量表 → “banner 后必回 ROM”，
  表现更干脆；ATK 裸机在 `BOOT0=1` 下同样无法靠中断稳定运行，只是在使能 SysTick 之前
  还能靠忙等跑一小段。

### 7.3 证据速览（均实测）

| 项 | 我的 RT-Thread | ATK 验收工程 |
|----|---------------|-------------|
| 向量表[0]（初始 SP） | `0x20010000` | `0x2000D338` |
| 向量表[1]（Reset） | `0x080012F1` | `0x08000249` |
| 引用 `0xE000ED08` 次数 | 1（读，restore MSP） | 0 |
| 是否写 `SCB->VTOR` | 否 | 否 |
| 有无 `SystemInit` | 无（有意：跑 HSI 8MHz） | 有（但 VTOR 写被注释） |

### 7.4 修复

1. **根本（必做）**：`BOOT0` 真正接 GND，并用**万用表实测 BOOT0 对 GND 电压 = 0V**。
2. **防御加固（已实施 ✅）**：在 `Reset_Handler` 最早处显式 `SCB->VTOR = 0x08000000`
   （对齐标准 ST SPL 的 `SystemInit` 行为），使即便是 `BOOT0=1` 透传场景，中断向量与
   restore-MSP 都正确指向 Flash，固件可持续运行。实施与验证见下方 7.5。

### 7.5 VTOR 加固实施与二进制验证（已落地）

- 改动文件：`bsp/stm32f103/startup_stm32f10x_md.S` 的 `Reset_Handler`（`ldr sp, =_estack`
  之后、拷贝 .data 之前）插入：
  ```asm
  ldr   r0, =0xE000ED08        /* SCB->VTOR */
  ldr   r1, =0x08000000        /* 主 Flash 基址 */
  str   r1, [r0]
  ```
- 构建方式：`python3 tools/devops.py build --clean`（Docker 容器 `stm32f103-toolchain` 内 `gcc-arm-none-eabi 13.2.1`）。
- 反汇编铁证（`Reset_Handler` 起始，容器产物）：
  ```
  080012f0: f8df d034  ldr.w sp, [pc, #52]    ; sp = 0x20010000
  080012f4: 480d       ldr   r0, [pc, #52]    ; r0 = 0xE000ED08 (SCB->VTOR)
  080012f6: f04f 6100  mov.w r1, #134217728   ; r1 = 0x08000000
  080012fa: 6001       str   r1, [r0, #0]     ; VTOR = 0x08000000  ← 写入 ✓
  ```
- 二进制对比：改动前 bin 全片 `0xE000ED08`（小端 `08 ED 00 E0`）仅 **1 处**（restore-MSP 读）；
  改动后 **2 处**（新增 Reset_Handler 的 VTOR 写）。向量表[0]/[1] = `0x20010000` / `0x080012F1` 正确。
- 附带修复（换 Xilinx arm-none-eabi gcc 10.2 重建时暴露）：该 GCC 默认生成 `.note.gnu.build-id`
  段，被 ld 摆到 `0x08000000`、把向量表挤偏 0x24 字节；已在 `CMakeLists.txt` 的
  `add_link_options` 加 `-Wl,--build-id=none` 禁用（对容器 gcc 13.2.1 亦无害，纯防御）。
- ⚠️ 教训：交叉编译**必须在 Docker 容器内进行**（`python3 tools/devops.py build`），工具链在
  `stm32f103-toolchain` 镜像内（`/usr/bin/arm-none-eabi-gcc`）；曾误在宿主机用 Xilinx
  petalinux SDK 工具链直接把路径固化进 `build/.../CMakeCache.txt` 的 `TOOLCHAIN_PATH`，
  导致容器内 cmake 找不到编译器。若再遇“compiler is not a full path”报错，用
  `python3 tools/devops.py build --clean` 清掉脏缓存重建即可。
