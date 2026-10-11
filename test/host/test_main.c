/*
 * Host unit tests for the pure-logic parts of the stability rework:
 * byte_ring, backoff and stream_sender. Build and run with run_tests.sh.
 *
 * The stream_sender tests include a real TCP back-pressure test over a Linux
 * socketpair: the reader stops reading, the kernel buffers fill, and the
 * sender must neither block nor corrupt data, then report a stall after
 * SENDER_STALL_MS of (simulated) time without progress.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "byte_ring.h"
#include "backoff.h"
#include "stream_sender.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---------------------------------------------------------------- byte_ring */

static void test_ring_basic(void) {
    uint8_t storage[8];
    byte_ring_t r;
    byte_ring_init(&r, storage, sizeof(storage));

    CHECK(byte_ring_write(&r, "abc", 3) == 0);
    CHECK(byte_ring_len(&r) == 3);

    char out[16] = {0};
    CHECK(byte_ring_read(&r, out, 2) == 2);
    CHECK(memcmp(out, "ab", 2) == 0);

    // Wrap around
    CHECK(byte_ring_write(&r, "defghi", 6) == 0);
    CHECK(byte_ring_len(&r) == 7);
    memset(out, 0, sizeof(out));
    CHECK(byte_ring_read(&r, out, sizeof(out)) == 7);
    CHECK(memcmp(out, "cdefghi", 7) == 0);
    CHECK(byte_ring_len(&r) == 0);
}

static void test_ring_overflow_drops_oldest(void) {
    uint8_t storage[8];
    byte_ring_t r;
    byte_ring_init(&r, storage, sizeof(storage));

    byte_ring_write(&r, "0123456", 7);
    CHECK(byte_ring_write(&r, "789", 3) == 2); // drops "01"
    char out[16] = {0};
    CHECK(byte_ring_read(&r, out, sizeof(out)) == 8);
    CHECK(memcmp(out, "23456789", 8) == 0);
    CHECK(r.dropped_total == 2);

    // Write larger than capacity keeps only the newest bytes
    CHECK(byte_ring_write(&r, "ABCDEFGHIJKL", 12) == 4);
    memset(out, 0, sizeof(out));
    CHECK(byte_ring_read(&r, out, sizeof(out)) == 8);
    CHECK(memcmp(out, "EFGHIJKL", 8) == 0);
}

static void test_ring_random(void) {
    // Compare against a simple model with random operations
    enum { CAP = 97 };
    uint8_t storage[CAP];
    byte_ring_t r;
    byte_ring_init(&r, storage, CAP);

    uint8_t model[CAP * 4];
    size_t model_len = 0;
    uint8_t next = 0;
    srand(1234);

    for (int i = 0; i < 100000; i++) {
        if (rand() % 2) {
            uint8_t buf[200];
            size_t n = rand() % 150;
            for (size_t k = 0; k < n; k++) buf[k] = next++;
            byte_ring_write(&r, buf, n);
            memcpy(model + model_len, buf, n);
            model_len += n;
            if (model_len > CAP) {
                memmove(model, model + model_len - CAP, CAP);
                model_len = CAP;
            }
        } else {
            uint8_t buf[200];
            size_t want = rand() % 150;
            size_t got = byte_ring_read(&r, buf, want);
            size_t expect = want < model_len ? want : model_len;
            CHECK(got == expect);
            CHECK(memcmp(buf, model, got) == 0);
            memmove(model, model + got, model_len - got);
            model_len -= got;
        }
        CHECK(byte_ring_len(&r) == model_len);
        if (failures) return;
    }
}

/* ------------------------------------------------------------------ backoff */

static void test_backoff_schedule(void) {
    backoff_t b;
    backoff_init(&b);
    uint32_t expect[] = {0, 5, 5, 5, 10, 20, 40, 60, 120, 300, 300, 300};
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        CHECK(backoff_delay_ms(&b) == expect[i] * 1000);
        backoff_attempt_failed(&b);
    }
}

static void test_backoff_reset_after_stable(void) {
    backoff_t b;
    backoff_init(&b);
    for (int i = 0; i < 8; i++) backoff_attempt_failed(&b);
    CHECK(backoff_delay_ms(&b) == 120000);

    // Short session does not reset
    backoff_session_ended(&b, 60 * 1000, false);
    CHECK(backoff_delay_ms(&b) == 300000);

    // Stable session resets to the first reconnect step
    backoff_session_ended(&b, BACKOFF_STABLE_MS, false);
    CHECK(backoff_delay_ms(&b) == 5000);
}

