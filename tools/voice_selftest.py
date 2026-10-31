import io
import os
import subprocess
import tempfile
import time

tools = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp(prefix="voice_test_")
txt = os.path.join(tmp, "tts_test.txt")
wav = os.path.join(tmp, "tts_test.wav")

io.open(txt, "w", encoding="utf-8").write("今天天气怎么样？")

t0 = time.time()
r = subprocess.run(
    ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
     "-File", os.path.join(tools, "tts_sapi.ps1"),
     "-TextFile", txt, "-OutFile", wav],
    capture_output=True, text=True, encoding="utf-8", errors="replace")
print("tts rc=%d %.1fs %s" % (r.returncode, time.time() - t0, r.stdout.strip()))
print("wav size=%d" % os.path.getsize(wav))

from faster_whisper import WhisperModel

t0 = time.time()
model = WhisperModel("base", device="cpu", compute_type="int8")
print("model loaded %.1fs" % (time.time() - t0))

t0 = time.time()
segments, info = model.transcribe(wav, language="zh", vad_filter=True, beam_size=1)
text = "".join(s.text for s in segments).strip()
print("asr %.1fs: %r" % (time.time() - t0, text))
