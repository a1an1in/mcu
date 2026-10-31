# VS1053 录音：驱动与 msh 命令

参考正点原子《STM32F103 战舰开发指南》第 55 章（录音机实验），按本工程分层重写：
录音走 VS1053 的 MIC 输入 + 内部 PCM FIFO，落盘成标准 WAV。

## 1. 分层

```text
modules/msh/record_cmd.c        msh：record start/stop/info
        │
modules/recorder/recorder.c     录音线程：vs1053_record_read → f_write
        │  WAV 头：modules/recorder/wav.c
        │  FatFs（复用 fs 挂载的卷）
        ▼
drivers/audio/vs1053.c          录音模式：AICTRL/MODE/patch、HDAT1/HDAT0
        │
drivers/spi（SPI1，与 CH395 共用）
```

## 2. 格式与文件

| 项 | 值 |
|---|---|
| 采样率 | 8000 Hz |
| 位宽 / 声道 | 16 bit / 单声道 |
| 编码 | 线性 PCM（WAV `AudioFormat=1`） |
| 数据率 | 16 KB/s |
| 目录 | `0:/RECORDER/`（不存在自动创建） |
| 文件名 | `REC00000.WAV` ~ `REC65534.WAV`（自动找空号） |

**为什么固定 8kHz PCM**：这是第 55 章验证过的配置，VS1053 的 MIC 录音在
`AICTRL3 = 6`（左声道、线性 PCM）时最稳；改成 ADPCM 需要另一套 WAV 头和
解码参数，暂不引入。

## 3. msh 命令

```text
msh > fs mount
msh > record start            # 一直录，直到 record stop
开始录音（手动停止，增益 4 倍）
  record stop 停止，record info 查看

msh > record info
状态: 录音中
文件: 0:/RECORDER/REC00000.WAV
时长: 12 秒
大小: 192000 字节

msh > record stop
已停止，文件: 0:/RECORDER/REC00000.WAV
```

带参数：

```text
record start 10         # 录 10 秒自动停
record start 0 8        # 手动停，MIC 增益 8 倍
record start 5 0        # 录 5 秒，自动 AGC
```

| 参数 | 说明 |
|---|---|
| `seconds` | 0 / 省略 = 手动停；最大 3600 |
| `gain` | 0 = 自动 AGC；1~15 → 1024×gain（默认 4） |

## 4. 驱动做了什么

`vs1053_record_start(dev, rate, gain)`：

1. 若正在放歌，先 `vs1053_cancel()` 清干净；
2. 关功放（录音时避免啸叫）；
3. `SCI_BASS=0`、`AICTRL0=采样率`、`AICTRL1=增益`、`AICTRL2=0`、
   `AICTRL3=6`（MIC、左声道、线性 PCM）；
4. `SCI_CLOCKF=0x2000`（2 倍频）；
5. `SCI_MODE=0x1804`（SM_RESET|SM_SDINEW|SM_ADPCM）进入录音；
6. 延时 5ms，加载官方 **WAV 录音 patch**（修正 VS1053 WAV 录音的已知 bug）。

`vs1053_record_read(dev, buf, len)`：

- 先读 `HDAT1` 看 FIFO 里有多少个 16 位字，够 `len/2` 才读，否则 1ms 轮询；
- 逐个读 `HDAT0`（低字节先出）拼成小端 PCM；
- 录音线程每次取 **512 字节**（256 字，约 32ms 一块），与第 55 章一致。

`vs1053_record_stop(dev)`：

- 写回 `MODE=0x0804` 退出录音；
- 重新做一次 `vs1053_init()`，恢复播放侧的时钟（0x9800）、音量和喇叭。

## 5. WAV 头

`modules/recorder/wav.c` 生成标准 44 字节头：

- 开录前先写头，`data` 长度填 0；
- 停止时回填 `data` 长度和 `RIFF` 长度（`36 + data`），再 `f_close`。

host 单测 `tests/recorder/test_wav.c` 把字段逐字节钉住（11/11 通过）。

## 6. 与播放的关系

- 录音和播放共用 VS1053，**不能同时进行**：
  - 放歌时 `record start` → 提示先 `music stop`；
  - 录音时 `music play` / `music test` → 提示先 `record stop`；
- 录音期间 `fs umount` / `fs mkfs` / `fs mount` 会被拒绝，避免拆掉正在写的卷；
- 录音时板载功放关闭，听不到 MIC 直通（正常）。

## 7. 资源占用

| 项目 | 增量 |
|---|---|
| Flash | 约 **+4 KB**（录音驱动 + 录音线程 + WAV + 命令） |
| RAM（BSS） | **+512B**（录音缓冲） |
| 堆 | 录音线程栈 2KB |

当前整体：Flash 175.6 KB / 512 KB，RAM 35.2 KB / 64 KB。

## 8. 上板验证

1. `fs mount`；
2. `record start`，对着板载 MIC 说话 5 秒；
3. `record stop`；
4. `fs ls /RECORDER` 看到 `REC00000.WAV`；
5. 把卡插到 PC，用播放器打开 WAV：应能听到刚才的录音；
6. 或直接在板上 `music play /RECORDER/REC00000.WAV` 回放。

如果录出来全是静音：先确认 `gain` 不是 0（自动 AGC 在小板子上可能反应慢），
再试 `record start 0 8`；如果仍然静音，检查板载 MIC 是否焊好/选对了输入。
