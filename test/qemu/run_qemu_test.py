#!/usr/bin/env python3
"""
End-to-end test of the caster logic in Espressif QEMU.

QEMU has no WiFi, so the firmware is built with CONFIG_NTRIP_QEMU_TEST
(emulated Ethernet in place of WiFi, see sdkconfig.qemu). This script:

  * writes an NVS image that enables both NTRIP servers, pointed at two fake
    casters run by this script on the host (reachable as 10.0.2.2 via slirp)
  * feeds framed, sequence-numbered "RTCM" into UART0 at ~1 KB/s
  * scripts caster behaviour: normal streaming, a caster-side drop, refused
    logins, and a long read stall on caster A
  * checks: no panic/reboot, both casters receive uncorrupted, in-order data,
    reconnect after a drop, back-off after refused logins, caster B keeps
    streaming while caster A is stalled.

Limitation: slirp buffers without limit, so a caster that stops reading never
pushes back on the ESP32's TCP stack here. The host tests (test/host) cover
real back-pressure with kernel sockets.

Usage: run_qemu_test.py <build_dir>   (needs IDF environment for esptool, nvs gen, qemu)
"""

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

PORT_A = 21010
PORT_B = 21011
UART_PORT = 5555
HTTP_PORT = 18080
HERE = os.path.dirname(os.path.abspath(__file__))
RECORDED = os.path.join(HERE, "..", "host", "data", "receiver_stream.bin")
FEED_RATE = 1000  # bytes/s

START = time.monotonic()


def now():
    return time.monotonic() - START


def log(msg):
    print(f"[{now():7.1f}] {msg}", flush=True)


# --------------------------------------------------------------------------- framing

TEST_MSG = 4093  # proprietary RTCM message number used for the sequence frames


def crc24q(data):
    crc = 0
    for b in data:
        crc ^= b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc & 0xFFFFFF


def rtcm_frame(payload):
    body = bytes([0xD3, (len(payload) >> 8) & 3, len(payload) & 0xFF]) + payload
    c = crc24q(body)
    return body + bytes([c >> 16, (c >> 8) & 0xFF, c & 0xFF])


def nmea_line(body):
    """body like '$GPGSV,...*': adds checksum and CRLF."""
    cs = 0
    for ch in body[1:-1]:
        cs ^= ord(ch)
    return f"{body}{cs:02X}\r\n".encode()


def make_frame(seq):
    # 12-bit message number, 4 spare bits, 32-bit sequence, filler: 100 bytes
    payload = struct.pack(">HI", TEST_MSG << 4, seq) + bytes((seq + i) & 0xFF for i in range(94))
    return rtcm_frame(payload)


class FrameChecker:
    """Parses the byte stream of one caster connection: it must contain only
    valid RTCM frames (any message type); sequence frames must be in order."""

    def __init__(self):
        self.buf = b""
        self.frames = 0
        self.bad = 0
        self.gaps = 0
        self.last_seq = None
        self.started = False
        self.frames_any = 0
        self.gap_list = []
        self.text_seen = b""

    def feed(self, data):
        self.buf += data
        while True:
            if not self.buf:
                return
            if self.buf[0] != 0xD3:
                # The ESP32 must only ever send whole RTCM frames
                self.bad += 1
                self.text_seen += self.buf[:1]
                self.buf = self.buf[1:]
                continue
            if len(self.buf) < 3:
                return
            length = ((self.buf[1] & 3) << 8) | self.buf[2]
            total = 3 + length + 3
            if len(self.buf) < total:
                return
            frame = self.buf[:total]
            if (self.buf[1] & 0xFC) or crc24q(frame[:-3]) != int.from_bytes(frame[-3:], "big"):
                # The first frame of a connection may be cut (ring cleared at connect)
                if self.started:
                    self.bad += 1
                self.buf = self.buf[1:]
                continue
            self.buf = self.buf[total:]
            self.started = True
            self.frames_any += 1
            msg = (frame[3] << 4) | (frame[4] >> 4)
            if msg != TEST_MSG:
                continue
            (seq,) = struct.unpack(">I", frame[5:9])
            if self.last_seq is not None and seq != self.last_seq + 1:
                self.gaps += 1
                self.gap_list.append((self.last_seq, seq))
            self.last_seq = seq
            self.frames += 1


