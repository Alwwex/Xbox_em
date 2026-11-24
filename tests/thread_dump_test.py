"""Runs a deliberately hung guest under xbem, sends Ctrl+\\ (SIGQUIT, dump and
continue) and then Ctrl+C (SIGINT, dump and exit), and checks the dumps show
both guest threads stuck in WaitForSingleObject."""
import re
import signal
import subprocess
import sys
import threading
import time

xbem, exe = sys.argv[1], sys.argv[2]
proc = subprocess.Popen([xbem, "run", exe, "--no-window"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
lines = []


def read_output():
    for line in proc.stdout:
        lines.append(line.decode(errors="replace"))


reader = threading.Thread(target=read_output, daemon=True)
reader.start()
# Signal only once both guest threads wait (startup takes longer under load).
deadline = time.time() + 20
while time.time() < deadline and not any("waiting forever" in l for l in lines):
    time.sleep(0.05)
time.sleep(0.3)
proc.send_signal(signal.SIGQUIT)
time.sleep(0.5)
proc.send_signal(signal.SIGINT)
try:
    proc.wait(timeout=10)
except subprocess.TimeoutExpired:
    proc.kill()
    print("xbem did not exit after SIGINT")
    sys.exit(1)
reader.join(timeout=5)
text = "".join(lines)
print(text)
checks = [
    (r"guest thread dump \(Ctrl\+\\, continuing\)", "SIGQUIT dump"),
    (r"interrupted \(Ctrl\+C\) - guest thread dump", "SIGINT dump"),
    (r"hang\.exe\+0x[0-9a-f]+  -> calls kernelx\.dll!WaitForSingleObject", "call site decoded"),
]
failed = False
for pattern, what in checks:
    if not re.search(pattern, text):
        print(f"MISSING: {what} ({pattern})")
        failed = True
if len(re.findall(r"--- guest thread", text)) < 4:  # 2 threads x 2 dumps
    print("MISSING: both threads in both dumps")
    failed = True
if proc.returncode != 130:
    print(f"unexpected exit code {proc.returncode}")
    failed = True
sys.exit(1 if failed else 0)
