#!/usr/bin/env python3
"""
qmon.py — Luminary QEMU monitor harness.

Boots QEMU with:
  - Serial output → /tmp/luminary.log  (+ live-streamed to stdout)
  - QEMU monitor  → TCP localhost:4444

Usage:
  python3 tools/qmon.py [--iso PATH] [--disk PATH] [--wait PATTERN] [--cmd CMD] ...

Examples:
  # Boot and stream serial live:
  python3 tools/qmon.py

  # Boot, wait for shell prompt, then query registers:
  python3 tools/qmon.py --wait "\\$" --cmd "info registers"

  # Boot, wait for a pattern, then run multiple monitor commands:
  python3 tools/qmon.py --wait "broken pipe" --cmd "stop" --cmd "info registers" --cmd "cont"

  # Interactive mode — drop into a monitor REPL after boot:
  python3 tools/qmon.py --interactive

Monitor commands (once connected):
  Any QEMU HMP command: info registers, info mem, stop, cont, quit, x/10i $eip, etc.
  Ctrl-C to detach from monitor (QEMU keeps running).
"""

import argparse
import os
import re
import select
import socket
import subprocess
import sys
import threading
import time

SERIAL_LOG   = "/tmp/luminary.log"
MONITOR_HOST = "localhost"
MONITOR_PORT = 4444
MONITOR_ADDR = f"tcp:{MONITOR_HOST}:{MONITOR_PORT},server=on,wait=off"

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_artifacts(iso_arg, disk_arg):
    iso  = iso_arg  or os.path.join(BASE_DIR, "_build", "boot.iso")
    disk = disk_arg or os.path.join(BASE_DIR, "_build", "disk.img")
    if not os.path.exists(iso):
        sys.exit(f"ISO not found: {iso}  (run make first)")
    if not os.path.exists(disk):
        sys.exit(f"Disk image not found: {disk}  (run make first)")
    return iso, disk


def build_qemu_cmd(iso, disk):
    return [
        "qemu-system-i386",
        "-m", "512",
        "-net", "nic,model=rtl8139", "-net", "user",
        "-serial", f"file:{SERIAL_LOG}",
        "-monitor", MONITOR_ADDR,
        "-cdrom", iso,
        "-drive", f"file={disk},format=raw,index=0,media=disk",
        "-boot", "d",
        "-display", "cocoa",   # macOS native window
    ]


class MonitorClient:
    def __init__(self):
        self.sock = None

    def connect(self, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.connect((MONITOR_HOST, MONITOR_PORT))
                self.sock = s
                self.sock.settimeout(2.0)
                # drain the QEMU welcome banner
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

    def cmd(self, command):
        if not self.sock:
            return "(not connected)"
        try:
            self.sock.sendall((command + "\n").encode())
            return self._drain()
        except OSError as e:
            return f"(error: {e})"

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


class SerialTailer:
    """Streams /tmp/luminary.log to stdout and accumulates a ring buffer."""

    RING = 8192

    def __init__(self):
        self._buf = ""
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        # Truncate log so we start fresh
        open(SERIAL_LOG, "w").close()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        # Wait for the file to appear / be written
        while not self._stop.is_set():
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    while not self._stop.is_set():
                        line = f.readline()
                        if line:
                            sys.stdout.write("\033[90m" + line + "\033[0m")
                            sys.stdout.flush()
                            with self._lock:
                                self._buf += line
                                if len(self._buf) > self.RING:
                                    self._buf = self._buf[-self.RING:]
                        else:
                            time.sleep(0.05)
            except FileNotFoundError:
                time.sleep(0.1)

    def wait_for(self, pattern, timeout=30.0):
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if rx.search(self._buf):
                    return True
            time.sleep(0.05)
        return False

    def stop(self):
        self._stop.set()

    def recent(self, n=2000):
        with self._lock:
            return self._buf[-n:]


def interactive_repl(mon):
    print("\n\033[1;33m=== QEMU monitor REPL (Ctrl-C or 'quit' to exit) ===\033[0m")
    try:
        while True:
            try:
                line = input("\033[33m(qemu) \033[0m").strip()
            except EOFError:
                break
            if not line:
                continue
            if line in ("exit", "quit", "q"):
                break
            result = mon.cmd(line)
            # strip trailing "(qemu) " prompt echo
            result = result.replace("(qemu) ", "").strip()
            if result:
                print(result)
    except KeyboardInterrupt:
        pass
    print()


def main():
    ap = argparse.ArgumentParser(description="Luminary QEMU monitor harness")
    ap.add_argument("--iso",  help="Path to luminary.iso")
    ap.add_argument("--disk", help="Path to disk.img")
    ap.add_argument("--wait", metavar="PATTERN",
                    help="Wait for regex pattern in serial output before running --cmd")
    ap.add_argument("--wait-timeout", type=float, default=30.0, metavar="SECS",
                    help="Timeout for --wait (default: 30s)")
    ap.add_argument("--cmd", action="append", default=[], metavar="CMD",
                    help="Monitor command to run (can be repeated)")
    ap.add_argument("--interactive", action="store_true",
                    help="Drop into interactive monitor REPL")
    ap.add_argument("--no-display", action="store_true",
                    help="Run headless (-display none)")
    ap.add_argument("--serial-only", action="store_true",
                    help="Stream serial output and exit when QEMU does (no monitor cmds)")
    args = ap.parse_args()

    iso, disk = find_artifacts(args.iso, args.disk)
    cmd = build_qemu_cmd(iso, disk)

    if args.no_display:
        # replace cocoa with none
        cmd = [c if c != "cocoa" else "none" for c in cmd]

    print(f"\033[1mBooting:\033[0m {' '.join(cmd)}\n")

    tailer = SerialTailer()
    tailer.start()

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    mon = MonitorClient()

    if not (args.serial_only):
        print(f"Connecting to monitor on port {MONITOR_PORT}...", end=" ", flush=True)
        if mon.connect(timeout=10.0):
            print("\033[32mOK\033[0m")
        else:
            print("\033[31mfailed\033[0m (monitor unavailable)")

    if args.wait:
        print(f"Waiting for pattern: \033[36m{args.wait}\033[0m  (timeout {args.wait_timeout}s)")
        found = tailer.wait_for(args.wait, timeout=args.wait_timeout)
        if found:
            print(f"\033[32mPattern matched.\033[0m")
        else:
            print(f"\033[31mTimeout — pattern not seen.\033[0m")
            print("Recent serial output:")
            print(tailer.recent(1000))

    for c in args.cmd:
        print(f"\033[33m>>> {c}\033[0m")
        result = mon.cmd(c)
        result = result.replace("(qemu) ", "").strip()
        if result:
            print(result)
        time.sleep(0.1)

    if args.interactive:
        interactive_repl(mon)

    if args.serial_only or (not args.cmd and not args.interactive and not args.wait):
        # Just stream serial until QEMU exits
        try:
            proc.wait()
        except KeyboardInterrupt:
            pass
    else:
        if not args.interactive:
            # Keep streaming serial; wait for user Ctrl-C or QEMU exit
            print("\n\033[90mStreaming serial (Ctrl-C to detach)...\033[0m")
            try:
                proc.wait()
            except KeyboardInterrupt:
                pass

    tailer.stop()
    mon.close()

    if proc.poll() is None:
        print("\n\033[90mQEMU still running.\033[0m")
    else:
        print(f"\n\033[90mQEMU exited (code {proc.returncode}).\033[0m")


if __name__ == "__main__":
    main()
