/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_BYTE_RING_H
#define ESP32_NTRIP_BYTE_RING_H

/*
 * Plain byte FIFO with drop-oldest overflow. No locking and no RTOS
 * dependencies, so it can be unit tested on the host. Callers that share a
 * ring between tasks must provide their own lock.
 */

#include <stddef.h>
#include <stdint.h>

typedef struct byte_ring {
    uint8_t *buf;
    size_t cap;
    size_t head;    // index of oldest byte
    size_t len;     // bytes stored
    uint32_t dropped_total; // bytes discarded because the ring was full
} byte_ring_t;

void byte_ring_init(byte_ring_t *ring, uint8_t *storage, size_t cap);
void byte_ring_clear(byte_ring_t *ring);

static inline size_t byte_ring_len(const byte_ring_t *ring) { return ring->len; }
static inline size_t byte_ring_free(const byte_ring_t *ring) { return ring->cap - ring->len; }

/* Append data, discarding the oldest bytes if needed. Returns bytes discarded. */
size_t byte_ring_write(byte_ring_t *ring, const void *data, size_t n);

/* Copy and remove up to max bytes from the front. Returns bytes copied. */
size_t byte_ring_read(byte_ring_t *ring, void *out, size_t max);

#endif //ESP32_NTRIP_BYTE_RING_H
