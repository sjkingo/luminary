#!/usr/bin/env python3
"""
test_kbd.py — Automated test for the keyboard-dies-after-x bug.

Sequence:
  1. Boot QEMU headless
  2. Wait for fbcon to start (shell is ready)
  3. Wait a bit for shell prompt
  4. Type 'x' + Enter to launch the GUI demo
  5. Wait for the GUI compositor to start
  6. Close all windows by sending Ctrl+F4 (close) or wait then send 'q'
     Actually: we use the monitor 'sendkey' to press Escape or close the app.
     Simpler: wait for compositor, then send keystrokes to close windows.
  7. Wait for compositor to die (self-kill log line)
  8. Type a test string, check if it reaches the shell (via serial DBGKs)
  9. Report pass/fail
"""

import os
import re
import socket
import subprocess
import sys
import threading
import time

BASE_DIR     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_LOG   = "/tmp/luminary.log"
MONITOR_HOST = "localhost"
MONITOR_PORT = 4444
ISO  = os.path.join(BASE_DIR, "_build", "boot.iso")
DISK = os.path.join(BASE_DIR, "_build", "disk.img")


class Serial:
    def __init__(self):
        self._buf = ""
        self._lock = threading.Lock()
        self._stop = threading.Event()

    def start(self):
        open(SERIAL_LOG, "w").close()
        t = threading.Thread(target=self._run, daemon=True)
        t.start()

    def _run(self):
        while not self._stop.is_set():
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    while not self._stop.is_set():
                        line = f.readline()
                        if line:
                            with self._lock:
                                self._buf += line
                            sys.stdout.write(f"  [serial] {line}")
                            sys.stdout.flush()
                        else:
                            time.sleep(0.02)
            except FileNotFoundError:
                time.sleep(0.1)

    def wait(self, pattern, timeout=30.0, label=""):
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if rx.search(self._buf):
                    return True
            time.sleep(0.05)
        print(f"  TIMEOUT waiting for: {label or pattern}")
        return False

    def seen(self, pattern):
        with self._lock:
            return bool(re.search(pattern, self._buf))

    def stop(self):
        self._stop.set()


