#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""llm_bridge.py - 让 STM32 板子通过局域网和 DeepSeek（或兼容 API）对话。

板子只发明文 HTTP 到这个脚本：

    POST /ask
    Content-Type: text/plain
    body = 用户问题（UTF-8）

脚本用你的 API Key 转发到 https://api.deepseek.com/chat/completions，
把 JSON 里的 choices[0].message.content 取出来，原样（纯文本）返回给板子。

为什么不在板子上直接 HTTPS：
    DeepSeek 只提供 HTTPS；STM32F103 + 64KB RAM 跑 mbedTLS 要几十 KB RAM
    和上百 KB Flash，还要处理证书与时间，性价比很低。API Key 也就不用
    烧进固件。

用法（PC 上）：
    set DEEPSEEK_API_KEY=sk-xxxx            # PowerShell: $env:DEEPSEEK_API_KEY="sk-xxxx"
    set LLM_MODEL=deepseek-chat             # 或你的服务商支持的模型 ID
    python tools/llm_bridge.py

    然后板子上：
    msh > ai server 192.168.1.100 8765      # 换成 PC 的局域网 IP
    msh > ai ask 你好，介绍一下你自己

依赖：Python 3.8+，只用标准库。
"""

import argparse
import json
import os
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error, request

DEFAULT_SYSTEM = (
    "你是嵌入式开发助手。回答用中文，尽量简短（不超过 200 字）；"
    "必要时给出 C 代码片段，不要长篇大论。"
)

# 可选：tools/llm_bridge.local.json（已在 .gitignore 里），用来“写死” Key，
# 这样每次启动不用配环境变量。优先级：命令行 > 环境变量 > 本地文件 > 默认值。
CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "llm_bridge.local.json")


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

    def ask(self, text):
        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": self.system},
                {"role": "user", "content": text},
            ],
            "stream": False,
            "temperature": 0.7,
            "max_tokens": self.max_tokens,
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


def make_handler(bridge):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            # 默认日志太吵；真正的请求日志在 do_POST 里打
            pass

        def _send(self, code, body):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(body)
            except BrokenPipeError:
                pass

        def do_GET(self):
            if self.path in ("/", "/health"):
                self._send(200, "ok")
            else:
                self._send(404, "not found")

        def do_POST(self):
            if self.path != "/ask":
                self._send(404, "not found")
                return

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
            except Exception as e:  # noqa: BLE001 - 把错误原文回给板子
                answer = "[bridge] 请求失败: %s" % e
            dt = time.time() - t0
            print("[ans] %.1fs, %d chars" % (dt, len(answer)))

            self._send(200, answer)

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

    try:
        httpd = ThreadingHTTPServer((args.host, args.port), make_handler(bridge))
    except OSError as e:
        print("[bridge] 监听 %s:%d 失败: %s" % (args.host, args.port, e))
        return 1

    if cfg:
        print("[bridge] local config: %s" % CONFIG_PATH)
    print("[bridge] listening on %s:%d" % (args.host, args.port))
    print("[bridge] model=%s  base=%s  max_tokens=%d" % (args.model, args.base_url, args.max_tokens))
    print("[bridge] API Key: %s..." % args.api_key[:6])
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
