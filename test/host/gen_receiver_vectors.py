#!/usr/bin/env python3
"""
Generate test vectors for the receiver framer / GNSS monitor host tests.

Inputs:
  * a real RTCM 3 MSM7 recording (RTKLIB test data GMSD7_20121014.rtcm3:
    GPS/GLONASS/QZSS/BeiDou MSM7 + 1033; RTKLIB is BSD-2 licensed)
  * synthetic frames built here: 1005 (Mitchell's base, so LLH can be checked
    against rtk2go's report) and a Galileo 1097 MSM7 (not in the recording)

Outputs (test/host/data/):
  receiver_stream.bin  RTCM frames interleaved with NMEA, UM980 command
                       replies, and garbage, as the UART would deliver them
  receiver_expect.json what must come out: frame list (sha256 of each), text
                       lines, and per-satellite/per-band C/N0 decoded by pyrtcm
                       (an independent decoder) for the last epoch of each MSM

Usage: gen_receiver_vectors.py <GMSD7_20121014.rtcm3>   (needs: pip install pyrtcm)
"""
import hashlib
import json
import os
import random
import sys

from pyrtcm import RTCMReader, parse_msm

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "data")


def crc24q(data):
    crc = 0
    for b in data:
        crc ^= b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc & 0xFFFFFF


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def s(self, value, n):
        self.u(value & ((1 << n) - 1), n)

    def payload(self):
        b = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


def frame(payload):
    hdr = bytes([0xD3, (len(payload) >> 8) & 3, len(payload) & 0xFF])
    body = hdr + payload
    c = crc24q(body)
    return body + bytes([c >> 16, (c >> 8) & 0xFF, c & 0xFF])


def make_1005(x, y, z, station=0):
    b = Bits()
    b.u(1005, 12); b.u(station, 12); b.u(0, 6); b.u(1, 1); b.u(1, 1); b.u(1, 1); b.u(0, 1)
    b.s(round(x * 10000), 38); b.u(1, 1); b.u(0, 1)
    b.s(round(y * 10000), 38); b.u(0, 2)
    b.s(round(z * 10000), 38)
    return frame(b.payload())


def make_msm7(msgnum, sats, sigs, cnr):
    """sats: list of sat numbers, sigs: list of signal ids, cnr: {(sat,sig): dBHz} (cells present)."""
    b = Bits()
    b.u(msgnum, 12); b.u(0, 12); b.u(123456000, 30); b.u(0, 1); b.u(0, 3); b.u(0, 7)
    b.u(0, 2); b.u(0, 2); b.u(0, 1); b.u(0, 3)
    for i in range(1, 65):
        b.u(1 if i in sats else 0, 1)
    for i in range(1, 33):
        b.u(1 if i in sigs else 0, 1)
    cells = []
    for s in sorted(sats):
        for g in sorted(sigs):
            present = (s, g) in cnr
            b.u(1 if present else 0, 1)
            if present:
                cells.append((s, g))
    n = len(sats)
    for _ in range(n): b.u(80, 8)        # DF397 rough range ms
    for _ in range(n): b.u(0, 4)         # extended sat info
    for _ in range(n): b.u(512, 10)      # DF398
    for _ in range(n): b.s(100, 14)      # DF399
    for _ in cells: b.s(1000, 20)        # DF405
    for _ in cells: b.s(2000, 24)        # DF406
    for _ in cells: b.u(700, 10)         # DF407
    for _ in cells: b.u(0, 1)            # DF420
    for c in cells: b.u(round(cnr[c] * 16), 10)  # DF408
    for _ in cells: b.s(-50, 15)         # DF404
    return frame(b.payload())


# RINEX observation code (band digit) -> our band index (0 L1, 1 L2, 2 L5, 3 L6)
def band_from_rinex(gnss, code):
    d = code[0]
    if gnss == "GPS":
        return {"1": 0, "2": 1, "5": 2}.get(d)
    if gnss == "GLONASS":
        return {"1": 0, "2": 1}.get(d)
    if gnss == "GALILEO":
        return {"1": 0, "7": 1, "5": 2, "8": 2, "6": 3}.get(d)
    if gnss == "QZSS":
        return {"1": 0, "2": 1, "5": 2, "6": 3}.get(d)
    if gnss == "BEIDOU":
        return {"2": 0, "1": 0, "7": 1, "5": 2, "6": 3}.get(d)
    return None


SYS = {"GPS": "G", "GLONASS": "R", "GALILEO": "E", "QZSS": "J", "BEIDOU": "C"}