class Monitor:
    def __init__(self):
        self.sock = None

    def connect(self, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.connect((MONITOR_HOST, MONITOR_PORT))
                self.sock = s
                self.sock.settimeout(3.0)
                self._drain()
                return True
            except (ConnectionRefusedError, OSError):
                time.sleep(0.1)
        return False

    def _drain(self):
        buf = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
                if buf.endswith(b"(qemu) "):
                    break
        except socket.timeout:
            pass
        return buf.decode("utf-8", errors="replace")

    def cmd(self, command, quiet=False):
        if not self.sock:
            return ""
        if not quiet:
            print(f"  [mon] {command}")
        try:
            self.sock.sendall((command + "\n").encode())
            r = self._drain().replace("(qemu) ", "").strip()
            if r and not quiet:
                print(f"        {r}")
            return r
        except OSError:
            return ""

    def sendkey(self, key):
        self.cmd(f"sendkey {key}", quiet=True)
        time.sleep(0.05)

    def typestr(self, s):
        """Type a string via sendkey."""
        keymap = {
            'a':'a','b':'b','c':'c','d':'d','e':'e','f':'f','g':'g','h':'h',
            'i':'i','j':'j','k':'k','l':'l','m':'m','n':'n','o':'o','p':'p',
            'q':'q','r':'r','s':'s','t':'t','u':'u','v':'v','w':'w','x':'x',
            'y':'y','z':'z',
            '0':'0','1':'1','2':'2','3':'3','4':'4',
            '5':'5','6':'6','7':'7','8':'8','9':'9',
            ' ':'spc', '\n':'ret', '\r':'ret',
        }
        for ch in s:
            key = keymap.get(ch)
            if key:
                self.sendkey(key)
                time.sleep(0.03)

    def close(self):
        if self.sock:
            try: self.sock.close()
            except: pass


def run_test():
    print("=" * 60)
    print("TEST: keyboard survives x GUI launch/close")
    print("=" * 60)

    serial = Serial()
    serial.start()

    cmd = [
        "qemu-system-i386",
        "-m", "512",
        "-net", "nic,model=rtl8139", "-net", "user",
        "-serial", f"file:{SERIAL_LOG}",
        "-monitor", f"tcp:{MONITOR_HOST}:{MONITOR_PORT},server=on,wait=off",
        "-cdrom", ISO,
        "-drive", f"file={DISK},format=raw,index=0,media=disk",
        "-boot", "d",
        "-display", "none",
    ]

    print(f"\nBooting QEMU...")
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    mon = Monitor()
    print("Connecting to monitor...", end=" ", flush=True)
    if not mon.connect():
        print("FAIL")
        proc.kill()
        return False
    print("OK")

    # Step 1: wait for fbcon to be forked by init
    print("\n[1] Waiting for system boot...")
    if not serial.wait(r"starting /bin/fbcon", timeout=20):
        print("FAIL: fbcon never started")
        proc.kill()
        return False
    # Give fbcon+sh time to start and print prompt
    time.sleep(3.0)
    print("    Boot complete, shell ready.")

    def launch_and_close_x(run_num):
        """Launch x, wait for compositor, close all windows, return True if compositor died."""
        print(f"\n[run {run_num}] Launching GUI ('x')...")
        mon.typestr("x\n")
        # Wait for a NEW compositor-started message (after our previous self-kill marker)
        deadline = time.time() + 15
        while time.time() < deadline:
            if serial.wait(r"compositor started", timeout=1):
                break
        else:
            print(f"  FAIL: compositor never started on run {run_num}")
            return False
        print(f"  Compositor up.")
        time.sleep(1.0)

        print(f"  Closing windows...")
        marker_before = serial.seen(r"self-kill pid \d+, jumping")
        for _ in range(4):
            mon.sendkey("alt-f4")
            time.sleep(0.3)

        if not serial.wait(r"self-kill pid \d+, jumping", timeout=8):
            print(f"  FAIL: compositor never died on run {run_num}")
            return False
        print(f"  Compositor dead.")
        time.sleep(0.8)
        return True

    def check_keyboard(run_num):
        """Type a string, check stdin_read_op delivers it without broken pipe."""
        print(f"\n[check {run_num}] Testing keyboard input...")
        broken_before = serial.seen(r"broken pipe")
        stdin_count_before = len(list(__import__('re').finditer(r"stdin_read_op", serial._buf)))

        mon.typestr("echo test\n")
        time.sleep(1.0)

        broken_after = serial.seen(r"broken pipe")
        stdin_count_after = len(list(__import__('re').finditer(r"stdin_read_op", serial._buf)))
        new_stdin = stdin_count_after - stdin_count_before

        if broken_after and not broken_before:
            print(f"  FAIL: broken pipe after run {run_num}")
            return False
        if new_stdin > 0:
            print(f"  OK: {new_stdin} keyboard bytes delivered, no broken pipe")
            return True
        print(f"  FAIL: no keyboard bytes delivered — dumping state:")
        # Freeze and inspect
        mon.cmd("stop")
        mon.cmd("info registers")
        # Disassemble at EIP to see what's running
        regs = mon.cmd("info registers", quiet=True)
        import re
        m = re.search(r'EIP=([0-9a-f]+)', regs)
        if m:
            eip = m.group(1)
            print(f"  EIP=0x{eip}")
            mon.cmd(f"x/5i 0x{eip}")
        mon.cmd("cont")
        return False

    # Run x twice, checking keyboard after each close
    for i in range(1, 3):
        if not launch_and_close_x(i):
            proc.kill()
            return False
        if not check_keyboard(i):
            mon.cmd("stop")
            mon.cmd("info registers")
            mon.cmd("cont")
            result = False
            break
    else:
        result = True

    time.sleep(1.0)
    mon.cmd("quit")
    serial.stop()
    mon.close()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()

    return result


if __name__ == "__main__":
    if not os.path.exists(ISO):
        sys.exit(f"ISO not found: {ISO}")
    ok = run_test()
    sys.exit(0 if ok else 1)
