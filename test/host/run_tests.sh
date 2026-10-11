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
