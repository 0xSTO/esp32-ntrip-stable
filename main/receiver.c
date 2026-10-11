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
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "receiver.h"
#include "rtcm_framer.h"
#include "gnss_monitor.h"
#include "line_log.h"
#include "uart.h"

static const char *TAG = "RECEIVER";

#define COMMAND_MAX 200

static bool forward_rtcm_only = true;
static receiver_rtcm_out_t rtcm_out = NULL;

static rtcm_framer_t framer;          // only touched by the UART task
static gnss_monitor_t monitor;        // guarded by lock
static gnss_monitor_t snapshot;       // copy used to build JSON outside the lock (HTTP task only)
static line_log_t console;            // guarded by lock
static SemaphoreHandle_t lock = NULL;

static uint32_t now_ms(void) {
    return (uint32_t) (esp_timer_get_time() / 1000);
}

/* A standard NMEA sentence: $ + 2-letter talker + 3-letter type, valid checksum. */
static bool is_nmea(const char *line) {
    if (line[0] != '$') return false;
    for (int i = 1; i <= 5; i++) if (!isupper((unsigned char) line[i])) return false;
    return line[6] == ',' && gnss_nmea_checksum_ok(line);
}

static void on_frame(void *ctx, const uint8_t *frame, size_t len) {
    if (forward_rtcm_only && rtcm_out) rtcm_out(frame, len);
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        gnss_monitor_rtcm(&monitor, frame, len, now_ms());
        xSemaphoreGive(lock);
    }
}

static void on_line(void *ctx, const char *line, size_t len) {
    uint32_t t = now_ms();
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (is_nmea(line)) {
        // NMEA feeds the live view (sky plot) but would flood the console.
        gnss_monitor_line(&monitor, line, t);
    } else {
        line_log_add(&console, '<', line, t);
    }
    xSemaphoreGive(lock);
}

void receiver_init(bool rtcm_only, receiver_rtcm_out_t out) {
    forward_rtcm_only = rtcm_only;
    rtcm_out = out;
    lock = xSemaphoreCreateMutex();
    rtcm_framer_init(&framer, on_frame, on_line, NULL);
    gnss_monitor_init(&monitor);
    line_log_init(&console);
    if (!rtcm_only) line_log_add(&console, '!', "Casters receive the raw UART stream (RTCM-only filter is off)", now_ms());
    ESP_LOGI(TAG, "Receiver monitor started (casters get %s)", rtcm_only ? "RTCM only" : "raw stream");
}

void receiver_process(const uint8_t *data, size_t len) {
    if (!forward_rtcm_only && rtcm_out) rtcm_out(data, len);
    rtcm_framer_feed(&framer, data, len);
}

void receiver_idle(void) {
    rtcm_framer_flush(&framer);
}

const char *receiver_send_command(const char *cmd, bool confirmed) {
    if (lock == NULL) return "Receiver monitor not running";

    // Trim
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    size_t n = strlen(cmd);
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t' || cmd[n - 1] == '\r' || cmd[n - 1] == '\n')) n--;
    if (n == 0) return "Empty command";
    if (n > COMMAND_MAX) return "Command too long";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) cmd[i];
        if (c < 0x20 || c > 0x7E) return "Command contains invalid characters";
    }
    if (!confirmed && n >= 6 && strncasecmp(cmd, "freset", 6) == 0) {
        return "FRESET wipes all receiver settings and needs confirmation";
    }

    char line[COMMAND_MAX + 3];
    memcpy(line, cmd, n);
    line[n] = '\r';
    line[n + 1] = '\n';
    line[n + 2] = '\0';

    int written = uart_write(line, n + 2);

    if (xSemaphoreTake(lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        line[n] = '\0';
        line_log_add(&console, written == (int) (n + 2) ? '>' : '!',
                     written == (int) (n + 2) ? line : "Command not sent: UART transmit buffer full", now_ms());
        xSemaphoreGive(lock);
    }
    if (written != (int) (n + 2)) return "UART transmit buffer full, try again";
    ESP_LOGI(TAG, "Sent to receiver: %.*s", (int) n, cmd);
    return NULL;
}

int receiver_live_json(char *out, size_t size) {
    // Copy under the lock (fast), format afterwards so the UART task never
    // waits on JSON formatting.
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(200)) != pdTRUE) return -1;
    memcpy(&snapshot, &monitor, sizeof(snapshot));
    xSemaphoreGive(lock);
    int n = gnss_monitor_json(&snapshot, now_ms(), out, size);
    // Append framer statistics (outside the monitor, owned by the UART task;
    // reading three counters without the lock is fine for display).
    if (n > 1 && (size_t) n + 120 < size) {
        n--; // drop final '}'
        n += snprintf(out + n, size - (size_t) n,
                      ",\"stream\":{\"rtcm_only\":%s,\"frames\":%lu,\"lines\":%lu,\"garbage\":%lu}}",
                      forward_rtcm_only ? "true" : "false", (unsigned long) framer.frames,
                      (unsigned long) framer.lines, (unsigned long) framer.garbage_bytes);
    }
    return n;
}

int receiver_console_json(uint32_t since, char *out, size_t size) {
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(200)) != pdTRUE) return -1;
    int n = line_log_json(&console, since, now_ms(), out, size);
    xSemaphoreGive(lock);
    return n;
}
