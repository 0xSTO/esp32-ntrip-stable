/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include <string.h>

#include "rtcm_framer.h"

uint32_t rtcm_crc24q(const uint8_t *data, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t) data[i] << 16;
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= 0x1864CFB;
        }
    }
    return crc & 0xFFFFFF;
}

void rtcm_framer_init(rtcm_framer_t *f, rtcm_frame_cb_t on_frame, rtcm_line_cb_t on_line, void *ctx) {
    memset(f, 0, sizeof(*f));
    f->on_frame = on_frame;
    f->on_line = on_line;
    f->ctx = ctx;
}

typedef enum { NEED_MORE, EMIT_FRAME, EMIT_LINE, REJECT } verdict_t;

static bool is_start(uint8_t c) {
    return c == 0xD3 || c == '$' || c == '#';
}

static bool printable(uint8_t c) {
    return (c >= 0x20 && c <= 0x7E) || c == '\r' || c == '\t';
}

/* Classify the bytes held in buf. Only the newest byte is new since last call. */
static verdict_t check(const rtcm_framer_t *f) {
    const uint8_t *b = f->buf;
    size_t n = f->len;

    if (b[0] == 0xD3) {
        if (n < 3) return NEED_MORE;
        if ((b[1] & 0xFC) != 0) return REJECT;  // 6 reserved bits must be zero
        size_t total = 3 + ((((size_t) b[1] & 0x03) << 8) | b[2]) + 3;
        if (n < total) return NEED_MORE;
        uint32_t crc = ((uint32_t) b[total - 3] << 16) | ((uint32_t) b[total - 2] << 8) | b[total - 1];
        return rtcm_crc24q(b, total - 3) == crc ? EMIT_FRAME : REJECT;
    }

    // '$' or '#': a text line
    uint8_t c = b[n - 1];
    if (n > 1 && c == '\n') return EMIT_LINE;
    if (!printable(c) || n >= RTCM_LINE_MAX) return REJECT;
    // A '$' never appears inside an NMEA sentence or a receiver reply: if one
    // shows up, the bytes before it were a fragment (e.g. corrupted data that
    // happened to contain '$'). Reject so the line restarts at the new '$'.
    if (n > 1 && c == '$') return REJECT;
    return NEED_MORE;
}

/*
 * Process bytes one at a time. When the held bytes turn out not to be a valid
 * frame/line, only the first byte is discarded and the rest are replayed: a
 * genuine frame may have started inside them. Replay is iterative (a pending
 * queue), never recursive, so stack use stays fixed.
 */
void rtcm_framer_feed(rtcm_framer_t *f, const uint8_t *data, size_t len) {
    // Replay queue: bytes taken back out of buf after a reject.
    uint8_t *replay = f->replay;
    size_t replay_len = 0, replay_pos = 0;
    size_t i = 0;

    while (replay_pos < replay_len || i < len) {
        uint8_t c = replay_pos < replay_len ? replay[replay_pos++] : data[i++];

        if (f->len == 0 && !is_start(c)) {
            f->garbage_bytes++;
            continue;
        }
        f->buf[f->len++] = c;

        verdict_t v = check(f);
        if (v == NEED_MORE) continue;

        if (v == EMIT_FRAME) {
            f->frames++;
            if (f->on_frame) f->on_frame(f->ctx, f->buf, f->len);
            f->len = 0;
        } else if (v == EMIT_LINE) {
            size_t n = f->len;
            while (n > 0 && (f->buf[n - 1] == '\n' || f->buf[n - 1] == '\r')) n--;
            f->buf[n] = '\0';
            f->lines++;
            if (f->on_line) f->on_line(f->ctx, (const char *) f->buf, n);
            f->len = 0;
        } else {
            // Drop the first held byte; replay the rest ahead of any unread replay bytes.
            f->garbage_bytes++;
            size_t back = f->len - 1;
            size_t unread = replay_len - replay_pos;
            // buf holds at most a frame, and every byte in it came from either
            // the replay queue or data, so back + unread always fits.
            memmove(replay + back, replay + replay_pos, unread);
            memcpy(replay, f->buf + 1, back);
            replay_len = back + unread;
            replay_pos = 0;
            f->len = 0;
        }
    }
}

void rtcm_framer_flush(rtcm_framer_t *f) {
    // Treat what is held as final: drop its first byte and re-scan the rest,
    // repeating until nothing incomplete is left. Complete items found in the
    // held bytes are emitted on the way.
    int guard = 0;
    while (f->len > 0 && guard++ < (int) sizeof(f->buf)) {
        size_t n = f->len - 1;
        memcpy(f->flush, f->buf + 1, n);
        f->garbage_bytes++;
        f->len = 0;
        rtcm_framer_feed(f, f->flush, n);
    }
}
