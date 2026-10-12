/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_RECEIVER_H
#define ESP32_NTRIP_RECEIVER_H

/*
 * GNSS receiver side channel: splits the UART stream into RTCM (for the
 * casters) and text (NMEA / command replies), keeps the live view and the
 * console log, and sends commands typed on the web page to the receiver.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*receiver_rtcm_out_t)(const uint8_t *data, size_t len);

void receiver_init(bool rtcm_only, receiver_rtcm_out_t out);

/* Called from the UART task with every chunk read. */
void receiver_process(const uint8_t *data, size_t len);

/* Called from the UART task when no data has arrived for ~100 ms. */
void receiver_idle(void);

/* Send one command line (CR LF appended). Returns NULL on success or an error message. */
const char *receiver_send_command(const char *cmd, bool confirmed);

/* JSON snapshots into a caller buffer. Return length or -1. */
int receiver_live_json(char *out, size_t size);

/* Announced base position (RTCM 1005/1006), if one has been received. */
bool receiver_position(double *lat_deg, double *lon_deg);
int receiver_console_json(uint32_t since, char *out, size_t size);

#endif // ESP32_NTRIP_RECEIVER_H
