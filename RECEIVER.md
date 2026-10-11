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

These use the same admin authentication setting as the rest of the web page. With authentication off,
anyone on the network can send receiver commands; set a username/password under **Admin** if the
device is on a shared network.

## Tests

- `test/host/run_tests.sh` – framer, decoder and console log, cross-checked against pyrtcm on a real
  MSM7 recording (RTKLIB test data, BSD-2) plus synthetic Galileo/1005 frames; random chunking; fuzzing
- `test/qemu/run_qemu_test.py` – firmware in QEMU: casters receive only valid RTCM, live view and console
  endpoints, commands reach the UART
- `test/qemu/run_ui_test.py` – the page in headless Chromium against a fake UM980
