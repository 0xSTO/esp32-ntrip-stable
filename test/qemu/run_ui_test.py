#!/usr/bin/env python3
"""
Browser test of the Receiver page against the firmware running in QEMU.

A fake UM980 sits on the emulated UART: it streams the recorded RTCM and
answers every command line the ESP32 sends with "$command,<cmd>,response: OK"
(or a scripted failure). Playwright drives receiver.html and checks that
"Apply base setup" only saves when every command was confirmed.

Needs: IDF env (esptool, nvs gen, qemu), `pip install playwright` (Chromium).
Usage: run_ui_test.py <build_dir>
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_qemu_test as T  # noqa: E402

from playwright.sync_api import sync_playwright  # noqa: E402

CMD_RE = re.compile(r"^[A-Z][A-Z0-9 .\-]*$")


class FakeUM980:
    def __init__(self, uart):
        self.uart = uart
        self.received = []          # commands seen, in order
        self.fail = {}              # command prefix -> reply text
        self.silent = set()         # command prefixes to ignore
        self.ignore_version = 0     # ignore this many VERSION commands (simulated restart)
        self.lock = threading.Lock()
        self.buf = b""
        self.all_lines = []
        threading.Thread(target=self._rx, daemon=True).start()

    def _reply(self, text):
        self.uart.sendall((text + "\r\n").encode())

    def _rx(self):
        while True:
            d = self.uart.recv(4096)
            if not d:
                return
            self.buf += d
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                cmd = line.decode(errors="replace").strip()
                self.all_lines.append(cmd)
                if not CMD_RE.match(cmd):
                    continue  # ESP log lines / $PESP
                with self.lock:
                    self.received.append(cmd)
                if any(cmd.startswith(p) for p in self.silent):
                    continue
                if cmd == "VERSION" and self.ignore_version > 0:
                    self.ignore_version -= 1
                    continue
                for p, text in self.fail.items():
                    if cmd.startswith(p):
                        self._reply(f"$command,{cmd},response: {text}*00")
                        break
                else:
                    self._reply(f"$command,{cmd},response: OK*00")
                    if cmd == "VERSION":
                        self._reply("#VERSION,97,GPS,FINE,2190,365000,0,0,18,511;UM980,R4.10Build11833*00000000")
                    if cmd.startswith("CONFIG SIGNALGROUP"):
                        self.ignore_version = 2  # 'rebooting'

    def take(self):
        with self.lock:
            r, self.received = self.received, []
        return r


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build_qemu"
    work = tempfile.mkdtemp(prefix="ntrip_ui_")
    flash = T.build_flash(build, work)
    T.FakeCaster("A", T.PORT_A)
    T.FakeCaster("B", T.PORT_B)
    qemu = subprocess.Popen(["qemu-system-xtensa", "-display", "none", "-monitor", "none", "-machine", "esp32", "-m", "4M",
                             "-drive", f"file={flash},if=mtd,format=raw",
                             "-serial", f"tcp:127.0.0.1:{T.UART_PORT},server=on,wait=on",
                             "-serial", f"file:{work}/console.log",
                             "-nic", f"user,model=open_eth,hostfwd=tcp:127.0.0.1:{T.HTTP_PORT}-:80"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    uart = None
    for _ in range(50):
        try:
            uart = socket.create_connection(("127.0.0.1", T.UART_PORT))
            break
        except OSError:
            time.sleep(0.2)
    rx = FakeUM980(uart)
    end = time.time() + 90
    while not any("INIT,COMPLETE" in l for l in rx.all_lines) and time.time() < end:
        time.sleep(0.5)

    rec = open(T.RECORDED, "rb").read()
    stop = threading.Event()

    def feeder():
        i = 0
        while not stop.is_set():
            uart.sendall(rec[i:i + 256])
            i = (i + 256) % len(rec)
            time.sleep(256 / 3000)
    threading.Thread(target=feeder, daemon=True).start()
    time.sleep(15)

    failures = []

    def check(cond, msg):
        print(("PASS " if cond else "FAIL ") + msg, flush=True)
        if not cond:
            failures.append(msg)

    try:
        with sync_playwright() as p:
            br = p.chromium.launch()
            pg = br.new_page(viewport={"width": 1280, "height": 900})
            errors = []
            pg.on("pageerror", lambda e: errors.append(str(e)))
            pg.goto(f"http://127.0.0.1:{T.HTTP_PORT}/receiver.html", timeout=300000)
            time.sleep(60)                       # first load writes ETag files (slow in QEMU)
            pg.reload(timeout=300000)
            pg.wait_for_function("document.querySelector('#t-sats').textContent.trim() !== '–'", timeout=60000)

            def console_text():
                return pg.inner_text("#console")

            def apply_setup(lat, lon, h, signals=False, sky=True):
                pg.click("text=Apply base setup")
                time.sleep(1.5)
                pg.check("#m-fixed", force=True)
                pg.fill("#f-lat", lat)
                pg.fill("#f-lon", lon)
                pg.fill("#f-h", h)
                pg.set_checked("#o-signals", signals, force=True)
                pg.set_checked("#o-sky", sky, force=True)
                pg.click("#setup-apply")

            def wait_done(timeout=120):
                end = time.time() + timeout
                while time.time() < end:
                    t = console_text()
                    if "— done" in t or "— stopped" in t:
                        return t
                    time.sleep(0.5)
                return console_text()

            # 1. Happy path with signal group restart and GNGSV -> GPGSV fallback
            rx.take()
            rx.fail = {"GNGSV": "PARSING FAILD NO MATCHING FUNC GNGSV"}
            apply_setup("-38.30942616", "146.39974564", "96.447", signals=True)
            t = wait_done(180)
            cmds = rx.take()
            print("  sent:", cmds)
            check("— done" in t, "setup completes when every command is confirmed")
            check(cmds and cmds[0] == "CONFIG SIGNALGROUP 2" and cmds.count("VERSION") >= 3,
                  "waits for the receiver to answer VERSION again after SIGNALGROUP restart")
            check("MODE BASE -38.30942616 146.39974564 96.447" in cmds, "fixed position sent with normalised numbers")
            check("GNGSV COM1 5" in cmds and "GPGSV COM1 5" in cmds, "falls back to GPGSV when GNGSV is rejected")
            check(cmds[-1] == "SAVECONFIG" and all(f"RTCM{m} COM1 1" in cmds for m in (1077, 1087, 1097, 1117, 1127)),
                  "all RTCM outputs set and SAVECONFIG sent last")

            # 2. A required command fails -> nothing saved
            pg.evaluate("document.getElementById('console').innerHTML=''")
            rx.fail = {"MODE BASE": "PARSING FAILD"}
            apply_setup("-38.30942616", "146.39974564", "96.447")
            t = wait_done(60)
            cmds = rx.take()
            print("  sent:", cmds)
            check("— stopped" in t and "SAVECONFIG" not in cmds and not any(c.startswith("UNLOG") for c in cmds),
                  "stops at a rejected MODE BASE before UNLOG/SAVECONFIG")

            # 3. Receiver goes silent -> timeout, nothing saved
            pg.evaluate("document.getElementById('console').innerHTML=''")
            rx.fail = {}
            rx.silent = {"RTCM1087"}
            apply_setup("-38.30942616", "146.39974564", "96.447")
            t = wait_done(60)
            cmds = rx.take()
            check("— stopped" in t and "no answer" in t and "SAVECONFIG" not in cmds, "stops when the receiver does not answer")
            rx.silent = set()

            # 4. Invalid coordinates are refused in the dialog (comma decimal)
            rx.take()
            apply_setup("-38,3", "146,4", "96")
            time.sleep(2)
            check(pg.is_visible("#coord-error") and not any(c.startswith("MODE") for c in rx.take()),
                  "comma-decimal coordinates refused, nothing sent")
            pg.keyboard.press("Escape")

            # 5. Manual command + reply shown
            pg.evaluate("document.getElementById('console').innerHTML=''")
            time.sleep(1)
            pg.fill("#cmd", "VERSION")
            pg.click("#cmd-form button")
            time.sleep(3)
            t = console_text()
            check("> VERSION" in t and "#VERSION" in t, "manual command and reply appear in the console")
            # 6. Recording through a real browser download: start, see it grow, stop, file complete and valid
            pg.select_option("#rec-minutes", "15")
            with pg.expect_download(timeout=60000) as dl_info:
                pg.click("#rec-start")
            dl = dl_info.value
            pg.wait_for_function("document.querySelector('#rec-status').textContent.includes('Recording base_')", timeout=30000)
            time.sleep(12)
            st_mid = pg.inner_text("#rec-status")
            pg.click("#rec-stop")
            path = dl.path()   # waits for the download to finish
            data = open(path, "rb").read() if path else b""
            nfr, pos, bad = 0, 0, 0
            while pos + 3 <= len(data):
                ln = ((data[pos + 1] & 3) << 8) | data[pos + 2]
                fr = data[pos:pos + ln + 6]
                if data[pos] == 0xD3 and len(fr) == ln + 6 and T.crc24q(fr[:-3]) == int.from_bytes(fr[-3:], "big"):
                    nfr += 1
                    pos += ln + 6
                else:
                    bad += 1
                    pos += 1
            time.sleep(2)
            st_end = pg.inner_text("#rec-status")
            print("  recording:", st_mid, "|", st_end, "|", dl.suggested_filename, len(data), "bytes")
            check(dl.failure() is None and nfr > 20 and bad == 0 and dl.suggested_filename.endswith(".rtcm3")
                  and "stopped" in st_end, f"browser recording completes: {nfr} valid frames, {bad} bad bytes")

            # 7. Hidden tab stops polling; visible again resumes immediately
            polls = []
            pg.on("request", lambda r: polls.append(time.time()) if "/receiver/" in r.url else None)
            pg.evaluate("""() => { window.__hidden = true;
                Object.defineProperty(document, 'hidden', { configurable: true, get: () => window.__hidden });
                document.dispatchEvent(new Event('visibilitychange')); }""")
            time.sleep(3)
            polls.clear()
            time.sleep(6)
            hidden_polls = len(polls)
            pg.evaluate("() => { window.__hidden = false; document.dispatchEvent(new Event('visibilitychange')); }")
            time.sleep(3)
            check(hidden_polls == 0 and len(polls) >= 2, f"no polling while hidden ({hidden_polls}), resumes when visible ({len(polls)})")

            pg.screenshot(path=os.path.join(work, "ui_after.png"), full_page=True)
            check(not errors, f"no JavaScript errors ({errors[:2]})")
            br.close()
    finally:
        stop.set()
        qemu.terminate()
        print("logs in", work)

    if failures:
        print(f"\n{len(failures)} UI check(s) failed")
        sys.exit(1)
    print("\nUI test passed")


if __name__ == "__main__":
    main()
