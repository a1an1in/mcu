# 大模型对话：`chat` 命令（板子 → PC 网关 → DeepSeek）

板子上的 `chat` 命令不是直接连 DeepSeek，而是把问题发给 PC 上的
`tools/llm_bridge.py`，由 PC 用你的 API Key 走 HTTPS。原因如下。

## 1. 为什么不直接 HTTPS

| 方案 | 结论 |
|---|---|
| 板子直接 HTTPS | DeepSeek 只提供 HTTPS。STM32F103（64KB RAM）+ mbedTLS 需要几十 KB RAM、上百 KB Flash，还要处理证书链和时间校验；F103 没有硬件加密/TRNG，握手慢且不稳。**不推荐** |
| 板子 → PC 网关（本方案） | 板子只发明文 HTTP（lwIP 已有），TLS/API Key 都在 PC 上。**推荐** |
| API Key 放板子 | 即使做了 TLS，密钥也会落在 Flash/EEPROM 里，容易泄露。本方案密钥不烧进固件 |

> 如果以后一定要板子直连，可以再评估 mbedTLS + 证书 + 密钥存储；那是另一个
> 量级的工程，不建议在 F103 上做。

## 2. 常用命令速查

### 2.1 板子端（msh）

| 命令 | 作用 |
|---|---|
| `chat server <ip> [port]` | 设置 PC 网关，默认端口 8765 |
| `chat text <文字>` | 文本对话（`chat ask` 是别名） |
| `chat voice` | 语音状态：KEY0 / 音量 / MIC 增益 / 网关 |
| `chat voice on\|off` | 打开/关闭 KEY0 语音键（默认 on） |
| `chat voice vol [0-100]` | 播放音量（与 `mplayer vol` 共用） |
| `chat voice gain [0-15]` | MIC 增益，0 = 自动 AGC，默认 4 |
| `chat info` | 网关 + 文本/语音整体状态 |
| `mplayer vol <0-100>` | 音乐/语音共用的播放音量 |

```text
msh > chat server 10.10.10.115 8765
msh > chat text 用一个 GPIO 控制 LED 的代码怎么写
msh > chat info
msh > chat voice gain 2
```

按住 KEY0 说话，松开后自动 ASR → 大模型 → TTS → 播放，详见第 9 节。

### 2.2 PC 端：网关

在仓库根目录（`C:\Users\a1an1in\workspace\mcu`）执行。

| 操作 | 命令 |
|---|---|
| 启动（前台，推荐） | `python -u tools/llm_bridge.py` |
| 启动（后台） | `Start-Process python -ArgumentList '-u','tools/llm_bridge.py' -WindowStyle Hidden -RedirectStandardOutput bridge.log -RedirectStandardError bridge.err` |
| 看是否在跑 | `Invoke-RestMethod http://127.0.0.1:8765/health`，返回 `ok` 即可 |
| 看进程 | `Get-CimInstance Win32_Process -Filter "Name='python.exe'" \| Where-Object { $_.CommandLine -match 'llm_bridge' } \| Select-Object ProcessId, CommandLine` |
| 看端口 | `netstat -ano \| findstr :8765` |
| 看后台日志 | `Get-Content bridge.log -Wait` |
| 清空对话历史 | `Invoke-RestMethod -Method Post http://127.0.0.1:8765/reset` |
| 停止 | 前台 `Ctrl+C`；后台 `Stop-Process -Id <pid> -Force` |

重启（先杀旧进程，再启动）：

```powershell
cd C:\Users\a1an1in\workspace\mcu

# 1) 结束旧网关（按命令行匹配，不影响其它 python 进程）
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -match 'llm_bridge' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }

# 2) 重新启动（前台运行，日志直接看）
python -u tools/llm_bridge.py
```

启动成功的标志：

```text
[voice] ASR 就绪
[bridge] listening on 0.0.0.0:8765
[bridge] voice=on  asr=base/en:small.en  lang=auto  tts=edge(zh-CN-XiaoxiaoNeural / en-US-AriaNeural)
```

- 端口被占用时网关会直接退出（提示“端口 8765 已被占用”），先按上面第 1 步杀旧进程；
- 网关重启不影响板子，`chat server` 配置还在板子内存里；**板子**重启后才需要重设。

