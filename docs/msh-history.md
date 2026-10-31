# msh 命令历史：终端方向键 + 板载按键

本工程启用了 RT-Thread finsh 的命令历史功能，**终端键盘**和**板载按键**都能翻。

## 1. 终端方向键（最常用）

`bsp/stm32f103/rtconfig.h`：

```c
#define FINSH_USING_HISTORY        /* 命令历史 */
#define FINSH_HISTORY_LINES     8  /* 保留最近 8 条 */
```

串口终端里直接按 **↑ / ↓** 即可翻历史，和 Linux shell 一样。终端需要发送
ANSI 转义序列（PuTTY / SecureCRT / MobaXterm / minicom 默认都支持）。

## 2. 板载按键

| 按键 | 引脚 | 功能 |
|---|---|---|
| KEY_UP（丝印 WK_UP） | PA0 | 上一条历史 |
| KEY1 | PE3 | 下一条历史 |

实现见 `modules/msh/shell_history_keys.c`：

```text
KEY_UP 按下  →  uart_inject_rx("ESC [ A", 3)  →  msh 当成按了 ↑
KEY1   按下  →  uart_inject_rx("ESC [ B", 3)  →  msh 当成按了 ↓
```

要点：

- 不修改 shell 内部结构，只是把方向键序列**注入 UART 接收环形缓冲**
  （`uart_inject_rx()`，关中断保护）；终端和按键走同一条解析路径；
- 用 `key_is_down()` + 上次电平做**边沿检测**，不消耗 `key_pressed()` 的
  事件队列，所以不会和 `main` 里的按键演示/自测抢事件；
- 线程由 `INIT_APP_EXPORT(shell_keys_start)` 在 `main` 之前启动，20ms 轮询
  一次；`key_init()` 是幂等的，重复调用无副作用。

## 3. 已知副作用

- **KEY1 原本用于「按住点亮 DS1」的硬件演示**，现在按一下也会翻历史。
  两个功能不冲突（一个看电平、一个看边沿），但同时存在；
- 历史只存在 RAM，**重启清空**（掉电不保存）；
- 命令执行期间按按键，转义序列会先留在接收缓冲里，等回到提示符再被解析
  —— 表现为「命令跑完后自动翻了一条历史」，属正常现象；
- `shell_history_keys.c` 里改两个 `KEY_xxx` 就能换成别的按键组合。

## 4. 资源

| 项目 | 占用 |
|---|---|
| 历史缓冲 | `FINSH_HISTORY_LINES × FINSH_CMD_SIZE`（8 × 80 = 640B，shell 内部） |
| 线程栈 | 1KB（`histkey`，从堆分配） |
| Flash | 约 +0.7KB（历史功能 + 按键桥接） |
