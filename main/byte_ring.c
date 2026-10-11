/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include <string.h>
#include "byte_ring.h"

void byte_ring_init(byte_ring_t *ring, uint8_t *storage, size_t cap) {
    ring->buf = storage;
    ring->cap = cap;
    ring->head = 0;
    ring->len = 0;
    ring->dropped_total = 0;
}

void byte_ring_clear(byte_ring_t *ring) {
    ring->head = 0;
    ring->len = 0;
}

static void drop_front(byte_ring_t *ring, size_t n) {
    if (n > ring->len) n = ring->len;
    ring->head = (ring->head + n) % ring->cap;
    ring->len -= n;
}

size_t byte_ring_write(byte_ring_t *ring, const void *data, size_t n) {
    if (ring->cap == 0 || n == 0) return 0;

    const uint8_t *src = data;
    size_t dropped = 0;

    // Larger than the whole ring: only the newest cap bytes can survive
    if (n > ring->cap) {
        dropped += n - ring->cap;
        src += n - ring->cap;
        n = ring->cap;
    }

    size_t free_space = ring->cap - ring->len;
    if (n > free_space) {
        size_t evict = n - free_space;
        drop_front(ring, evict);
        dropped += evict;
    }

    size_t tail = (ring->head + ring->len) % ring->cap;
    size_t first = ring->cap - tail;
    if (first > n) first = n;
    memcpy(ring->buf + tail, src, first);
    if (n > first) memcpy(ring->buf, src + first, n - first);
    ring->len += n;

    ring->dropped_total += dropped;
    return dropped;
}

size_t byte_ring_read(byte_ring_t *ring, void *out, size_t max) {
    size_t n = ring->len < max ? ring->len : max;
    if (n == 0) return 0;

    uint8_t *dst = out;
    size_t first = ring->cap - ring->head;
    if (first > n) first = n;
    memcpy(dst, ring->buf + ring->head, first);
    if (n > first) memcpy(dst + first, ring->buf, n - first);

    drop_front(ring, n);
    return n;
}
