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

// Thin wrapper: the connection logic lives in ntrip_caster.c and is shared by
// both servers (it used to be a copy-pasted file per server).

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "config.h"
#include "interface/ntrip.h"
#include "interface/ntrip_caster.h"

static const ntrip_caster_def_t def = {
        .name = "ntrip_server_2",
        .log_tag = "NTRIP_SERVER_2",
        .nmea_id = "SRV2",
        .key_active = KEY_CONFIG_NTRIP_SERVER_2_ACTIVE,
        .key_color = KEY_CONFIG_NTRIP_SERVER_2_COLOR,
        .key_host = KEY_CONFIG_NTRIP_SERVER_2_HOST,
        .key_port = KEY_CONFIG_NTRIP_SERVER_2_PORT,
        .key_mountpoint = KEY_CONFIG_NTRIP_SERVER_2_MOUNTPOINT,
        .key_password = KEY_CONFIG_NTRIP_SERVER_2_PASSWORD,
};

static ntrip_caster_handle_t handle = NULL;

void ntrip_server_2_init() {
    handle = ntrip_caster_start(&def);
}

ntrip_caster_handle_t ntrip_server_2_handle() {
    return handle;
}
