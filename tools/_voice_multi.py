import audioop
import os
import socket
import subprocess
import tempfile
import time
import wave

TOOLS = os.path.dirname(os.path.abspath(__file__))
TMP = tempfile.mkdtemp(prefix="voice_multi_")


def ask(text):
    txt = os.path.join(TMP, "q.txt")
    wav = os.path.join(TMP, "q.wav")
    out = os.path.join(TMP, "reply.wav")
    with open(txt, "w", encoding="utf-8") as f:
        f.write(text)
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", os.path.join(TOOLS, "tts_sapi.ps1"),
                    "-TextFile", txt, "-OutFile", wav], check=True,
                   capture_output=True)
    with wave.open(wav, "rb") as w:
        rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
        pcm = w.readframes(w.getnframes())
    if ch == 2:
        pcm = audioop.tomono(pcm, sw, 0.5, 0.5)
    if rate != 8000:
        pcm, _ = audioop.ratecv(pcm, sw, 1, rate, 8000, None)

    s = socket.create_connection(("127.0.0.1", 8765), timeout=120)
    req = (b"POST /voice HTTP/1.0\r\nHost: 127.0.0.1:8765\r\n"
           b"Content-Type: application/octet-stream\r\n\r\n")
    t0 = time.time()
    s.sendall(req + pcm)
    s.shutdown(socket.SHUT_WR)
    chunks = []
    while True:
        b = s.recv(4096)
        if not b:
            break
        chunks.append(b)
    s.close()
    resp = b"".join(chunks)
    head, _, body = resp.partition(b"\r\n\r\n")
    with open(out, "wb") as f:
        f.write(body)
    dur = 0.0
    try:
        with wave.open(out, "rb") as w:
            dur = w.getnframes() / w.getframerate()
    except Exception:
        pass
    print("Q=%r  %.1fs  audio=%.1fs  body=%d" % (text, time.time() - t0, dur, len(body)))


ask("我叫小明，请记住我的名字。")
ask("我叫什么名字？")
ask("我最喜欢什么颜色？")
