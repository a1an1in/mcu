import audioop
import os
import socket
import subprocess
import tempfile
import time
import wave

TOOLS = os.path.dirname(os.path.abspath(__file__))
TMP = tempfile.mkdtemp(prefix="voice_client_")
TXT = os.path.join(TMP, "q.txt")
WAV = os.path.join(TMP, "q.wav")

with open(TXT, "w", encoding="utf-8") as f:
    f.write("今天天气怎么样？")

t0 = time.time()
subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", os.path.join(TOOLS, "tts_sapi.ps1"),
                "-TextFile", TXT, "-OutFile", WAV], check=True,
               capture_output=True)
print("tts question %.1fs" % (time.time() - t0))

with wave.open(WAV, "rb") as w:
    rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
    pcm = w.readframes(w.getnframes())
print("question wav %d Hz %d ch %d bit %.1fs" % (rate, ch, sw * 8, len(pcm) / (rate * ch * sw)))
if ch == 2:
    pcm = audioop.tomono(pcm, sw, 0.5, 0.5)
if rate != 8000:
    pcm, _ = audioop.ratecv(pcm, sw, 1, rate, 8000, None)
print("pcm 8k mono bytes=%d (%.1fs)" % (len(pcm), len(pcm) / 16000))

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
dt = time.time() - t0
resp = b"".join(chunks)
head, _, body = resp.partition(b"\r\n\r\n")
head_text = head.decode("utf-8", "replace")
print("response %.1fs header:\n%s" % (dt, head_text))
print("body bytes=%d" % len(body))

ctype = ""
for line in head_text.splitlines():
    if line.lower().startswith("content-type:"):
        ctype = line.split(":", 1)[1].strip().lower()
out_ext = ".mp3" if "mpeg" in ctype else ".wav"
out = os.path.join(TMP, "reply" + out_ext)
with open(out, "wb") as f:
    f.write(body)

if out_ext == ".wav":
    try:
        with wave.open(out, "rb") as w:
            print("reply wav %d Hz %d ch %.1fs" % (w.getframerate(), w.getnchannels(),
                                                   w.getnframes() / w.getframerate()))
    except Exception as e:
        print("reply is not wav:", e, body[:80])
else:
    print("reply %s %d bytes" % (ctype, len(body)))

try:
    from faster_whisper import WhisperModel
    m = WhisperModel("base", device="cpu", compute_type="int8")
    segs, _ = m.transcribe(out, language="zh", vad_filter=True)
    print("reply text:", "".join(x.text for x in segs).strip())
except Exception as e:
    print("reply transcribe skipped:", e)
