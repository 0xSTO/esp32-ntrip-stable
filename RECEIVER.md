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
- **Factory reset** asks for confirmation (the device also refuses `FRESET` without it).
- The **Port to ESP32** selector is the receiver port wired to the ESP32 (COM1 on this base).

## Record RTCM log

Like RTKLIB STRSVR's file output: choose a duration (15 min to 24 h) and press **Start recording**. The browser
downloads `base_<UTC date>_<time>Z.rtcm3`, which the ESP32 keeps streaming into until the time is up or you press
**Stop and finish file**. The file holds exactly the RTCM the casters receive (from the moment you start).
The recording runs in its own task, so the rest of the web interface and both casters keep working.
One recording at a time; a 32 KB buffer rides out short WiFi hiccups (anything lost is shown as "dropped").
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

## Tests

- `test/host/run_tests.sh` – framer, decoder and console log, cross-checked against pyrtcm on a real
  MSM7 recording (RTKLIB test data, BSD-2) plus synthetic Galileo/1005 frames; random chunking; fuzzing
- `test/qemu/run_qemu_test.py` – firmware in QEMU: casters receive only valid RTCM, live view and console
  endpoints, commands reach the UART
- `test/qemu/run_ui_test.py` – the page in headless Chromium against a fake UM980
