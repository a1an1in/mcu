# RT-Thread 内核笔记：调度、中断、内存、信号

> 基于 STM32F103（Cortex-M3 单核 + GCC）工程的源码学习笔记，供回顾用。
> 核心源码：[scheduler_up.c](../rtos/rt-thread/src/scheduler_up.c)、[context_gcc.S](../rtos/rt-thread/libcpu/arm/cortex-m3/context_gcc.S)、[cpuport.c](../rtos/rt-thread/libcpu/arm/cortex-m3/cpuport.c)、[irq.c](../rtos/rt-thread/src/irq.c)、[mem.c](../rtos/rt-thread/src/mem.c)、[signal.c](../rtos/rt-thread/src/signal.c)

---

## 1. 线程调度

### 1.1 只有线程一种调度实体

RT-Thread 内核里**被调度的只有 `struct rt_thread`**：没有进程、没有协程。
IPC 对象（信号量/互斥量/邮箱等）不占 CPU，它们"起作用"的方式是把线程挂到自己的等待队列上，改变线程的调度状态。

线程三种运行时状态（TCB `stat` 低 3 位，互斥三选一）：

```
             调度器选中（从就绪表摘下）
  READY ──────────────────────────→  RUNNING（正在 CPU 上跑）
    ↑  被抢占 / 时间片到 / yield       │
    └──────────────────────────────  │  阻塞（take 信号量、收邮箱、等超时…）
                                     ↓
  就绪表：优先级位图可见       SUSPEND（挂在某内核对象的等待队列上，
                                     调度器看不见，直到事件到达被 resume）
```

关键理解：**READY ≠ 马上执行**，只是"有资格被选中"；RUNNING 和 READY 互斥——就绪表里从来没有正在运行的线程（切入前会 `rt_sched_remove_thread` 摘下来）。

### 1.2 调度器怎么"选"：位图 O(1)

```c
rt_list_t   rt_thread_priority_table[RT_THREAD_PRIORITY_MAX]; /* 256 级，每级一条链表 */
rt_uint32_t rt_thread_ready_priority_group;                  /* 位图：哪些优先级有线程就绪 */
```

选线程过程（`_scheduler_get_highest_priority_thread`）：

```
位图 rt_thread_ready_priority_group
  0b 0000...0101 0000
              ↑    ↑
           prio=4  prio=6
→ __builtin_clz 找到最低置位位 = 最高优先级 4
→ 从 rt_thread_priority_table[4] 链表取一个线程
```

与优先级数无关的常数时间，这就是 O(1) 调度器。

### 1.3 调度锁与 rt_schedule

```c
void rt_schedule(void)
{
    level = rt_hw_interrupt_disable();        /* 关中断保护选线程过程 */
    if (rt_scheduler_lock_nest == 0)          /* 调度锁没上锁才允许调度 */
    {
        to_thread = 选出最高优先级线程;
        if (to_thread != curr_thread)
        {
            rt_cpu_self()->current_thread = to_thread;   /* 更新"当前线程" */
            if (rt_interrupt_nest == 0)
                rt_hw_context_switch(&from->sp, &to->sp);        /* 线程态：直接切 */
            else
                rt_hw_context_switch_interrupt(&from->sp, &to->sp, ...); /* 中断态：只标记 */
        }
    }
    rt_hw_interrupt_enable(level);
}
```

- `rt_scheduler_lock_nest`：调度锁计数，`rt_enter_critical/rt_exit_critical` 增减；锁住期间 `rt_schedule` 不切换，解锁时补一次调度
- 中断态为什么只标记：中断还没退完，硬件现场没准备好，切不了

### 1.4 C 调度器 → 汇编 PendSV 的交接

两个阶段靠 **3 个全局变量**（定义在 cpuport.c，`.bss` 段）交接：

