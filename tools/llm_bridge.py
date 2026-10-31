#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""llm_bridge.py - 让 STM32 板子通过局域网和 DeepSeek（或兼容 API）对话。

文本：
    POST /ask      body = 问题（UTF-8 纯文本） → 回答（text/plain）

语音（流式，板子边录边传）：
    POST /voice    body = 8kHz/16bit/单声道 PCM（裸数据，无 WAV 头）
                   板子发完 PCM 后半关闭连接（shutdown TX），脚本读到 EOF 后：
                   faster-whisper 识别 → DeepSeek 回答 → Windows SAPI 合成
                   → 返回 audio/wav（整段，板子边收边喂 VS1053）

为什么不在板子上直接 HTTPS：
    DeepSeek 只提供 HTTPS；STM32F103 + 64KB RAM 跑 mbedTLS 要几十 KB RAM
    和上百 KB Flash，还要证书/时间，性价比低。API Key 也就不落板子。

用法（PC 上）：
    tools/llm_bridge.local.json 里填 api_key（已 gitignore），或设环境变量：
    DEEPSEEK_API_KEY / LLM_MODEL / LLM_BASE_URL

    然后：
    python tools/llm_bridge.py

依赖：Python 3.8+；语音需要
    pip install faster-whisper
    （TTS 用 Windows 自带 SAPI，不需要装包）
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error, request

# 中国网络访问 HuggingFace 经常超时；默认走镜像，并禁用 Xet（镜像不支持 CAS）。
# 已有环境变量时以用户设置为准。
os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")
os.environ.setdefault("HF_HUB_DISABLE_XET", "1")

DEFAULT_SYSTEM = (
    "你是嵌入式开发助手。回答用中文，尽量简短（不超过 200 字）；"
    "必要时给出 C 代码片段，不要长篇大论。"
)
DEFAULT_VOICE_SYSTEM = (
    "你是嵌入式开发助手，正在和用户语音对话。"
    "回答用中文、口语化，2~4 句话，尽量给出有用的信息，不超过 150 字；"
    "不要 Markdown、不要代码块、不要列表，适合直接朗读。"
)

# 可选：tools/llm_bridge.local.json（已在 .gitignore 里），用来“写死” Key，
# 这样每次启动不用配环境变量。优先级：命令行 > 环境变量 > 本地文件 > 默认值。
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(TOOLS_DIR, "llm_bridge.local.json")
TTS_SCRIPT = os.path.join(TOOLS_DIR, "tts_sapi.ps1")

VOICE_RATE = 8000
VOICE_MAX_SECONDS = 30
VOICE_MAX_PCM = VOICE_RATE * 2 * VOICE_MAX_SECONDS


def load_local_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except FileNotFoundError:
        return {}
    except Exception as e:  # noqa: BLE001
        print("[bridge] 读取 %s 失败: %s" % (CONFIG_PATH, e))
        return {}