# --------------------------------------------------------------------------- fake caster

class FakeCaster:
    def __init__(self, name, port):
        self.name = name
        self.port = port
        self.logins = []        # times of SOURCE requests
        self.refuse = 0         # refuse this many upcoming logins
        self.drop_now = False   # close the active connection
        self.stall = False      # stop reading
        self.conn = None
        self.checkers = []
        self.bytes_total = 0
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(4)
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def bytes_in_window(self, t0, t1):
        with self.lock:
            return sum(n for (t, n) in self.rx_log if t0 <= t < t1)

    rx_log = None

    def _accept_loop(self):
        self.rx_log = []
        while True:
            conn, _ = self.srv.accept()
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, conn):
        conn.settimeout(15)
        try:
            req = b""
            while b"\r\n\r\n" not in req:
                chunk = conn.recv(512)
                if not chunk:
                    return
                req += chunk
        except OSError:
            conn.close()
            return
        line = req.split(b"\r\n")[0].decode(errors="replace")
        self.logins.append(now())
        if self.refuse > 0:
            self.refuse -= 1
            log(f"{self.name}: login '{line[:30]}...' -> refused")
            conn.sendall(b"ERROR - Bad Password\r\n")
            conn.close()
            return
        log(f"{self.name}: login '{line.split(' ')[0]} ... {line.split(' ')[-1]}' -> accepted")
        conn.sendall(b"ICY 200 OK\r\n")
        rest = req.split(b"\r\n\r\n", 1)[1]
        checker = FrameChecker()
        self.checkers.append(checker)
        checker.feed(rest)
        self.conn = conn
        conn.settimeout(0.2)
        while True:
            if self.drop_now:
                self.drop_now = False
                log(f"{self.name}: dropping connection (caster side)")
                conn.close()
                return
            if self.stall:
                time.sleep(0.2)
                continue
            try:
                data = conn.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                data = b""
            if not data:
                log(f"{self.name}: connection closed by ESP32")
                conn.close()
                return
            with self.lock:
                self.rx_log.append((now(), len(data)))
            self.bytes_total += len(data)
            checker.feed(data)


# --------------------------------------------------------------------------- QEMU plumbing