```c
rt_uint32_t rt_interrupt_from_thread;        /* 存 from 线程 sp 字段的地址 */
rt_uint32_t rt_interrupt_to_thread;          /* 存 to   线程 sp 字段的地址 */
rt_uint32_t rt_thread_switch_interrupt_flag; /* 是否有待处理切换 */
```

注意：存的是 **sp 字段的地址（二级指针）**，不是 sp 值，也不是 TCB 指针。C/汇编只约定这 3 个名字，完全解耦。

地址推演（设 from TCB 在 0x2000_1234，其 sp 字段在 0x2000_124C）：

```
全局变量 rt_interrupt_from_thread (0x2000_0A00) ──存──→ 0x2000_124C（from->sp 字段地址）
0x2000_124C 里的值 ──────────────────────────────存──→ 0x2000_3000（from 的当前栈顶）
```

汇编只做两件事：`LDR R1,[R0]` 拿到 sp 字段地址 → `STR R2,[R1]` 把最新栈顶写进去 / `LDR R1,[R1]` 解引用拿到栈顶。

### 1.5 rt_hw_context_switch：登记 + 扣扳机

```asm
rt_hw_context_switch_interrupt:        /* R0=&from->sp, R1=&to->sp */
rt_hw_context_switch:
    LDR  R2, =rt_thread_switch_interrupt_flag
    LDR  R3, [R2]
    CMP  R3, #1
    BEQ  _reswitch                     /* 已有第一次，只更新 to，from 保留 */
    MOV  R3, #1
    STR  R3, [R2]                      /* 置标志 */
    LDR  R2, =rt_interrupt_from_thread
    STR  R0, [R2]                      /* 存 from 的 sp 字段地址 */
_reswitch:
    LDR  R2, =rt_interrupt_to_thread
    STR  R1, [R2]                      /* 存 to 的 sp 字段地址（可覆盖） */
    LDR  R0, =ICSR
    LDR  R1, =PENDSVSET_BIT            /* 0x10000000 */
    STR  R1, [R0]                      /* 扣扳机：挂起 PendSV 异常 */
    BX   LR
```

连续两次切换请求（比如中断里线程 A 两次挂起不同对象）：from 只记第一次，to 不断覆盖为最新——**只有最后一次的选择有效**，中间的浪费掉。

### 1.6 PendSV_Handler：真正"切"的地方

PendSV 是 Cortex-M 异常 #14，被设为**最低优先级**——保证它执行时所有其他中断都已处理完，切换不会打断中断。

切线程时寄存器分两批保存：

| 批次 | 寄存器 | 谁来做 |
|---|---|---|
| 第一批 | xPSR, PC, LR, R12, R3, R2, R1, R0（8 个） | **硬件**自动压栈（异常进入时） |
| 第二批 | R4-R11 | **软件**（PendSV_Handler 汇编） |

硬件做的是"反汇编里找不到"的电路行为；软件部分每条都能单步：

```asm
PendSV_Handler:
    MRS  R2, PRIMASK                   /* 保存中断状态 */
    CPSID I
    LDR  R0, =rt_thread_switch_interrupt_flag
    LDR  R1, [R0]
    CBZ  R1, pendsv_exit               /* 没有切换请求 → 直接退出 */
    MOV  R1, #0
    STR  R1, [R0]                      /* 清标志 */
    LDR  R0, =rt_interrupt_from_thread
    LDR  R1, [R0]                      /* R1 = &from->sp */
    CBZ  R1, switch_to_thread          /* 无 from（冷启动首次）→ 跳过保存 */
    MRS  R1, PSP                       /* 当前线程栈顶 */
    STMFD R1!, {R4 - R11}              /* 保存 R4-R11 到线程自己的栈 */
    LDR  R0, [R0]
    STR  R1, [R0]                      /* 新栈顶写回 from->sp ★ */
switch_to_thread:
    LDR  R1, =rt_interrupt_to_thread
    LDR  R1, [R1]                      /* R1 = &to->sp */
    LDR  R1, [R1]                      /* R1 = to->sp 的值 = to 的栈顶 */
    LDMFD R1!, {R4 - R11}              /* 恢复 to 的 R4-R11 */
    MSR  PSP, R1                       /* 换栈 ★ */
pendsv_exit:
    MSR  PRIMASK, R2                   /* 恢复中断状态 */
    ORR  LR, LR, #0x04                 /* 异常返回用 PSP */
    BX   LR                            /* 硬件自动弹 8 个寄存器，PC = to 线程的断点 */
```

