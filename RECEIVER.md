# Receiver page: live view and console

Open the device's web page and click **Receiver: live view & console** (or go to `/receiver.html`).

## Live view

Built entirely from what the GNSS receiver already sends, so it works without internet access:

| Shown | Source |
|---|---|
| Satellites, signals, mean C/N0, signal level per band, constellation table | RTCM MSM4–7 (1077/1087/1097/1117/1127…) |
| Announced base position (lat/lon/height, ECEF) | RTCM 1005/1006 |
| Receiver / antenna description | RTCM 1033 (1007/1008) |
| Sky view (satellite positions) | NMEA GSV from the receiver (click **Enable sky view** once) |
| RTCM message list with intervals | every RTCM frame |

Things onocoy computes on its side (code/phase RMS, latency, position offset) need a reference
network and aren't available on the device.

## Console

Type any receiver command (e.g. `VERSION`) and the receiver's reply appears underneath, as in UPrecise.
NMEA sentences are kept out of the console (they feed the sky view).

- **Apply base setup…** sends the full UM980 base configuration: fixed coordinates (recommended, e.g.
  from AUSPOS; "Use the current announced position" pre-fills them) or a 60 s survey-in, the RTCM
  messages, optional sky-view data, optional `CONFIG SIGNALGROUP 2` (only needed after a factory reset),
  then `SAVECONFIG`. The dialog shows every command before sending. Each command must be answered with
  `OK` by the receiver; if one fails or gets no answer the sequence stops **before** `SAVECONFIG`.
- **Heights:** enter the **ellipsoidal** height (AUSPOS "Ellipsoidal height"), not AHD / sea level. The UM980 manual
  doesn't say which height `MODE BASE lat lon height` expects (it says to set `CONFIG UNDULATION` first, which
  suggests sea level; users report ellipsoidal). To avoid the question the page converts the coordinates to
  ECEF and sends `MODE BASE X Y Z`, which has only one meaning. After applying, the page waits for the next
  RTCM 1005 and reports whether the receiver announces the position you entered (within 1 cm) or how far off it is.
- **Factory reset** asks for confirmation (the device also refuses `FRESET` without it).
- The **Port to ESP32** selector is the receiver port wired to the ESP32 (COM1 on this base).

## Record RTCM log

Like RTKLIB STRSVR's file output: choose a duration (15 min to 24 h) and press **Start recording**. The browser
downloads `base_<UTC date>_<time>Z.rtcm3`, which the ESP32 keeps streaming into until the time is up or you press
**Stop and finish file**. The file holds exactly the RTCM the casters receive (from the moment you start).
The recording runs in its own task, so the rest of the web interface and both casters keep working.
One recording at a time; a 12 KB buffer (about 6 s of data) rides out short WiFi hiccups (anything lost is shown as "dropped").
Keep the computer awake and on the WiFi until it finishes. Convert with RTKLIB RTKCONV to RINEX, e.g. for AUSPOS.

## Casters get RTCM only

The ESP32 now splits the receiver's output: complete RTCM frames with a valid checksum go to the
casters; NMEA and command replies stay on the device. Garbage and corrupted frames are dropped.
This is the **Casters get: RTCM only** switch in the UART settings (on by default). Turn it off to
forward the raw stream unchanged.

The `$PESP` status messages the firmware used to send to the receiver are now only sent when
**Log forward** is on (they only produced "PARSING FAILD" replies).

## API (for scripts)

- `GET /receiver/live` – live view JSON
- `GET /receiver/console?since=<seq>` – console lines after `seq`
- `POST /receiver/command` – body `{"cmd": "VERSION"}`; `FRESET` needs `"confirm": true`
- `GET /receiver/record?minutes=N` – streams an `.rtcm3` file for N minutes (1–1440)
- `POST /receiver/record/stop` – ends the running recording (the download completes normally)

These use the same admin authentication setting as the rest of the web page. With authentication off,
anyone on the network can send receiver commands; set a username/password under **Admin** if the
device is on a shared network.

## Caster buffer fix (dropped data)

