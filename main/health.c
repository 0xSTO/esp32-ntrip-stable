/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "health.h"
#include "tasks.h"

static const char *TAG = "HEALTH";

#define CAUSE_MAGIC 0x52435331u // "RCS1"
#define MAX_WATCHED 8

typedef struct {
    uint32_t magic;
    uint32_t checksum;
    char cause[120];
} restart_record_t;

static RTC_NOINIT_ATTR restart_record_t restart_record;

static char last_cause[sizeof(restart_record.cause)];
static bool have_last_cause = false;

typedef struct {
    const char *name;
    uint32_t timeout_ms;
    volatile int64_t last_kick_us;
} watched_t;

static watched_t watched[MAX_WATCHED];
static int watched_count = 0;
static portMUX_TYPE watched_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t record_checksum(const restart_record_t *r) {
    uint32_t sum = r->magic;
    for (size_t i = 0; i < sizeof(r->cause); i++) sum = sum * 31u + (uint8_t) r->cause[i];
    return sum;
}

static void health_task(void *ctx) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        int64_t now = esp_timer_get_time();
        for (int i = 0; i < watched_count; i++) {
            int64_t idle_ms = (now - watched[i].last_kick_us) / 1000;
            if (idle_ms > watched[i].timeout_ms) {
                health_restart("Watchdog: %s made no progress for %lld s", watched[i].name, idle_ms / 1000);
            }
        }
    }
}

void health_init() {
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT &&
            restart_record.magic == CAUSE_MAGIC && restart_record.checksum == record_checksum(&restart_record)) {
        memcpy(last_cause, restart_record.cause, sizeof(last_cause));
        last_cause[sizeof(last_cause) - 1] = '\0';
        have_last_cause = true;
    }
    memset(&restart_record, 0, sizeof(restart_record));

    xTaskCreate(health_task, "health", 3072, NULL, TASK_PRIORITY_HEALTH, NULL);
}

const char *health_restart_cause() {
    return have_last_cause ? last_cause : NULL;
}

health_handle_t health_register(const char *name, uint32_t timeout_ms) {
    health_handle_t handle = -1;
    taskENTER_CRITICAL(&watched_lock);
    if (watched_count < MAX_WATCHED) {
        handle = watched_count;
        watched[handle].name = name;
        watched[handle].timeout_ms = timeout_ms;
        watched[handle].last_kick_us = esp_timer_get_time();
        watched_count++;
    }
    taskEXIT_CRITICAL(&watched_lock);
    if (handle < 0) ESP_LOGE(TAG, "Too many watched tasks, %s not watched", name);
    return handle;
}

void health_kick(health_handle_t handle) {
    if (handle < 0 || handle >= MAX_WATCHED) return;
    watched[handle].last_kick_us = esp_timer_get_time();
}

void health_restart(const char *fmt, ...) {
    restart_record_t r = {0};
    va_list args;
    va_start(args, fmt);
    vsnprintf(r.cause, sizeof(r.cause), fmt, args);
    va_end(args);
    r.magic = CAUSE_MAGIC;
    r.checksum = record_checksum(&r);
    restart_record = r;

    ESP_LOGE(TAG, "Restarting: %s", r.cause);
    vTaskDelay(pdMS_TO_TICKS(500)); // let the log reach the web log / UART
    esp_restart();
}