### 2.3 常用环境变量

| 变量 | 作用 | 默认 |
|---|---|---|
| `DEEPSEEK_API_KEY` | API Key（也可写进 `tools/llm_bridge.local.json`） | — |
| `LLM_MODEL` / `LLM_BASE_URL` | 模型 / 兼容 OpenAI 的地址 | `deepseek-flash` / `https://api.deepseek.com` |
| `ASR_LANGUAGE` | ASR 语言：`auto` / `zh` / `en` | `auto` |
| `ASR_MODEL` | 中文/通用 ASR 模型 | `base` |
| `ASR_MODEL_EN` | 英文专用 ASR 模型 | `small.en` |
| `ASR_VAD` | VAD 过滤：`1` 开 / `0` 关 | `1` |
| `TTS_EDGE_VOICE` / `TTS_EDGE_VOICE_EN` | 中文 / 英文音色 | Xiaoxiao / Aria |
| `VOICE_DUMP` | 每次录音存成 WAV 的目录（排查用） | 不存 |
| `LLM_BRIDGE_PORT` | 监听端口 | `8765` |

## 3. PC 端：启动网关（细节）

### 方式 A：本地配置文件（推荐，写死一次）

新建 `tools/llm_bridge.local.json`（已在 `.gitignore` 里，**不会提交**）：

```json
{
  "api_key": "sk-你的key",
  "model": "deepseek-flash",
  "base_url": "https://api.deepseek.com",
  "port": 8765,
  "max_tokens": 800
}
```

之后直接运行即可，不用再配环境变量：

```powershell
python tools/llm_bridge.py
```

参考模板：`tools/llm_bridge.local.example.json`。

> 优先级：命令行参数 > 环境变量 > 本地文件 > 默认值。
> 如果之前用 `setx` 设过旧 Key，环境变量会盖住文件；删掉即可：
> `[Environment]::SetEnvironmentVariable('DEEPSEEK_API_KEY',$null,'User')`

### 方式 B：环境变量（适合 CI / 临时）

```powershell
$env:DEEPSEEK_API_KEY = "sk-你的key"
$env:LLM_MODEL        = "deepseek-flash"
python tools/llm_bridge.py
```

输出类似：

```text
[bridge] listening on 0.0.0.0:8765
[bridge] model=deepseek-chat  base=https://api.deepseek.com  max_tokens=800
[bridge] board command: chat server 10.10.10.115 8765
[bridge] Ctrl+C 停止
```

- 端口默认 8765，可用 `LLM_BRIDGE_PORT` 或 `--port` 改；
- `LLM_BASE_URL` 可指向任何兼容 OpenAI `/chat/completions` 的服务；
- `LLM_MODEL` 就是模型 ID。**如果服务商支持 `deepseek-v4.1-flash` 这种名字，
  原样填进去即可**；标准 DeepSeek 公开 API 的 ID 是 `deepseek-chat` /
  `deepseek-reasoner`，填错会返回 `model not found`，网关会把 API 原文回给板子；
- API Key 只存在 PC 环境变量里，不经过串口、不进固件。

**Windows 防火墙**：第一次运行会弹窗，允许“专用网络”。也可以先在本机浏览器
打开 `http://127.0.0.1:8765/health`，看到 `ok` 说明网关正常。

## 4. 板子端：配置与提问

```text
msh > chat server 10.10.10.115 8765
网关: 10.10.10.115:8765
  确认 PC 上已运行: python tools/llm_bridge.py

msh > chat text 用一个 GPIO 控制 LED 的代码怎么写
正在请求大模型...（最长等 20 秒）
（回答……）

msh > chat info
网关: 10.10.10.115:8765
模型 / API Key: 由 PC 端 llm_bridge.py 的环境变量决定
```

命令表见 2.1。前置条件：板子网络已经起来（`ifconfig` 有 IP，`ping <PC_IP>` 通）。

## 5. 协议（板子 ↔ 网关）

