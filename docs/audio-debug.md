# 声音 / MIC 调试指南

按「喇叭 → MIC → 语音链路」顺序排查，每一步都有对应命令。

## 1. 喇叭（播放）能不能响

### 1.1 正弦测试（最直接）

```text
msh > mplayer test
正弦测试完成
```

- 应听到两声“嘀”；
- 这个命令会初始化 VS1053、打开板载功放（HT6872，VS1053 GPIO4），
  不依赖 SD 卡和文件系统。

没声音时依次查：

| 检查 | 说明 |
|---|---|
| `mplayer vol` / `chat voice vol` | 不要是 0；先 `chat voice vol 80` |
| 耳机/喇叭插哪 | 板载功放推的是 SPEAKER 座子；耳机要插 PHONE 口 |
| 功放供电 | 战舰 V4 的 HT6872 需要 5V；只插 USB 时确认 5V 跳线/开关 |
| VS1053 复位 | `mplayer test` 会释放 RST；若之前 `video`/CH395 把它按住，重上电再试 |
| 音量寄存器 | `mplayer vol 100` 仍很小 → 硬件/喇叭问题，不是软件 |

### 1.2 播放文件

```text
msh > fs mount
msh > fs ls /MUSIC
msh > mplayer play /MUSIC/xxx.mp3
```

正弦能响、文件不响 → 路径/格式/文件系统问题，不是功放。

## 2. MIC 有没有声音

### 2.1 电平条（推荐）

```text
msh > mic 2 4
MIC 测试 2 秒，增益 4 倍
  256 ms  RMS   320  peak  1800  [#####...............]
  512 ms  RMS   980  peak  4200  [###############.....]
  ...
完成：平均 RMS 812，峰值 5200
  电平正常，增益合适
```

- 对着 MIC 说话，条子应明显跳动；
- **峰值 < 100**：几乎没信号 —— 检查 MIC 是否焊好，先 `mic 2 8` 加大增益；
- **峰值 > 32000**：削波，`mic 2 2` 调小；
- `mic` 不写 SD，纯粹看电平，反复调不会磨损卡。

### 2.2 录一段再回放（闭环）

```text
msh > record start 5       # 说 5 秒
msh > record stop
msh > fs ls /RECORDER
msh > mplayer play /RECORDER/REC00000.WAV
```

能听到自己的声音 → MIC、VS1053、SD、播放整条链路都通。

录音增益：

```text
msh > record start 5 8     # 8 倍
```

`record` 的增益参数 0 = 自动 AGC，1~15 = 倍数。

### 2.3 语音链路专用的增益

```text
msh > chat voice gain 8         # 0~15，默认 4
msh > chat voice vol 80         # 回答音量
msh > chat voice
```

## 3. 语音对话链路

### 3.1 板子侧

```text
msh > chat server 10.10.10.115 8765
msh > chat voice
按住 KEY0 说话，松开
```

串口会打印：

```text
[voice] 已发送 39258 字节，等待识别/回答...
[voice] 收到回答，边下边播...
```

### 3.2 PC 侧日志（最有用）

网关每次都会打印：

```text
[voice] asr 0.5s llm 0.9s tts 1.2s | '今天天气怎么样?' -> '我这边拿不到实时天气...'
```

按日志定位：

| 日志 | 含义 / 下一步 |
|---|---|
| `heard` 为空或“没听清” | MIC 太小/太远：`mic 2 8` 看电平，再 `chat voice gain 8` |
| `heard` 是错的字 | ASR 识别问题：靠近 MIC、说慢一点；PC 端 `ASR_MODEL=small` |
| `heard` 对，但回答为空/报错 | API Key / 模型名 / 余额；看 `[bridge] API HTTP ...` |
| 回答文字对，但板子没声 | `chat voice vol`、喇叭、MP3 解码；先用 `mplayer test` 确认 |
| 第一次特别慢 | ASR 模型首次下载，之后缓存 |

### 3.3 不接板子，只测 PC 链路

```powershell
python tools/voice_test_client.py
```

它用 SAPI 生成一句问题 → 发到 `/voice` → 保存回答音频并转写，
用来确认 ASR/LLM/TTS 是否正常。

## 4. 常见组合

| 现象 | 结论 |
|---|---|
| `mplayer test` 响，`mic` 有电平，语音没声 | 板子链路没问题，查 PC 日志 / `chat server` / 防火墙 |
| `mplayer test` 响，`mic` 无电平 | MIC 或录音模式问题：换 `mic 2 8`，检查 MIC 焊点 |
| `mic` 有电平，`record` 回放无声 | 播放侧问题：`mplayer vol`、喇叭 |
| 语音回答总是“没听清” | 按键太短（<0.25s）或 MIC 增益太小 |
| 声音机械 | edge-tts 失败回退 SAPI：看网关启动日志 `tts=` |
