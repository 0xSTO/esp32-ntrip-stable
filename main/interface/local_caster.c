/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

/*
 * Built-in NTRIP caster for rovers on the same network.
 *
 * Design (same rules as the outgoing casters, see STABILITY_CHANGES.md):
 *  - The UART task only copies each RTCM frame into one shared broadcast ring
 *    under a spinlock (microseconds). It never touches a socket and never
 *    waits for a rover.
 *  - One task owns all sockets: a listening socket plus up to
 *    LOCAL_CASTER_MAX_CLIENTS rovers, multiplexed with select(). All sends are
 *    non-blocking.
 *  - Each rover has its own read cursor into the ring. A rover that falls more
 *    than the ring (about 2 s of data) behind skips to the newest data; one
 *    that accepts nothing for LC_STALL_MS is disconnected. Neither affects the
 *    other rover or the outgoing casters.
 *  - Nothing is allocated unless the caster is switched on. When on, ~7 KB
 *    (task + state); the 4 KB ring is only allocated while a rover streams.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#include "interface/local_caster.h"
#include "local_caster_proto.h"
#include "config.h"
#include "health.h"
#include "receiver.h"
#include "stream_stats.h"
#include "tasks.h"
#include "uart.h"
#include "util.h"

static const char *TAG = "LOCAL_CASTER";

#define LC_RING_SIZE 4096          // power of two; ~2 s of RTCM at 2 kB/s
#define LC_REQ_MAX 512             // request headers
#define LC_TX_CHUNK 512
#define LC_HANDSHAKE_MS 10000
#define LC_STALL_MS 10000
#define LC_SELECT_MS 50
#define LC_TASK_STACK 4096
#define LC_RETRY_LISTEN_MS 5000
#define LC_MIN_FREE_HEAP 16384     // refuse new rovers below this (the casters come first)

typedef enum {
    SLOT_FREE = 0,
    SLOT_HANDSHAKE,
    SLOT_STREAMING,
} slot_state_t;

typedef struct {
    int sock;
    volatile slot_state_t state;
    uint32_t cursor;
    // Data taken from the ring but not yet accepted by the socket. Always sent
    // completely (so a v2 chunk or an RTCM frame is never cut by a skip).
    uint8_t stage[LCP_CHUNK_HEAD + LC_TX_CHUNK + 2];
    uint16_t st_off, st_len;
    uint16_t req_len;
    char req[LC_REQ_MAX];
    volatile int64_t start_us;
    uint32_t last_progress_ms;
    volatile uint32_t bytes;
    bool v2;
    char peer[24];
} slot_t;

typedef struct {
    portMUX_TYPE mux;          // guards ring, and slot status fields for the web UI
    bcast_ring_t ring;         // ring.buf is NULL while no rover streams
    char reply[640];           // response being built (caster task only)
    slot_t slots[LOCAL_CASTER_MAX_CLIENTS];

    int listen_sock;
    uint16_t port;
    char mount[LCP_MOUNT_MAX + 1];
    char *auth;                // expected "Basic ..." or NULL

    volatile bool listening;
    char error[64];
    volatile uint32_t sessions;
    volatile uint32_t refused;
    volatile uint32_t dropped;

    stream_stats_handle_t stats;
    health_handle_t health;
    TaskHandle_t task;
} local_caster_t;

static local_caster_t *lc = NULL;
static bool lc_configured = false;

static uint32_t now_ms(void) {
    return (uint32_t) (esp_timer_get_time() / 1000);
}

// --- UART side (runs in uart_task; must never block) ---------------------------

static void lc_uart_sink(void *ctx, const uint8_t *data, size_t len) {
    local_caster_t *c = ctx;
    taskENTER_CRITICAL(&c->mux);
    if (c->ring.buf != NULL) bcast_write(&c->ring, data, (uint32_t) len);
    taskEXIT_CRITICAL(&c->mux);
}

static int streaming_count(local_caster_t *c) {
    int n = 0;
    for (int i = 0; i < LOCAL_CASTER_MAX_CLIENTS; i++) n += c->slots[i].state == SLOT_STREAMING;
    return n;
}

// --- Socket helpers ---------------------------------------------------------------

// Short replies (< 1 KB) always fit in a fresh socket's send buffer.
static bool send_reply(int sock, const char *buf, int len) {
    if (len <= 0) return false;
    int r = send(sock, buf, (size_t) len, MSG_DONTWAIT);
    return r == len;
}

// abort: reset the connection instead of a graceful close, so lwIP frees the
// queued send data at once instead of retrying for minutes (heap is scarce).
static void close_slot(local_caster_t *c, slot_t *s, const char *reason, bool abort) {
    if (s->state == SLOT_STREAMING) {
        uint32_t secs = (uint32_t) ((esp_timer_get_time() - s->start_us) / 1000000);
        ESP_LOGI(TAG, "Rover %s disconnected after %u s (%u bytes): %s", s->peer, (unsigned) secs,
                 (unsigned) s->bytes, reason);
    } else if (reason != NULL) {
        ESP_LOGI(TAG, "Connection from %s closed: %s", s->peer, reason);
    }
    if (s->sock >= 0) {
        if (abort) {
            struct linger lg = {.l_onoff = 1, .l_linger = 0};
            setsockopt(s->sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        }
        close(s->sock);
    }
    uint8_t *release = NULL;
    taskENTER_CRITICAL(&c->mux);
    s->sock = -1;
    s->state = SLOT_FREE;
    s->bytes = 0;
    s->start_us = 0;
    if (streaming_count(c) == 0) {   // last rover gone: give the ring's memory back
        release = c->ring.buf;
        c->ring.buf = NULL;
    }
    taskEXIT_CRITICAL(&c->mux);
    free(release);
}

static bool open_listener(local_caster_t *c) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        snprintf(c->error, sizeof(c->error), "no socket available (%s)", strerror(errno));
        return false;
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(c->port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        snprintf(c->error, sizeof(c->error), "port %u unavailable (%s)", c->port, strerror(errno));
        close(s);
        return false;
    }
    if (listen(s, 2) != 0) {
        snprintf(c->error, sizeof(c->error), "listen failed (%s)", strerror(errno));
        close(s);
        return false;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    c->listen_sock = s;
    c->error[0] = '\0';
    c->listening = true;
    ESP_LOGI(TAG, "Listening on port %u, mountpoint /%s%s", c->port, c->mount, c->auth ? " (password)" : "");
    return true;
}

static void accept_client(local_caster_t *c) {
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int s = accept(c->listen_sock, (struct sockaddr *) &addr, &alen);
    if (s < 0) return;

    // Own buffer: util's sockaddrtostr() returns a static buffer that the web
    // server's /status handler uses at the same time from another task.
    char peer[24];
    char ip[INET_ADDRSTRLEN] = "?";
    if (addr.sin_family == AF_INET) inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    snprintf(peer, sizeof(peer), "%s:%u", ip, (unsigned) ntohs(addr.sin_port));
    slot_t *slot = NULL;
    for (int i = 0; i < LOCAL_CASTER_MAX_CLIENTS; i++) {
        if (c->slots[i].state == SLOT_FREE) {
            slot = &c->slots[i];
            break;
        }
    }
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (slot == NULL || free_heap < LC_MIN_FREE_HEAP) {
        char reply[160];
        send_reply(s, reply, lcp_response_busy(reply, sizeof(reply)));
        // Read whatever request already arrived: closing with unread data
        // would make lwIP reset the connection and the rover lose the reply.
        char junk[128];
        while (recv(s, junk, sizeof(junk), MSG_DONTWAIT) > 0) {}
        close(s);
        c->refused++;
        if (slot == NULL) ESP_LOGW(TAG, "Refused %s: already serving %d rovers", peer, LOCAL_CASTER_MAX_CLIENTS);
        else ESP_LOGW(TAG, "Refused %s: low memory (%u bytes free)", peer, (unsigned) free_heap);
        return;
    }

    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    // No TCP_NODELAY: with Nagle, small RTCM pieces are merged into full
    // segments while one is in flight, which keeps lwIP's buffer use low.
    socket_set_keepalive(s, 5, 2, 3);

    taskENTER_CRITICAL(&c->mux);
    slot->sock = s;
    slot->req_len = 0;
    slot->st_off = slot->st_len = 0;
    slot->bytes = 0;
    slot->v2 = false;
    slot->start_us = esp_timer_get_time();
    slot->state = SLOT_HANDSHAKE;
    taskEXIT_CRITICAL(&c->mux);
    snprintf(slot->peer, sizeof(slot->peer), "%s", peer);
}

static void handle_request(local_caster_t *c, slot_t *s) {
    lcp_request_t rq;
    lcp_parse(s->req, s->req_len, c->mount, c->auth, &rq);
    char *reply = c->reply;
    const size_t reply_size = sizeof(c->reply);

    switch (rq.kind) {
        case LCP_REQ_INCOMPLETE:
            if (s->req_len >= LC_REQ_MAX - 1) {
                send_reply(s->sock, reply, lcp_response_bad(reply, reply_size));
                c->refused++;
                close_slot(c, s, "request too long", false);
            }
            return;
        case LCP_REQ_BAD:
            send_reply(s->sock, reply, lcp_response_bad(reply, reply_size));
            c->refused++;
            close_slot(c, s, "not an NTRIP request", false);
            return;
        case LCP_REQ_SOURCETABLE: {
            double lat = 0, lon = 0;
            receiver_position(&lat, &lon);
            send_reply(s->sock, reply, lcp_response_sourcetable(rq.v2, c->mount, c->auth != NULL, lat, lon,
                                                                "esp32-ntrip-stable", reply, reply_size));
            close_slot(c, s, rq.mount[0] ? "unknown mountpoint, sent the mountpoint list" : "sent the mountpoint list", false);
            return;
        }
        case LCP_REQ_UNAUTHORIZED:
            send_reply(s->sock, reply, lcp_response_unauthorized(rq.v2, c->mount, reply, reply_size));
            c->refused++;
            ESP_LOGW(TAG, "Wrong or missing username/password from %s", s->peer);
            close_slot(c, s, NULL, false);
            return;
        case LCP_REQ_STREAM: {
            uint8_t *storage = NULL;
            if (c->ring.buf == NULL) {   // first rover: allocate the ring now
                storage = heap_caps_get_free_size(MALLOC_CAP_8BIT) >= LC_MIN_FREE_HEAP + LC_RING_SIZE ?
                          malloc(LC_RING_SIZE) : NULL;
                if (storage == NULL) {
                    send_reply(s->sock, reply, lcp_response_busy(reply, reply_size));
                    c->refused++;
                    ESP_LOGW(TAG, "Refused %s: low memory", s->peer);
                    close_slot(c, s, NULL, false);
                    return;
                }
            }
            if (!send_reply(s->sock, reply, lcp_response_stream(rq.v2, reply, reply_size))) {
                free(storage);
                close_slot(c, s, "could not send reply", true);
                return;
            }
            taskENTER_CRITICAL(&c->mux);
            if (storage != NULL) bcast_init(&c->ring, storage, LC_RING_SIZE);
            s->cursor = c->ring.head;   // start at the next frame
            s->st_off = s->st_len = 0;
            s->v2 = rq.v2;
            s->bytes = 0;
            s->start_us = esp_timer_get_time();
            s->state = SLOT_STREAMING;
            taskEXIT_CRITICAL(&c->mux);
            s->last_progress_ms = now_ms();
            c->sessions++;
            ESP_LOGI(TAG, "Rover %s connected to /%s (NTRIP v%d)", s->peer, c->mount, rq.v2 ? 2 : 1);
            return;
        }
    }
}

static void handle_readable(local_caster_t *c, slot_t *s) {
    if (s->state == SLOT_HANDSHAKE) {
        int r = recv(s->sock, s->req + s->req_len, LC_REQ_MAX - 1 - s->req_len, MSG_DONTWAIT);
        if (r == 0) {
            close_slot(c, s, "closed before sending a request", false);
            return;
        }
        if (r < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) close_slot(c, s, strerror(errno), true);
            return;
        }
        s->req_len += (uint16_t) r;
        s->req[s->req_len] = '\0';
        handle_request(c, s);
    } else if (s->state == SLOT_STREAMING) {
        // Rovers may send their position (NMEA GGA); it isn't needed here.
        char sink[128];
        int r = recv(s->sock, sink, sizeof(sink), MSG_DONTWAIT);
        if (r == 0) close_slot(c, s, "rover closed the connection", false);
        else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) close_slot(c, s, strerror(errno), true);
    }
}

static void pump(local_caster_t *c, slot_t *s, uint32_t now) {
    for (int i = 0; i < 8; i++) {
        if (s->st_off == s->st_len) {
            // Stage the next piece from the ring
            bool overrun;
            uint32_t missed = 0;
            uint8_t *dst = s->stage + LCP_CHUNK_HEAD;
            taskENTER_CRITICAL(&c->mux);
            size_t n = bcast_peek(&c->ring, s->cursor, dst, LC_TX_CHUNK, &overrun);
            if (overrun) {
                missed = c->ring.head - s->cursor;
                s->cursor = c->ring.head;
            } else {
                s->cursor += (uint32_t) n;
            }
            taskEXIT_CRITICAL(&c->mux);

            if (overrun) {
                c->dropped += missed;
                ESP_LOGW(TAG, "Rover %s too slow: skipped %u bytes", s->peer, (unsigned) missed);
                continue;
            }
            if (n == 0) {
                s->last_progress_ms = now;   // nothing waiting is not a stall
                return;
            }
            if (s->v2) {
                size_t end;
                s->st_off = (uint16_t) lcp_chunk_frame(s->stage, n, &end);
                s->st_len = (uint16_t) end;
            } else {
                s->st_off = LCP_CHUNK_HEAD;
                s->st_len = (uint16_t) (LCP_CHUNK_HEAD + n);
            }
        }

        size_t len = s->st_len - s->st_off;
        int r = send(s->sock, s->stage + s->st_off, len, MSG_DONTWAIT);
        if (r > 0) {
            s->st_off += (uint16_t) r;
            s->bytes += (uint32_t) r;
            s->last_progress_ms = now;
            stream_stats_increment(c->stats, 0, (uint32_t) r);
            if ((size_t) r < len) break;   // socket full
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            close_slot(c, s, r == 0 ? "send returned 0" : strerror(errno), true);
            return;
        }
    }
    if (now - s->last_progress_ms > LC_STALL_MS) close_slot(c, s, "rover stopped reading", true);
}

static void lc_task(void *ctx) {
    local_caster_t *c = ctx;
    uint32_t last_listen_try = 0;

    while (true) {
        health_kick(c->health);

        if (c->listen_sock < 0) {
            if (last_listen_try == 0 || now_ms() - last_listen_try >= LC_RETRY_LISTEN_MS) {
                last_listen_try = now_ms();
                if (!open_listener(c)) {
                    ESP_LOGW(TAG, "Not listening: %s (retrying)", c->error);
                }
            }
            if (c->listen_sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(c->listen_sock, &rfds);
        int maxfd = c->listen_sock;
        for (int i = 0; i < LOCAL_CASTER_MAX_CLIENTS; i++) {
            slot_t *s = &c->slots[i];
            if (s->state != SLOT_FREE) {
                FD_SET(s->sock, &rfds);
                if (s->sock > maxfd) maxfd = s->sock;
            }
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = LC_SELECT_MS * 1000};
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno != EINTR) {
                ESP_LOGW(TAG, "select failed: %s", strerror(errno));
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        if (n > 0) {
            for (int i = 0; i < LOCAL_CASTER_MAX_CLIENTS; i++) {
                slot_t *s = &c->slots[i];
                if (s->state != SLOT_FREE && FD_ISSET(s->sock, &rfds)) handle_readable(c, s);
            }
            if (FD_ISSET(c->listen_sock, &rfds)) accept_client(c);
        }

        uint32_t now = now_ms();
        for (int i = 0; i < LOCAL_CASTER_MAX_CLIENTS; i++) {
            slot_t *s = &c->slots[i];
            if (s->state == SLOT_HANDSHAKE &&
                (esp_timer_get_time() - s->start_us) / 1000 > LC_HANDSHAKE_MS) {
                c->refused++;
                close_slot(c, s, "no request within 10 s", false);
            } else if (s->state == SLOT_STREAMING) {
                pump(c, s, now);
            }
        }
    }
}

// --- Public ---------------------------------------------------------------------

static void sanitize_mount(char *dst, size_t size, const char *src) {
    size_t n = 0;
    for (const char *p = src; p && *p && n + 1 < size; p++) {
        char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
            dst[n++] = ch;
        }
    }
    dst[n] = '\0';
    if (n == 0) snprintf(dst, size, "BASE");
}

void local_caster_init(void) {
    lc_configured = config_get_bool1(CONF_ITEM(KEY_CONFIG_LOCAL_CASTER_ACTIVE));
    if (!lc_configured) return;

    local_caster_t *c = calloc(1, sizeof(local_caster_t));
    if (c == NULL) {
        ESP_LOGE(TAG, "Out of memory starting the built-in caster");
        return;
    }
    portMUX_INITIALIZE(&c->mux);
    c->ring.buf = NULL;
    c->listen_sock = -1;
    for (int i = 0; i < LOCAL_CASTER_MAX_CLIENTS; i++) c->slots[i].sock = -1;

    c->port = config_get_u16(CONF_ITEM(KEY_CONFIG_LOCAL_CASTER_PORT));
    if (c->port == 0) c->port = 2101;

    char *mount = NULL, *user = NULL, *pass = NULL;
    config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_LOCAL_CASTER_MOUNTPOINT), (void **) &mount);
    config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_LOCAL_CASTER_USERNAME), (void **) &user);
    config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_LOCAL_CASTER_PASSWORD), (void **) &pass);
    sanitize_mount(c->mount, sizeof(c->mount), mount);
    if (user != NULL && user[0] != '\0') c->auth = http_auth_basic_header(user, pass ? pass : "");
    free(mount);
    free(user);
    free(pass);

    c->stats = stream_stats_new("local_caster");
    c->health = health_register("local_caster", HEALTH_DEFAULT_TIMEOUT_MS);
    strcpy(c->error, "starting");

    if (xTaskCreate(lc_task, "local_caster", LC_TASK_STACK, c, TASK_PRIORITY_INTERFACE, &c->task) != pdPASS) {
        ESP_LOGE(TAG, "Out of memory starting the built-in caster task");
        free(c->auth);
        free(c);
        return;
    }
    lc = c;
    uart_add_read_sink(lc_uart_sink, c);
}

void local_caster_get_status(local_caster_status_t *st) {
    memset(st, 0, sizeof(*st));
    st->active = lc_configured;
    local_caster_t *c = lc;
    if (c == NULL) {
        if (lc_configured) snprintf(st->error, sizeof(st->error), "out of memory");
        return;
    }
    st->listening = c->listening;
    st->port = c->port;
    snprintf(st->mountpoint, sizeof(st->mountpoint), "%s", c->mount);
    st->auth = c->auth != NULL;
    snprintf(st->error, sizeof(st->error), "%s", c->error);
    st->sessions = c->sessions;
    st->refused = c->refused;
    st->dropped = c->dropped;
    st->stack_free = c->task ? (uint32_t) uxTaskGetStackHighWaterMark(c->task) : 0;

    int64_t now = esp_timer_get_time();
    for (int i = 0; i < LOCAL_CASTER_MAX_CLIENTS; i++) {
        slot_t *s = &c->slots[i];
        taskENTER_CRITICAL(&c->mux);
        bool streaming = s->state == SLOT_STREAMING;
        int64_t start = s->start_us;
        uint32_t bytes = s->bytes;
        bool v2 = s->v2;
        taskEXIT_CRITICAL(&c->mux);
        if (!streaming) continue;
        int k = st->clients++;
        snprintf(st->client[k].peer, sizeof(st->client[k].peer), "%s", s->peer);
        st->client[k].uptime_s = start ? (uint32_t) ((now - start) / 1000000) : 0;
        st->client[k].bytes = bytes;
        st->client[k].v2 = v2;
    }
}
