/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_LOCAL_CASTER_PROTO_H
#define ESP32_NTRIP_LOCAL_CASTER_PROTO_H

/*
 * Pure logic for the built-in NTRIP caster (rovers on the same WiFi). No RTOS
 * or socket code, so it is unit tested on the host.
 *
 * bcast_ring: one writer (the UART task, whole RTCM frames) and several
 * readers (one per rover), each with its own cursor. The writer never waits
 * for readers; a reader that falls more than the ring size behind is told it
 * overran and restarts at the newest data. Capacity must be a power of two so
 * the 32-bit cursors can wrap freely. The caller provides the locking.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint32_t cap;      // power of two
    uint32_t head;     // total bytes ever written (wraps)
} bcast_ring_t;

void bcast_init(bcast_ring_t *r, uint8_t *storage, uint32_t cap_pow2);
void bcast_write(bcast_ring_t *r, const uint8_t *data, uint32_t n);

/*
 * Copy up to max bytes starting at cursor. Returns bytes copied. If the reader
 * fell behind by more than the capacity, *overrun is set, nothing is copied and
 * the caller should continue from r->head (a frame boundary when the receiver
 * module hands over whole RTCM frames, i.e. with the RTCM-only filter on;
 * rovers resync on the RTCM CRC otherwise).
 */
size_t bcast_peek(const bcast_ring_t *r, uint32_t cursor, uint8_t *out, size_t max, bool *overrun);

static inline uint32_t bcast_pending(const bcast_ring_t *r, uint32_t cursor) { return r->head - cursor; }

/*
 * HTTP chunked framing for NTRIP 2.0 data. The n payload bytes must already be
 * at buf[LCP_CHUNK_HEAD]; buf must have room for LCP_CHUNK_HEAD + n + 2 bytes.
 * Writes "<hex n>\r\n" just before the data and "\r\n" after it. Returns the
 * offset where the chunk starts; *end receives the offset just past it.
 */
#define LCP_CHUNK_HEAD 8
size_t lcp_chunk_frame(uint8_t *buf, size_t n, size_t *end);

/* ------------------------------------------------------------ NTRIP request */

#define LCP_MOUNT_MAX 32

typedef enum {
    LCP_REQ_INCOMPLETE,   // headers not finished yet
    LCP_REQ_BAD,          // not an NTRIP/HTTP GET request
    LCP_REQ_SOURCETABLE,  // GET / or an unknown mountpoint
    LCP_REQ_UNAUTHORIZED, // right mountpoint, missing/wrong credentials
    LCP_REQ_STREAM,       // right mountpoint, allowed
} lcp_req_kind_t;

typedef struct {
    lcp_req_kind_t kind;
    bool v2;                       // client sent "Ntrip-Version: Ntrip/2.0"
    char mount[LCP_MOUNT_MAX + 1]; // requested mountpoint (may be truncated)
} lcp_request_t;

/*
 * Parse a request from the bytes received so far (need not be NUL terminated).
 * auth_expected is the full expected Authorization value ("Basic dXNlcjpwYXNz"),
 * or NULL/"" when the caster is open.
 */
void lcp_parse(const char *buf, size_t len, const char *mount_cfg, const char *auth_expected, lcp_request_t *out);

/* Responses. Each returns the length written (always NUL terminated), or -1 if it did not fit. */
/* v2 replies announce "Transfer-Encoding: chunked": send data with lcp_chunk_frame(). */
int lcp_response_stream(bool v2, char *out, size_t size);
int lcp_response_unauthorized(bool v2, const char *mount, char *out, size_t size);
int lcp_response_busy(char *out, size_t size);
int lcp_response_bad(char *out, size_t size);
int lcp_response_sourcetable(bool v2, const char *mount, bool auth, double lat, double lon,
                             const char *agent, char *out, size_t size);

#endif // ESP32_NTRIP_LOCAL_CASTER_PROTO_H
