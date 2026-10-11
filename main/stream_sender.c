/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include "stream_sender.h"

void stream_sender_reset(stream_sender_t *s, uint32_t now_ms) {
    s->off = 0;
    s->len = 0;
    s->last_progress_ms = now_ms;
    s->slow_reported = false;
    s->bytes_sent = 0;
}

sender_result_t stream_sender_pump(stream_sender_t *s, sender_fill_fn fill, sender_write_fn write,
        void *ctx, uint32_t now_ms, size_t *bytes_out) {
    size_t total = 0;
    sender_result_t result = SENDER_IDLE;

    while (true) {
        if (s->off >= s->len) {
            s->off = 0;
            s->len = fill(ctx, s->chunk, sizeof(s->chunk));
            if (s->len == 0) {
                // Drained: nothing is waiting, so the link is not stalled
                s->last_progress_ms = now_ms;
                s->slow_reported = false;
                break;
            }
        }

        int ret = write(ctx, s->chunk + s->off, s->len - s->off);
        if (ret < 0) {
            result = SENDER_ERROR;
            break;
        }
        if (ret == 0) {
            uint32_t waited = now_ms - s->last_progress_ms;
            if (waited >= SENDER_STALL_MS) {
                result = SENDER_STALLED;
            } else if (waited >= SENDER_SLOW_MS && !s->slow_reported) {
                s->slow_reported = true;
                result = SENDER_SLOW;
            } else {
                result = SENDER_BLOCKED;
            }
            break;
        }

        if ((size_t) ret > s->len - s->off) ret = (int) (s->len - s->off); // defensive
        s->off += ret;
        total += ret;
        s->bytes_sent += ret;
        s->last_progress_ms = now_ms;
        s->slow_reported = false;
        result = SENDER_OK;
    }

    if (result == SENDER_IDLE && total > 0) result = SENDER_OK;
    if (bytes_out) *bytes_out = total;
    return result;
}
