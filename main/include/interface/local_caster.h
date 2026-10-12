/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_LOCAL_CASTER_H
#define ESP32_NTRIP_LOCAL_CASTER_H

/*
 * Built-in NTRIP caster: rovers on the same network (home WiFi or the ESP32's
 * hotspot) connect straight to the base at <ip>:<port>/<mountpoint> and get
 * the same RTCM the casters get. Up to LOCAL_CASTER_MAX_CLIENTS at once.
 * Nothing is allocated unless it is switched on.
 */

#include <stdbool.h>
#include <stdint.h>

#define LOCAL_CASTER_MAX_CLIENTS 2

typedef struct {
    bool active;          // switched on in the settings
    bool listening;
    uint16_t port;
    char mountpoint[33];
    bool auth;            // username/password required
    char error[64];       // why it isn't listening
    uint32_t sessions;    // rovers served since boot
    uint32_t refused;     // turned away (full, bad request, wrong password)
    uint32_t dropped;     // bytes a slow rover missed
    uint32_t stack_free;  // caster task stack never used (bytes), for diagnostics
    int clients;
    struct {
        char peer[24];
        uint32_t uptime_s;
        uint32_t bytes;
        bool v2;
    } client[LOCAL_CASTER_MAX_CLIENTS];
} local_caster_status_t;

void local_caster_init(void);
void local_caster_get_status(local_caster_status_t *status);

#endif // ESP32_NTRIP_LOCAL_CASTER_H
