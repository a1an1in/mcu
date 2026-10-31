# 存储 / FLASH 自测：`test` 命令

原来这两个自测由 `main()` 里「按一下 WK_UP」触发；现在改成 msh 命令，
随时可以跑，不再占用按键。

## 用法

```text
msh > test
```

依次跑「存储自测」和「FLASH 自测」。

```text
msh > test storage     # 只测 EEPROM（AT24C02）
msh > test flash       # 只测 SPI NOR（25Q128）
```

## 两个测试分别做什么

| 命令 | 行为 | 会写片子吗 |
|---|---|---|
| `test storage` | 写 `selftest = "ok"` → `save` 落盘 → `load` 冲缓存重读 → 比对 | 值已存在时一个字节都不写；首次写一次 |
| `test flash` | 从末尾扫最多 4 个扇区找**全 0xFF** 的 → 擦除 → 0xF5 处写 20B（跨页）→ 读回 → 再擦回 0xFF | 2 次扇区擦除（只碰空扇区，测完恢复为空） |

输出示例：

```text
========== 存储自测 ==========
[storage-test] PASS：selftest = ok（save -> load 读回一致）

========== FLASH 自测 ==========
[flash-test] PASS：扇区 0x0ff000 擦除/跨页写/读回一致（已擦回空）

[test] 完成
```

失败时的排查提示会直接打印（例如 `i2c scan` 看 0x50、`flash info` 看 JEDEC ID）。

## 实现位置

`modules/msh/test_cmd.c`（代码从 `applications/main.c` 搬过来，判断逻辑没变）。
`applications/main.c` 现在只保留启动日志、硬件按键/LED 演示和 3 秒心跳，
不再轮询 WK_UP。
