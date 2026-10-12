#!/bin/sh
# Host unit tests for byte_ring, backoff and stream_sender (no ESP-IDF needed).
set -e
cd "$(dirname "$0")"
ROOT=../..
OUT=${TMPDIR:-/tmp}/esp32_ntrip_host_tests
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$ROOT/main/include" \
    test_main.c "$ROOT/main/byte_ring.c" "$ROOT/main/backoff.c" "$ROOT/main/stream_sender.c" \
    -o "$OUT"
"$OUT"

# Built-in caster: broadcast ring and NTRIP request parsing
LC=${TMPDIR:-/tmp}/esp32_ntrip_local_caster_tests
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$ROOT/main/include" \
    local_caster_test.c "$ROOT/main/local_caster_proto.c" \
    -o "$LC"
"$LC"

# Receiver framer / GNSS monitor / console log
RX=${TMPDIR:-/tmp}/esp32_ntrip_receiver_tests
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$ROOT/main/include" \
    receiver_driver.c "$ROOT/main/rtcm_framer.c" "$ROOT/main/gnss_monitor.c" "$ROOT/main/line_log.c" \
    -lm -o "$RX"
"$RX"
for seed in 1 2 3 4 5; do
    D=$(mktemp -d)
    "$RX" stream data/receiver_stream.bin "$D" "$seed" >/dev/null
    python3 check_receiver.py "$D"
    rm -rf "$D"
done
