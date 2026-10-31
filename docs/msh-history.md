# msh 命令历史（串口方向键）

本工程启用了 RT-Thread finsh 的命令历史功能，**只用串口终端的 ↑ / ↓**，
不占用板载按键。

## 配置

`bsp/stm32f103/rtconfig.h`：

```c
#define FINSH_USING_HISTORY        /* 命令历史 */
#define FINSH_HISTORY_LINES     8  /* 保留最近 8 条 */
```

## 用法

在串口终端（PuTTY / SecureCRT / MobaXterm / minicom 等）里直接按：

| 按键 | 功能 |
|---|---|
| `↑` | 上一条 |
| `↓` | 下一条 |

终端需要发送 ANSI 转义序列（`ESC [ A` / `ESC [ B`），上面这些终端默认都支持。

## 说明

- 历史只存在 RAM，**重启清空**；
- 只保留命令，不保留输出；
- 8 条 × `FINSH_CMD_SIZE`（默认 80 字节）≈ 640B 静态内存；
- 板载 KEY_UP / KEY1 不再参与历史翻页（它们仍然用于 `[hw-test]` 的
  LED / 按键演示）。

## 相关：自测改为命令

原来「按一下 WK_UP 跑一次存储 / FLASH 自测」已经改成 msh 命令，
见 `selftest`（`docs/selftest.md`）。
