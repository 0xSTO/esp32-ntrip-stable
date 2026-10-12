/*
 * Host unit tests for the built-in caster's pure logic (local_caster_proto.c):
 * the broadcast ring with per-rover cursors, and NTRIP request parsing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "local_caster_proto.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void parse(const char *req, const char *auth, lcp_request_t *r) {
    lcp_parse(req, strlen(req), "STON", auth, r);
}

static void test_requests(void) {
    lcp_request_t r;
    const char *auth = "Basic dXNlcjpwYXNz";  // user:pass

    parse("GET /STON HTTP/1.0\r\nUser-Agent: NTRIP x\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_INCOMPLETE);

    parse("GET /STON HTTP/1.0\r\nUser-Agent: NTRIP x\r\n\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_STREAM && !r.v2 && strcmp(r.mount, "STON") == 0);

    parse("GET /ston HTTP/1.1\r\nNtrip-Version: Ntrip/2.0\r\n\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_STREAM && r.v2);

    parse("GET /STON HTTP/1.1\r\nntrip-version:Ntrip/2.0\r\n\r\n", "", &r);
    CHECK(r.kind == LCP_REQ_STREAM && r.v2);

    parse("GET / HTTP/1.0\r\n\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_SOURCETABLE);

    parse("GET /OTHER HTTP/1.0\r\n\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_SOURCETABLE && strcmp(r.mount, "OTHER") == 0);

    parse("GET /STONX HTTP/1.0\r\n\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_SOURCETABLE);

    // Bare LF line endings and no HTTP version
    parse("GET /STON\n\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_STREAM);

    // Auth
    parse("GET /STON HTTP/1.0\r\n\r\n", auth, &r);
    CHECK(r.kind == LCP_REQ_UNAUTHORIZED);
    parse("GET /STON HTTP/1.0\r\nAuthorization: Basic dXNlcjpwYXNz\r\n\r\n", auth, &r);
    CHECK(r.kind == LCP_REQ_STREAM);
    parse("GET /STON HTTP/1.0\r\nauthorization: basic dXNlcjpwYXNz  \r\n\r\n", auth, &r);
    CHECK(r.kind == LCP_REQ_STREAM);
    parse("GET /STON HTTP/1.0\r\nAuthorization: Basic dXNlcjpwYXNZ\r\n\r\n", auth, &r);
    CHECK(r.kind == LCP_REQ_UNAUTHORIZED);
    parse("GET /STON HTTP/1.0\r\nAuthorization: Basic dXNlcjpwYXN\r\n\r\n", auth, &r);
    CHECK(r.kind == LCP_REQ_UNAUTHORIZED);
    parse("GET /STON HTTP/1.0\r\nX-Authorization: Basic dXNlcjpwYXNz\r\n\r\n", auth, &r);
    CHECK(r.kind == LCP_REQ_UNAUTHORIZED);
    // Sourcetable needs no credentials
    parse("GET / HTTP/1.0\r\n\r\n", auth, &r);
    CHECK(r.kind == LCP_REQ_SOURCETABLE);

    // Not a GET
    parse("SOURCE pass /STON\r\n\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_BAD);
    parse("POST /STON HTTP/1.1\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_BAD);
    parse("GET", NULL, &r);
    CHECK(r.kind == LCP_REQ_INCOMPLETE);

    // Over-long mountpoint is never a match
    parse("GET /STONSTONSTONSTONSTONSTONSTONSTONSTON HTTP/1.0\r\n\r\n", NULL, &r);
    CHECK(r.kind == LCP_REQ_SOURCETABLE && strlen(r.mount) == LCP_MOUNT_MAX);

    // Random garbage never crashes (ASan/UBSan)
    srand(7);
    char buf[300];
    for (int i = 0; i < 20000; i++) {
        size_t n = (size_t) (rand() % (int) sizeof(buf));
        for (size_t k = 0; k < n; k++) buf[k] = "GET /STON\r\n: AuthorizationBasic"[rand() % 31];
        if (i % 3 == 0 && n > 5) memcpy(buf, "GET /", 5);
        lcp_parse(buf, n, "STON", auth, &r);
        CHECK(r.kind >= LCP_REQ_INCOMPLETE && r.kind <= LCP_REQ_STREAM);
    }
}

static void test_responses(void) {
    char out[700];
    CHECK(lcp_response_stream(false, out, sizeof(out)) > 0 && strcmp(out, "ICY 200 OK\r\n\r\n") == 0);
    CHECK(lcp_response_stream(true, out, sizeof(out)) > 0 && strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0 &&
          strstr(out, "Ntrip-Version: Ntrip/2.0\r\n") && strstr(out, "Transfer-Encoding: chunked\r\n") &&
          strstr(out, "\r\n\r\n"));
    CHECK(lcp_response_unauthorized(false, "STON", out, sizeof(out)) > 0 && strstr(out, "401") &&
          strstr(out, "realm=\"/STON\""));
    CHECK(lcp_response_busy(out, sizeof(out)) > 0 && strstr(out, "503"));
    CHECK(lcp_response_bad(out, sizeof(out)) > 0 && strstr(out, "400"));

    int n = lcp_response_sourcetable(false, "STON", true, -38.309, 146.4, "esp32-ntrip-stable", out, sizeof(out));
    CHECK(n > 0 && strncmp(out, "SOURCETABLE 200 OK\r\n", 20) == 0);
    CHECK(strstr(out, "STR;STON;STON;RTCM 3.3;") && strstr(out, ";-38.31;146.40;") && strstr(out, ";B;N;0;\r\n"));
    // Content-Length matches the body
    const char *body = strstr(out, "\r\n\r\n") + 4;
    int cl = atoi(strstr(out, "Content-Length: ") + 16);
    CHECK(cl == (int) strlen(body) && strstr(body, "ENDSOURCETABLE\r\n"));
    n = lcp_response_sourcetable(true, "STON", false, 0, 0, "x", out, sizeof(out));
    CHECK(n > 0 && strstr(out, "Content-Type: gnss/sourcetable") && strstr(out, ";N;N;0;\r\n"));
    CHECK(lcp_response_sourcetable(false, "STON", false, 0, 0, "x", out, 50) == -1);
}

/* Reference model: every reader must receive exactly the bytes written after
 * it joined, in order, unless it overran (then it resumes at head). */