def main():
    src = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    rnd = random.Random(42)

    frames = []
    with open(src, "rb") as f:
        for raw, msg in RTCMReader(f, quitonerror=0):
            if msg is not None:
                frames.append(raw)
    frames = frames[:400]  # ~ first 100 epochs: keeps the committed file small

    # Synthetic Galileo MSM7 and 1005
    gal_cnr = {(1, 2): 45.25, (1, 14): 47.5, (1, 22): 46.0625, (1, 8): 42.0,
               (5, 2): 38.5, (5, 22): 39.0, (21, 2): 50.0, (21, 8): 44.75}
    gal = make_msm7(1097, [1, 5, 21], [2, 8, 14, 22], gal_cnr)
    pos = make_1005(-4173994.6057, 2773222.0951, -3932511.6382, station=7)

    # Validate the synthetic frames with pyrtcm
    for fr in (gal, pos):
        msg = RTCMReader.parse(fr)
        assert msg is not None, "pyrtcm rejected a synthetic frame"
    frames += [gal, pos]

    nmea = [
        "$GPGSV,2,1,07,02,45,123,44,05,30,045,40,13,10,300,35,15,60,210,47,1*6A",
        "$GPGSV,2,2,07,18,05,170,30,24,20,090,42,29,80,010,48,1*6C",
        "$GAGSV,1,1,03,01,55,250,45,05,12,100,38,21,70,330,50,7*7F",
        "$GBGSV,1,1,02,01,40,140,44,06,35,200,43,1*70",
        "$GLGSV,1,1,02,65,25,060,41,72,50,280,46,1*70",
    ]
    # Fix up checksums (written by hand above)
    def nmea_cs(s):
        body = s[1:s.index("*")]
        c = 0
        for ch in body:
            c ^= ord(ch)
        return f"${body}*{c:02X}"
    nmea = [nmea_cs(s) for s in nmea]
    replies = [
        "$command,VERSION,response: OK*45",
        "#VERSION,97,GPS,FINE,2190,365000,0,0,18,511;UM980,R4.10Build11833,HRPT00-S10C-P,2310415000001-LR23A0225104240,ff339d6470bec59b,2023/11/24*4d2f2c0b",
        "$command,rtcm1230 COM3 10,response: PARSING FAILD NO MATCHING FUNC RTCM1230*33",
    ]

    stream = bytearray()
    exp_frames, exp_lines = [], []
    texts = nmea + replies
    ti = 0
    for i, fr in enumerate(frames):
        if i % 37 == 5:
            stream += bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 40)))  # garbage
        if i % 23 == 3:
            stream += b"\xd3\x00\x13" + bytes(rnd.randrange(256) for _ in range(20))   # fake frame start, bad CRC
        if i % 11 == 7 and ti < len(texts):
            stream += (texts[ti] + "\r\n").encode()
            exp_lines.append(texts[ti])
            ti += 1
        stream += fr
        exp_frames.append(hashlib.sha256(fr).hexdigest())
    while ti < len(texts):
        stream += (texts[ti] + "\r\n").encode()
        exp_lines.append(texts[ti])
        ti += 1

    # Expected decode: the last message of each MSM type, by pyrtcm
    last = {}
    for fr in frames:
        msg = RTCMReader.parse(fr)
        if msg.identity in ("1077", "1087", "1097", "1117", "1127"):
            last[msg.identity] = msg
    sats = {}
    for ident, msg in last.items():
        meta, _, cells = parse_msm(msg)
        sysc = SYS[meta["gnss"]]
        for c in cells:
            if not c["DF408"]:
                continue
            prn = int(c["CELLPRN"])
            if sysc == "J" and prn > 192:
                prn -= 192  # pyrtcm reports QZSS as PRN 193+; RINEX/receivers use J01..
            sid = f"{sysc}{prn:02d}"
            band = band_from_rinex(meta["gnss"], c["CELLSIG"])
            s = sats.setdefault(sid, {"bands": [0, 0, 0, 0], "cnr_max": 0.0})
            if band is not None:
                s["bands"][band] += 1
            s["cnr_max"] = max(s["cnr_max"], c["DF408"])

    with open(os.path.join(OUT, "receiver_stream.bin"), "wb") as f:
        f.write(stream)
    with open(os.path.join(OUT, "receiver_expect.json"), "w") as f:
        json.dump({"frames": exp_frames, "lines": exp_lines, "sats": sats,
                   "position": {"lat": -38.30942616, "lon": 146.39974564, "height": 96.447}}, f, indent=1)
    print(f"{len(exp_frames)} frames, {len(exp_lines)} lines, {len(sats)} satellites, {len(stream)} bytes")


if __name__ == "__main__":
    main()
