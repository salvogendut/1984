#!/usr/bin/env python3
"""Boot-only regression check for #128 (HDCPM reaches A0:CPM>).

Drives the same boot path as probe.py but stops after the CP/M+ prompt
appears — no NCFG, no ping. Sanity check: PR #128 establishes 40/40
on real Cyboard CF. Anything ≤9/10 here = CPU-timing regression and
the experimental fix needs to be reverted.

Usage: boot_only_check.py [N]
"""
import os, sys, re, time, signal, subprocess, select, threading, termios, tty

REPO = os.path.abspath(os.path.dirname(__file__) + "/../..")
ART  = "/tmp/boot-probe"
os.makedirs(ART, exist_ok=True)
HEADLESS = ":99"


def ensure_xvfb():
    try:
        r = subprocess.run(["distrobox", "enter", "my-distrobox", "--",
                            "xdpyinfo", "-display", HEADLESS],
                           capture_output=True, timeout=3)
        if r.returncode == 0: return
    except: pass
    subprocess.Popen(
        ["distrobox", "enter", "my-distrobox", "--", "bash", "-c",
         f"Xvfb {HEADLESS} -screen 0 1024x768x24 >/dev/null 2>&1 &"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)


class Pty:
    def __init__(self, path):
        # Open the slave twice: one fd for reading (drain), one for writing
        # (send). A single shared O_RDWR fd plus a draining background
        # thread on the same fd is the root cause of issue #129's
        # apparent "30% failure rate" — kbd_pty bytes silently got
        # dropped when the drain thread and send thread raced on the same
        # file table entry. Separate fds = separate kernel file structs.
        self.rfd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        self.wfd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
        # Raw mode on the slave: kills ECHO/ICANON/OPOST so the screen-text
        # stream goes through unchanged.
        tty.setraw(self.rfd, termios.TCSANOW)
        self.last_frame = ""
        self.alive = True
        self.lock = threading.Lock()
        threading.Thread(target=self._drain, daemon=True).start()
    def _drain(self):
        while self.alive:
            r, _, _ = select.select([self.rfd], [], [], 0.1)
            if self.rfd not in r: continue
            try: chunk = os.read(self.rfd, 16384)
            except (BlockingIOError, OSError): break
            if not chunk: break
            with self.lock:
                # Just keep the most recent frame's content for matching
                last = chunk.rfind(b"\f")
                if last >= 0:
                    self.last_frame = chunk[last+1:].decode("latin-1", errors="replace")
                else:
                    self.last_frame = (self.last_frame + chunk.decode("latin-1", errors="replace"))[-4000:]
    def send(self, b): os.write(self.wfd, b)
    def wait_for(self, pat, timeout):
        rx = re.compile(pat); end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.lock: f = self.last_frame
            if rx.search(f): return f
            time.sleep(0.1)
        return None
    def current(self):
        with self.lock: return self.last_frame
    def close(self):
        self.alive = False
        try: os.close(self.rfd)
        except: pass
        try: os.close(self.wfd)
        except: pass


def run_once(i):
    log = f"{ART}/run{i}.log"
    rep = f"{ART}/run{i}-report.txt"
    proc = subprocess.Popen(
        ["./1984", "--memory=576", "--ocr-monitor",
         f"--config={REPO}/debug/issue-129-ncfg-freeze/test-nonet.conf"],
        cwd=REPO, stderr=open(log, "w"), stdout=subprocess.DEVNULL,
        env={**os.environ, "DISPLAY": HEADLESS, "SDL_VIDEODRIVER": "x11",
             "ONE_K_TRACE_LBA": "1",
             "ONE_K_TRACE_IM1": "1",
             "ONE_K_FAKE_RTC": "1",
             "ONE_K_FAKE_RTC_TIME": "12:00:00",
             "ONE_K_TRACE_KBDPTY": "1"},
        preexec_fn=os.setsid)
    kbd = None
    end = time.monotonic() + 8
    while time.monotonic() < end and not kbd:
        time.sleep(0.1)
        try:
            with open(log) as f:
                for line in f:
                    m = re.search(r"kbd PTY: (\S+)", line)
                    if m: kbd = m.group(1); break
        except FileNotFoundError: continue
    if not kbd: proc.kill(); return "NO_PTY"
    pty = Pty(kbd)
    try:
        if not pty.wait_for(r"Ready", timeout=30):
            with open(rep, "w") as fp: fp.write("no BASIC ready\n")
            return "BOOTFAIL"
        time.sleep(5)
        pty.send(b"|hdcpm\r")
        if not pty.wait_for(r"CP/M Plus|ZCPR", timeout=60):
            with open(rep, "w") as fp:
                fp.write("no CP/M+ banner\n" + pty.current() + "\n")
            return "HDCPM_FAIL"
        if not pty.wait_for(r"CPM>", timeout=45):
            with open(rep, "w") as fp:
                fp.write("no A0:CPM> prompt\n" + pty.current() + "\n")
            return "PROMPT_FAIL"
        # Got prompt? Check it's stable (not about to reboot).
        time.sleep(2)
        final = pty.current()
        with open(rep, "w") as fp:
            fp.write("PASS\n--- final ---\n" + final + "\n")
        if "Amstrad 128K" in final or "Locomotive" in final:
            return "REBOOTED"
        return "PASS"
    finally:
        pty.close()
        # Hard SIGKILL the whole process group, then block until the kernel
        # actually reaps. SIGTERM was leaving 1984 alive long enough that the
        # next run's posix_openpt() raced with the dying instance's master fd,
        # silently dropping all subsequent keystroke writes.
        try: os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError: pass
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: pass
        # Give the kernel time to fully release the pty (master fd close,
        # slave-side hangup, /dev/pts/N free). Without this delay the next
        # iteration's posix_openpt() races with the dying instance and
        # ends up with a half-initialised pty pair.
        time.sleep(2)


def main():
    N = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    ensure_xvfb()
    tally = {}
    for i in range(1, N+1):
        out = run_once(i)
        tally[out] = tally.get(out, 0) + 1
        print(f"[{i:2d}/{N}] {out}")
    print("---")
    for k, v in sorted(tally.items()): print(f"  {k}: {v}")

if __name__ == "__main__":
    main()