**线程在调度器眼里就是"sp + 优先级"两个数字**——保存现场 = 把 sp 存进 TCB，恢复现场 = 从 TCB 拿 sp 弹栈。

### 1.7 线程栈的出生：rt_hw_stack_init

线程创建时在栈顶伪造一份"异常返回帧"，让第一次切换像恢复一个"从未跑过但刚被中断"的线程：

```
高地址 ┌─────────────────────┐
       │ r4-r11 = 0xdeadbeef │ ← PendSV 软件 LDMFD 弹
       ├─────────────────────┤
       │ r0 = parameter      │ ← 线程入口函数的参数
       │ r1-r3, r12 = 0      │
       │ lr = texit          │ ← 线程函数返回时去 rt_thread_exit
       │ pc = tentry   ★     │ ← 第一次运行的第一条指令
       │ psr = 0x01000000    │ ← T 位=1（Thumb 状态）
低地址 └─────────────────────┘ ← 该地址写进 TCB->sp
```

---

## 2. 中断

### 2.1 中断管理结构

核心是计数器 `rt_interrupt_nest`（irq.c）：

```c
void rt_interrupt_enter(void)  { rt_interrupt_nest++; }   /* ISR 开头调用 */
void rt_interrupt_leave(void)  { rt_interrupt_nest--; }   /* ISR 结尾调用 */
```

作用：**区分当前代码在什么上下文**。

| | nest == 0 | nest > 0 |
|---|---|---|
| 上下文 | 线程（PSP 栈） | 中断（MSP 栈） |
| 能否阻塞/调度 | 能 | **绝对不能**（没有线程现场可保存） |
| rt_schedule 行为 | 直接 rt_hw_context_switch | 只标记 PendSV |

### 2.2 标准 ISR 模板（本工程 board.c）

```c
void SysTick_Handler(void)
{
    rt_interrupt_enter();      /* nest++ */
    rt_tick_increase();        /* 内核心跳 */
    rt_interrupt_leave();      /* nest-- */
}                              /* 返回后硬件检测到 PendSV 挂起 → 执行切换 */
```

### 2.3 tick 与时间相关的全部逻辑

```c
void rt_tick_increase(void)
{
    rt_atomic_add(&rt_tick, 1);       /* 全局心跳 +1 */
    rt_sched_tick_increase(1);        /* 时间片轮转：同优先级线程轮流跑 */
    rt_timer_check();                 /* 扫描到期定时器：唤醒睡眠线程、
                                         线程超时、IPC 等待超时都在这触发 */
}
```

时间线：线程 `rt_thread_sleep(100)` → 定时器挂起 → 100 次 tick 后 `rt_timer_check` 到期 → `rt_thread_resume`（放回就绪表）→ PendSV 切换。

### 2.4 中断里触发调度的完整时序

```
ISR 运行中（nest=1）
  └─ 某操作唤醒了高优先级线程 → rt_schedule()
       └─ nest != 0 → 只写交接变量 + 挂起 PendSV（不切换）
ISR 返回（nest=0）
  └─ 硬件发现 PendSV 挂起且优先级最高 → 进入 PendSV_Handler → 真正切换
```

**结论：中断退出后才能切换线程**——nest 归 0 + PendSV 最低优先级，两道保险。

### 2.5 临界区保护

