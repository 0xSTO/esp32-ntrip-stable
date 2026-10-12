/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "local_caster_proto.h"

void bcast_init(bcast_ring_t *r, uint8_t *storage, uint32_t cap_pow2) {
    r->buf = storage;
    r->cap = cap_pow2;
    r->head = 0;
}

void bcast_write(bcast_ring_t *r, const uint8_t *data, uint32_t n) {
    if (n > r->cap) {  // keep only the newest cap bytes (never happens for RTCM frames)
        data += n - r->cap;
        r->head += n - r->cap;
        n = r->cap;
    }
    uint32_t pos = r->head & (r->cap - 1);
    uint32_t first = r->cap - pos < n ? r->cap - pos : n;
    memcpy(r->buf + pos, data, first);
    memcpy(r->buf, data + first, n - first);
    r->head += n;
}

size_t bcast_peek(const bcast_ring_t *r, uint32_t cursor, uint8_t *out, size_t max, bool *overrun) {
    uint32_t avail = r->head - cursor;
    *overrun = avail > r->cap;
    if (*overrun || avail == 0 || max == 0) return 0;
    uint32_t n = avail < max ? avail : (uint32_t) max;
    uint32_t pos = cursor & (r->cap - 1);
    uint32_t first = r->cap - pos < n ? r->cap - pos : n;
    memcpy(out, r->buf + pos, first);
    memcpy(out + first, r->buf, n - first);
    return n;
}

size_t lcp_chunk_frame(uint8_t *buf, size_t n, size_t *end) {
    char hdr[LCP_CHUNK_HEAD + 1];
    int h = snprintf(hdr, sizeof(hdr), "%X\r\n", (unsigned) n);
    size_t start = LCP_CHUNK_HEAD - (size_t) h;
    memcpy(buf + start, hdr, (size_t) h);
    buf[LCP_CHUNK_HEAD + n] = '\r';
    buf[LCP_CHUNK_HEAD + n + 1] = '\n';
    *end = LCP_CHUNK_HEAD + n + 2;
    return start;
}

/* ------------------------------------------------------------ NTRIP request */

// Find the value of header `name` (case-insensitive) in the header block
// [hdr, end). Returns a pointer to the value (leading spaces skipped) and its
// length without trailing whitespace, or NULL.
static const char *find_header(const char *hdr, const char *end, const char *name, size_t *vlen) {
    size_t nl = strlen(name);
    const char *line = hdr;
    while (line < end) {
        const char *eol = memchr(line, '\n', (size_t) (end - line));
        if (eol == NULL) eol = end;
        if ((size_t) (eol - line) > nl && strncasecmp(line, name, nl) == 0 && line[nl] == ':') {
            const char *v = line + nl + 1;
            const char *ve = eol;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && isspace((unsigned char) ve[-1])) ve--;
            *vlen = (size_t) (ve - v);
            return v;
        }
        line = eol + 1;
    }
    return NULL;
}

void lcp_parse(const char *buf, size_t len, const char *mount_cfg, const char *auth_expected, lcp_request_t *out) {
    memset(out, 0, sizeof(*out));
    out->kind = LCP_REQ_INCOMPLETE;

    // End of headers: blank line (CRLF CRLF, or bare LF LF from sloppy clients)
    const char *end = NULL;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && (buf[i + 1] == '\n' || (buf[i + 1] == '\r' && i + 2 < len && buf[i + 2] == '\n'))) {
            end = buf + i + 1;
            break;
        }
    }
    if (end == NULL) {
        // A request line that already isn't a GET can be refused straight away
        if (len >= 4 && strncmp(buf, "GET ", 4) != 0) out->kind = LCP_REQ_BAD;
        return;
    }

    // Request line: GET /mount[ HTTP/1.x]
    const char *eol = memchr(buf, '\n', (size_t) (end - buf));
    if (eol == NULL || eol - buf < 5 || strncmp(buf, "GET /", 5) != 0) {
        out->kind = LCP_REQ_BAD;
        return;
    }
    const char *p = buf + 5, *pe = p;
    while (pe < eol && *pe != ' ' && *pe != '\r' && *pe != '?') pe++;
    size_t mlen = (size_t) (pe - p);
    if (mlen > LCP_MOUNT_MAX) mlen = LCP_MOUNT_MAX;
    memcpy(out->mount, p, mlen);
    out->mount[mlen] = '\0';

    const char *hdr = eol + 1;
    size_t vlen;
    const char *v = find_header(hdr, end, "Ntrip-Version", &vlen);
    out->v2 = v != NULL && vlen >= 9 && strncasecmp(v, "Ntrip/2.0", 9) == 0;

    if (mlen == 0 || strcasecmp(out->mount, mount_cfg) != 0 || (size_t) (pe - p) > LCP_MOUNT_MAX) {
        out->kind = LCP_REQ_SOURCETABLE;
        return;
    }

    if (auth_expected != NULL && auth_expected[0] != '\0') {
        v = find_header(hdr, end, "Authorization", &vlen);
        size_t elen = strlen(auth_expected);
        // "Basic" is case-insensitive; the credentials are compared exactly
        bool ok = v != NULL && vlen == elen && elen > 6 &&
                  strncasecmp(v, auth_expected, 6) == 0 && memcmp(v + 6, auth_expected + 6, elen - 6) == 0;
        if (!ok) {
            out->kind = LCP_REQ_UNAUTHORIZED;
            return;
        }
    }
    out->kind = LCP_REQ_STREAM;
}

