# Stability changes

This fork is based on [incarvr6/esp32-ntrip-DUO](https://github.com/incarvr6/esp32-ntrip-DUO) at commit `28ef71b`
(root `main/` tree, classic ESP32 such as the ESP32-WROOM-32U). Built and tested with ESP-IDF v5.4.2.

## Why the stream dropped (rtk2go "restarts")

rtk2go logs a "restart" every time a mount sends a new `SOURCE` login, so each one is either the firmware
dropping and re-opening the socket, or the ESP32 rebooting.

In upstream, `uart_task` posted every UART chunk to the shared default event loop, and each NTRIP server's
handler called a blocking `write()` on its socket from inside that loop (`ntrip_server.c:71`). With a 10 s
`SO_SNDTIMEO`, any WiFi hiccup that filled the 8 KB lwIP send buffer blocked the event loop for up to 10 s,
then `write()` failed and the socket was destroyed, causing a reconnect and an rtk2go restart. While blocked, the
other caster starved too, `uart_task` blocked on `esp_event_post`, the UART RX buffer overflowed (lost RTCM),
and WiFi events were delayed. One slow caster knocked out both.

## Changes

### Caster connections (`main/interface/ntrip_caster.c`)
- Both servers share one implementation; `ntrip_server.c` / `ntrip_server_2.c` are now thin config wrappers.
- Each caster has its own 16 KB ring buffer and its own task that owns the socket. The UART task only copies
  data into the rings (`uart_add_read_sink`); nothing on the UART or event-loop path touches a socket.
- Non-blocking sends (`MSG_DONTWAIT`). Partial writes are completed, so RTCM frames are never cut.
- A connection is dropped only if the caster accepts nothing for 20 s while data is waiting. While the link is
  merely slow, the ring drops its oldest data instead and logs `Network slow: ...`.
- TCP keepalive (idle 10 s, interval 5 s, 3 probes), detection of the caster closing the connection, and
  immediate disconnect when WiFi loses its IP.
- 10 s connect timeout (non-blocking connect), 10 s login timeout.
- Every disconnect is logged as `Disconnected from host:port/mount after N s: <reason>`.
- Reconnect back-off (`main/backoff.c`): 0 / 5 / 5 / 5 / 10 / 20 / 40 / 60 / 120 / 300 s. Failed logins and
  sessions shorter than 5 minutes move along the schedule; a session that lasted 5 minutes resets it to 5 s.
  Disconnects caused by WiFi losing its IP don't escalate it.

### WiFi (`main/wifi.c`)
- The reconnect task used `vTaskSuspend`/`vTaskResume` from event handlers. A resume that lands before the
  matching suspend is lost, which could leave WiFi disconnected for good. It now uses task notifications,
  which latch.
- WiFi power save off (`WIFI_PS_NONE`).
- Reboot after 15 minutes with no IP, unless a client is connected to the AP.

### System
- Default event loop task stack 2304 -> 4096 bytes, queue 16 -> 32 (`sdkconfig`).
- `uart_write` never blocks: if the TX buffer is full the message is dropped instead of stalling the caller
  (it is called from logging and event handlers).
- `log_vprintf`: fixed length handling (used `vsnprintf`'s wanted length, not what fit, and assumed colour codes
  were always present).
- Web server: fixed a use-after-free of the `If-None-Match` header in the ETag check.
- Progress watchdog (`main/health.c`): the UART, WiFi and caster tasks check in regularly. If one makes no
  progress for 3 minutes, the device records the cause in RTC RAM and reboots. The cause is shown at the next
  boot as `Restart cause: ...` (web log, and `$PESP,INIT,RESTART_CAUSE,...` on the UART) and in `/status`.
- sdkconfig migrated to ESP-IDF v5.4.2; deprecated API calls replaced.

### Web UI
- NTRIP server B now shows its own (`ntrip_server_2`) stream stats instead of server A's.
- Each server card shows its state, session length, login/failure counts, dropped bytes and the last
  disconnect reason. `/status` returns the same under `casters`.

## Tests

- `test/host/run_tests.sh`: host unit tests (gcc + ASan/UBSan) for the ring buffer, back-off and sender,
  including a real socket back-pressure test where the reader stops and the kernel buffers fill.
- `test/qemu/run_qemu_test.py`: end-to-end in Espressif QEMU with two fake casters. See `test/qemu/README.md`.
  QEMU's network (slirp) buffers without limit, so it can't reproduce TCP back-pressure; the host tests cover it.

## Notes

- Flashing a merged image at 0x0 wipes NVS: re-enter settings afterwards.
- Not yet soak-tested on a real board. Watch rtk2go "restarts" and the web log for `Disconnected ...` and
  `Network slow` lines.
- Builds for ESP32-S2/S3/C3 (the `esp32s3test` tree and the prebuilt upstream `.bin` files) are not reworked.