```asm
rt_hw_interrupt_disable:        /* 返回旧 PRIMASK，支持嵌套 */
    MRS  R0, PRIMASK
    CPSID I
    BX   LR
rt_hw_interrupt_enable:
    MSR  PRIMASK, R0            /* 恢复为调用 disable 时的状态 */
    BX   LR
```

两层保护的选择：

| 场景 | 用法 | 保护范围 |
|---|---|---|
| 短小数据操作（改一个链表指针） | `level = rt_hw_interrupt_disable()` … enable(level) | 挡中断 |
| 可能阻塞的长临界区 | `rt_enter_critical()` … `rt_exit_critical()` | 只挡调度不挡中断 |

---

## 3. 内存管理与使用

### 3.1 堆内存从哪来

单块连续 RAM 的 MCU 没有虚拟内存，堆是**链接脚本在 RAM 里划出的一段**：

```
STM32F103 64KB SRAM
┌───────────────────────────┐ 0x2000_0000
│ .data（已初始化全局变量）    │
│ .bss（零初始化全局变量）     │
├───────────────────────────┤ ← __bss_end
│                           │
│   堆区（约 56KB）           │ ← 所有动态分配的来源
│                           │
├───────────────────────────┤ ← __stack_bottom
│ MSP 主栈（约 4KB）          │ ← 中断和启动代码用
└───────────────────────────┘ 0x2000_FFFF
```

初始化链：链接脚本定义 `__bss_end/__stack_bottom` → [board.h](../bsp/stm32f103/board.h) 取地址为 `HEAP_BEGIN/HEAP_END` → `rt_system_heap_init(HEAP_BEGIN, HEAP_END)` → 此后所有 `rt_malloc` 从这里来。

谁在用这个堆：线程创建（`rt_thread_create` 的 TCB+栈）、IPC 对象创建、用户 `rt_malloc`、信号 handler 表——**全部来自同一个堆**。

### 3.2 small mem：本工程的堆实现

配置：`RT_USING_SMALL_MEM` + `RT_USING_SMALL_MEM_AS_HEAP`（RT-Thread 默认，与官方一致）。

内存块头部（mem.c）：

```c
struct rt_small_mem_item {
    rt_uintptr_t pool_ptr;   /* 指回堆结构 + 最低位当 USED/FREED 标记用 */
    rt_size_t    next;       /* 相邻下一块偏移 */
    rt_size_t    prev;       /* 相邻上一块偏移 */
};                           /* 32 位平台：每块头 12 字节 */
```

分配（first-fit + 分裂）：

```
请求 100B → 从头遍历空闲链，找到第一个 ≥108B 的空洞
  ├─ 恰好/差几字节 → 整块给出（内部碎片少量）
  └─ 大很多 → 分裂成 [108B 已用][剩余空闲] 两块，剩余挂回空闲链
```

释放（合并）：

```
free(p) → 标记 FREED → 检查 prev/next 邻居
  └─ 邻居也空闲 → 合并成一大块（next/prev 指针双向缝合）
防止碎片无限增长的关键步骤
```

### 3.3 四种内存管理方案对比

| 方案 | 实现 | 算法 | 头开销 | 适用场景 |
|---|---|---|---|---|
| **small mem** ✅ 本工程 | mem.c | first-fit + 分裂 + 合并 | 12B/块 | 单块 RAM、通用动态分配 |
| memheap | memheap.c | best-fit，多堆管理 | ~32B/块 | 外接 SRAM、多块不连续内存 |
| slab | slab.c | zone 分级固定块 | 大（初始化要预建 zone） | 大 RAM（≥几百 KB）、内核对象高频分配 |
| **mempool** | mempool.c | 固定大小槽位，独立 API `rt_mp_alloc` | 无额外头 | 协议栈缓冲、信号 siginfo 等**固定大小**对象 |

选型结论（本工程）：

- 单 RAM → small mem 就够了，memheap 的多堆能力无收益
- 64KB 级 RAM → slab 的 zone 预建开销吃不消
- **固定大小 + 中断上下文分配 → mempool**：O(1)、确定性、不碰堆、无碎片无 OOM 风险

