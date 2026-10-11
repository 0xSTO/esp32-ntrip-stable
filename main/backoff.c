/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include "backoff.h"

static const uint32_t schedule_s[] = {0, 5, 5, 5, 10, 20, 40, 60, 120, 300};
#define SCHEDULE_LEN (sizeof(schedule_s) / sizeof(schedule_s[0]))

void backoff_init(backoff_t *b) {
    b->step = 0;
}

uint32_t backoff_delay_ms(const backoff_t *b) {
    uint8_t step = b->step < SCHEDULE_LEN ? b->step : SCHEDULE_LEN - 1;
    return schedule_s[step] * 1000u;
}

void backoff_attempt_failed(backoff_t *b) {
    if (b->step < SCHEDULE_LEN - 1) b->step++;
}

void backoff_session_ended(backoff_t *b, uint32_t session_ms, bool wifi_caused) {
    if (session_ms >= BACKOFF_STABLE_MS) {
        b->step = 1;
    } else if (wifi_caused) {
        // Leave the schedule where it is
        if (b->step == 0) b->step = 1;
    } else {
        // Short session: treat like a failed attempt
        if (b->step == 0) b->step = 1;
        backoff_attempt_failed(b);
    }
}