class Bridge:
    def __init__(self, base_url, model, api_key, system, max_tokens, timeout):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.api_key = api_key
        self.system = system
        self.max_tokens = max_tokens
        self.timeout = timeout

    def ask(self, text, system=None, max_tokens=None):
        messages = [
            {"role": "system", "content": system or self.system},
            {"role": "user", "content": text},
        ]
        return self.chat(messages, max_tokens)

    def chat(self, messages, max_tokens=None):
        payload = {
            "model": self.model,
            "messages": messages,
            "stream": False,
            "temperature": 0.7,
            "max_tokens": max_tokens or self.max_tokens,
        }
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = request.Request(
            self.base_url + "/chat/completions",
            data=body,
            method="POST",
            headers={
                "Content-Type": "application/json",
                "Authorization": "Bearer " + self.api_key,
            },
        )
        try:
            with request.urlopen(req, timeout=self.timeout) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:300]
            raise RuntimeError("API HTTP %s: %s" % (e.code, detail)) from None
        except error.URLError as e:
            raise RuntimeError("网络错误: %s" % (e.reason,)) from None

        try:
            return data["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError):
            return "[bridge] 无法解析 API 响应: " + json.dumps(data, ensure_ascii=False)[:300]


class VoiceEngine:
    """PCM → ASR → LLM（带上下文）→ TTS(WAV)。模型只加载一次，串行处理。"""

    def __init__(self, bridge, asr_model, asr_language, tts_voice, tts_rate,
                 voice_max_tokens, history_turns=6, idle_reset=600, max_chars=200):
        self.bridge = bridge
        self.asr_model_name = asr_model
        self.asr_language = asr_language
        self.tts_voice = tts_voice
        self.tts_rate = tts_rate
        self.voice_max_tokens = voice_max_tokens
        self.history_turns = max(0, int(history_turns))
        self.idle_reset = max(0, int(idle_reset))
        self.max_chars = max(40, int(max_chars))
        self._model = None
        self._model_lock = threading.Lock()
        self._run_lock = threading.Lock()
        self._history = []              # [{"role": "user"/"assistant", ...}]
        self._last_time = 0.0

    def reset(self):
        with self._run_lock:
            self._history = []
            self._last_time = 0.0
        print("[voice] 对话历史已清空")

    def ask_with_history(self, text):
        """把最近 history_turns 轮对话一起送给模型。"""
        messages = [{"role": "system", "content": DEFAULT_VOICE_SYSTEM}]
        if self.history_turns > 0:
            messages.extend(self._history[-self.history_turns * 2:])
        messages.append({"role": "user", "content": text})
        answer = self.bridge.chat(messages, max_tokens=self.voice_max_tokens)

        self._history.append({"role": "user", "content": text})
        self._history.append({"role": "assistant", "content": answer})
        if self.history_turns > 0:
            self._history = self._history[-self.history_turns * 2:]
        else:
            self._history = []
        return answer

    def preload(self):
        with self._model_lock:
            if self._model is None:
                from faster_whisper import WhisperModel  # 延迟导入
                self._model = WhisperModel(self.asr_model_name, device="cpu",
                                           compute_type="int8")
        return self._model

    def transcribe(self, pcm):
        model = self.preload()
        fd, path = tempfile.mkstemp(suffix=".wav")
        os.close(fd)
        try:
            with wave.open(path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(VOICE_RATE)
                w.writeframes(pcm)
            segments, _info = model.transcribe(path, language=self.asr_language,
                                               vad_filter=True, beam_size=1)
            return "".join(s.text for s in segments).strip()
        finally:
            try:
                os.unlink(path)
            except OSError:
                pass

    def synthesize(self, text):
        fd, txt = tempfile.mkstemp(suffix=".txt")
        os.close(fd)
        fd, wav = tempfile.mkstemp(suffix=".wav")
        os.close(fd)
        try:
            with open(txt, "w", encoding="utf-8") as f:
                f.write(text)
            cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                   "-File", TTS_SCRIPT, "-TextFile", txt, "-OutFile", wav,
                   "-Rate", str(self.tts_rate)]
            if self.tts_voice:
                cmd += ["-Voice", self.tts_voice]
            r = subprocess.run(cmd, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=30)
            if r.returncode != 0:
                raise RuntimeError("SAPI TTS 失败: %s" % (r.stderr or r.stdout).strip())
            with open(wav, "rb") as f:
                return f.read()
        finally:
            for p in (txt, wav):
                try:
                    os.unlink(p)
                except OSError:
                    pass

    def handle(self, pcm):
        with self._run_lock:
            if (self.idle_reset > 0) and self._last_time and \
               (time.time() - self._last_time > self.idle_reset):
                self._history = []
                print("[voice] 空闲超时，自动清空对话历史")

            t0 = time.time()
            heard = self.transcribe(pcm)
            t1 = time.time()
            if heard:
                answer = self.ask_with_history(heard)
                if not answer.strip():
                    answer = "这个我暂时答不上来，你换个问法试试？"
            else:
                answer = "我没听清，请再说一遍。"
            t2 = time.time()
            # 太长的回答朗读会很久，按配置截断
            speak = answer[:self.max_chars]
            wav = self.synthesize(speak)
            t3 = time.time()
            self._last_time = time.time()
            print("[voice] asr %.1fs llm %.1fs tts %.1fs | %r -> %r (history %d)"
                  % (t1 - t0, t2 - t1, t3 - t2, heard, answer, len(self._history) // 2))
            return wav, heard, answer


def make_handler(bridge, voice):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def _send(self, code, body, ctype="text/plain; charset=utf-8"):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            if self.path in ("/", "/health"):
                self._send(200, "ok")
            else:
                self._send(404, "not found")

        def do_POST(self):
            if self.path == "/ask":
                self._do_ask()
            elif self.path == "/voice":
                self._do_voice()
            elif self.path == "/reset":
                if voice is not None:
                    voice.reset()
                    self._send(200, "history cleared")
                else:
                    self._send(501, "voice disabled")
            else:
                self._send(404, "not found")

        def _do_ask(self):
            try:
                n = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                n = 0
            raw = self.rfile.read(n) if n > 0 else b""
            text = raw.decode("utf-8", "replace").strip()
            if not text:
                self._send(400, "empty question")
                return

            print("[ask] %s" % text)
            t0 = time.time()
            try:
                answer = bridge.ask(text)
            except Exception as e:  # noqa: BLE001
                answer = "[bridge] 请求失败: %s" % e
            print("[ans] %.1fs, %d chars" % (time.time() - t0, len(answer)))
            self._send(200, answer)

        def _do_voice(self):
            if voice is None:
                self._send(501, "[bridge] 语音未启用（faster-whisper 未安装或模型加载失败）")
                return

            # 板子用 HTTP/1.0、不带 Content-Length：发完 PCM 后半关闭，读到 EOF 为止
            raw = self.rfile.read()
            if len(raw) > VOICE_MAX_PCM:
                raw = raw[:VOICE_MAX_PCM]
            if len(raw) < VOICE_RATE // 2:      # < 0.25 秒
                raw = b""                        # 让 ASR 返回空，走“没听清”

            t0 = time.time()
            try:
                wav, heard, answer = voice.handle(raw)
                print("[voice] total %.1fs" % (time.time() - t0))
                self._send(200, wav, ctype="audio/wav")
                return
            except Exception as e:  # noqa: BLE001
                print("[voice] 失败: %s" % e)

            # 失败也要尽量“说出来”，说不出来就回文本
            msg = "出错了：%s" % e
            try:
                self._send(200, voice.synthesize(msg[:120]), ctype="audio/wav")
            except Exception:  # noqa: BLE001
                self._send(500, msg)

    return Handler


def local_ips():
    ips = set()
    try:
        host = socket.gethostname()
        for ip in socket.gethostbyname_ex(host)[2]:
            ips.add(ip)
    except OSError:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return sorted(i for i in ips if not i.startswith("127."))


def port_in_use(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.5):
            return True
    except OSError:
        return False


def main():
    cfg = load_local_config()

    ap = argparse.ArgumentParser(description="LAN HTTP -> LLM HTTPS bridge")
    ap.add_argument("--host", default=os.environ.get("LLM_BRIDGE_HOST") or cfg.get("host", "0.0.0.0"))
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("LLM_BRIDGE_PORT") or cfg.get("port", 8765)))
    ap.add_argument("--model", default=os.environ.get("LLM_MODEL") or cfg.get("model", "deepseek-chat"))
    ap.add_argument("--base-url",
                    default=os.environ.get("LLM_BASE_URL") or cfg.get("base_url", "https://api.deepseek.com"))
    ap.add_argument("--api-key",
                    default=os.environ.get("DEEPSEEK_API_KEY") or os.environ.get("LLM_API_KEY")
                    or cfg.get("api_key", ""))
    ap.add_argument("--system", default=os.environ.get("LLM_SYSTEM") or cfg.get("system", DEFAULT_SYSTEM))
    ap.add_argument("--max-tokens", type=int,
                    default=int(os.environ.get("LLM_MAX_TOKENS") or cfg.get("max_tokens", 800)))
    ap.add_argument("--timeout", type=int,
                    default=int(os.environ.get("LLM_TIMEOUT") or cfg.get("timeout", 60)))
    ap.add_argument("--asr-model", default=os.environ.get("ASR_MODEL") or cfg.get("asr_model", "base"))
    ap.add_argument("--asr-language", default=os.environ.get("ASR_LANGUAGE") or cfg.get("asr_language", "zh"))
    ap.add_argument("--tts-voice", default=os.environ.get("TTS_VOICE") or cfg.get("tts_voice", ""))
    ap.add_argument("--tts-rate", type=int, default=int(os.environ.get("TTS_RATE") or cfg.get("tts_rate", 2)))
    ap.add_argument("--voice-max-tokens", type=int,
                    default=int(os.environ.get("VOICE_MAX_TOKENS") or cfg.get("voice_max_tokens", 500)))
    ap.add_argument("--voice-max-chars", type=int,
                    default=int(os.environ.get("VOICE_MAX_CHARS") or cfg.get("voice_max_chars", 200)))
    ap.add_argument("--voice-history-turns", type=int,
                    default=int(os.environ.get("VOICE_HISTORY_TURNS") or cfg.get("voice_history_turns", 6)))
    ap.add_argument("--voice-idle-reset", type=int,
                    default=int(os.environ.get("VOICE_IDLE_RESET") or cfg.get("voice_idle_reset", 600)))
    ap.add_argument("--no-voice", action="store_true")
    args = ap.parse_args()

    if not args.api_key:
        print("[bridge] 缺少 API Key：设置环境变量 DEEPSEEK_API_KEY，或写 %s，或传 --api-key"
              % CONFIG_PATH)
        return 2

    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8")
        except Exception:
            pass

    bridge = Bridge(args.base_url, args.model, args.api_key,
                    args.system, args.max_tokens, args.timeout)

    voice = None
    if not args.no_voice:
        voice = VoiceEngine(bridge, args.asr_model, args.asr_language,
                            args.tts_voice, args.tts_rate, args.voice_max_tokens,
                            args.voice_history_turns, args.voice_idle_reset,
                            args.voice_max_chars)
        print("[voice] 预加载 ASR 模型 %s（首次会下载，请稍等）..." % args.asr_model)
        try:
            voice.preload()
            print("[voice] ASR 就绪")
        except Exception as e:  # noqa: BLE001
            print("[voice] ASR 不可用: %s" % e)
            print("[voice] 先运行: pip install faster-whisper")
            voice = None

    if port_in_use(args.port):
        print("[bridge] 端口 %d 已被占用（可能已经有一个网关在跑）。" % args.port)
        print("[bridge] 先结束旧进程，再启动这一个；否则两个网关会各记一半对话历史。")
        return 3

    try:
        httpd = ThreadingHTTPServer((args.host, args.port), make_handler(bridge, voice))
    except OSError as e:
        print("[bridge] 监听 %s:%d 失败: %s" % (args.host, args.port, e))
        return 1

    if cfg:
        print("[bridge] local config: %s" % CONFIG_PATH)
    print("[bridge] listening on %s:%d" % (args.host, args.port))
    print("[bridge] model=%s  base=%s  max_tokens=%d" % (args.model, args.base_url, args.max_tokens))
    print("[bridge] API Key: %s..." % args.api_key[:6])
    print("[bridge] voice=%s  asr=%s  tts=SAPI(%s)"
          % ("on" if voice else "off", args.asr_model, args.tts_voice or "auto zh"))
    if voice:
        print("[voice] history=%d 轮  idle_reset=%ds  max_chars=%d  max_tokens=%d"
              % (args.voice_history_turns, args.voice_idle_reset,
                 args.voice_max_chars, args.voice_max_tokens))
    for ip in local_ips():
        print("[bridge] board command: ai server %s %d" % (ip, args.port))
    print("[bridge] Ctrl+C 停止")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[bridge] bye")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
