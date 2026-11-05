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
    pip install faster-whisper edge-tts zhconv
    （TTS 用 Windows 自带 SAPI，不需要装包；zhconv 用于繁体转简体）
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

try:
    import zhconv
except ImportError:  # 没装时退化为原文，不影响启动
    zhconv = None


def to_simplified(text):
    """把模型/ASR 输出统一转成简体中文。"""
    if not text or zhconv is None:
        return text
    try:
        return zhconv.convert(text, "zh-cn")
    except Exception:  # noqa: BLE001
        return text

DEFAULT_SYSTEM = (
    "你是嵌入式开发助手。回答用简体中文，尽量简短（不超过 200 字）；"
    "必要时给出 C 代码片段，不要长篇大论。"
    "不要输出繁体字。"
)
DEFAULT_VOICE_SYSTEM = (
    "你是嵌入式开发助手，正在和用户语音对话。"
    "用和用户相同的语言回答：用户说中文就用中文，用户说英文就用英文。"
    "口语化，2~4 句话，尽量给出有用的信息，不超过 150 字；"
    "中文必须使用简体中文，不要使用繁体字；即使问题里出现繁体字，也用简体回答。"
    "不要 Markdown、不要代码块、不要列表，适合直接朗读。"
)

# 可选：tools/llm_bridge.local.json（已在 .gitignore 里），用来“写死” Key，
# 这样每次启动不用配环境变量。优先级：命令行 > 环境变量 > 本地文件 > 默认值。
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(TOOLS_DIR, "llm_bridge.local.json")
TTS_SCRIPT = os.path.join(TOOLS_DIR, "tts_sapi.ps1")