def build_flash(build_dir, workdir):
    csv = os.path.join(workdir, "nvs.csv")
    with open(csv, "w") as f:
        f.write("key,type,encoding,value\n"
                "config,namespace,,\n"
                "ntr_srv_active,data,i8,1\n"
                "ntr_srv_host,data,string,10.0.2.2\n"
                f"ntr_srv_port,data,u16,{PORT_A}\n"
                "ntr_srv_mp,data,string,MOUNTA\n"
                "ntr_srv_pass,data,string,passA\n"
                "ntr_srv2_active,data,i8,1\n"
                "ntr_srv2_host,data,string,10.0.2.2\n"
                f"ntr_srv2_port,data,u16,{PORT_B}\n"
                "ntr_srv2_mp,data,string,MOUNTB\n"
                "ntr_srv2_pass,data,string,passB\n"
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


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "build_qemu"
    workdir = tempfile.mkdtemp(prefix="ntrip_qemu_")
    flash = build_flash(build_dir, workdir)
    console_log = os.path.join(workdir, "console.log")
    uart_log = os.path.join(workdir, "uart0.log")
    log(f"work dir {workdir}")

    a = FakeCaster("A", PORT_A)
    b = FakeCaster("B", PORT_B)

    qemu = subprocess.Popen([
        "qemu-system-xtensa", "-display", "none", "-monitor", "none", "-machine", "esp32", "-m", "4M",
        "-drive", f"file={flash},if=mtd,format=raw",
        "-serial", f"tcp:127.0.0.1:{UART_PORT},server=on,wait=on",
        "-serial", f"file:{console_log}",
        "-nic", f"user,model=open_eth,hostfwd=tcp:127.0.0.1:{HTTP_PORT}-:80",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    uart = None
    for _ in range(50):
        try:
            uart = socket.create_connection(("127.0.0.1", UART_PORT))
            break
        except OSError:
            time.sleep(0.2)
    assert uart, "could not connect to QEMU UART"

    uart_lines = []
    stop = threading.Event()

    def reader():
        buf = b""
        with open(uart_log, "wb") as f:
            while not stop.is_set():
                try:
                    data = uart.recv(4096)
                except OSError:
                    return
                if not data:
                    return
                f.write(data)
                f.flush()
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    uart_lines.append((now(), line.decode(errors="replace").rstrip()))

    feeding = threading.Event()

    text_lines = [
        nmea_line("$GPGSV,1,1,02,02,45,123,44,05,30,045,40,1*"),
        b"$command,VERSION,response: OK*45\r\n",
    ]

    def feeder():
        seq = 0
        frame_len = len(make_frame(0))
        interval = frame_len / FEED_RATE
        nxt = time.monotonic()
        while not stop.is_set():
            if feeding.is_set():
                try:
                    uart.sendall(make_frame(seq))
                    if seq % 20 == 10:
                        # NMEA and command replies mixed in: must never reach a caster
                        uart.sendall(text_lines[(seq // 20) % 2])
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
            if pred():
                return True
            time.sleep(0.2)
        return False

    try:
        # Boot
        booted = wait_for(lambda: any("INIT,COMPLETE" in l for _, l in uart_lines), 90)
        check(booted, "firmware booted")
        # Phase 0: receiver side channel, using a real RTCM recording with
        # NMEA, command replies, garbage and corrupt frames mixed in
        rec = open(RECORDED, "rb").read()
        log(f"feeding recorded receiver stream ({len(rec)} bytes)")
        # Paced like a real receiver at 115200 baud would be at most; QEMU's UART
        # has no flow control and drops bytes if fed in large bursts.
        def feed_paced(data, rate=3000):
            for i in range(0, len(data), 256):
                uart.sendall(data[i:i + 256])
                time.sleep(256 / rate)
        feed_paced(rec)
        ok = wait_for(lambda: a.checkers and b.checkers, 60)
        check(ok, "both casters logged in on the recorded stream")

        def http(path, body=None):
            req = urllib.request.Request(f"http://127.0.0.1:{HTTP_PORT}{path}",
                                         data=json.dumps(body).encode() if body is not None else None,
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=15) as r:
                return json.loads(r.read())

        # Replay the tail of the recording so the live view is fresh
        feed_paced(rec[-12000:])
        # A real receiver repeats GSV every few seconds; the recording has it near the start only
        expect = json.load(open(os.path.join(HERE, "..", "host", "data", "receiver_expect.json")))
        feed_paced("".join(l + "\r\n" for l in expect["lines"] if "GSV" in l).encode())
        time.sleep(1)
        live = http("/receiver/live")
        open(os.path.join(workdir, "live.json"), "w").write(json.dumps(live, indent=1))
        check(live["satellites"] >= 20 and live["signals"] >= 40,
              f"live view: {live['satellites']} satellites, {live['signals']} signals, mean C/N0 {live['mean_cnr']}")
        pos = live.get("position", {})
        check(abs(pos.get("lat", 0) + 38.30942616) < 1e-6 and abs(pos.get("lon", 0) - 146.39974564) < 1e-6,
              f"live view base position {pos.get('lat')}, {pos.get('lon')}")
        types = {m["type"] for m in live["messages"]}
        check({1077, 1087, 1097, 1117, 1127, 1005} <= types, f"live view message types {sorted(types)}")
        check(any(s["el"] >= 0 for s in live["sats"]), "live view has sky positions from GSV")
        check(live["receiver"]["type"] == "TRIMBLE NETR9", f"receiver from 1033: {live['receiver']['type']!r}")
        check(live["stream"]["rtcm_only"] is True and live["stream"]["garbage"] > 0,
              f"stream filter on, garbage dropped: {live['stream']}")

        cons = http("/receiver/console?since=0")
        texts = [l["text"] for l in cons["lines"]]
        check("$command,VERSION,response: OK*45" in texts and any(t.startswith("#VERSION") for t in texts)
              and not any(t.startswith("$GPGSV") for t in texts),
              f"console shows receiver replies but not NMEA ({len(texts)} lines)")

        r = http("/receiver/command", {"cmd": "VERSION"})
        ok = r.get("ok") and wait_for(lambda: any(l.strip() == "VERSION" for _, l in uart_lines), 10)
        check(ok, "command from web page reaches the receiver UART as 'VERSION\\r\\n'")
        r = http("/receiver/command", {"cmd": "FRESET"})
        check(r.get("ok") is False and not any(l.strip().upper() == "FRESET" for _, l in uart_lines),
              f"FRESET without confirmation refused: {r}")
        r = http("/receiver/command", {"cmd": "bad\u0001cmd"})
        check(r.get("ok") is False, "command with control characters refused")
        cons = http(f"/receiver/console?since={cons['seq']}")
        check(any(l["dir"] == ">" and l["text"] == "VERSION" for l in cons["lines"]), "sent command echoed in console")

        for c in (a, b):
            ck = c.checkers[-1]
            check(ck.bad == 0 and ck.frames_any > 100 and b"$" not in ck.text_seen,
                  f"caster {c.name} got only valid RTCM from the recording ({ck.frames_any} frames, bad={ck.bad})")

        heap = http("/status")["heap"]
        log(f"heap free {heap['free']} of {heap['total']}")
        check(heap["free"] > 40000, f"enough free heap ({heap['free']} bytes)")

        feeding.set()

        # Phase 1: both casters connect and stream
        ok = wait_for(lambda: a.checkers and b.checkers, 60)
        check(ok, "both casters logged in after UART data started")
        time.sleep(30)
        for c in (a, b):
            ck = c.checkers[-1]
            check(ck.frames > 150 and ck.bad == 0 and ck.gaps == 0,
                  f"caster {c.name} streaming clean data ({ck.frames} frames, bad={ck.bad}, gaps={ck.gaps} {ck.gap_list[:3]})")
        if os.environ.get("QUICK"):
            raise SystemExit

        # Phase 2: caster-side drop, expect reconnect in ~5 s
        n = len(a.logins)
        t_drop = now()
        a.drop_now = True
        ok = wait_for(lambda: len(a.logins) > n, 30)
        dt = (a.logins[-1] - t_drop) if ok else None
        check(ok and 3 <= dt <= 12, f"caster A reconnected after drop in {dt and round(dt, 1)} s")
        time.sleep(5)

        # Phase 3: refused logins -> back-off grows, never faster than 5 s
        n = len(a.logins)
        a.refuse = 4
        a.drop_now = True
        ok = wait_for(lambda: len(a.logins) >= n + 5, 150)
        times = a.logins[n:n + 5]
        gaps = [round(t2 - t1, 1) for t1, t2 in zip(times, times[1:])]
        check(ok and len(gaps) == 4 and all(g >= 4 for g in gaps) and gaps == sorted(gaps) and gaps[-1] > gaps[0],
              f"back-off grows between refused logins: {gaps} s")
        wait_for(lambda: a.checkers and a.checkers[-1].frames > 20, 60)

        # Phase 4: caster A stops reading for 150 s; B must keep streaming
        a.stall = True
        t0 = now()
        log("caster A stalled (not reading)")
        time.sleep(150)
        t1 = now()
        a.stall = False
        b_rate = b.bytes_in_window(t0, t1) / (t1 - t0)
        check(b_rate > FEED_RATE * 0.8, f"caster B unaffected during A stall: {b_rate:.0f} B/s")
        time.sleep(20)
        ckb = b.checkers[-1]
        check(ckb.bad == 0, f"caster B data never corrupted (bad={ckb.bad}, gaps={ckb.gaps})")

        # Global: no panic / reboot
        console = open(console_log, "rb").read().decode(errors="replace")
        uart_text = "\n".join(l for _, l in uart_lines)
        boots = uart_text.count("INIT,START")
        check(boots == 1, f"no reboot during test (boots={boots})")
        check("Guru Meditation" not in console + uart_text and "abort()" not in console + uart_text, "no panic")

        disc = [l for _, l in uart_lines if "Disconnected from" in l]
        slow = [l for _, l in uart_lines if "Network slow" in l]
        log(f"{len(disc)} disconnect lines, e.g.: {disc[:3]}")
        log(f"{len(slow)} 'Network slow' lines")
    finally:
        stop.set()
        qemu.terminate()
        qemu.wait(10)
        log(f"logs in {workdir}")

    if failures:
        print(f"\n{len(failures)} check(s) failed")
        sys.exit(1)
    print("\nQEMU end-to-end test passed")


if __name__ == "__main__":
    main()
