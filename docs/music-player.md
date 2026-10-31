# VS1053 音乐播放器：驱动与 msh 命令

参考正点原子《STM32F103 战舰开发指南》第 54 章（音乐播放器实验），
按本工程的分层重写：**芯片协议在 drivers，播放逻辑在 modules，命令在 msh**。

## 1. 分层

```text
modules/msh/music_cmd.c     msh：music play/stop/vol/info/test
        │
modules/music/music.c       播放线程：f_read 4KB → vs1053_write_data
        │  FatFs（复用 fs 命令挂载的卷）      │
        │                                    ▼
        │                        drivers/audio/vs1053.c
        │                        SCI 寄存器 / SDI 数据 / 音量 / 切歌
        │                                    │
        └──────────────┬─────────────────────┘
                       ▼
                drivers/spi（spi_device / spi_sync）
                       │ 总线锁：SPI1 与 CH395 共用
                       ▼
              stm32f1_spi.c → SPI1（PA5/PA6/PA7）
```

| 文件 | 职责 |
|---|---|
| `drivers/audio/vs1053.h/.c` | 芯片协议：寄存器读写、32B 数据块、DREQ 等待、软/硬复位、音量/音调、喇叭、正弦测试。OS 无关 |
| `drivers/audio/vs1053_config.h` | 板级事实：XCS=PF7、XDCS=PF6、DREQ=PC13、RST=PE6、SPI 速率 |
| `modules/music/music.c` | 播放线程、打开/读取文件、停止、音量、状态查询 |
| `modules/msh/music_cmd.c` | msh 命令 |
| `bsp/stm32f103/board.c` | 注册 SPI1、绑定 VS1053、把 XCS/XDCS 拉高、RST 保持低（隔离） |

## 2. 硬件与总线共享

战舰 V4 上 **VS1053 与 CH395Q 共用 SPI1**：

| 信号 | 引脚 | 说明 |
|---|---|---|
| SCK / MISO / MOSI | PA5 / PA6 / PA7 | 与 CH395 共用 |
| XCS | PF7 | 命令片选（低有效） |
| XDCS | PF6 | 数据片选（低有效） |
| DREQ | PC13 | 输入，高 = 可以收数据 |
| RST | PE6 | 低有效 |

两个器件各用各的 CS，但 SCLK/MOSI 是共享的，所以**一次事务必须独占总线**。

本工程的做法：

- SPI1 也注册成通用控制器（`stm32f1_spi1`），VS1053 用 `spi_device` +
  `spi_sync`，总线锁由 SPI 核心提供；
- CH395 仍是自己的寄存器级传输（它的命令是变长、边写边读），但
  `ch395_cs_low()/high()` 改成借 **同一把 SPI1 总线锁**（`spi_bus_lock`），
  不再用自己建的 `rt_mutex`；
- 因此音乐播放（每 32B 拿一次锁）和网络收发可以安全交错。

> 板级初始化阶段只配引脚（XCS/XDCS 高、RST 低），**不做任何 SPI 通信**；
> 真正的复位与软复位推迟到第一次 `music` 命令，那时调度器已经起来了。

## 3. msh 用法

```text
msh > fs mount
已挂载 0:（分区 0，自动）

msh > music test
正弦测试完成                 ← 喇叭“嘀”两声，先验证硬件通路

msh > music play /MUSIC/SONG01.MP3
开始播放 /MUSIC/SONG01.MP3（music stop 可停止）

msh > music info
状态: 播放中
文件: 0:/MUSIC/SONG01.MP3
时间: 0:35
码率: 128 kbps
音量: 60

msh > music vol 80
音量: 80

msh > music stop
已停止
```

| 命令 | 说明 |
|---|---|
| `music play <path>` | 播放一个文件；路径规则与 `fs` 相同（`/x` 绝对、`x` 相对当前目录） |
| `music stop` | 停止并等线程退出（最多 2s） |
| `music vol [0-100]` | 查询/设置音量，播放中也可调 |
| `music info` | 状态、解码时间、码率、音量 |
| `music test` | 正弦测试（会重置解码器） |

