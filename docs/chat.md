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

## 2. PC 端：启动网关

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
[bridge] board command: chat server 192.168.1.100 8765
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

## 3. 板子端：配置与提问

```text
msh > chat server 192.168.1.100 8765
网关: 192.168.1.100:8765
  确认 PC 上已运行: python tools/llm_bridge.py

msh > chat ask 用一个 GPIO 控制 LED 的代码怎么写
正在请求大模型...（最长等 20 秒）
（回答……）

msh > chat info
网关: 192.168.1.100:8765
模型 / API Key: 由 PC 端 llm_bridge.py 的环境变量决定
```

| 命令 | 说明 |
|---|---|
| `chat server <ip> [port]` | 设置 PC 网关（默认端口 8765） |
| `chat ask <text...>` | 发送问题，打印纯文本回答 |
| `chat info` | 查看当前网关 |

前置条件：板子网络已经起来（`ifconfig` 有 IP，`ping <PC_IP>` 通）。

## 4. 协议（板子 ↔ 网关）

```http
POST /ask HTTP/1.1
Host: 192.168.1.100:8765
Content-Type: text/plain; charset=utf-8
Content-Length: <UTF-8 字节数>
Connection: close

<问题原文>
```

网关返回 `text/plain`，body 就是 `choices[0].message.content`，板子直接打印。
好处是 **MCU 不需要 JSON 解析**，也不需要处理 chunked。

## 5. 限制

| 限制 | 说明 |
|---|---|
| 单行问题长度 | `FINSH_CMD_SIZE = 256`，去掉命令名后约 240 字节（中文约 80 字） |
| 回答长度 | 板子最多打印 4KB，超出提示截断 |
| 超时 | 20 秒；超时会打印“连不上/失败” |
| 无流式输出 | 等完整回答后一次打印 |
| 无上下文 | 每次 `chat ask` 都是独立一问一答；不保存会话 |
| 无鉴权 | 网关只建议在可信局域网使用；不要在公网暴露 8765 |

## 6. 资源

| 项目 | 增量 |
|---|---|
| Flash | 约 **+5 KB**（HTTP 构造 + netconn 客户端 + 命令） |
| RAM（BSS） | 约 **+1.2 KB**（请求缓冲 1KB + 输出块） |
| Shell | `FINSH_CMD_SIZE` 80 → 256，行缓冲与历史各增大（历史 8 条共 2KB） |

当前整体：Flash 180.6 KB / 512 KB，RAM 36.4 KB / 64 KB。

## 7. 排查

| 现象 | 检查 |
|---|---|
| `连不上 PC 网关` | 板子 `ping <PC_IP>`；Windows 防火墙是否放行 8765；`chat server` 的 IP 是否是 PC 的局域网 IP |
| `[bridge] API HTTP 401` | API Key 错了/过期 |
| `[bridge] API HTTP 400 ... model` | `LLM_MODEL` 写错，换成服务商支持的 ID |
| 回答是乱码 | 串口终端要是 UTF-8；bridge 与板子都按 UTF-8 传 |
| `问题太长` | 缩短到约 80 个汉字以内，或改大 `FINSH_CMD_SIZE` |

## 8. 语音对话

文本 `ai` 之外，还支持**按住 KEY0 说话**的语音对话（流式录音 → ASR →
大模型 → TTS → 边下边播），见 `docs/voice.md`。
