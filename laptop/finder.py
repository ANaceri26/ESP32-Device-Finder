"""Laptop side of the signal finder: receives UDP reports and serves a live page
with signal list, tracking beeps and a walk-around heatmap.

Usage: python finder.py [--simulate] [--no-firewall]
"""
import argparse
import atexit
import ctypes
import json
import math
import os
import random
import socket
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPORT_PORT = 4210   # ESP -> laptop
COMMAND_PORT = 4211  # laptop -> ESP
HERE = Path(__file__).parent
RULE_NAME = "SignalFinder"
IS_WINDOWS = sys.platform == "win32"

def is_admin():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False

def relaunch_as_admin():
    """Re-run this script with admin rights (shows the Windows UAC prompt). Returns True if started."""
    script = str(Path(__file__).resolve())
    params = " ".join(f'"{a}"' for a in [script, *sys.argv[1:], "--elevated"])
    rc = ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, params, os.getcwd(), 1)
    return rc > 32  # ShellExecute returns > 32 on success

def this_program_path():
    """Real path of the running python.exe (a venv's python.exe hands off to the base one)."""
    try:
        buf, size = ctypes.create_unicode_buffer(1024), ctypes.c_ulong(1024)
        k32 = ctypes.windll.kernel32
        if k32.QueryFullProcessImageNameW(k32.GetCurrentProcess(), 0, buf, ctypes.byref(size)):
            return buf.value
    except Exception:
        pass
    return sys.executable

class TempFirewallRule:
    """Adds the inbound rule on start, deletes it on exit (normal exit, Ctrl+C, or window closed)."""

    def __init__(self):
        self.active = False
        self.lock = threading.Lock()
        self._handler = None  # keep a reference so the console callback isn't garbage-collected

    @staticmethod
    def netsh(*args):
        return subprocess.run(["netsh", "advfirewall", "firewall", *args], capture_output=True, text=True)

    def open(self):
        self.netsh("delete", "rule", f"name={RULE_NAME}")  # clean up leftovers from a crashed run
        r = self.netsh("add", "rule", f"name={RULE_NAME}", "dir=in", "action=allow", "protocol=UDP",
                       f"localport={REPORT_PORT}", "remoteip=localsubnet", f"program={this_program_path()}")
        if r.returncode != 0:
            print("WARNING: could not add the firewall rule:", (r.stdout + r.stderr).strip())
            return False
        self.active = True
        atexit.register(self.close)
        # console close bypasses atexit
        handler_type = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_uint)

        def on_console_event(event):
            if event in (2, 5, 6):  # window closed, log off, shutdown
                self.close()
            return 0  # let Windows / Python continue as normal (Ctrl+C still raises KeyboardInterrupt)

        self._handler = handler_type(on_console_event)
        ctypes.windll.kernel32.SetConsoleCtrlHandler(self._handler, True)
        print(f"Firewall: UDP port {REPORT_PORT} OPEN (local network only) until you stop this script.")
        return True

    def close(self):
        with self.lock:
            if not self.active:
                return
            self.active = False
            self.netsh("delete", "rule", f"name={RULE_NAME}")
            print(f"Firewall: rule removed, port {REPORT_PORT} closed again.")