### 3.4 small mem 的管辖边界

只管 `[__bss_end, __stack_bottom]` 这一段。不含：`.data/.bss`（编译期定死）、MSP 主栈、mempool 的静态槽（调用方自己提供的数组）。

---

## 4. 信号原理

### 4.1 本质：不是中断

信号"看起来像中断"（异步、随时到达、可屏蔽、有 handler 表），但机制完全是两回事：

| | 真中断 | 信号 |
|---|---|---|
| 进入方式 | 硬件压栈、跳向量表 | **内核改写线程的 sp** + 普通函数调用 |
| 执行上下文 | MSP，nest>0，不可调度 | **线程自己的 PSP 栈**，nest=0 |
| 能否阻塞/被抢占 | 不能 | **能**（它就是线程在跑） |

判别金句：**中断上下文不能睡觉，信号 handler 能睡觉——能睡觉的就不是中断。**
机制一句话：**软中断 = 软件触发、硬件执行；信号 = 软件触发、软件执行。**

### 4.2 TCB 上的信号数据

```c
sig_vectors[]  handler 数组（首次 rt_signal_install 才从堆分配，不用的线程零开销）
sig_pending    位图：已到达未处理
sig_mask       位图：未屏蔽集合（屏蔽 = 延迟投递，不是丢弃）
si_list        siginfo 链表，节点来自专用 mempool（32 槽，rt_mp_alloc——中断安全）
```

### 4.3 发送：rt_thread_kill（只登记，不调度）

```
校验 sig_valid → 查重（同号已 pending 则只更新 info，直接返回——
               "标准信号不排队"，kill N 次只处理 1 次）
→ rt_mp_alloc 取节点（timeout=0 绝不阻塞，池满返回 -RT_EEMPTY 信号丢失）
→ 挂 si_list 链尾 + 置 sig_pending 位
→ _signal_deliver(tid)   ← 执行时机全部委托给它
```

### 4.4 投递：_signal_deliver 的四分支

```
sig_pending & sig_mask == 0 → 静默退出（信号留存，等 unmask 再投）
        │
        ├─① 目标挂起   → resume + 打标 → rt_schedule()
        │                （不动 sp！sp 指着内核阻塞现场）
        ├─② 目标是自己 → nest==0 ? 直接同步调 handle_sig() : 只打标
        │                （唯一"立即执行"的分支）
        ├─③ 目标就绪   → 【核心 trick】劫持 sp → rt_schedule()
        └─④ 正在处理信号 → 什么都不做，攒 pending（绝不二次劫持）
```

### 4.5 核心 trick：劫持 sp（借尸还魂）

```
劫持（纯软件三步）：                恢复（_signal_entry 末尾两步）：
tid->sig_ret = tid->sp;            tid->sp = tid->sig_ret;
tid->sp = rt_hw_stack_init(          /* 指回原现场帧 */
    _signal_entry, RT_NULL,        rt_hw_context_switch_to(&tid->sp);
    tid->sig_ret - 32,               /* 不是函数返回，是显式上下文切换 */
    RT_NULL);
rt_schedule();  /* 之后普通 PendSV 切换 */
```

栈布局——为什么不会破坏原现场：

```
线程栈（向下生长 ↓）
高地址 ┌─────────────────────┐
       │ 原现场帧（64B）       │ ← sig_ret【冻结区：信号全程无人写】
       ├─────────────────────┤
       │ 32B 安全垫           │
       ├─────────────────────┤
       │ 伪造帧（64B）         │ ← 新 sp（pc = _signal_entry）
       │ handler 压栈…        │ ← 继续向下，离原帧越来越远
低地址 ↓ 栈底
```

栈向下生长 + 所有写入都在原帧下方 → 原现场物理上够不着。**真实代价是栈水位叠加**：峰值 = 原帧 64B + 垫 32B + 伪造帧 64B + handler 调用链深度。工程纪律：handler 短浅、不递归、不调 printf，线程栈预留 ≈160B 余量。