static void test_bcast(void) {
    enum { CAP = 64 };
    uint8_t storage[CAP];
    bcast_ring_t r;
    bcast_init(&r, storage, CAP);

    uint8_t out[CAP];
    bool ov;
    CHECK(bcast_peek(&r, 0, out, sizeof(out), &ov) == 0 && !ov);

    bcast_write(&r, (const uint8_t *) "hello", 5);
    CHECK(bcast_peek(&r, 0, out, 3, &ov) == 3 && !ov && memcmp(out, "hel", 3) == 0);
    CHECK(bcast_peek(&r, 3, out, 10, &ov) == 2 && memcmp(out, "lo", 2) == 0);
    CHECK(bcast_pending(&r, 0) == 5);

    // Random test with 2 readers at different speeds, cursors starting near
    // UINT32_MAX to exercise wrap-around.
    bcast_init(&r, storage, CAP);
    r.head = 0xFFFFFF00u;
    uint32_t cur[2] = {r.head, r.head};
    uint8_t *log = malloc(1 << 20);
    uint32_t logn = 0, base = r.head;
    uint32_t expect[2] = {0, 0};  // index into log
    int overruns = 0;
    srand(3);
    for (int step = 0; step < 50000; step++) {
        // writer: a "frame" of 1..40 bytes
        uint32_t n = 1 + (uint32_t) (rand() % 40);
        uint8_t fr[40];
        for (uint32_t k = 0; k < n; k++) fr[k] = (uint8_t) rand();
        bcast_write(&r, fr, n);
        memcpy(log + logn, fr, n);
        logn += n;
        for (int i = 0; i < 2; i++) {
            if (rand() % (i == 0 ? 2 : 5) != 0) continue;  // reader 1 is slower
            uint32_t want = 1 + (uint32_t) (rand() % CAP);
            size_t got = bcast_peek(&r, cur[i], out, want, &ov);
            if (ov) {
                overruns++;
                cur[i] = r.head;
                expect[i] = r.head - base;
                continue;
            }
            CHECK(got <= want && got <= bcast_pending(&r, cur[i]));
            CHECK(memcmp(out, log + expect[i], got) == 0);
            // reader may "send" fewer bytes than it copied
            size_t sent = got ? (size_t) (rand() % (int) (got + 1)) : 0;
            cur[i] += (uint32_t) sent;
            expect[i] += (uint32_t) sent;
        }
        if (logn > (1 << 20) - 64) break;
    }
    CHECK(overruns > 0);  // the slow reader must have overrun at least once
    free(log);
}

static void test_chunks(void) {
    uint8_t buf[LCP_CHUNK_HEAD + 600 + 2];
    size_t sizes[] = {1, 15, 16, 255, 256, 512, 600};
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        size_t n = sizes[k], end;
        memset(buf, 0xAA, sizeof(buf));
        for (size_t i = 0; i < n; i++) buf[LCP_CHUNK_HEAD + i] = (uint8_t) i;
        size_t start = lcp_chunk_frame(buf, n, &end);
        char hdr[16];
        int h = snprintf(hdr, sizeof(hdr), "%zx\r\n", n);
        CHECK(start == LCP_CHUNK_HEAD - (size_t) h && end == LCP_CHUNK_HEAD + n + 2);
        CHECK(strncasecmp((char *) buf + start, hdr, (size_t) h) == 0);
        CHECK(buf[end - 2] == '\r' && buf[end - 1] == '\n');
        for (size_t i = 0; i < n; i++) if (buf[LCP_CHUNK_HEAD + i] != (uint8_t) i) { CHECK(0); break; }
    }
}

int main(void) {
    test_chunks();
    test_requests();
    test_responses();
    test_bcast();
    if (failures) {
        fprintf(stderr, "%d local caster check(s) failed\n", failures);
        return 1;
    }
    printf("local caster tests passed\n");
    return 0;
}
