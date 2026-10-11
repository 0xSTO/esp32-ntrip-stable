#!/usr/bin/env python3
"""Compare receiver_driver output with the expectations from gen_receiver_vectors.py."""
import hashlib
import json
import os
import struct
import sys

outdir = sys.argv[1]
exp = json.load(open(os.path.join(os.path.dirname(__file__), "data", "receiver_expect.json")))
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


# Frames: exactly the valid ones, in order, byte-identical
frames = []
data = open(os.path.join(outdir, "frames.bin"), "rb").read()
pos = 0
while pos < len(data):
    (n,) = struct.unpack("<I", data[pos:pos + 4])
    frames.append(data[pos + 4:pos + 4 + n])
    pos += 4 + n
got = [hashlib.sha256(f).hexdigest() for f in frames]
check(got == exp["frames"], f"frames differ: got {len(got)}, expected {len(exp['frames'])}")

# Lines
lines = open(os.path.join(outdir, "lines.txt")).read().splitlines()
check(lines == exp["lines"], f"lines differ:\n got {lines}\n exp {exp['lines']}")

# Live view vs pyrtcm decode
live = json.load(open(os.path.join(outdir, "live.json")))
sats = {s["id"]: s for s in live["sats"] if s["t"] == 1}
check(set(sats) == set(exp["sats"]), f"satellite sets differ: extra {set(sats) - set(exp['sats'])}, missing {set(exp['sats']) - set(sats)}")
for sid, e in exp["sats"].items():
    s = sats.get(sid)
    if not s:
        continue
    check(s["b"] == e["bands"], f"{sid}: bands {s['b']} != {e['bands']}")
    check(abs(s["cnr"] - e["cnr_max"]) < 0.051, f"{sid}: cnr {s['cnr']} != {e['cnr_max']}")
check(live["satellites"] == len(exp["sats"]), f"satellite count {live['satellites']}")
check(live["signals"] == sum(sum(e["bands"]) for e in exp["sats"].values()), f"signal count {live['signals']}")

p = live.get("position")
check(p is not None, "no position")
if p:
    check(abs(p["lat"] - exp["position"]["lat"]) < 1e-7 and abs(p["lon"] - exp["position"]["lon"]) < 1e-7
          and abs(p["height"] - exp["position"]["height"]) < 0.002, f"position {p}")
check(live["receiver"]["type"] == "TRIMBLE NETR9" and live["receiver"]["antenna"] == "", f"1033 decode: {live['receiver']}")

# Sky view from GSV
allsats = {s["id"]: s for s in live["sats"]}
check(allsats.get("G02", {}).get("el") == 45 and allsats.get("G02", {}).get("az") == 123, f"G02 az/el {allsats.get('G02')}")
check(allsats.get("R08", {}).get("el") == 50 and allsats.get("R08", {}).get("az") == 280, f"R08 (GLONASS 72) az/el {allsats.get('R08')}")
check(allsats.get("E21", {}).get("el") == 70, f"E21 az/el {allsats.get('E21')}")
check(allsats.get("C06", {}).get("az") == 200, f"C06 az/el {allsats.get('C06')}")

# Message stats
types = {m["type"] for m in live["messages"]}
check({1077, 1087, 1117, 1127, 1097, 1005, 1033} <= types, f"message types {sorted(types)}")

if fails:
    print("\n".join("FAIL " + f for f in fails))
    sys.exit(1)
print(f"receiver stream check passed: {len(got)} frames byte-identical, {len(lines)} lines, "
      f"{len(sats)} satellites match pyrtcm, position and sky view correct")
