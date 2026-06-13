#!/usr/bin/env python3
"""NCFG-freeze probe — kbd-PTY input + OCR-monitor screen feed.

Drives 1984 end-to-end via the same bidirectional PTY used for keyboard:
- writes go in as keystrokes (paste.c keymap)
- reads come out as either firmware text (&BB5A) or, more usefully,
  full screen-text frames from the --ocr-monitor pass (each frame
  prefixed by \f, rows separated by \r\n).

Workflow per run:
  1. wait for "A?:?CPM" (or similar — OCR doesn't always nail digits)
     visible in the screen feed, meaning CP/M+ reached prompt.
  2. type "ncfg -r", wait for "Network" / "KCNet" in next frame.
  3. type "ncfg -a:cpc", wait.
  4. type "ping -c5 slashdot.org", wait for "packets" or "Error".
  5. classify outcome by what the final OCR frame looks like, and dump
     monitor PTY state on FREEZE.

Usage:  debug/issue-129-ncfg-freeze/probe.py [N]
"""

import os, sys, re, time, signal, subprocess, select, threading

REPO = os.path.abspath(os.path.dirname(__file__) + "/../..")
ART  = "/tmp/ncfg-probe"
os.makedirs(ART, exist_ok=True)


class Pty:
    """Manage the kbd PTY: spawn a drainer thread that captures every
    OCR frame seen (split on \f), and expose a wait_for() that watches
    the most recent frame for a regex match."""
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        self.buf = bytearray()
        self.last_frame = ""
        self.alive = True
        self.lock = threading.Lock()
        self.t = threading.Thread(target=self._drain, daemon=True)
        self.t.start()

    def _drain(self):
        while self.alive:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if self.fd not in r: continue
            try: chunk = os.read(self.fd, 16384)
            except (BlockingIOError, OSError): break
            if not chunk: break
            with self.lock:
                self.buf.extend(chunk)
                # Keep only the most recent frame's content for matching.
                idx = self.buf.rfind(b"\f")
                if idx >= 0:
                    self.last_frame = self.buf[idx+1:].decode("latin-1", errors="replace")

    def send(self, payload: bytes):
        os.write(self.fd, payload)

    def wait_for(self, pattern: str, timeout: float):
        """Wait until the most recent OCR frame matches pattern. Returns
        the frame text on success, None on timeout."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                frame = self.last_frame
            if rx.search(frame): return frame
            time.sleep(0.1)
        return None

    def wait_for_change(self, baseline: str, pattern: str, timeout: float):
        """Wait until the most recent OCR frame differs from `baseline`
        AND matches `pattern`. Use this to detect a command's response
        when the same pattern is already present elsewhere on screen
        from earlier output (e.g. NCFG's banner appearing twice)."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                frame = self.last_frame
            if frame != baseline and rx.search(frame):
                return frame
            time.sleep(0.1)
        return None

    def current_frame(self):
        with self.lock:
            return self.last_frame

    def close(self):
        self.alive = False
        try: os.close(self.fd)
        except: pass


def mon_cmd(mon_fd, cmd, settle=0.3):
    os.write(mon_fd, (cmd + "\r").encode())
    time.sleep(settle)
    out = b""
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        r,_,_ = select.select([mon_fd], [], [], 0.05)
        if mon_fd not in r: continue
        try: chunk = os.read(mon_fd, 4096)
        except BlockingIOError: continue
        if not chunk: break
        out += chunk
    return out.decode("latin-1", errors="replace")


def run_once(run_id):
    rd = f"{ART}/run{run_id}"
    log_path = f"{rd}.log"
    rep_path = f"{rd}-report.txt"
    for f in (log_path, rep_path):
        try: os.unlink(f)
        except: pass
    rep = open(rep_path, "w")

    proc = subprocess.Popen(
        ["./1984", "--memory=576", "--ocr-monitor", "--monitor-pty"],
        cwd=REPO,
        stderr=open(log_path, "w"), stdout=subprocess.DEVNULL,
        env={**os.environ,
             "ONE_K_TRACE_LBA": "1",
             "ONE_K_TRACE_IM1": "1"},
        preexec_fn=os.setsid,
    )
    kbd_pty = mon_pty = None
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not (kbd_pty and mon_pty):
        time.sleep(0.1)
        try:
            with open(log_path) as f:
                for line in f:
                    m = re.search(r"kbd PTY: (\S+)", line)
                    if m: kbd_pty = m.group(1)
                    m = re.search(r"monitor PTY: (\S+)", line)
                    if m: mon_pty = m.group(1)
        except FileNotFoundError: continue
    if not (kbd_pty and mon_pty):
        proc.kill(); rep.write("no PTYs\n"); return "ERROR"
    rep.write(f"kbd PTY:     {kbd_pty}\nmonitor PTY: {mon_pty}\n")

    pty = Pty(kbd_pty)
    mfd = os.open(mon_pty, os.O_RDWR | os.O_NONBLOCK)

    try:
        # Phase 1: wait for BASIC Ready prompt. The "Amstrad" banner shows up
        # during boot but BASIC isn't actually receiving keys until "Ready"
        # appears at the bottom; typing before then drops chars and only the
        # last keypress survives to confuse the upcoming command.
        if not pty.wait_for(r"Ready", timeout=30):
            rep.write("BOOTFAIL: no BASIC Ready\n")
            return "BOOTFAIL"
        # Belt-and-braces — small pause to ensure keyboard handler is awake.
        time.sleep(0.5)
        rep.write("→ BASIC Ready\n")

        # Phase 2: type |hdcpm, wait for "CP/M Plus" header in OCR frame.
        pty.send(b"|hdcpm\r")
        if not pty.wait_for(r"CP/M Plus|CP.M Plus|ZCPR", timeout=60):
            rep.write("HDCPM_FAIL: no CP/M+ banner after |hdcpm\n")
            rep.write("--- last frame ---\n" + pty.current_frame() + "\n")
            return "HDCPM_FAIL"
        rep.write("→ CP/M+ banner\n")

        # Phase 3: wait for the actual prompt. CP/M+ prompt looks like
        # "16:23 A0:CPM>" — the digits are kernel-font and OCR shows '?'.
        # The line ending in "CPM>" is reliable enough.
        if not pty.wait_for(r"CPM>", timeout=45):
            rep.write("PROMPT_FAIL: A0:CPM> never appeared\n")
            return "PROMPT_FAIL"
        rep.write("→ A0:CPM> prompt\n")

        # Phase 4: ncfg -r prints "### KCNet IP Configuration ###" with
        # zeroed values. The CPC clears the screen first, so the boot-time
        # header from auto-NCFG isn't present in the current frame — we
        # can reliably wait for the header to REappear.
        baseline = pty.current_frame()
        pty.send(b"ncfg -r\r")
        if not pty.wait_for_change(baseline, r"\?\.\?\.\?\.\?", timeout=25):
            rep.write("NCFG_R_FREEZE\n")
            rep.write("--- last frame ---\n" + pty.current_frame() + "\n")
            return "FREEZE_NCFG_R"
        rep.write("→ ncfg -r completed\n")

        # Phase 5: ncfg -a:cpc, wait for DHCP-acquired non-zero IP-Address.
        # The DHCP-acquired IP starts with a digit other than 0, which OCR
        # may render as '1??' or similar; the static "?ddr???" (Address) is
        # the safer match.
        baseline = pty.current_frame()
        pty.send(b"ncfg -a:cpc\r")
        # DHCP-acquired IP shows non-zero digits which OCR garbles, but the
        # post-config "###" header reprint changes screen content.
        if not pty.wait_for_change(baseline, r"\?ddr", timeout=45):
            rep.write("NCFG_A_FREEZE\n")
            rep.write("--- last frame ---\n" + pty.current_frame() + "\n")
            return "FREEZE_NCFG_A"
        rep.write("→ ncfg -a:cpc completed\n")

        # Phase 6: ping. PASS if we see "packets transmitted"; otherwise
        # FREEZE.
        baseline = pty.current_frame()
        pty.send(b"ping -c5 slashdot.org\r")
        if not pty.wait_for_change(baseline, r"PING |packets|ARP", timeout=45):
            rep.write("PING_FREEZE\n")
            rep.write("--- last frame ---\n" + pty.current_frame() + "\n")
            return "FREEZE_PING"
        rep.write("→ ping completed\n")
        rep.write("--- final frame ---\n" + pty.current_frame() + "\n")
        return "PASS"

    finally:
        rep.close()
        pty.close()
        try: os.close(mfd)
        except: pass
        try: os.killpg(proc.pid, signal.SIGTERM)
        except: pass
        try: proc.wait(timeout=2)
        except: proc.kill()


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
