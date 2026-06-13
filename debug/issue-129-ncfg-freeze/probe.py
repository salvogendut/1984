#!/usr/bin/env python3
"""NCFG-freeze probe — kbd-PTY input, final screenshot OCR.

For each run: launch 1984 with --kbd-pty and --screenshot-at, send
`|hdcpm` after the UniDOS probe, send `ncfg -r` once CP/M+ should be up,
let the screenshot fire and 1984 exit, then OCR the final image to
classify: PASS / FREEZE / REBOOTED. Per-run artifacts in /tmp/ncfg-probe/.

Usage: debug/issue-129-ncfg-freeze/probe.py [N]
"""
import os, sys, re, time, subprocess, signal

REPO = os.path.abspath(os.path.dirname(__file__) + "/../..")
ART  = "/tmp/ncfg-probe"
os.makedirs(ART, exist_ok=True)

# Frame plan (50 Hz). Wall clock for each frame N: N/50 seconds.
#  0–900       UniDOS probe (paste is gated by ONE_K_AUTOSTART_FRAMES=900).
#  900         send "|hdcpm\r".
#  900-2700    HDCPM ROM banner + CP/M+ kernel load → A0:CPM> by ~2700.
#  3500        send "ncfg -r\r".
#  3500-4500   NCFG reset (~5–10 s).
#  4500        send "ncfg -a:cpc\r".
#  4500-6000   DHCP cycle (~10–15 s).
#  6000        send "ping -c5 slashdot.org\r".
#  6000-10000  ping completes (5 ICMP, ~5–10 s).
# 11000        --screenshot-at fires, 1984 exits.
HDCPM_SEND_FRAME  = 900
NCFG_R_FRAME      = 3500
NCFG_A_FRAME      = 4500
PING_FRAME        = 6000
SHOT_FRAME        = 11000
FRAME_S           = 1 / 50.0

def ocr(png):
    try:
        out = subprocess.run(
            ["distrobox", "enter", "my-distrobox", "--", "tesseract", png, "-"],
            capture_output=True, text=True, timeout=20)
        return out.stdout
    except Exception as e:
        return f"<<ocr error: {e}>>"

def run_once(run_id):
    rd = f"{ART}/run{run_id}"
    ppm    = f"{rd}.ppm"
    png    = f"{rd}.png"
    log    = f"{rd}.log"
    rep_p  = f"{rd}-report.txt"
    for f in (ppm, png, log, rep_p):
        try: os.unlink(f)
        except: pass

    rep = open(rep_p, "w")
    proc = subprocess.Popen(
        ["./1984", "--memory=576", "--kbd-pty",
         f"--screenshot-at={SHOT_FRAME}:{ppm}"],
        cwd=REPO,
        stderr=open(log, "w"), stdout=subprocess.DEVNULL,
        env={**os.environ,
             "ONE_K_TRACE_LBA": "1",
             "ONE_K_TRACE_IM1": "1"},
        preexec_fn=os.setsid,
    )
    # Find the kbd PTY path from the stderr file.
    kbd_pty = None
    deadline = time.monotonic() + 5
    while not kbd_pty:
        try:
            with open(log) as f:
                for line in f:
                    m = re.search(r"kbd PTY: (\S+)", line)
                    if m: kbd_pty = m.group(1); break
        except FileNotFoundError: pass
        if time.monotonic() > deadline: break
        time.sleep(0.1)
    if not kbd_pty:
        proc.kill(); rep.write("no kbd PTY\n"); return "ERROR"
    rep.write(f"kbd PTY: {kbd_pty}\n")

    kfd = os.open(kbd_pty, os.O_RDWR | os.O_NONBLOCK)
    t0 = time.monotonic()

    def send_at_frame(frame, payload):
        target = t0 + frame * FRAME_S
        delay = max(0, target - time.monotonic())
        time.sleep(delay)
        os.write(kfd, payload)

    try:
        send_at_frame(HDCPM_SEND_FRAME, b"|hdcpm\r")
        send_at_frame(NCFG_R_FRAME,     b"ncfg -r\r")
        send_at_frame(NCFG_A_FRAME,     b"ncfg -a:cpc\r")
        send_at_frame(PING_FRAME,       b"ping -c5 slashdot.org\r")
        # Wait for 1984 to take the screenshot and exit on its own.
        proc.wait(timeout=180)
    except subprocess.TimeoutExpired:
        rep.write("emu timed out (did not exit at screenshot frame)\n")
    finally:
        os.close(kfd)
        try: os.killpg(proc.pid, signal.SIGTERM)
        except: pass

    if not os.path.exists(ppm) or os.path.getsize(ppm) == 0:
        rep.write("no screenshot produced\n")
        return "ERROR"
    subprocess.run(["convert", ppm, png], check=False)
    text = ocr(png)
    rep.write("--- OCR ---\n" + text + "---\n")

    low = text.lower()
    # PASS: see the ping result table (PING statistic / packets transmitted)
    if "packets transmitted" in low or "ping statistic" in low:
        return "PASS_PING"
    # REBOOT: BASIC banner reappeared
    if "amstrad 128k" in low or "locomotive" in low:
        return "REBOOTED"
    # NCFG completed config but ping didn't run / didn't return
    if ("network" in low and "gateway" in low) and "slashdot" in low:
        return "PING_HUNG"
    if "network" in low and "gateway" in low:
        return "POST_NCFG_NO_PING"
    # ncfg -r typed but no NCFG output → freeze in ncfg -r
    if "ncfg -r" in low and re.search(r"a.:.pm", low):
        return "FREEZE_NCFG_R"
    return "OTHER"

def main():
    N = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    tally = {}
    for i in range(1, N+1):
        out = run_once(i)
        tally[out] = tally.get(out, 0) + 1
        print(f"[{i:2d}/{N}] {out}")
    print("---")
    for k, v in sorted(tally.items()):
        print(f"  {k}: {v}")

if __name__ == "__main__":
    main()