同一个 TCP 端口（默认 8765）上两种请求，按路径区分：`/ask` 文本、`/voice` 语音。
两者都是一问一答，网关回 `Connection: close`，板子下次请求重新 `connect`。

### 5.1 文本 `/ask`

**请求**（板子 → 网关，HTTP/1.1，带 `Content-Length`）：

```http
POST /ask HTTP/1.1
Host: 10.10.10.115:8765
Content-Type: text/plain; charset=utf-8
Content-Length: 35
Connection: close

用一个 GPIO 控制 LED 怎么写
```

- `Host` 由 `chat server` 的 IP/端口拼出来；
- `Content-Length` 是问题原文的 **UTF-8 字节数**（上面这行是 35 字节，不是 16 个字符）；
- HTTP/1.1 必须带 `Content-Length`，否则网关不知道 body 有多长。

**响应**（网关 → 板子，`Content-Length` + `Connection: close`）：

```http
HTTP/1.1 200 OK
Content-Type: text/plain; charset=utf-8
Content-Length: 156
Connection: close

（大模型的纯文本回答）
```

body 就是 `choices[0].message.content`，网关已经拆好 JSON，**MCU 不需要 JSON 解析**，
也不需要处理 chunked。板子跳过 `\r\n\r\n` 后按 UTF-8 分块打印，最多打印 4KB。

### 5.2 语音 `/voice`

**请求**（板子 → 网关，HTTP/1.0，**不带** `Content-Length`）：

```http
POST /voice HTTP/1.0
Host: 10.10.10.115:8765
Content-Type: application/octet-stream
X-Sample-Rate: 8000
X-Channels: 1
X-Bits: 16

<PCM 裸数据>
```

| 项目 | 说明 |
|---|---|
| `HTTP/1.0` | 不用 `Content-Length`，靠 **半关闭** 表示“说完了” |
| body | 8kHz / 16bit 小端 / 单声道 PCM，边录边发（每块 512 字节） |
| 结束标志 | 松键后 `shutdown(SHUT_WR)`，网关 `read()` 到 EOF 就开始 ASR |
| 上限 | 30 秒（约 480KB），网关超长截断 |
| `X-Sample-Rate` 等 | 描述 PCM 格式；网关目前按 8kHz 处理，以后改 16kHz 需要网关一起支持 |

时序：

```text
板子                                 PC 网关
  |-- POST /voice + 头 --------------->|
  |-- 512B PCM ------------------------>|  边录边发
  |-- 512B PCM ------------------------>|
  |-- ... ----------------------------->|
  |-- shutdown(SHUT_WR) --------------->|  EOF → ASR（自动判中/英）
  |                                     |  LLM（带 6 轮上下文）
  |                                     |  TTS（edge：中文/英文音色）
  |<-------------- 200 + 音频 ----------|  边收边喂 VS1053
  |<-------------- 关闭 ----------------|
```

**响应**（网关 → 板子，`Content-Length` + `Connection: close`）：

```http
HTTP/1.1 200 OK
Content-Type: audio/mpeg
Content-Length: 91296
Connection: close

<MP3 数据>
```

| 情况 | Content-Type | 板子行为 |
|---|---|---|
| 正常（edge-tts） | `audio/mpeg` | 边收边 `vs1053_write_data()` 播放 |
| edge 失败回退 SAPI | `audio/wav` | 同上（VS1053 直接解 WAV/PCM） |
| 语音未启用（501） | `text/plain` | 不当音频，按文本打印在串口 |
| 处理失败（ASR/LLM/TTS 异常） | 通常 `audio/mpeg` | 朗读“出错了：…”；连兜底 TTS 也失败才回 500 `text/plain` |
| 没听清（< 0.25 秒或 ASR 为空） | `audio/mpeg` | 播放“我没听清，请再说一遍” |

板子侧判断很简单：`\r\n\r\n` 之后看响应头里有没有 `Content-Type: audio/`，
有就喂 VS1053，没有就当文本打印。

### 5.3 其他端点

| 路径 | 方法 | 用途 |
|---|---|---|
| `/health` | GET | 返回 `ok`，确认网关活着 |
| `/reset` | POST | 清空语音对话历史 |
| `/ask` | POST | 文本对话 |
| `/voice` | POST | 语音对话 |