**前置条件**：先 `fs mount`（播放用的就是 fs 命令挂载的卷）。
播放期间 `fs umount` / `fs mkfs` 会被拒绝，先 `music stop`。

## 4. 支持的格式

VS1053B 原生解码：

- MP3（含 MP1/MP2）
- WAV（PCM）
- WMA
- MIDI
- AAC（ADTS/ADIF）、M4A（部分）

FLAC / OGG 需要加载官方 patch（`patch_flac.h` / `patch_spec.h`），本工程暂未
引入。命令不会拦你，但放不出来是正常的 —— 需要时把 patch 数组加进
`vs1053.c`，播放前调用写 RAM 的 patch 加载函数即可。

## 5. 实现要点（踩坑记录）

1. **DREQ 是唯一的节奏信号**：SCI 命令也必须在 DREQ 高时发；数据一次最多
   32 字节，DREQ 低就等。驱动里统一用 `vs1053_wait_dreq()`，超时 500ms。
2. **32 字节一块**：SDI 没有地址，VS1053 只按流解析；块大小不影响正确性，
   但 32B 是正点原子验证过的粒度，DREQ 响应最及时。
3. **切歌用 SM_CANCEL**：直接停数据会留残音/爆音。`vs1053_cancel()` 置
   SM_CANCEL 后喂 0，直到芯片清掉该位，再软复位。
4. **音量映射**：用户 0~100 → VS1053 衰减档 0~250（寄存器值 `254-raw`，
   双边）。默认 60。
5. **板载功放**：HT6872 由 VS1053 的 GPIO4 控制，软件写内部 RAM
   （`0xC017` 方向、`0xC019` 电平），不占 MCU 引脚。
6. **SPI 速率**：VS1053B 上限 9.216MHz。命令用 2MHz 档（实际 2.25MHz），
   数据用 8MHz 档（实际 9MHz）；两者都是模式 3。
7. **隔离**：board 初始化把 VS1053 的 RST 拉低、XCS/XDCS 拉高，避免它在
   CH395 读写时驱动 MISO。第一次 `music` 才释放复位。

## 6. 资源占用

| 项目 | 增量 |
|---|---|
| Flash | 约 **+5 KB**（VS1053 驱动 + 播放模块 + msh 命令） |
| RAM（BSS） | **+4 KB**（播放缓冲 `s_buf[4096]`） |
| 堆 | 播放线程栈 4KB（`osal_thread_create` 时分配） |

当前整体：Flash 170.7 KB / 512 KB，RAM 34.6 KB / 64 KB。

## 7. 已知限制

| 限制 | 说明 |
|---|---|
| 没有播放列表 | 一次一个文件；下一首手动 `music play` |
| 没有暂停/seek | 只有 stop；需要的话可在 VS1053 上做 `f_lseek` + 重新送数据 |
| 没有 FLAC/OGG patch | 见第 4 节 |
| 播放时网络会变慢 | 共用 SPI1，每 32B 抢一次锁；不会冲突，但吞吐会受影响 |
| 8.3 文件名 | 与 `fs` 命令一致（当前 `FF_USE_LFN = 0`），长名用 `fs ls` 看短名 |

## 8. 上板验证顺序

1. `music test` —— 听到两声“嘀”，说明 SPI1、复位、DREQ、功放都通；
2. `fs mount` + `fs ls /MUSIC` —— 确认文件可见；
3. `music play /MUSIC/xxx.mp3` —— 正常出声；
4. 播放中 `music vol 20` / `music vol 90` —— 音量变化；
5. `music stop` —— 立即停、无长尾爆音；
6. 播放中 `ping` —— 网络不丢包（共享 SPI1 的锁是否生效）。