### 4.6 执行与返回

```c
_signal_entry:                          /* 借线程躯壳跑起来的入口 */
    rt_thread_handle_sig(RT_FALSE);     /* while 循环：取 pending&mask 的信号，
                                           逐个 handler(signo)——普通 C 调用 */
    tid->sp = tid->sig_ret;             /* 恢复原现场指针 */
    rt_hw_context_switch_to(&tid->sp);  /* 切回去 */
```

被信号打断的阻塞调用（如 `rt_sem_take`）醒来后返回 `-RT_EINTR`——Unix"系统调用被信号打断"的语义来源。

### 4.7 调度检查点

每个线程被切回来时都会自检（scheduler_up.c `rt_hw_context_switch` 返回后）：

```c
if (RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_SIGNAL_PENDING)
{
    /* 清标记，就地处理信号 */
    rt_thread_handle_sig(RT_TRUE);
}
```

挂起线程被信号 resume 后，就是在这个检查点处理信号的——不需要劫持 sp。

### 4.8 stat 字段位图

```
bit:   7   6   5   4   3   2   1   0
     ┌───┬─────┬────┬───┬───┬───────────┐
     │   │PEND │WAIT│SIG│YLD│ 核心调度状态 │
     └───┴─────┴────┴───┴───┴───────────┘
       └─ 0xF0 信号位域（可叠加）    └─ 0x07：READY/RUNNING/SUSPEND 互斥
```

两组状态并行叠加：线程可以同时 `READY+SIGNAL+PENDING`。调度器只看低 3 位，signal.c 只看高 4 位。

---

## 附：全链路一图流

```
【调度】rt_schedule()
   ├ 位图 O(1) 选最高优先级 → 写 3 个交接全局变量（存 &sp）
   └ rt_hw_context_switch：挂起 PendSV
        ↓ 硬件异常进入（自动压 8 寄存器）
【切换】PendSV_Handler：清标志 → from 的 R4-R11 存栈、sp 写回 TCB
        → to 的 sp 出 TCB、弹 R4-R11、换 PSP → 异常返回（硬件弹 8 寄存器）
        → PC = to 线程的断点，继续跑

【中断】ISR = rt_interrupt_enter → 业务 → rt_interrupt_leave
   └ nest>0 时调度只标记；nest=0 后 PendSV 才真切

【内存】链接脚本划堆 → rt_system_heap_init → small mem（first-fit+分裂+合并）
   └ 固定大小对象用 mempool（如信号的 siginfo 池）

【信号】kill 登记进 si_list → _signal_deliver 按状态路由
   └ 就绪目标：sig_ret 存档 sp → 伪造帧(_signal_entry) 换 sp → 普通调度切入
     handler 在线程上下文跑完 → 还原 sp → 切回原断点
```

## 附：源码索引

| 主题 | 文件 | 位置 |
|---|---|---|
| 位图选线程 | src/scheduler_up.c | L282-401（rt_schedule） |
| 切换检查点（信号） | src/scheduler_up.c | L369-385 |
| PendSV / 扣扳机 | libcpu/arm/cortex-m3/context_gcc.S | 全文 |
| 交接变量 / 栈帧格式 / rt_hw_stack_init | libcpu/arm/cortex-m3/cpuport.c | L28-31、L62-90 |
| rt_interrupt_nest | src/irq.c | L10 附近 |
| tick / 定时器扫描 | src/clock.c | rt_tick_increase |
| 堆初始化 | bsp/stm32f103/board.c + board.h | L162-167、L23-26 |
| small mem | src/mem.c | rt_small_mem_item（L59 起） |
| 信号全部 | src/signal.c | _signal_entry(L43)、_signal_deliver(L95)、kill(L593) |
| stat 位定义 | include/rtdef.h | L627-636 |