## 6. 限制

| 限制 | 说明 |
|---|---|
| 单行问题长度 | `FINSH_CMD_SIZE = 256`，去掉命令名后约 240 字节（中文约 80 字） |
| 回答长度 | 板子最多打印 4KB，超出提示截断 |
| 超时 | 文本 20 秒；语音录完等回答 60 秒 |
| 无流式输出 | 等完整回答后一次打印 |
| 文本无上下文 | 每次 `chat text` 都是独立一问一答；语音有 6 轮记忆 |
| 无鉴权 | 网关只建议在可信局域网使用；不要在公网暴露 8765 |

## 7. 资源

| 项目 | 增量 |
|---|---|
| Flash | 约 **+5 KB**（HTTP 构造 + netconn 客户端 + 命令） |
| RAM（BSS） | 约 **+1.2 KB**（请求缓冲 1KB + 输出块） |
| Shell | `FINSH_CMD_SIZE` 80 → 256，行缓冲与历史各增大（历史 8 条共 2KB） |

当前整体：Flash 185.4 KB / 512 KB，RAM 36.9 KB / 64 KB。

## 8. 排查

| 现象 | 检查 |
|---|---|
| `连不上 PC 网关` | 板子 `ping <PC_IP>`；Windows 防火墙是否放行 8765；`chat server` 的 IP 是否是 PC 的局域网 IP |
| 网关起不来 / 提示端口占用 | 见 2.2 的“重启”，先杀旧进程；`netstat -ano \| findstr :8765` 确认 |
| `[bridge] API HTTP 401` | API Key 错了/过期 |
| `[bridge] API HTTP 400 ... model` | `LLM_MODEL` 写错，换成服务商支持的 ID |
| 回答是乱码 | 串口终端要是 UTF-8；bridge 与板子都按 UTF-8 传 |
| `问题太长` | 缩短到约 80 个汉字以内，或改大 `FINSH_CMD_SIZE` |
| 语音串词 / 错字 | 看网关 `[voice] pcm ... rms= peak=`；`peak ≥ 30000` 是削波，`chat voice gain 2` 后再说（目标 `rms 1000~3000`、`peak < 20000`） |
| 英文识别不准 | 必须看到 `[voice] lang=en`；英文会自动切到 `small.en`。仍不准就 `ASR_MODEL_EN=medium.en` |
| 播放没声音 | `chat info` 看音量是否 0；`mplayer test` 验证喇叭 |

## 9. 语音对话

### 9.1 用法

```text
msh > chat server 10.10.10.115 8765
msh > chat voice gain 2        # 离 MIC 20~30cm，先压低电平
按住 KEY0 说话 → 松开
```

串口会打印：

```text
[voice] 已发送 57344 字节，等待识别/回答...
[voice] 收到回答，边下边播...
```

### 9.2 中英文

- 网关 `ASR_LANGUAGE=auto`（默认），自动判断中/英；
- 中文用 `base`，英文再用 `small.en` 识别一遍（更准，约多 1 秒）；
- 回答语言跟随提问语言，TTS 中文用 `zh-CN-XiaoxiaoNeural`、英文用 `en-US-AriaNeural`。

网关日志：

```text
[voice] pcm 61422 bytes (3.84s) rms=3475 peak=22646
[voice] lang=en p=0.96
[voice] asr 2.0s llm 1.7s tts 2.0s | [en] 'Can you help me debug the I2C driver?' -> '...'
```

### 9.3 记忆与控制

| 项目 | 默认 | 调整 |
|---|---|---|
| 对话记忆 | 6 轮 | `--voice-history-turns` |
| 空闲清空 | 600 秒 | `--voice-idle-reset` |
| 手动清空 | — | `Invoke-RestMethod -Method Post http://127.0.0.1:8765/reset` |
| 回答长度 | 200 字 | `--voice-max-chars` |

### 9.4 调试

- 录音电平：网关日志 `rms/peak`，或 `VOICE_DUMP=目录` 把每次录音存成 WAV；
- 详细流程见 `docs/voice.md`，硬件/MIC 排查见 `docs/audio-debug.md`。
