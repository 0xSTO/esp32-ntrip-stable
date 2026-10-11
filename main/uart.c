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

#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <string.h>
#include <protocol/nmea.h>
#include <stream_stats.h>

#include "uart.h"
#include "config.h"
#include "tasks.h"
#include "health.h"
#include "receiver.h"

static const char *TAG = "UART";

// Direct read sinks, called from uart_task. Sinks must not block: they only
// copy data into their own buffers. UART data used to be posted to the shared
// default event loop, where a slow consumer stalled WiFi events and the other
// caster as well.
#define UART_MAX_READ_SINKS 4
typedef struct {
    uart_read_sink_t fn;
    void *ctx;
} read_sink_t;
static read_sink_t read_sinks[UART_MAX_READ_SINKS];
static volatile int read_sink_count = 0;

void uart_add_read_sink(uart_read_sink_t fn, void *ctx) {
    if (read_sink_count >= UART_MAX_READ_SINKS) {
        ESP_LOGE(TAG, "Too many UART read sinks");
        return;
    }
    read_sinks[read_sink_count].fn = fn;
    read_sinks[read_sink_count].ctx = ctx;
    read_sink_count++;
}

static void dispatch_read(const uint8_t *data, size_t len) {
    // Called by the receiver module with RTCM frames (or the raw stream when
    // the RTCM-only filter is off).
    int count = read_sink_count;
    for (int i = 0; i < count; i++) read_sinks[i].fn(read_sinks[i].ctx, data, len);
}

static int uart_port = -1;
static volatile bool uart_ready = false;
static bool uart_log_forward = false;

static stream_stats_handle_t stream_stats;
static uint32_t uart_tx_dropped = 0;

static void uart_task(void *ctx);

void uart_init() {
    uart_log_forward = config_get_bool1(CONF_ITEM(KEY_CONFIG_UART_LOG_FORWARD));
    receiver_init(config_get_bool1(CONF_ITEM(KEY_CONFIG_UART_RTCM_ONLY)), dispatch_read);

    uart_port = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_NUM));

    uart_hw_flowcontrol_t flow_ctrl;
    bool flow_ctrl_rts = config_get_bool1(CONF_ITEM(KEY_CONFIG_UART_FLOW_CTRL_RTS));
    bool flow_ctrl_cts = config_get_bool1(CONF_ITEM(KEY_CONFIG_UART_FLOW_CTRL_CTS));
    if (flow_ctrl_rts && flow_ctrl_cts) {
        flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
    } else if (flow_ctrl_rts) {
        flow_ctrl = UART_HW_FLOWCTRL_RTS;
    } else if (flow_ctrl_cts) {
        flow_ctrl = UART_HW_FLOWCTRL_CTS;
    } else {
        flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    }

    uart_config_t uart_config = {
            .baud_rate = config_get_u32(CONF_ITEM(KEY_CONFIG_UART_BAUD_RATE)),
            .data_bits = config_get_i8(CONF_ITEM(KEY_CONFIG_UART_DATA_BITS)),
            .parity = config_get_i8(CONF_ITEM(KEY_CONFIG_UART_PARITY)),
            .stop_bits = config_get_i8(CONF_ITEM(KEY_CONFIG_UART_STOP_BITS)),
            .flow_ctrl = flow_ctrl
    };
    ESP_ERROR_CHECK(uart_param_config(uart_port, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(
            uart_port,
            config_get_u8(CONF_ITEM(KEY_CONFIG_UART_TX_PIN)),
            config_get_u8(CONF_ITEM(KEY_CONFIG_UART_RX_PIN)),
            config_get_u8(CONF_ITEM(KEY_CONFIG_UART_RTS_PIN)),
            config_get_u8(CONF_ITEM(KEY_CONFIG_UART_CTS_PIN))
    ));
    ESP_ERROR_CHECK(uart_driver_install(uart_port, UART_BUFFER_SIZE, UART_BUFFER_SIZE, 0, NULL, 0));
    uart_ready = true;

    stream_stats = stream_stats_new("uart");

    xTaskCreate(uart_task, "uart_task", 8192, NULL, TASK_PRIORITY_UART, NULL);
}

static void uart_task(void *ctx) {
    static uint8_t buffer[UART_BUFFER_SIZE];
    int idle_reads = 0;
    health_handle_t health = health_register("uart_task", HEALTH_DEFAULT_TIMEOUT_MS);

    while (true) {
        health_kick(health);

        int32_t len = uart_read_bytes(uart_port, buffer, sizeof(buffer), pdMS_TO_TICKS(50));
        if (len < 0) {
            ESP_LOGE(TAG, "Error reading from UART");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else if (len == 0) {
            // ~100 ms with no data: let the framer give up on anything held.
            if (++idle_reads == 2) receiver_idle();
            continue;
        }
        idle_reads = 0;

        stream_stats_increment(stream_stats, len, 0);

        // Split into RTCM (to casters) and text (NMEA / command replies).
        receiver_process(buffer, len);
    }
}

void uart_inject(void *buf, size_t len) {
    receiver_process(buf, len);
}

int uart_log(char *buf, size_t len) {
    if (!uart_log_forward) return 0;
    return uart_write(buf, len);
}

int uart_nmea(const char *fmt, ...) {
    // $PESP status sentences were meant for an XBee host device. Sent to a GNSS
    // receiver they only provoke "PARSING FAILD" replies, which then went to
    // the casters. Only send them when log forwarding is switched on.
    if (!uart_log_forward) return 0;

    va_list args;
    va_start(args, fmt);

    char *nmea = NULL;
    int l = 0;
    if (nmea_vasprintf(&nmea, fmt, args) >= 0 && nmea != NULL) {
        l = uart_write(nmea, strlen(nmea));
    }
    free(nmea);

    va_end(args);

    return l;
}

int uart_write(char *buf, size_t len) {
    if (!uart_ready) return 0;
    if (len == 0) return 0;

    // Never block: this is called from logging and from event handlers. If the
    // TX buffer is full (receiver not reading, or flow control holding us off)
    // drop the message instead of stalling the caller.
    size_t tx_free = 0;
    if (uart_get_tx_buffer_free_size(uart_port, &tx_free) != ESP_OK || tx_free < len) {
        uart_tx_dropped += len;
        return 0;
    }

    int written = uart_write_bytes(uart_port, buf, len);
    if (written < 0) return written;

    stream_stats_increment(stream_stats, 0, written);

    return written;
}