VOICE_RATE = 8000
VOICE_MAX_SECONDS = 60
VOICE_MAX_PCM = VOICE_RATE * 2 * VOICE_MAX_SECONDS
VOICE_TEXT_MAX = 3800       # 响应里附带的对话文字上限（板子缓冲 4KB）


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
                 voice_max_tokens, history_turns=6, idle_reset=600, max_chars=200,
                 tts_backend="edge", tts_edge_voice="zh-CN-XiaoxiaoNeural",
                 tts_edge_voice_en="en-US-AriaNeural",
                 tts_edge_rate="+5%", asr_vad=True, asr_beam_size=5,
                 asr_model_en="", dump_dir=""):
        self.bridge = bridge
        self.asr_model_name = asr_model
        self.asr_model_en = asr_model_en
        self.dump_dir = dump_dir
        self.asr_language = asr_language
        self.tts_voice = tts_voice
        self.tts_rate = tts_rate
        self.tts_backend = tts_backend
        self.tts_edge_voice = tts_edge_voice
        self.tts_edge_voice_en = tts_edge_voice_en
        self.tts_edge_rate = tts_edge_rate
        self.asr_vad = bool(asr_vad)
        self.asr_beam_size = max(1, int(asr_beam_size))
        self.voice_max_tokens = voice_max_tokens
        self.history_turns = max(0, int(history_turns))
        self.idle_reset = max(0, int(idle_reset))
        self.max_chars = max(40, int(max_chars))
        self._models = {}
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
        answer = to_simplified(answer)

        self._history.append({"role": "user", "content": text})
        self._history.append({"role": "assistant", "content": answer})
        if self.history_turns > 0:
            self._history = self._history[-self.history_turns * 2:]
        else:
            self._history = []
        return answer

    def _load(self, name):
        with self._model_lock:
            model = self._models.get(name)
            if model is None:
                from faster_whisper import WhisperModel  # 延迟导入
                print("[voice] 加载 ASR 模型 %s ..." % name)
                model = WhisperModel(name, device="cpu", compute_type="int8")
                self._models[name] = model
            return model

    def preload(self):
        model = self._load(self.asr_model_name)
        if self.asr_model_en and self.asr_model_en != self.asr_model_name:
            self._load(self.asr_model_en)      # 英文用专用模型，首次启动就下好
        return model

    def _dump(self, pcm, rms, peak, rate=VOICE_RATE):
        """把上传的 PCM 存成 WAV，方便事后对比识别效果（VOICE_DUMP=目录）。"""
        try:
            os.makedirs(self.dump_dir, exist_ok=True)
            name = time.strftime("voice-%Y%m%d-%H%M%S") + \
                "-rms%d-peak%d-%dHz.wav" % (rms, peak, rate)
            path = os.path.join(self.dump_dir, name)
            with wave.open(path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(rate)
                w.writeframes(pcm)
            keep = sorted(f for f in os.listdir(self.dump_dir)
                          if f.startswith("voice-") and f.endswith(".wav"))
            for old in keep[:-20]:
                try:
                    os.unlink(os.path.join(self.dump_dir, old))
                except OSError:
                    pass
        except Exception as e:  # noqa: BLE001
            print("[voice] 保存 PCM 失败: %s" % e)

    @staticmethod
    def _highpass(pcm, rate, lo=100.0, hi=140.0):
        """100~140Hz 高通：板子的 MIC 有很强的 50Hz 工频噪声（占噪声能量 95%），
        滤掉以后语音的信噪比明显变好。用 FFT 实现，numpy 即可，无 scipy 依赖。"""
        try:
            import numpy as np
            x = np.frombuffer(pcm, dtype=np.int16).astype(np.float64)
            if len(x) < 64:
                return pcm
            spec = np.fft.rfft(x)
            freq = np.fft.rfftfreq(len(x), 1.0 / rate)
            gain = np.clip((freq - lo) / (hi - lo), 0.0, 1.0)
            y = np.fft.irfft(spec * gain, n=len(x))
            return np.clip(y, -32768, 32767).astype(np.int16).tobytes()
        except Exception:
            return pcm

    def transcribe(self, pcm, rate=VOICE_RATE):
        """返回 (识别文本, 语言)。asr_language=auto 时由 Whisper 自动判断。"""
        if pcm:
            try:
                import audioop
                rms = audioop.rms(pcm, 2)
                peak = audioop.max(pcm, 2)
                print("[voice] pcm %d bytes (%.2fs @%dHz) rms=%d peak=%d%s"
                      % (len(pcm), len(pcm) / (rate * 2.0), rate, rms, peak,
                         "  << 可能削波，调小 MIC 增益" if peak >= 30000 else ""))
                if self.dump_dir:
                    self._dump(pcm, rms, peak, rate)
            except Exception:
                print("[voice] pcm %d bytes" % len(pcm))

            # 存档是原始 PCM；送 ASR 前先滤掉 50Hz 工频噪声
            pcm = self._highpass(pcm, rate)

        auto = (self.asr_language or "").lower() in ("", "auto", "detect")
        model = self._models.get(self.asr_model_name) or self.preload()
        fd, path = tempfile.mkstemp(suffix=".wav")
        os.close(fd)
        try:
            with wave.open(path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(rate)
                w.writeframes(pcm)
            segments, info = model.transcribe(
                path,
                language=None if auto else self.asr_language,
                vad_filter=self.asr_vad,
                beam_size=self.asr_beam_size,
                condition_on_previous_text=False)
            text = "".join(s.text for s in segments).strip()
            lang = (getattr(info, "language", None) or
                    ("zh" if not auto else "unknown"))
            if auto:
                print("[voice] lang=%s p=%.2f" %
                      (lang, getattr(info, "language_probability", 0.0)))

            # 英文换专用模型再识别一遍（small.en 比 base 准很多）
            if (str(lang).lower().startswith("en") and self.asr_model_en
                    and self.asr_model_en != self.asr_model_name):
                m2 = self._load(self.asr_model_en)
                seg2, _ = m2.transcribe(
                    path,
                    language=None if self.asr_model_en.endswith(".en") else "en",
                    vad_filter=self.asr_vad,
                    beam_size=self.asr_beam_size,
                    condition_on_previous_text=False)
                text2 = "".join(s.text for s in seg2).strip()
                if text2:
                    text = text2
            return text, lang
        finally:
            try:
                os.unlink(path)
            except OSError:
                pass

    def synthesize(self, text, lang="zh"):
        """返回 (音频字节, MIME)。默认 edge-tts（自然），失败回退 SAPI（离线）。"""
        is_en = str(lang).lower().startswith("en")
        if self.tts_backend == "edge":
            voice = self.tts_edge_voice_en if is_en else self.tts_edge_voice
            fd, mp3 = tempfile.mkstemp(suffix=".mp3")
            os.close(fd)
            try:
                import asyncio
                import edge_tts
                asyncio.run(edge_tts.Communicate(
                    text, voice, rate=self.tts_edge_rate).save(mp3))
                with open(mp3, "rb") as f:
                    return f.read(), "audio/mpeg"
            except Exception as e:  # noqa: BLE001 - 网络/包缺失都回退
                print("[voice] edge-tts 失败，回退 SAPI: %s" % e)
            finally:
                try:
                    os.unlink(mp3)
                except OSError:
                    pass

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
            sapi_voice = self.tts_voice or ("Microsoft Zira Desktop" if is_en else "")
            if sapi_voice:
                cmd += ["-Voice", sapi_voice]
            r = subprocess.run(cmd, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=30)
            if r.returncode != 0:
                raise RuntimeError("SAPI TTS 失败: %s" % (r.stderr or r.stdout).strip())
            with open(wav, "rb") as f:
                return f.read(), "audio/wav"
        finally:
            for p in (txt, wav):
                try:
                    os.unlink(p)
                except OSError:
                    pass

    def answer(self, pcm, rate=VOICE_RATE):
        with self._run_lock:
            if (self.idle_reset > 0) and self._last_time and \
               (time.time() - self._last_time > self.idle_reset):
                self._history = []
                print("[voice] 空闲超时，自动清空对话历史")

            t0 = time.time()
            heard, lang = self.transcribe(pcm, rate)
            heard = to_simplified(heard)
            t1 = time.time()
            if heard:
                answer = self.ask_with_history(heard)
                if not answer.strip():
                    answer = "这个我暂时答不上来，你换个问法试试？"
            else:
                answer = "我没听清，请再说一遍。"
                print("[voice] 识别为空：如果麦克风有电平，试 ASR_VAD=0")
            t2 = time.time()
            self._last_time = time.time()
            print("[voice] asr %.1fs llm %.1fs | [%s] %r -> %r (history %d)"
                  % (t1 - t0, t2 - t1, lang, heard, answer,
                     len(self._history) // 2))
            return heard, answer, lang

    def _edge_chunks(self, text, voice):
        """逐块产出 edge-tts 的音频（边生成边下发，首音更快）。"""
        import asyncio
        import edge_tts

        async def _agen():
            comm = edge_tts.Communicate(text, voice, rate=self.tts_edge_rate)
            async for ch in comm.stream():
                if (ch.get("type") == "audio") and ch.get("data"):
                    yield ch["data"]

        loop = asyncio.new_event_loop()
        try:
            it = _agen()
            while True:
                try:
                    chunk = loop.run_until_complete(it.__anext__())
                except StopAsyncIteration:
                    break
                yield chunk
        finally:
            loop.close()

    def tts_start(self, text, lang="zh"):
        """返回 (mime, 音频块迭代器)；edge 不可用时回退 SAPI 整段。"""
        is_en = str(lang).lower().startswith("en")

        if self.tts_backend == "edge":
            voice = self.tts_edge_voice_en if is_en else self.tts_edge_voice
            try:
                gen = self._edge_chunks(text, voice)
                first = next(gen)          # 先拿到首块，确定能播再发响应头
                def _chain(head, rest):
                    yield head
                    for c in rest:
                        yield c
                return "audio/mpeg", _chain(first, gen)
            except Exception as e:  # noqa: BLE001
                print("[voice] edge-tts 流式失败，回退 SAPI: %s" % e)

        audio, mime = self.synthesize(text, lang)
        return mime, iter([audio])


def make_handler(bridge, voice):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def _send(self, code, body, ctype="text/plain; charset=utf-8", extra=None):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
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
                answer = to_simplified(answer)
            except Exception as e:  # noqa: BLE001
                answer = "[bridge] 请求失败: %s" % e
            print("[ans] %.1fs, %d chars" % (time.time() - t0, len(answer)))
            self._send(200, answer)

        def _do_voice(self):
            if voice is None:
                self._send(501, "[bridge] 语音未启用（faster-whisper 未安装或模型加载失败）")
                return

            # 板子用 X-Sample-Rate 说明 PCM 采样率（8k/16k…），没写就按 8k
            try:
                rate = int(self.headers.get("X-Sample-Rate") or VOICE_RATE)
            except (TypeError, ValueError):
                rate = VOICE_RATE
            if rate < 8000 or rate > 48000:
                rate = VOICE_RATE

            # 板子用 HTTP/1.0、不带 Content-Length：发完 PCM 后半关闭，读到 EOF 为止
            raw = self.rfile.read()
            max_pcm = rate * 2 * VOICE_MAX_SECONDS
            if len(raw) > max_pcm:
                raw = raw[:max_pcm]
            if len(raw) < rate // 2:            # < 0.25 秒
                raw = b""                        # 让 ASR 返回空，走“没听清”

            t0 = time.time()
            started = False
            try:
                heard, answer, lang = voice.answer(raw, rate)

                # 板子带 X-Want-Text: 1 时，把「你/AI」两行文字放在音频前面
                want_text = (self.headers.get("X-Want-Text") or "").strip() \
                    not in ("", "0")
                text = ("你: %s\nAI: %s\n"
                        % (heard or "(没听清)", answer)).encode("utf-8")
                if len(text) > VOICE_TEXT_MAX:
                    text = (text[:VOICE_TEXT_MAX].decode("utf-8", "ignore")
                            .encode("utf-8") + "\n…(文字过长已截断)\n".encode("utf-8"))

                speak = answer[:voice.max_chars]

                # 先把响应头和文字发出去（板子马上能显示），再开始流式 TTS。
                # 预计的 MIME 先按后端给；edge 失败回退 SAPI 时，VS1053 会自己识别 WAV。
                expect_mime = "audio/mpeg" if voice.tts_backend == "edge" else "audio/wav"
                self.send_response(200)
                started = True
                self.send_header("Content-Type", expect_mime)
                self.send_header("Connection", "close")
                if want_text:
                    self.send_header("X-Text-Length", str(len(text)))
                self.end_headers()
                self.close_connection = True

                if want_text:
                    self.wfile.write(text)
                    self.wfile.flush()

                mime, chunks = voice.tts_start(speak, lang)
                if mime != expect_mime:
                    print("[voice] TTS 回退为 %s" % mime)

                t_first = 0.0
                audio_bytes = 0
                for chunk in chunks:
                    if t_first == 0.0:
                        t_first = time.time() - t0
                        print("[voice] 首块音频 %.1fs" % t_first)
                    audio_bytes += len(chunk)
                    self.wfile.write(chunk)
                print("[voice] total %.1fs（首块 %.1fs，音频 %d 字节）"
                      % (time.time() - t0, t_first, audio_bytes))
                return
            except (BrokenPipeError, ConnectionResetError):
                print("[voice] 板子断开连接")
                return
            except Exception as e:  # noqa: BLE001
                err = e
                print("[voice] 失败: %s" % e)
                if started:
                    return

            # 失败也要尽量“说出来”，说不出来就回文本
            msg = "出错了：%s" % err
            try:
                data, mime = voice.synthesize(msg[:120])
                self._send(200, data, ctype=mime)
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
    ap.add_argument("--asr-model", default=os.environ.get("ASR_MODEL") or cfg.get("asr_model", "small"))
    ap.add_argument("--asr-model-en",
                    default=os.environ.get("ASR_MODEL_EN") or cfg.get("asr_model_en", "small.en"))
    ap.add_argument("--asr-language", default=os.environ.get("ASR_LANGUAGE") or cfg.get("asr_language", "auto"))
    ap.add_argument("--asr-vad", type=int,
                    default=int(os.environ.get("ASR_VAD") or cfg.get("asr_vad", 0)))
    ap.add_argument("--asr-beam-size", type=int,
                    default=int(os.environ.get("ASR_BEAM_SIZE") or cfg.get("asr_beam_size", 5)))
    ap.add_argument("--tts-voice", default=os.environ.get("TTS_VOICE") or cfg.get("tts_voice", ""))
    ap.add_argument("--tts-rate", type=int, default=int(os.environ.get("TTS_RATE") or cfg.get("tts_rate", 2)))
    ap.add_argument("--tts-backend", choices=["edge", "sapi"],
                    default=os.environ.get("TTS_BACKEND") or cfg.get("tts_backend", "edge"))
    ap.add_argument("--tts-edge-voice",
                    default=os.environ.get("TTS_EDGE_VOICE") or cfg.get("tts_edge_voice", "zh-CN-XiaoxiaoNeural"))
    ap.add_argument("--tts-edge-voice-en",
                    default=os.environ.get("TTS_EDGE_VOICE_EN") or cfg.get("tts_edge_voice_en", "en-US-AriaNeural"))
    ap.add_argument("--tts-edge-rate",
                    default=os.environ.get("TTS_EDGE_RATE") or cfg.get("tts_edge_rate", "+5%"))
    ap.add_argument("--voice-max-tokens", type=int,
                    default=int(os.environ.get("VOICE_MAX_TOKENS") or cfg.get("voice_max_tokens", 500)))
    ap.add_argument("--voice-max-chars", type=int,
                    default=int(os.environ.get("VOICE_MAX_CHARS") or cfg.get("voice_max_chars", 200)))
    ap.add_argument("--voice-history-turns", type=int,
                    default=int(os.environ.get("VOICE_HISTORY_TURNS") or cfg.get("voice_history_turns", 6)))
    ap.add_argument("--voice-idle-reset", type=int,
                    default=int(os.environ.get("VOICE_IDLE_RESET") or cfg.get("voice_idle_reset", 600)))
    ap.add_argument("--no-voice", action="store_true")
    ap.add_argument("--dump-voice", metavar="DIR",
                    default=os.environ.get("VOICE_DUMP") or cfg.get("dump_voice", ""))
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

    if port_in_use(args.port):
        print("[bridge] 端口 %d 已被占用（可能已经有一个网关在跑）。" % args.port)
        print("[bridge] 先结束旧进程，再启动这一个；否则两个网关会各记一半对话历史。")
        return 3

    bridge = Bridge(args.base_url, args.model, args.api_key,
                    args.system, args.max_tokens, args.timeout)

    voice = None
    if not args.no_voice:
        voice = VoiceEngine(bridge, args.asr_model, args.asr_language,
                            args.tts_voice, args.tts_rate, args.voice_max_tokens,
                            args.voice_history_turns, args.voice_idle_reset,
                            args.voice_max_chars, args.tts_backend,
                            args.tts_edge_voice, args.tts_edge_voice_en,
                            args.tts_edge_rate,
                            args.asr_vad != 0, args.asr_beam_size,
                            asr_model_en=args.asr_model_en,
                            dump_dir=args.dump_voice)
        print("[voice] 预加载 ASR 模型 %s（首次会下载，请稍等）..." % args.asr_model)
        try:
            voice.preload()
            print("[voice] ASR 就绪")
        except Exception as e:  # noqa: BLE001
            print("[voice] ASR 不可用: %s" % e)
            print("[voice] 先运行: pip install faster-whisper")
            voice = None

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
    tts_desc = ("edge(%s / %s)" % (args.tts_edge_voice, args.tts_edge_voice_en)) \
               if args.tts_backend == "edge" \
               else ("SAPI(%s)" % (args.tts_voice or "auto zh"))
    print("[bridge] voice=%s  asr=%s%s  lang=%s  tts=%s"
          % ("on" if voice else "off", args.asr_model,
             ("/en:" + args.asr_model_en)
             if (args.asr_model_en and args.asr_model_en != args.asr_model) else "",
             args.asr_language, tts_desc))
    if voice:
        print("[voice] history=%d 轮  idle_reset=%ds  max_chars=%d  max_tokens=%d  vad=%s  beam=%d"
              % (args.voice_history_turns, args.voice_idle_reset,
                 args.voice_max_chars, args.voice_max_tokens,
                 "on" if args.asr_vad else "off", args.asr_beam_size))
        if args.dump_voice:
            print("[voice] PCM 存档: %s（只保留最近 20 个）" % args.dump_voice)
    for ip in local_ips():
        print("[bridge] board command: chat server %s %d" % (ip, args.port))
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