class EspLink:
    """Receives UDP reports from the ESP and sends it commands."""

    def __init__(self, hello_targets):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind(("0.0.0.0", REPORT_PORT))
        self.sock.settimeout(0.5)
        self.hello_targets = hello_targets
        self.lock = threading.Lock()
        self.report = {}
        self.esp_addr = None
        self.last_heard = 0.0
        self.packets = 0
        threading.Thread(target=self._receive, daemon=True).start()
        threading.Thread(target=self._hello, daemon=True).start()

    def _receive(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(16384)
                msg = json.loads(data)
            except socket.timeout:
                continue
            except (OSError, ValueError):
                continue
            with self.lock:
                self.report = msg
                self.esp_addr = (addr[0], COMMAND_PORT)
                self.last_heard = time.time()
                self.packets += 1

    def _hello(self):
        """Until the ESP is heard, broadcast 'hello' so it learns this laptop's address."""
        while True:
            if time.time() - self.last_heard > 3:
                for host in self.hello_targets:
                    try:
                        self.sock.sendto(b"hello", (host, COMMAND_PORT))
                    except OSError:
                        pass  # e.g. no network on that address
            time.sleep(2)

    def send(self, cmd):
        with self.lock:
            addr = self.esp_addr
        if not addr:
            return False
        self.sock.sendto(cmd.encode()[:60], addr)
        return True

    def snapshot(self):
        with self.lock:
            age = time.time() - self.last_heard if self.last_heard else None
            return {"connected": age is not None and age < 2.5, "esp": self.esp_addr[0] if self.esp_addr else None,
                    "age": age, "packets": self.packets, **self.report}

class FakeEsp(threading.Thread):
    """Pretends to be the ESP for --simulate: fake phones/earbuds/hotspots in an 8 x 6 m room,
    a 'walker' holding the ESP, realistic noisy RSSI, and the same UDP protocol."""

    W, H = 8.0, 6.0

    def __init__(self):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", COMMAND_PORT))
        self.sock.settimeout(0.05)
        self.mode, self.target = "ble", ""
        self.pos = [4.0, 3.0]
        self.goal = [4.0, 3.0]
        rnd = random.Random(7)
        ble = [("Sam's iPhone", 76), ("", 76), ("Galaxy Buds2", 117), ("", 6), ("Pixel 8", 224), ("Forerunner 255", 135)]
        wifi = [("uwindsor-secure", 1), ("uwindsor-secure", 6), ("Ahmed's iPhone", 11), ("DIRECT-HP-Printer", 6)]
        self.devices = {"ble": [], "wifi": []}
        for i, (name, company) in enumerate(ble):
            self.devices["ble"].append(dict(id=f"ble:5c:1a:{i:02x}:9e:3b:{rnd.randrange(256):02x}", name=name,
                                            company=company, ch=0, x=rnd.uniform(0.5, 7.5), y=rnd.uniform(0.5, 5.5), p1m=-59))
        for i, (name, ch) in enumerate(wifi):
            self.devices["wifi"].append(dict(id=f"wifi:a4:2b:{i:02x}:10:77:{rnd.randrange(256):02x}", name=name,
                                             company=-1, ch=ch, x=rnd.uniform(0.5, 7.5), y=rnd.uniform(0.5, 5.5), p1m=-45))
        self.smooth = {}

    def rssi(self, d):
        dist = max(math.hypot(d["x"] - self.pos[0], d["y"] - self.pos[1]), 0.3)
        return d["p1m"] - 25 * math.log10(dist) + random.gauss(0, 4)  # path-loss model + noise

    def run(self):
        last_report, laptop = 0.0, ("127.0.0.1", REPORT_PORT)
        while True:
            try:
                data, _addr = self.sock.recvfrom(256)
                self.handle(data.decode(errors="ignore"))
            except socket.timeout:
                pass
            # walk toward the goal at 1 m/s
            dx, dy = self.goal[0] - self.pos[0], self.goal[1] - self.pos[1]
            step = min(0.05, math.hypot(dx, dy))
            if step > 1e-6:
                n = math.hypot(dx, dy)
                self.pos[0] += dx / n * step
                self.pos[1] += dy / n * step
            if time.time() - last_report >= 0.3:
                last_report = time.time()
                self.sock.sendto(json.dumps(self.report()).encode(), laptop)

    def handle(self, cmd):
        parts = cmd.split()
        if not parts:
            return
        if parts[0] == "mode" and len(parts) == 2 and parts[1] != self.mode:
            self.mode, self.target, self.smooth = parts[1], "", {}
        elif parts[0] == "track":
            self.target = parts[1] if len(parts) > 1 else ""
        elif parts[0] == "walk" and len(parts) == 5:  # simulation only: walk to grid cell
            c, r, cols, rows = map(int, parts[1:])
            self.goal = [(c + 0.5) * self.W / cols, (rows - r - 0.5) * self.H / rows]

    def report(self):
        dev = []
        for d in self.devices[self.mode]:
            raw = self.rssi(d)
            if raw < -98:
                continue
            old = self.smooth.get(d["id"])
            val = raw if old is None else old + 0.3 * (raw - old)
            self.smooth[d["id"]] = val
            dev.append([d["id"], d["name"], round(val, 1), d["company"], d["ch"], 100])
        dev.sort(key=lambda r: -r[2])
        return {"mode": self.mode, "target": self.target, "dev": dev, "sim": True}

def make_handler(link):
    page = (HERE / "finder_page.html").read_bytes()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass  # keep the terminal quiet

        def reply(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                self.reply(200, "text/html; charset=utf-8", page)
            elif self.path.startswith("/api/state"):
                self.reply(200, "application/json", json.dumps(link.snapshot()).encode())
            else:
                self.reply(404, "text/plain", b"not found")

        def do_POST(self):
            if self.path != "/api/cmd":
                return self.reply(404, "text/plain", b"not found")
            n = int(self.headers.get("Content-Length", 0))
            cmd = self.rfile.read(min(n, 100)).decode(errors="ignore").strip()
            ok = link.send(cmd)
            self.reply(200, "application/json", json.dumps({"ok": ok}).encode())

    return Handler

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--simulate", action="store_true", help="fake ESP and devices, no hardware")
    ap.add_argument("--port", type=int, default=8000, help="web page port")
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--no-firewall", action="store_true", help="don't add/remove the firewall rule")
    ap.add_argument("--elevated", action="store_true", help=argparse.SUPPRESS)  # set by relaunch_as_admin
    args = ap.parse_args()

    rule = None
    if IS_WINDOWS and not args.simulate and not args.no_firewall:
        if not is_admin():
            print("Asking for admin rights to open the firewall port while the finder runs...")
            if relaunch_as_admin():
                print("Continuing in the new (administrator) window.")
                return
            print("Admin rights refused - continuing without the rule (ESP data may be blocked).")
        else:
            rule = TempFirewallRule()
            rule.open()
    try:
        run(args)
    finally:
        if rule:
            rule.close()
        if args.elevated:
            input("Press Enter to close this window...")

def run(args):
    targets = ["255.255.255.255", "192.168.137.255"]  # broadcast + Windows hotspot subnet
    if args.simulate:
        FakeEsp().start()
        targets = ["127.0.0.1"]
    link = EspLink(targets)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(link))
    url = f"http://localhost:{args.port}"
    print(f"Signal Finder running at {url}  (Ctrl+C to stop)")
    print("Waiting for the ESP..." if not args.simulate else "SIMULATION: fake ESP running")
    if not args.no_browser:
        threading.Timer(0.8, webbrowser.open, args=(url,)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
