/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "recorder.h"
#include "byte_ring.h"
#include "tasks.h"

static const char *TAG = "RECORDER";

// Reserved once at boot: on the real board a 32 KB malloc when pressing Start
// failed ("Not enough memory to record"). 12 KB is ~6 s of RTCM, plenty for a
// browser on the local network.
#define RECORDER_RING_SIZE (12 * 1024)
#define RECORDER_CHUNK 2048
#define RECORDER_MAX_MINUTES (24 * 60)
#define RECORDER_TASK_STACK 4096

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static byte_ring_t ring;
static uint8_t ring_storage[RECORDER_RING_SIZE];
static volatile bool active = false;       // guarded by mux for feed/stop transitions
static volatile bool stop_requested = false;
static TaskHandle_t task = NULL;

// Status (written by the recorder task)
static volatile uint32_t bytes_sent = 0;
static volatile int64_t started_us = 0;
static volatile uint32_t duration_s = 0;
static volatile uint32_t dropped = 0;
static char last_result[64] = "";
static char filename[48] = "";

void recorder_feed(const uint8_t *frame, size_t len) {
    if (!active) return;
    taskENTER_CRITICAL(&mux);
    if (active) dropped += (uint32_t) byte_ring_write(&ring, frame, len);
    taskEXIT_CRITICAL(&mux);
    // No task notification here: the recorder task polls every 200 ms, which
    // avoids ever notifying a task that is in the middle of deleting itself.
}

static void recorder_task(void *arg) {
    httpd_req_t *req = arg;
    static uint8_t chunk[RECORDER_CHUNK];
    const char *result = "finished";
    int64_t end_us = started_us + (int64_t) duration_s * 1000000;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(200));

        bool done = stop_requested || esp_timer_get_time() >= end_us;

        // Drain everything buffered (also after stop, so the file ends cleanly)
        while (true) {
            taskENTER_CRITICAL(&mux);
            size_t n = byte_ring_read(&ring, chunk, sizeof(chunk));
            taskEXIT_CRITICAL(&mux);
            if (n == 0) break;
            if (httpd_resp_send_chunk(req, (const char *) chunk, (ssize_t) n) != ESP_OK) {
                result = "download stopped by the browser or network";
                done = true;
                goto out;
            }
            bytes_sent += (uint32_t) n;
        }
        if (done) {
            if (stop_requested) result = "stopped";
            break;
        }
    }

out:
    taskENTER_CRITICAL(&mux);
    active = false;
    taskEXIT_CRITICAL(&mux);

    httpd_resp_send_chunk(req, NULL, 0);   // end of file (ignored if the socket is gone)
    httpd_req_async_handler_complete(req);

    snprintf(last_result, sizeof(last_result), "%s", result);
    ESP_LOGI(TAG, "Recording %s: %u bytes, %u dropped", result, (unsigned) bytes_sent, (unsigned) dropped);

    task = NULL;
    vTaskDelete(NULL);
}

esp_err_t recorder_start_handler(httpd_req_t *req) {
    if (active || task != NULL) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "A recording is already running");
    }

    uint32_t minutes = 60;
    char query[48], value[12];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "minutes", value, sizeof(value)) == ESP_OK) {
        minutes = (uint32_t) strtoul(value, NULL, 10);
    }
    if (minutes < 1) minutes = 1;
    if (minutes > RECORDER_MAX_MINUTES) minutes = RECORDER_MAX_MINUTES;

    byte_ring_init(&ring, ring_storage, RECORDER_RING_SIZE);

    // File name: UTC start time if the clock is set (SNTP), else device uptime
    time_t now = time(NULL);
    if (now > 1600000000) {
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(filename, sizeof(filename), "base_%Y%m%d_%H%M%SZ.rtcm3", &tm);
    } else {
        snprintf(filename, sizeof(filename), "base_uptime%llus.rtcm3",
                 (unsigned long long) (esp_timer_get_time() / 1000000));
    }
    char disposition[96];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);

    httpd_req_t *async = NULL;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK) {
        snprintf(last_result, sizeof(last_result), "could not start (web server busy)");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start recording");
    }
    httpd_resp_set_type(async, "application/octet-stream");
    // Header values must stay valid until the first chunk is sent: keep them static
    static char disposition_hdr[96];
    memcpy(disposition_hdr, disposition, sizeof(disposition_hdr));
    httpd_resp_set_hdr(async, "Content-Disposition", disposition_hdr);
    httpd_resp_set_hdr(async, "Cache-Control", "no-store");

    bytes_sent = 0;
    dropped = 0;
    stop_requested = false;
    duration_s = minutes * 60;
    started_us = esp_timer_get_time();
    last_result[0] = '\0';

    taskENTER_CRITICAL(&mux);
    active = true;
    taskEXIT_CRITICAL(&mux);

    if (xTaskCreate(recorder_task, "recorder", RECORDER_TASK_STACK, async, TASK_PRIORITY_INTERFACE, &task) != pdPASS) {
        taskENTER_CRITICAL(&mux);
        active = false;
        taskEXIT_CRITICAL(&mux);
        task = NULL;
        snprintf(last_result, sizeof(last_result), "could not start (out of memory for task)");
        httpd_resp_send_err(async, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start recording");
        httpd_req_async_handler_complete(async);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Recording %s for %u min", filename, (unsigned) minutes);
    return ESP_OK;
}

esp_err_t recorder_stop_handler(httpd_req_t *req) {
    bool was_active = active;
    stop_requested = true;   // the recorder task notices within 200 ms
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, was_active ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Not recording\"}");
}

int recorder_status_json(char *out, size_t size) {
    uint32_t elapsed = started_us ? (uint32_t) ((esp_timer_get_time() - started_us) / 1000000) : 0;
    int n = snprintf(out, size,
                     ",\"recording\":{\"active\":%s,\"file\":\"%s\",\"bytes\":%lu,\"elapsed_s\":%lu,"
                     "\"duration_s\":%lu,\"dropped\":%lu,\"result\":\"%s\"}",
                     active ? "true" : "false", filename, (unsigned long) bytes_sent,
                     (unsigned long) (active ? elapsed : 0), (unsigned long) duration_s,
                     (unsigned long) dropped, last_result);
    return (n < 0 || (size_t) n >= size) ? -1 : n;
}
