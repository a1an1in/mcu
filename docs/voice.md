# 语音对话：按住 KEY0 说话（流式）

参考第 55 章的录音 + 第 54 章的播放，把「录音 → 识别 → 大模型 → 合成 → 播放」
串成一条**流式**链路，目标是 2~4 秒内听到回答，而不是先落 SD 再整段上传。

## 1. 链路

```text
板子                                               PC（tools/llm_bridge.py）
─────────────────────────────────────────────────────────────────────────
按住 KEY0
  VS1053 MIC 录音（8k/16bit/单声道）
  每 512B ──TCP──► POST /voice（HTTP/1.0，无 Content-Length）
                       ── PCM 裸数据流 ──►  faster-whisper ASR
松开 KEY0                                    DeepSeek（deepseek-flash）
  半关闭发送 ────────────────────────────►  Windows SAPI TTS
  VS1053 切回播放                               │
  收到 WAV ◄──────────── audio/wav ─────────────┘
  边收边喂 VS1053（不落 SD）
```

两个关键决定：

1. **HTTP/1.0 + 无 Content-Length + 半关闭**：板子发完 PCM 后
   `netconn_shutdown(conn, 0, 1)`，PC 读到 EOF 就知道说完了——比在 MCU 上
   实现 HTTP chunked 简单，也省掉一个“长度”字段。
2. **边下边播**：PC 返回 `audio/wav`，板子解析到 `\r\n\r\n` 后就把 WAV
   （含 44 字节头）按 128B 喂给 VS1053，不等整个文件下载完。

## 2. PC 端

```powershell
cd C:\Users\a1an1in\workspace\mcu
python tools/llm_bridge.py
```

首次需要装 ASR：

```powershell
pip install faster-whisper
```

模型默认 `base`（约 145MB）。国内访问 HuggingFace 会超时，脚本已默认：

```python
HF_ENDPOINT=https://hf-mirror.com
HF_HUB_DISABLE_XET=1
```

TTS 用 Windows 自带 SAPI，中文语音（本机是 `Microsoft Huihui Desktop`），
不需要装包。

可选环境变量 / `llm_bridge.local.json`：

| 项 | 默认 | 说明 |
|---|---|---|
| `ASR_MODEL` | `base` | `tiny` 更快但中文差；`small` 更准但慢 |
| `ASR_LANGUAGE` | `zh` | 识别语言 |
| `TTS_VOICE` | 自动选 zh-CN | 如 `Microsoft Huihui Desktop` |
| `TTS_RATE` | `2` | SAPI 语速（-10~10） |
| `VOICE_MAX_TOKENS` | `80` | 语音回答长度上限 |
| `--no-voice` | 关 | 只保留文本 `/ask`，不加载 ASR |

启动日志会打印：

```text
[voice] ASR 就绪
[bridge] voice=on  asr=base  tts=SAPI(auto zh)
[bridge] board command: ai server 10.10.10.115 8765
```

## 3. 板子端

```text
msh > ai server 10.10.10.115 8765
msh > voice info
语音键: on（按住 KEY0 说话）
状态: 空闲
网关: 10.10.10.115:8765

（按住 KEY0，对着 MIC 说话，松开）
[voice] 按住 KEY0 说话，松开结束...
[voice] 已发送 39258 字节，等待识别/回答...
[voice] 收到回答，边下边播...
```

命令：

| 命令 | 说明 |
|---|---|
| `voice key [on\|off]` | 打开/关闭 KEY0 语音键（默认 on） |
| `voice info` | 状态 + 网关 |

- 录音上限 30 秒；
- 录音时功放关闭（防啸叫）；
- 松键后 VS1053 会重新初始化（恢复播放时钟/音量），与 PC 的 ASR 并行；
- 正在 `record` 或播放音乐时按 KEY0：语音流程会先拒绝/停止音乐。

## 4. 实测延迟

PC 端（本机，8k PCM，2.5 秒问题）：

| 阶段 | 耗时 |
|---|---|
| faster-whisper base 识别 | ~0.5s |
| DeepSeek `deepseek-flash` | ~0.9s |
| SAPI 合成 | ~0.4s |
| **PC 合计** | **~1.8s** |

板子侧再加：上传（边录边传，不额外等待）+ 回答下载/边下边播（几十 KB~300KB，
局域网 0.2~1s）。**松键到出声大约 2.5~4 秒**，比文件式（5~10 秒）快一倍以上。

## 5. WAV 头与格式

- 录音：8kHz / 16bit / 单声道，**裸 PCM** 上传（无 WAV 头）；
- PC 用 `wave` 模块加 44 字节头后交给 faster-whisper；
- 回答：SAPI 生成 22.05kHz / 16bit / 单声道 WAV，整段带回；
- 板子把整个 WAV（含头）喂 VS1053，由芯片自己解析格式；
- 超过 120 字的回答会被截断，避免朗读太久。

## 6. 排查

| 现象 | 原因 |
|---|---|
| 串口打印 `语音未启用（faster-whisper…）` | PC 没装 faster-whisper 或模型下载失败；看 bridge 日志 |
| `连不上网关` | 板子没 `ai server` / IP 错 / 防火墙（Python 已在 Public 放行） |
| 识别总是空 | MIC 增益低：`voice` 暂时固定 4 倍；可改 `VOICE_GAIN_REG` 到 8×1024 |
| 回答是“没听清” | 按键时间太短（<0.25s）或 MIC 没声音 |
| 声音卡顿 | 网络太慢/丢包；回答 WAV 较大时 TCP 会背压，不影响正确性 |
| 第一次特别慢 | 模型首次下载（base 约 145MB），之后缓存 |

## 7. 资源

| 项目 | 增量 |
|---|---|
| 板子 Flash | 约 **+4 KB**（voice 模块 + 命令） |
| 板子 RAM（BSS） | 约 **+0.5 KB**（512B PCM 缓冲） |
| 板子线程 | 4KB 栈（`voice`） |
| PC | faster-whisper + base 模型（约 145MB 下载），SAPI 不占空间 |
