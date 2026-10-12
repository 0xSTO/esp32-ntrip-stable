#!/usr/bin/env python3
"""
QEMU test for the built-in NTRIP caster (rovers on the same network).

Boots the QEMU build with both outgoing casters and the built-in caster
enabled (mountpoint STON, username rover / password secret), streams paced
RTCM sequence frames (with NMEA mixed in) into the UART, and checks with real
NTRIP clients:
  - mountpoint list (sourcetable) for v1 and v2 clients, unknown mountpoint
  - missing / wrong password refused (401), garbage refused (400)
  - a connection that never sends a request is closed after ~10 s
  - a v1 and a v2 rover stream at the same time: only valid RTCM, no gaps,
    no NMEA, full rate; a rover sending GGA upstream is unaffected
  - a third rover is turned away (503)
  - a rover that stops reading is dropped without affecting the other rover
    or the outgoing casters
  - /status reports rovers, counters; no reboot or panic

Usage: python test/qemu/run_local_caster_test.py build_qemu
"""

import base64
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_qemu_test as T  # noqa: E402

LC_PORT = 12101
FEED_RATE = 2000
AUTH = "Basic " + base64.b64encode(b"rover:secret").decode()
log = T.log
now = T.now


def build_flash(build_dir, workdir):
    csv = os.path.join(workdir, "nvs.csv")
    with open(csv, "w") as f:
        f.write("key,type,encoding,value\n"
                "config,namespace,,\n"
                "ntr_srv_active,data,i8,1\n"
                "ntr_srv_host,data,string,10.0.2.2\n"
                f"ntr_srv_port,data,u16,{T.PORT_A}\n"
                "ntr_srv_mp,data,string,MOUNTA\n"
                "ntr_srv_pass,data,string,passA\n"
                "ntr_srv2_active,data,i8,1\n"
                "ntr_srv2_host,data,string,10.0.2.2\n"
                f"ntr_srv2_port,data,u16,{T.PORT_B}\n"
                "ntr_srv2_mp,data,string,MOUNTB\n"
                "ntr_srv2_pass,data,string,passB\n"
                "lcst_active,data,i8,1\n"
                "lcst_port,data,u16,2101\n"
                "lcst_mp,data,string,STON\n"
                "lcst_user,data,string,rover\n"
                "lcst_pass,data,string,secret\n"
                "uart_log_fwd,data,i8,1\n")
    nvs = os.path.join(workdir, "nvs.bin")
    subprocess.check_call([sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate", csv, nvs, "0x6000"],
                          stdout=subprocess.DEVNULL)
    flash = os.path.join(workdir, "flash.bin")
    b = build_dir
    subprocess.check_call([sys.executable, "-m", "esptool", "--chip", "esp32", "merge_bin", "--fill-flash-size", "4MB",
                           "-o", flash,
                           "0x1000", f"{b}/bootloader/bootloader.bin",
                           "0x8000", f"{b}/partition_table/partition-table.bin",
                           "0x9000", nvs,
                           "0x10000", f"{b}/esp32-xbee.bin",
                           "0x210000", f"{b}/www.bin"], stdout=subprocess.DEVNULL)
    return flash


def one_shot(request, timeout=10):
    """Send a request, return everything until the caster closes the connection."""
    s = socket.create_connection(("127.0.0.1", LC_PORT), timeout=timeout)
    if request:
        s.sendall(request)
    data = b""
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
            data += d
    except socket.timeout:
        data += b"<timeout>"
    s.close()
    return data


class Dechunker:
    """HTTP chunked decoding for the NTRIP 2.0 stream; any framing error is counted."""

    def __init__(self, sink):
        self.sink = sink
        self.buf = b""
        self.need = None   # bytes of chunk data still expected (+2 for CRLF)
        self.errors = 0
        self.chunks = 0

    def feed(self, data):
        self.buf += data
        while True:
            if self.need is None:
                i = self.buf.find(b"\r\n")
                if i < 0:
                    if len(self.buf) > 16:
                        self.errors += 1
                        self.buf = b""
                    return
                try:
                    n = int(self.buf[:i], 16)
                except ValueError:
                    self.errors += 1
                    self.buf = self.buf[i + 2:]
                    continue
                self.buf = self.buf[i + 2:]
                self.need = n
            if len(self.buf) < self.need + 2:
                return
            payload, tail = self.buf[:self.need], self.buf[self.need:self.need + 2]
            if tail != b"\r\n":
                self.errors += 1
            self.sink(payload)
            self.chunks += 1
            self.buf = self.buf[self.need + 2:]
            self.need = None


class Rover:
    def __init__(self, name, v2=False, read=True, rcvbuf=None, send_gga=False):
        self.name = name
        self.checker = T.FrameChecker()
        self.dechunk = Dechunker(self.checker.feed) if v2 else None
        self.rx_log = []
        self.lock = threading.Lock()
        self.closed_at = None
        self.read = read
        self.stop = False
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        s.settimeout(10)
        s.connect(("127.0.0.1", LC_PORT))
        req = (f"GET /STON HTTP/1.{1 if v2 else 0}\r\n"
               f"Host: esp32\r\nUser-Agent: NTRIP TestRover/1.0\r\n"
               + ("Ntrip-Version: Ntrip/2.0\r\n" if v2 else "")
               + f"Authorization: {AUTH}\r\n\r\n").encode()
        s.sendall(req)
        hdr = b""
        while b"\r\n\r\n" not in hdr:
            d = s.recv(1)
            if not d:
                break
            hdr += d
        self.header = hdr.decode(errors="replace")
        self.status = self.header.split("\r\n")[0]
        self.sock = s
        if read:
            threading.Thread(target=self._read, daemon=True).start()
        if send_gga:
            threading.Thread(target=self._gga, daemon=True).start()

    def _read(self):
        self.sock.settimeout(0.5)
        while not self.stop:
            try:
                d = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                d = b""
            if not d:
                self.closed_at = now()
                return
            with self.lock:
                self.rx_log.append((now(), len(d)))
            (self.dechunk or self.checker).feed(d)

    def _gga(self):
        line = T.nmea_line("$GPGGA,123519,3818.5651,S,14623.9848,E,4,12,0.9,95.2,M,0.0,M,1.0,0000*")
        while not self.stop and self.closed_at is None:
            try:
                self.sock.sendall(line)
            except OSError:
                return
            time.sleep(1)

    def rate(self, t0, t1):
        with self.lock:
            return sum(n for (t, n) in self.rx_log if t0 <= t < t1) / (t1 - t0)

    def close(self):
        self.stop = True
        try:
            self.sock.close()
        except OSError:
            pass


def status():
    with urllib.request.urlopen(f"http://127.0.0.1:{T.HTTP_PORT}/status", timeout=10) as r:
        return json.loads(r.read())


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build_qemu"
    workdir = tempfile.mkdtemp(prefix="ntrip_lc_")
    flash = build_flash(build_dir, workdir)
    console_log = os.path.join(workdir, "console.log")
    log(f"work dir {workdir}")

    a = T.FakeCaster("A", T.PORT_A)
    b = T.FakeCaster("B", T.PORT_B)
    qemu = subprocess.Popen([
        "qemu-system-xtensa", "-display", "none", "-monitor", "none", "-machine", "esp32", "-m", "4M",
        "-drive", f"file={flash},if=mtd,format=raw",
        "-serial", f"tcp:127.0.0.1:{T.UART_PORT},server=on,wait=on",
        "-serial", f"file:{console_log}",
        "-nic", f"user,model=open_eth,hostfwd=tcp:127.0.0.1:{T.HTTP_PORT}-:80,"
                f"hostfwd=tcp:127.0.0.1:{LC_PORT}-:2101",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    uart = None
    for _ in range(50):
        try:
            uart = socket.create_connection(("127.0.0.1", T.UART_PORT))
            break
        except OSError:
            time.sleep(0.2)
    assert uart, "could not connect to QEMU UART"

    uart_lines = []
    stop = threading.Event()

    def reader():
        buf = b""
        while not stop.is_set():
            try:
                data = uart.recv(4096)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                uart_lines.append(line.decode(errors="replace").rstrip())

    gsv = T.nmea_line("$GPGSV,1,1,02,02,45,123,44,05,30,045,40,1*")
    feeding = threading.Event()

    def feeder():
        seq = 0
        interval = len(T.make_frame(0)) / FEED_RATE
        nxt = time.monotonic()
        while not stop.is_set():
            if feeding.is_set():
                try:
                    uart.sendall(T.make_frame(seq))
                    if seq % 20 == 10:
                        uart.sendall(gsv)
                except OSError:
                    return
                seq += 1
            nxt += interval
            time.sleep(max(0, nxt - time.monotonic()))

    threading.Thread(target=reader, daemon=True).start()
    threading.Thread(target=feeder, daemon=True).start()

    failures = []

    def check(cond, msg):
        log(("PASS " if cond else "FAIL ") + msg)
        if not cond:
            failures.append(msg)

    def wait_for(pred, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                if pred():
                    return True
            except Exception:
                pass
            time.sleep(0.3)
        return False

    heap_min = None

    def heap_sample():
        nonlocal heap_min
        try:
            free = status()["heap"]["free"]
            heap_min = free if heap_min is None else min(heap_min, free)
        except Exception:
            pass

    rovers = []
    wmem_path = "/proc/sys/net/ipv4/tcp_wmem"
    old_wmem = None
    try:
        check(wait_for(lambda: any("INIT,COMPLETE" in l for l in uart_lines), 90), "firmware booted")
        feeding.set()
        check(wait_for(lambda: status()["local_caster"]["listening"], 30), "built-in caster listening")
        st = status()["local_caster"]
        check(st["port"] == 2101 and st["mountpoint"] == "STON" and st["auth"], f"status reports settings: {st}")
        wait_for(lambda: a.checkers and b.checkers, 30)

        # --- Phase 1: requests that don't stream
        r = one_shot(b"GET / HTTP/1.0\r\nUser-Agent: NTRIP test\r\n\r\n")
        check(r.startswith(b"SOURCETABLE 200 OK\r\n") and b"STR;STON;STON;RTCM 3.3;" in r and b";B;N;0;\r\n" in r
              and r.endswith(b"ENDSOURCETABLE\r\n"), f"v1 mountpoint list: {r[:60]!r}")
        r = one_shot(b"GET / HTTP/1.1\r\nNtrip-Version: Ntrip/2.0\r\n\r\n")
        check(r.startswith(b"HTTP/1.1 200 OK\r\n") and b"gnss/sourcetable" in r and b"STR;STON;" in r,
              "v2 mountpoint list")
        r = one_shot(b"GET /NOPE HTTP/1.0\r\n\r\n")
        check(r.startswith(b"SOURCETABLE 200 OK"), "unknown mountpoint gets the list")
        r = one_shot(b"GET /STON HTTP/1.0\r\nUser-Agent: NTRIP test\r\n\r\n")
        check(b" 401 " in r.split(b"\r\n")[0] and b"WWW-Authenticate: Basic" in r, "missing password refused (401)")
        bad = "Basic " + base64.b64encode(b"rover:wrong").decode()
        r = one_shot(f"GET /STON HTTP/1.0\r\nAuthorization: {bad}\r\n\r\n".encode())
        check(b" 401 " in r.split(b"\r\n")[0], "wrong password refused (401)")
        r = one_shot(b"HELLO THERE\r\n\r\n")
        check(b" 400 " in r.split(b"\r\n")[0], "garbage refused (400)")
        t0 = now()
        r = one_shot(b"", timeout=30)
        dt = now() - t0
        check(r == b"" and 8 <= dt <= 16, f"silent connection closed after {dt:.1f} s")
        st = status()["local_caster"]
        check(st["clients"] == [] and st["refused"] >= 4 and st["sessions"] == 0, f"counters after refusals: {st}")

        # --- Phase 2: two rovers at once, v1 (sending GGA) and v2
        r1 = Rover("v1", v2=False, send_gga=True)
        rovers.append(r1)
        r2 = Rover("v2", v2=True)
        rovers.append(r2)
        check(r1.status == "ICY 200 OK", f"v1 rover accepted: {r1.status!r}")
        check(r2.status == "HTTP/1.1 200 OK" and "Ntrip-Version: Ntrip/2.0" in r2.header
              and "Transfer-Encoding: chunked" in r2.header, f"v2 rover accepted (chunked): {r2.status!r}")
        time.sleep(3)
        t0 = now()
        for _ in range(10):
            heap_sample()
            time.sleep(2)
        t1 = now()
        for rv in (r1, r2):
            ck = rv.checker
            rate = rv.rate(t0, t1)
            check(rate > FEED_RATE * 0.9, f"rover {rv.name} gets full rate: {rate:.0f} B/s")
            check(ck.bad == 0 and ck.gaps == 0 and ck.frames > 300 and ck.text_seen == b"",
                  f"rover {rv.name}: {ck.frames} frames, bad {ck.bad}, gaps {ck.gaps}, text {ck.text_seen[:20]!r}")
        check(r2.dechunk.errors == 0 and r2.dechunk.chunks > 50,
              f"v2 chunked framing valid ({r2.dechunk.chunks} chunks, {r2.dechunk.errors} errors)")
        for c in (a, b):
            rate = c.bytes_in_window(t0, t1) / (t1 - t0)
            check(rate > FEED_RATE * 0.9, f"caster {c.name} unaffected: {rate:.0f} B/s")
        st = status()["local_caster"]
        check(len(st["clients"]) == 2 and st["sessions"] == 2 and sorted(c["v2"] for c in st["clients"]) == [False, True]
              and all(c["bytes"] > 10000 for c in st["clients"]), f"status lists both rovers: {st['clients']}")

        # --- Phase 3: a third rover is turned away
        r = one_shot(f"GET /STON HTTP/1.0\r\nAuthorization: {AUTH}\r\n\r\n".encode())
        check(b" 503 " in r.split(b"\r\n")[0], "third rover refused (503)")

        # --- Phase 4: v2 rover leaves; a rover that never reads takes its place
        r2.close()
        check(wait_for(lambda: len(status()["local_caster"]["clients"]) == 1, 10), "closed rover removed")
        # QEMU's user networking buffers whatever the guest sends until the host
        # socket's send buffer is full, and Linux lets that grow to 4 MB (half an
        # hour of RTCM). Cap it during this phase so the back-pressure reaches
        # the ESP32 within a minute, as on a real network.
        try:
            old_wmem = open(wmem_path).read().strip()
            with open(wmem_path, "w") as f:
                f.write("4096 8192 16384")
        except OSError as e:
            log(f"could not cap tcp_wmem ({e}); the slow-rover phase may take very long")
            old_wmem = None
        slow = Rover("slow", read=False, rcvbuf=2048)
        rovers.append(slow)
        check(slow.status == "ICY 200 OK", "non-reading rover accepted")
        t0 = now()
        dropped = wait_for(lambda: (heap_sample() or True) and status()["local_caster"]["sessions"] == 3
                           and len(status()["local_caster"]["clients"]) == 1, 240)
        t1 = now()
        if old_wmem:   # Linux grows the buffer up to the current maximum, so restore only now
            with open(wmem_path, "w") as f:
                f.write(old_wmem)
            old_wmem = None
        check(dropped, f"non-reading rover dropped after {t1 - t0:.0f} s")
        log(f"bytes skipped for slow rovers: {status()['local_caster']['dropped']} (0 is fine: a rover that stops"
            " completely is dropped before it is ever skipped ahead)")
        rate = r1.rate(t0, t1)
        check(rate > FEED_RATE * 0.9 and r1.checker.gaps == 0 and r1.checker.bad == 0,
              f"other rover unaffected while one stalled: {rate:.0f} B/s, gaps {r1.checker.gaps}")
        for c in (a, b):
            rate = c.bytes_in_window(t0, t1) / (t1 - t0)
            ck = c.checkers[-1]
            check(rate > FEED_RATE * 0.9 and ck.bad == 0, f"caster {c.name} unaffected while rover stalled: {rate:.0f} B/s")

        # --- Phase 5: rover reconnects after leaving
        r1.close()
        check(wait_for(lambda: status()["local_caster"]["clients"] == [], 10), "all rovers gone")
        r3 = Rover("again")
        rovers.append(r3)
        time.sleep(5)
        check(r3.status == "ICY 200 OK" and r3.checker.frames > 50 and r3.checker.bad == 0, "rover can reconnect")

        text = "\n".join(uart_lines)
        check(text.count("INIT,START") == 1, f"no reboot (boots={text.count('INIT,START')})")
        console = open(console_log, "rb").read().decode(errors="replace")
        check("Guru Meditation" not in console + text and "abort()" not in console + text, "no panic")
        st = status()["local_caster"]
        check(st["stack_free"] > 1024, f"caster task stack headroom: {st['stack_free']} bytes unused")
        log(f"lowest free heap seen via /status: {heap_min} bytes (QEMU has no WiFi buffers; real board is lower)")
    finally:
        if old_wmem:
            with open(wmem_path, "w") as f:
                f.write(old_wmem)
        for rv in rovers:
            rv.close()
        stop.set()
        qemu.kill()
        qemu.wait()

    if failures:
        log(f"{len(failures)} check(s) FAILED")
        for f in failures:
            log("  - " + f)
        sys.exit(1)
    log("built-in caster test passed")


if __name__ == "__main__":
    main()
