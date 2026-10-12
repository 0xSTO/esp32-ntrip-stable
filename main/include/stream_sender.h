/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_STREAM_SENDER_H
#define ESP32_NTRIP_STREAM_SENDER_H

/*
 * Non-blocking sender for one caster connection. Pure logic, host tested.
 *
 * Data is pulled from the caller's buffer (normally a locked byte_ring) into
 * a small staging chunk and written with a non-blocking send. Short writes
 * keep the unsent tail in the staging chunk, so an RTCM frame is never cut.
 * If the socket accepts nothing for SENDER_STALL_MS while data is waiting,
 * the connection is reported as stalled; a warning flag is raised once at
 * SENDER_SLOW_MS.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENDER_CHUNK_SIZE 1436
#define SENDER_SLOW_MS 5000u
#define SENDER_STALL_MS 10000u

/* Return >0 bytes written, 0 if the socket would block, <0 on a fatal error. */
typedef int (*sender_write_fn)(void *ctx, const uint8_t *data, size_t len);
/* Copy up to max bytes of pending data into out; return bytes copied. */
typedef size_t (*sender_fill_fn)(void *ctx, uint8_t *out, size_t max);

typedef enum {
    SENDER_IDLE,     // nothing to send
    SENDER_OK,       // progress made, may have more
    SENDER_BLOCKED,  // socket full, still within tolerance
    SENDER_SLOW,     // socket full for SENDER_SLOW_MS (reported once per episode)
    SENDER_STALLED,  // socket full for SENDER_STALL_MS, give up on the connection
    SENDER_ERROR,    // write failed
} sender_result_t;

typedef struct stream_sender {
    uint8_t chunk[SENDER_CHUNK_SIZE];
    size_t off;
    size_t len;
    uint32_t last_progress_ms;
    bool slow_reported;
    uint32_t bytes_sent;
} stream_sender_t;

void stream_sender_reset(stream_sender_t *s, uint32_t now_ms);

static inline bool stream_sender_pending(const stream_sender_t *s) { return s->off < s->len; }

/*
 * Push as much data as the socket will take right now. Never blocks (as long
 * as write_fn doesn't). bytes_out (optional) receives the bytes written.
 */
sender_result_t stream_sender_pump(stream_sender_t *s, sender_fill_fn fill, sender_write_fn write,
        void *ctx, uint32_t now_ms, size_t *bytes_out);

#endif //ESP32_NTRIP_STREAM_SENDER_H
