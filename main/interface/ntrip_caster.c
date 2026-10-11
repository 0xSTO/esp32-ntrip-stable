/*
 * This file is part of esp32-ntrip-stable.
 * Based on ntrip_server.c from ESP32-XBee (https://github.com/nebkat/esp32-xbee),
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Design (see STABILITY_CHANGES.md):
 *  - The UART task hands each chunk to every caster's sink, which only copies
 *    it into that caster's ring buffer. Nothing on the UART or event-loop path
 *    ever touches a socket, so a slow caster can't stall the other caster,
 *    WiFi event handling or the UART.
 *  - Each caster has its own task that owns the socket and drains the ring
 *    with non-blocking sends, completing partial writes.
 *  - If the socket accepts nothing for 20 s the connection is dropped; while
 *    it is merely slow, the ring drops its oldest data instead.
 *  - TCP keepalive, caster-close detection and WiFi IP loss end the session
 *    promptly instead of waiting for the send buffer to fill.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR >= 5
#include <esp_app_desc.h>
#else
#include <esp_ota_ops.h>
#endif

#include "interface/ntrip.h"
#include "interface/ntrip_caster.h"
#include "byte_ring.h"
#include "backoff.h"
#include "stream_sender.h"
#include "health.h"
#include "config.h"
#include "status_led.h"
#include "stream_stats.h"
#include "tasks.h"
#include "uart.h"
#include "util.h"
#include "wifi.h"

#define CASTER_RING_SIZE (16 * 1024)
#define CASTER_CONNECT_TIMEOUT_MS 10000
#define CASTER_HANDSHAKE_TIMEOUT_MS 10000
#define CASTER_KEEPALIVE_IDLE_S 10
#define CASTER_KEEPALIVE_INTERVAL_S 5
#define CASTER_KEEPALIVE_COUNT 3
#define CASTER_POLL_MS 200
#define CASTER_DROP_LOG_INTERVAL_MS 10000
#define CASTER_TASK_STACK 5120

struct ntrip_caster {
    const ntrip_caster_def_t *def;

    SemaphoreHandle_t lock;
    byte_ring_t ring;
    volatile bool accepting;
    volatile int64_t last_uart_us;
    volatile uint32_t sink_lock_drops;

    TaskHandle_t task;
    int sock;
    int last_errno;

    stream_sender_t sender;
    stream_stats_handle_t stats;
    status_led_handle_t led;
    health_handle_t health;

    // Status for the web UI, written by the caster task only
    volatile ntrip_caster_state_t state;
    volatile uint32_t sessions;
    volatile uint32_t failures;
    volatile int64_t session_start_us;
    char last_disconnect[96];
};

static uint32_t now_ms() {
    return (uint32_t) (esp_timer_get_time() / 1000);
}

static const char *app_version() {
#if ESP_IDF_VERSION_MAJOR >= 5
    const char *v = esp_app_get_description()->version;
#else
    const char *v = esp_ota_get_app_description()->version;
#endif
    return v[0] == 'v' ? v + 1 : v;
}

const char *ntrip_caster_state_name(ntrip_caster_state_t state) {
    switch (state) {
        case NTRIP_CASTER_DISABLED: return "disabled";
        case NTRIP_CASTER_WAITING_DATA: return "waiting for UART data";
        case NTRIP_CASTER_BACKOFF: return "waiting to reconnect";
        case NTRIP_CASTER_WAITING_WIFI: return "waiting for WiFi";
        case NTRIP_CASTER_CONNECTING: return "connecting";
        case NTRIP_CASTER_CONNECTED: return "connected";
        default: return "unknown";
    }
}

// --- UART side (runs in uart_task; must never block for long) --------------

static void caster_uart_sink(void *ctx, const uint8_t *data, size_t len) {
    struct ntrip_caster *c = ctx;
    c->last_uart_us = esp_timer_get_time();

    if (c->accepting) {
        if (xSemaphoreTake(c->lock, pdMS_TO_TICKS(5)) == pdTRUE) {
            byte_ring_write(&c->ring, data, len);
            xSemaphoreGive(c->lock);
        } else {
            c->sink_lock_drops += len;
        }
    }

    xTaskNotifyGive(c->task);
}

static bool uart_recently_active(struct ntrip_caster *c) {
    int64_t last = c->last_uart_us;
    return last != 0 && (esp_timer_get_time() - last) / 1000 < NTRIP_KEEP_ALIVE_THRESHOLD;
}

// --- Sender callbacks --------------------------------------------------------

static size_t caster_fill(void *ctx, uint8_t *out, size_t max) {
    struct ntrip_caster *c = ctx;
    xSemaphoreTake(c->lock, portMAX_DELAY);
    size_t n = byte_ring_read(&c->ring, out, max);
    xSemaphoreGive(c->lock);
    return n;
}

static int caster_write(void *ctx, const uint8_t *data, size_t len) {
    struct ntrip_caster *c = ctx;
    int ret = send(c->sock, data, len, MSG_DONTWAIT);
    if (ret > 0) return ret;
    if (ret == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    c->last_errno = errno;
    return -1;
}

// --- Task helpers --------------------------------------------------------------

// Sleep while keeping the progress watchdog fed
static void sleep_ms(struct ntrip_caster *c, uint32_t ms) {
    while (ms > 0) {
        uint32_t slice = ms > 1000 ? 1000 : ms;
        health_kick(c->health);
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
    health_kick(c->health);
}

static void set_reason(struct ntrip_caster *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_reason(struct ntrip_caster *c, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(c->last_disconnect, sizeof(c->last_disconnect), fmt, args);
    va_end(args);
}

// Send the SOURCE request and check the reply. Returns true when logged in.
static bool caster_login(struct ntrip_caster *c, const char *password, const char *mountpoint) {
    char *buffer = malloc(512);
    if (buffer == NULL) {
        set_reason(c, "out of memory");
        return false;
    }

    int len = snprintf(buffer, 512, "SOURCE %s /%s" NEWLINE
            "Source-Agent: NTRIP %s/%s" NEWLINE
            NEWLINE, password, mountpoint, NTRIP_SERVER_NAME, app_version());

    bool ok = false;
    if (write_all(c->sock, buffer, len) != ESP_OK) {
        set_reason(c, "could not send login: %s", strerror(errno));
        goto done;
    }

    // Read until the end of the status line (or timeout / buffer full)
    int got = 0;
    while (got < 511) {
        health_kick(c->health);
        int r = recv(c->sock, buffer + got, 511 - got, 0);
        if (r == 0) {
            set_reason(c, "caster closed connection during login");
            goto done;
        }
        if (r < 0) {
            set_reason(c, "no login response: %s", strerror(errno));
            goto done;
        }
        got += r;
        buffer[got] = '\0';
        if (strstr(buffer, "\r\n") || strchr(buffer, '\n')) break;
    }
    buffer[got] = '\0';

    char *status = extract_http_header(buffer, "");
    if (status == NULL) {
        // Tolerate a bare "\n" line ending
        char *nl = strchr(buffer, '\n');
        if (nl) *nl = '\0';
        status = strdup(buffer);
    }

    if (status != NULL && ntrip_response_ok(status)) {
        ok = true;
    } else {
        set_reason(c, "login refused: %.60s", status == NULL ? "malformed response" : status);
    }
    free(status);

    done:
    free(buffer);
    return ok;
}

typedef enum {
    SESSION_END_CASTER,
    SESSION_END_WIFI,
} session_end_t;

// Pump data until something goes wrong. Returns why the session ended.
static session_end_t caster_session(struct ntrip_caster *c) {
    const char *TAG = c->def->log_tag;
    uint32_t ip_generation = wifi_sta_ip_generation();
    uint32_t dropped_seen = c->ring.dropped_total + c->sink_lock_drops;
    uint32_t last_drop_log = 0;
    uint8_t rx[64];

    while (true) {
        health_kick(c->health);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CASTER_POLL_MS));

        // The route is gone; don't wait for TCP to notice
        if (wifi_sta_ip_generation() != ip_generation || !wifi_sta_has_ip()) {
            set_reason(c, "WiFi lost IP");
            return SESSION_END_WIFI;
        }

        // Detect the caster closing the connection or a socket error
        int r = recv(c->sock, rx, sizeof(rx), MSG_DONTWAIT);
        if (r == 0) {
            set_reason(c, "caster closed connection");
            return SESSION_END_CASTER;
        } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            set_reason(c, "socket error: %s", strerror(errno));
            return SESSION_END_CASTER;
        } else if (r > 0) {
            ESP_LOGD(TAG, "Ignoring %d bytes from caster", r);
        }

        size_t sent = 0;
        sender_result_t res = stream_sender_pump(&c->sender, caster_fill, caster_write, c, now_ms(), &sent);
        if (sent > 0) stream_stats_increment(c->stats, 0, sent);

        switch (res) {
            case SENDER_SLOW:
                ESP_LOGW(TAG, "Network slow: caster has accepted nothing for %u s (%u bytes buffered)",
                        SENDER_SLOW_MS / 1000, (unsigned) byte_ring_len(&c->ring));
                break;
            case SENDER_STALLED:
                set_reason(c, "send stalled for %u s", SENDER_STALL_MS / 1000);
                return SESSION_END_CASTER;
            case SENDER_ERROR:
                set_reason(c, "send error: %s", strerror(c->last_errno));
                return SESSION_END_CASTER;
            default:
                break;
        }

        // Report overflow (rate limited)
        uint32_t dropped = c->ring.dropped_total + c->sink_lock_drops;
        if (dropped != dropped_seen && now_ms() - last_drop_log >= CASTER_DROP_LOG_INTERVAL_MS) {
            ESP_LOGW(TAG, "Network slow: dropped %u bytes of RTCM (buffer full)", (unsigned) (dropped - dropped_seen));
            dropped_seen = dropped;
            last_drop_log = now_ms();
        }
    }
}

static void caster_task(void *ctx) {
    struct ntrip_caster *c = ctx;
    const ntrip_caster_def_t *def = c->def;
    const char *TAG = def->log_tag;

    c->health = health_register(def->name, HEALTH_DEFAULT_TIMEOUT_MS);

    backoff_t backoff;
    backoff_init(&backoff);

    while (true) {
        health_kick(c->health);

        // Only connect while the receiver is producing data
        if (!uart_recently_active(c)) {
            c->state = NTRIP_CASTER_WAITING_DATA;
            ESP_LOGI(TAG, "Waiting for UART input to connect to caster");
            uart_nmea("$PESP,NTRIP,%s,WAITING", def->nmea_id);
            while (!uart_recently_active(c)) {
                health_kick(c->health);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            }
            ESP_LOGI(TAG, "Data received by UART, will now connect to caster");
        }

        uint32_t delay = backoff_delay_ms(&backoff);
        if (delay > 0) {
            c->state = NTRIP_CASTER_BACKOFF;
            ESP_LOGI(TAG, "Reconnecting in %u s", (unsigned) (delay / 1000));
            sleep_ms(c, delay);
        }

        if (!wifi_sta_has_ip()) {
            c->state = NTRIP_CASTER_WAITING_WIFI;
            while (!wait_for_ip_timeout(1000)) health_kick(c->health);
        }

        c->state = NTRIP_CASTER_CONNECTING;

        char *host = NULL, *mountpoint = NULL, *password = NULL;
        uint16_t port = config_get_u16(config_get_item(def->key_port));
        config_get_str_blob_alloc(config_get_item(def->key_host), (void **) &host);
        config_get_str_blob_alloc(config_get_item(def->key_password), (void **) &password);
        config_get_str_blob_alloc(config_get_item(def->key_mountpoint), (void **) &mountpoint);
        if (host == NULL || password == NULL || mountpoint == NULL || host[0] == '\0') {
            set_reason(c, "caster not configured");
            ESP_LOGE(TAG, "Caster host/mountpoint not configured");
            backoff_attempt_failed(&backoff);
            c->failures++;
            goto cleanup;
        }

        ESP_LOGI(TAG, "Connecting to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,%s,CONNECTING,%s:%d,%s", def->nmea_id, host, port, mountpoint);

        health_kick(c->health);
        c->sock = connect_socket_timeout(host, port, CASTER_CONNECT_TIMEOUT_MS);
        health_kick(c->health);
        if (c->sock < 0) {
            if (c->sock == CONNECT_SOCKET_ERROR_RESOLVE) set_reason(c, "could not resolve host");
            else set_reason(c, "could not connect: %s", strerror(errno));
            c->sock = -1;
            ESP_LOGE(TAG, "Failed to connect to %s:%d/%s: %s", host, port, mountpoint, c->last_disconnect);
            backoff_attempt_failed(&backoff);
            c->failures++;
            goto cleanup;
        }

        socket_set_timeouts(c->sock, CASTER_HANDSHAKE_TIMEOUT_MS);
        if (socket_set_keepalive(c->sock, CASTER_KEEPALIVE_IDLE_S, CASTER_KEEPALIVE_INTERVAL_S,
                CASTER_KEEPALIVE_COUNT) != 0) {
            ESP_LOGW(TAG, "Could not enable TCP keepalive: %s", strerror(errno));
        }

        if (!caster_login(c, password, mountpoint)) {
            ESP_LOGE(TAG, "Failed to log in to %s:%d/%s: %s", host, port, mountpoint, c->last_disconnect);
            backoff_attempt_failed(&backoff);
            c->failures++;
            goto cleanup;
        }

        ESP_LOGI(TAG, "Successfully connected to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,%s,CONNECTED,%s:%d,%s", def->nmea_id, host, port, mountpoint);

        // Start streaming fresh data
        xSemaphoreTake(c->lock, portMAX_DELAY);
        byte_ring_clear(&c->ring);
        xSemaphoreGive(c->lock);
        stream_sender_reset(&c->sender, now_ms());
        c->accepting = true;
        c->sessions++;
        c->session_start_us = esp_timer_get_time();
        c->state = NTRIP_CASTER_CONNECTED;
        if (c->led != NULL) c->led->active = true;

        session_end_t end = caster_session(c);

        c->accepting = false;
        if (c->led != NULL) c->led->active = false;
        uint32_t session_ms = (uint32_t) ((esp_timer_get_time() - c->session_start_us) / 1000);
        c->session_start_us = 0;

        ESP_LOGW(TAG, "Disconnected from %s:%d/%s after %u s: %s", host, port, mountpoint,
                (unsigned) (session_ms / 1000), c->last_disconnect);
        uart_nmea("$PESP,NTRIP,%s,DISCONNECTED,%s:%d,%s", def->nmea_id, host, port, mountpoint);

        backoff_session_ended(&backoff, session_ms, end == SESSION_END_WIFI);

        cleanup:
        c->state = NTRIP_CASTER_BACKOFF;
        destroy_socket(&c->sock);
        free(host);
        free(mountpoint);
        free(password);
    }
}

ntrip_caster_handle_t ntrip_caster_start(const ntrip_caster_def_t *def) {
    if (!config_get_bool1(config_get_item(def->key_active))) return NULL;

    struct ntrip_caster *c = calloc(1, sizeof(struct ntrip_caster));
    uint8_t *storage = malloc(CASTER_RING_SIZE);
    if (c == NULL || storage == NULL) {
        ESP_LOGE(def->log_tag, "Out of memory starting %s", def->name);
        free(c);
        free(storage);
        return NULL;
    }

    c->def = def;
    c->sock = -1;
    c->lock = xSemaphoreCreateMutex();
    byte_ring_init(&c->ring, storage, CASTER_RING_SIZE);
    c->state = NTRIP_CASTER_WAITING_DATA;
    strcpy(c->last_disconnect, "");

    config_color_t led_color = config_get_color(config_get_item(def->key_color));
    if (led_color.rgba != 0) c->led = status_led_add(led_color.rgba, STATUS_LED_FADE, 500, 2000, 0);
    if (c->led != NULL) c->led->active = false;

    c->stats = stream_stats_new(def->name);

    xTaskCreate(caster_task, def->name, CASTER_TASK_STACK, c, TASK_PRIORITY_INTERFACE, &c->task);
    uart_add_read_sink(caster_uart_sink, c);

    return c;
}

void ntrip_caster_get_status(ntrip_caster_handle_t c, ntrip_caster_status_t *status) {
    memset(status, 0, sizeof(*status));
    if (c == NULL) {
        status->state = NTRIP_CASTER_DISABLED;
        return;
    }
    status->state = c->state;
    status->sessions = c->sessions;
    status->failures = c->failures;
    status->dropped_bytes = c->ring.dropped_total + c->sink_lock_drops;
    int64_t start = c->session_start_us;
    status->uptime_s = start ? (uint32_t) ((esp_timer_get_time() - start) / 1000000) : 0;
    strlcpy(status->last_disconnect, c->last_disconnect, sizeof(status->last_disconnect));
}
