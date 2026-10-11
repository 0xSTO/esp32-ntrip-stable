/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_CASTER_H
#define ESP32_NTRIP_CASTER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * One outgoing NTRIP server (SOURCE) connection. ntrip_server.c and
 * ntrip_server_2.c each describe their config keys with this struct and share
 * the implementation in ntrip_caster.c.
 */
typedef struct ntrip_caster_def {
    const char *name;       // task and stream-stats name, e.g. "ntrip_server"
    const char *log_tag;    // ESP_LOG tag
    const char *nmea_id;    // $PESP,NTRIP,<id>,... e.g. "SRV"
    const char *key_active;
    const char *key_color;
    const char *key_host;
    const char *key_port;
    const char *key_mountpoint;
    const char *key_password;
} ntrip_caster_def_t;

typedef enum {
    NTRIP_CASTER_DISABLED,
    NTRIP_CASTER_WAITING_DATA,
    NTRIP_CASTER_BACKOFF,
    NTRIP_CASTER_WAITING_WIFI,
    NTRIP_CASTER_CONNECTING,
    NTRIP_CASTER_CONNECTED,
} ntrip_caster_state_t;

typedef struct ntrip_caster_status {
    ntrip_caster_state_t state;
    uint32_t sessions;          // successful logins since boot
    uint32_t failures;          // failed attempts since boot
    uint32_t dropped_bytes;     // RTCM dropped because the link was too slow
    uint32_t uptime_s;          // current session length
    char last_disconnect[96];   // reason for the last disconnect / failure
} ntrip_caster_status_t;

typedef struct ntrip_caster *ntrip_caster_handle_t;

ntrip_caster_handle_t ntrip_caster_start(const ntrip_caster_def_t *def);
void ntrip_caster_get_status(ntrip_caster_handle_t caster, ntrip_caster_status_t *status);
const char *ntrip_caster_state_name(ntrip_caster_state_t state);

#endif //ESP32_NTRIP_CASTER_H
