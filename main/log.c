/*
 * This file is part of the ESP32-XBee distribution (https://github.com/nebkat/esp32-xbee).
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

#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <string.h>
#include <uart.h>
#include "log.h"

#define INITIAL_MAGIC "@@@@\n"

static const char *TAG = "LOG";

static RingbufHandle_t ringbuf_handle;

esp_err_t log_init() {
    ringbuf_handle = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);
    if (ringbuf_handle == NULL) {
        ESP_LOGE(TAG, "Could not create log ring buffer");
        return ESP_FAIL;
    }

    // Magic string to let web log know that ESP32 has restart (to reset line counter)
    xRingbufferSend(ringbuf_handle, INITIAL_MAGIC, strlen(INITIAL_MAGIC), 0);

    return ESP_OK;
}

#define LOG_LINE_MAX 512

// Strip ANSI colour codes and trailing newlines from a log line. out may equal in. Returns the new length. Works whether or not colours are enabled
// (the previous version assumed they were and could compute a negative length).
static size_t strip_for_web(const char *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\033' && i + 1 < len && in[i + 1] == '[') {
            i += 2;
            while (i < len && in[i] != 'm') i++;
            continue;
        }
        out[o++] = in[i];
    }
    while (o > 0 && (out[o - 1] == '\n' || out[o - 1] == '\r')) o--;
    return o;
}

int log_vprintf(const char * format, va_list arg) {
    char buffer[LOG_LINE_MAX];
    int n = vsnprintf(buffer, sizeof(buffer), format, arg);
    if (n < 0) return n;

    // vsnprintf returns the length it wanted, not what fits
    size_t len = (size_t) n < sizeof(buffer) ? (size_t) n : sizeof(buffer) - 1;

    // Strip in place (output never runs ahead of input) to keep stack use low:
    // this runs on whatever task logged, some of which have small stacks.
    len = strip_for_web(buffer, len, buffer);
    if (len < sizeof(buffer)) buffer[len++] = '\n';

    if (ringbuf_handle != NULL) xRingbufferSend(ringbuf_handle, buffer, len, 0);

    uart_log(buffer, len);

    return n;
}

void *log_receive(size_t *length, TickType_t ticksToWait) {
    return xRingbufferReceive(ringbuf_handle, length, ticksToWait);
}

void log_return(void *item) {
    vRingbufferReturnItem(ringbuf_handle, item);
}