static void test_backoff_wifi_does_not_escalate(void) {
    backoff_t b;
    backoff_init(&b);
    backoff_attempt_failed(&b);
    backoff_attempt_failed(&b);
    backoff_attempt_failed(&b);
    backoff_attempt_failed(&b);
    uint32_t before = backoff_delay_ms(&b);
    for (int i = 0; i < 10; i++) backoff_session_ended(&b, 10 * 1000, true);
    CHECK(backoff_delay_ms(&b) == before);

    // First session after boot dropped by WiFi: reconnect after 5 s, not 0
    backoff_init(&b);
    backoff_session_ended(&b, 1000, true);
    CHECK(backoff_delay_ms(&b) == 5000);
}

static void test_backoff_short_sessions_escalate(void) {
    // Caster accepts the login then drops us straight away (e.g. mount in use)
    backoff_t b;
    backoff_init(&b);
    uint32_t prev = 0;
    for (int i = 0; i < 12; i++) {
        backoff_session_ended(&b, 2000, false);
        uint32_t d = backoff_delay_ms(&b);
        CHECK(d >= prev);
        prev = d;
    }
    CHECK(prev == 300000);
}

/* ------------------------------------------------------------ stream_sender */

typedef struct {
    byte_ring_t ring;
    uint8_t storage[16384];
    // Mock socket
    size_t accept_per_call;   // max bytes accepted per write (0 = would block)
    long budget;              // total bytes accepted before blocking (-1 = unlimited)
    int fail;                 // return error
    uint8_t sink[1 << 20];
    size_t sink_len;
    int fd;                   // real socket when >= 0
} harness_t;

static size_t h_fill(void *ctx, uint8_t *out, size_t max) {
    harness_t *h = ctx;
    return byte_ring_read(&h->ring, out, max);
}

static int h_write(void *ctx, const uint8_t *data, size_t len) {
    harness_t *h = ctx;
    if (h->fd >= 0) {
        ssize_t r = send(h->fd, data, len, MSG_DONTWAIT);
        if (r > 0) return (int) r;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    if (h->fail) return -1;
    size_t n = len < h->accept_per_call ? len : h->accept_per_call;
    if (h->budget >= 0 && n > (size_t) h->budget) n = (size_t) h->budget;
    if (h->budget >= 0) h->budget -= n;
    memcpy(h->sink + h->sink_len, data, n);
    h->sink_len += n;
    return (int) n;
}

static harness_t *harness_new(void) {
    harness_t *h = calloc(1, sizeof(harness_t));
    byte_ring_init(&h->ring, h->storage, sizeof(h->storage));
    h->fd = -1;
    h->budget = -1;
    return h;
}

static void test_sender_partial_writes_preserve_stream(void) {
    harness_t *h = harness_new();
    static stream_sender_t s;
    stream_sender_reset(&s, 0);

    uint8_t data[10000];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t) (i * 7 + 3);
    byte_ring_write(&h->ring, data, sizeof(data));

    uint32_t t = 0;
    // Socket takes odd-sized short writes
    h->accept_per_call = 333;
    while (byte_ring_len(&h->ring) > 0 || stream_sender_pending(&s)) {
        size_t sent = 0;
        sender_result_t r = stream_sender_pump(&s, h_fill, h_write, h, t, &sent);
        CHECK(r == SENDER_OK || r == SENDER_IDLE);
        t += 10;
    }
    CHECK(h->sink_len == sizeof(data));
    CHECK(memcmp(h->sink, data, sizeof(data)) == 0);
    free(h);
}

static void test_sender_slow_then_stall(void) {
    harness_t *h = harness_new();
    static stream_sender_t s;
    stream_sender_reset(&s, 0);
    byte_ring_write(&h->ring, "hello world", 11);

    h->accept_per_call = 0; // would block
    CHECK(stream_sender_pump(&s, h_fill, h_write, h, 1000, NULL) == SENDER_BLOCKED);
    CHECK(stream_sender_pump(&s, h_fill, h_write, h, SENDER_SLOW_MS, NULL) == SENDER_SLOW);
    // Slow is reported once per episode
    CHECK(stream_sender_pump(&s, h_fill, h_write, h, SENDER_SLOW_MS + 1000, NULL) == SENDER_BLOCKED);
    CHECK(stream_sender_pump(&s, h_fill, h_write, h, SENDER_STALL_MS - 1, NULL) == SENDER_BLOCKED);
    CHECK(stream_sender_pump(&s, h_fill, h_write, h, SENDER_STALL_MS, NULL) == SENDER_STALLED);
    free(h);
}

static void test_sender_progress_resets_stall(void) {
    harness_t *h = harness_new();
    static stream_sender_t s;
    stream_sender_reset(&s, 0);

    // Trickle: one byte every 15 s keeps the connection alive (slower than
    // the stall limit between progress events would not)
    uint8_t data[64];
    memset(data, 'x', sizeof(data));
    byte_ring_write(&h->ring, data, sizeof(data));
    uint32_t t = 0;
    h->accept_per_call = 64;
    for (int i = 0; i < 10; i++) {
        h->budget = 0;
        t += 15000;
        CHECK(stream_sender_pump(&s, h_fill, h_write, h, t, NULL) != SENDER_STALLED);
        h->budget = 1;
        CHECK(stream_sender_pump(&s, h_fill, h_write, h, t, NULL) == SENDER_BLOCKED); // 1 byte then full
        CHECK(h->sink_len == (size_t) i + 1);
    }
    free(h);
}