The first real-board run showed "dropped" climbing on both casters (about 1–2 % of the data). The UART task gave up
after waiting 5 ms for a caster's buffer lock, and with RTCM now handed over frame by frame that happened whenever
the caster task was pre-empted by WiFi while holding the lock. The buffer now uses a spinlock around a single
memcpy (microseconds), so the UART side never has to give up; "dropped" now only counts a genuinely slow network
(the 16 KB buffer full).

The Receiver page also stops polling while its browser tab is hidden.

## v4: real-board fixes (crash on Save, memory)

- **Crash when saving settings (inherited from upstream).** `config_restart()` created its restart task with
  priority 100, above FreeRTOS's limit of 25, so ESP-IDF 5 hit an assert and the board rebooted with
  "Reset reason: PANIC" (and wrote a core dump) every time **Submit** was pressed. Now priority 20.
- **Memory.** On the real board free heap was much tighter than in QEMU (WiFi buffers): recording failed with
  "Not enough memory to record" and a caster stalled right after the page loaded. Static use was cut: satellite
  table 160 → 96, console 48 → 32 lines, live-JSON buffer 16 → 10 KB (worst case 8.4 KB, host-tested), no
  snapshot copy. The recording buffer (12 KB) is reserved at boot instead of a 32 KB malloc on demand.
- The Receiver page shows device memory (free / lowest / largest block) in its footer, and says clearly when
  a recording fails to start.

## Built-in caster (rovers on the same network)

Rovers on your WiFi (or on the ESP32's own hotspot) can take corrections straight from the base, without rtk2go
or onocoy. Switch on **Built-in caster** on the settings page, pick a mountpoint (default `BASE`) and optionally a
username and password, and save. In the rover app (e.g. SW Maps → NTRIP client) enter:

| Setting | Value |
|---|---|
| Host | the base's IP address (shown on the card, e.g. `192.168.0.180`; reserve it in your router) |
| Port | `2101` (or what you set) |
| Mountpoint | what you set, e.g. `BASE` |
| Username / password | only if you set them |

- Up to **2 rovers** at once; a third gets "busy" (503). NTRIP v1 and v2 (v2 uses chunked transfer as the
  standard requires). The mountpoint list (`GET /`) works without a password.
- Rovers get exactly what the casters get (RTCM only, with the filter on). Position (GGA) sent by the rover is ignored.
- A rover that stops reading for 10 s is disconnected; one that is merely slow skips ahead to fresh data. Neither
  affects the other rover or rtk2go/onocoy.
- Memory: nothing is used while it's off; about 7 KB while on, plus a 4 KB buffer and the network buffers while
  rovers are connected. New rovers are refused if free memory would fall below 16 KB, so rtk2go/onocoy always come
  first. Watch "lowest" in the Receiver page footer the first time you use it.
- The card shows connected rovers (address, time, data), how many were served and refused.

## Faster rtk2go recovery

rtk2go sometimes stops reading a connection for 20+ s while a fresh connection works straight away (seen in the
11 Oct device log: only rtk2go stalled, onocoy kept running). The firmware now gives up on a connection that has
accepted nothing for **10 s** (was 20 s) and reconnects, so these outages shrink from about 25 s to about 15 s.

## Tests

- `test/host/run_tests.sh` – framer, decoder and console log, cross-checked against pyrtcm on a real
  MSM7 recording (RTKLIB test data, BSD-2) plus synthetic Galileo/1005 frames; random chunking; fuzzing
- `test/qemu/run_qemu_test.py` – firmware in QEMU: casters receive only valid RTCM, live view and console
  endpoints, commands reach the UART
- `test/qemu/run_ui_test.py` – the page in headless Chromium against a fake UM980, plus the settings card
- `test/qemu/run_local_caster_test.py` – built-in caster in QEMU with real NTRIP clients: mountpoint list (v1/v2),
  wrong/missing password, garbage, silent connections, two rovers at once (v1 + chunked v2) with full-rate gap-free
  RTCM, third rover refused, a rover that stops reading is dropped without affecting the other rover or the casters