static int fit(int n, size_t size) {
    return (n < 0 || (size_t) n >= size) ? -1 : n;
}

int lcp_response_stream(bool v2, char *out, size_t size) {
    if (v2) {
        // NTRIP 2.0 sends the data stream with HTTP chunked transfer encoding
        return fit(snprintf(out, size, "HTTP/1.1 200 OK\r\n"
                                       "Ntrip-Version: Ntrip/2.0\r\n"
                                       "Server: NTRIP esp32-ntrip-stable\r\n"
                                       "Content-Type: gnss/data\r\n"
                                       "Cache-Control: no-store, no-cache, max-age=0\r\n"
                                       "Transfer-Encoding: chunked\r\n"
                                       "Connection: close\r\n\r\n"), size);
    }
    return fit(snprintf(out, size, "ICY 200 OK\r\n\r\n"), size);
}

int lcp_response_unauthorized(bool v2, const char *mount, char *out, size_t size) {
    return fit(snprintf(out, size, "HTTP/1.%c 401 Unauthorized\r\n"
                                   "%s"
                                   "Server: NTRIP esp32-ntrip-stable\r\n"
                                   "WWW-Authenticate: Basic realm=\"/%s\"\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n",
                        v2 ? '1' : '0', v2 ? "Ntrip-Version: Ntrip/2.0\r\n" : "", mount), size);
}

int lcp_response_busy(char *out, size_t size) {
    return fit(snprintf(out, size, "HTTP/1.0 503 Service Unavailable\r\n"
                                   "Server: NTRIP esp32-ntrip-stable\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n"), size);
}

int lcp_response_bad(char *out, size_t size) {
    return fit(snprintf(out, size, "HTTP/1.0 400 Bad Request\r\n"
                                   "Server: NTRIP esp32-ntrip-stable\r\n"
                                   "Content-Length: 0\r\n"
                                   "Connection: close\r\n\r\n"), size);
}

int lcp_response_sourcetable(bool v2, const char *mount, bool auth, double lat, double lon,
                             const char *agent, char *out, size_t size) {
    char body[320];
    int blen = snprintf(body, sizeof(body),
                        "STR;%s;%s;RTCM 3.3;1005(10),1033(30),1077(1),1087(1),1097(1),1117(1),1127(1);2;"
                        "GPS+GLO+GAL+BDS+QZS;local;;%.2f;%.2f;0;0;%s;none;%c;N;0;\r\n"
                        "ENDSOURCETABLE\r\n",
                        mount, mount, lat, lon, agent, auth ? 'B' : 'N');
    if (blen < 0 || (size_t) blen >= sizeof(body)) return -1;
    int n;
    if (v2) {
        n = snprintf(out, size, "HTTP/1.1 200 OK\r\n"
                                "Ntrip-Version: Ntrip/2.0\r\n"
                                "Server: NTRIP esp32-ntrip-stable\r\n"
                                "Content-Type: gnss/sourcetable\r\n"
                                "Content-Length: %d\r\n"
                                "Connection: close\r\n\r\n%s", blen, body);
    } else {
        n = snprintf(out, size, "SOURCETABLE 200 OK\r\n"
                                "Server: NTRIP esp32-ntrip-stable\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: %d\r\n\r\n%s", blen, body);
    }
    return fit(n, size);
}
