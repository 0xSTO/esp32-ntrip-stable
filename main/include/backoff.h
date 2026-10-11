/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_BACKOFF_H
#define ESP32_NTRIP_BACKOFF_H

/*
 * Caster reconnect back-off. Pure logic (no RTOS), host tested.
 *
 * Delay schedule (seconds): 0, 5, 5, 5, 10, 20, 40, 60, 120, 300 (cap).
 *  - The very first connection after boot is immediate.
 *  - Each failed attempt (connect/login failure, or a session that ends
 *    before it was stable) moves one step along the schedule.
 *  - A session that stayed up for BACKOFF_STABLE_MS resets the schedule to
 *    the first reconnect step (5 s), so a one-off drop reconnects quickly
 *    without hammering the caster.
 *  - A session ended by the WiFi link losing its IP does not move the
 *    schedule: the network was at fault, not the caster.
 */

#include <stdbool.h>
#include <stdint.h>

#define BACKOFF_STABLE_MS (5u * 60u * 1000u)

typedef struct backoff {
    uint8_t step;
} backoff_t;

void backoff_init(backoff_t *b);

/* Delay to wait before the next connection attempt. */
uint32_t backoff_delay_ms(const backoff_t *b);

/* A connection attempt failed before a session was established. */
void backoff_attempt_failed(backoff_t *b);

/* An established session ended. */
void backoff_session_ended(backoff_t *b, uint32_t session_ms, bool wifi_caused);

#endif //ESP32_NTRIP_BACKOFF_H