static void test_sender_idle_is_not_stall(void) {
    harness_t *h = harness_new();
    static stream_sender_t s;
    stream_sender_reset(&s, 0);
    h->accept_per_call = 0;
    // Nothing to send for a long time: never a stall
    CHECK(stream_sender_pump(&s, h_fill, h_write, h, 10 * 60 * 1000, NULL) == SENDER_IDLE);
    // Data arrives after a long idle period; first block is not an instant stall
    byte_ring_write(&h->ring, "abc", 3);
    CHECK(stream_sender_pump(&s, h_fill, h_write, h, 10 * 60 * 1000 + 100, NULL) == SENDER_BLOCKED);
    free(h);
}

static void test_sender_error(void) {
    harness_t *h = harness_new();
    static stream_sender_t s;
    stream_sender_reset(&s, 0);
    byte_ring_write(&h->ring, "abc", 3);
    h->fail = 1;
    CHECK(stream_sender_pump(&s, h_fill, h_write, h, 0, NULL) == SENDER_ERROR);
    free(h);
}

/*
 * Real TCP-style back-pressure: a UNIX stream socketpair with small buffers.
 * The producer keeps adding RTCM-like data at a steady rate while the reader
 * is paused, so the socket fills, the ring overflows and drops oldest data.
 * Checks:
 *  - the sender never blocks (socket is non-blocking via MSG_DONTWAIT)
 *  - it reports SLOW, then STALLED after SENDER_STALL_MS with no progress
 *  - every byte the reader got is in order (sequence numbers only skip
 *    forward where the ring dropped data, never repeat or reorder)
 */
static void test_sender_socket_backpressure(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int small = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    fcntl(sv[1], F_SETFL, O_NONBLOCK);

    harness_t *h = harness_new();
    h->fd = sv[0];
    static stream_sender_t s;
    stream_sender_reset(&s, 0);

    uint32_t seq = 0;  // producer writes 32-bit little-endian sequence numbers
    uint32_t t = 0;
    bool saw_slow = false, saw_stall = false, reader_paused = false;
    uint8_t rx[65536];
    size_t rx_len = 0;

    for (int step = 0; step < 4000 && !saw_stall; step++) {
        t += 10; // 10 ms per step

        // ~4 KB/s of data, like a busy RTCM stream
        uint32_t words[10];
        for (int k = 0; k < 10; k++) words[k] = seq++;
        byte_ring_write(&h->ring, words, sizeof(words));

        // Reader works for the first second, then stops
        reader_paused = t > 1000;
        if (!reader_paused) {
            ssize_t r = recv(sv[1], rx + rx_len, sizeof(rx) - rx_len, 0);
            if (r > 0) rx_len += r;
        }

        sender_result_t res = stream_sender_pump(&s, h_fill, h_write, h, t, NULL);
        CHECK(res != SENDER_ERROR);
        if (res == SENDER_SLOW) saw_slow = true;
        if (res == SENDER_STALLED) {
            saw_stall = true;
            CHECK(t >= 1000 + SENDER_STALL_MS - 100);
        }
    }
    CHECK(saw_slow);
    CHECK(saw_stall);
    CHECK(h->ring.dropped_total > 0); // overflowed while blocked

    // Drain whatever made it into the socket and check ordering
    while (true) {
        ssize_t r = recv(sv[1], rx + rx_len, sizeof(rx) - rx_len, 0);
        if (r <= 0) break;
        rx_len += r;
    }
    CHECK(rx_len % 4 == 0);
    uint32_t prev = 0;
    for (size_t i = 0; i + 4 <= rx_len; i += 4) {
        uint32_t v;
        memcpy(&v, rx + i, 4);
        if (i == 0) CHECK(v == 0);
        else CHECK(v == prev + 1);  // nothing dropped before the stall point
        prev = v;
    }

    close(sv[0]);
    close(sv[1]);
    free(h);
}

int main(void) {
    test_ring_basic();
    test_ring_overflow_drops_oldest();
    test_ring_random();
    test_backoff_schedule();
    test_backoff_reset_after_stable();
    test_backoff_wifi_does_not_escalate();
    test_backoff_short_sessions_escalate();
    test_sender_partial_writes_preserve_stream();
    test_sender_slow_then_stall();
    test_sender_progress_resets_stall();
    test_sender_idle_is_not_stall();
    test_sender_error();
    test_sender_socket_backpressure();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("All host tests passed\n");
    return 0;
}
