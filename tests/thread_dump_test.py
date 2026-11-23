"""Runs a deliberately hung guest under xbem, sends Ctrl+\\ (SIGQUIT, dump and
continue) and then Ctrl+C (SIGINT, dump and exit), and checks the dumps show
both guest threads stuck in WaitForSingleObject."""
import re
import signal
import subprocess
import sys
import time

xbem, exe = sys.argv[1], sys.argv[2]
proc = subprocess.Popen([xbem, "run", exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
deadline = time.time() + 10
time.sleep(0.5)
proc.send_signal(signal.SIGQUIT)
time.sleep(0.5)
proc.send_signal(signal.SIGINT)
try:
    out, _ = proc.communicate(timeout=10)
except subprocess.TimeoutExpired:
    proc.kill()
    print("xbem did not exit after SIGINT")
    sys.exit(1)
text = out.decode(errors="replace")
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
