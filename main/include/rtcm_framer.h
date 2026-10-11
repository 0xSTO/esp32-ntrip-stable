/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_RTCM_FRAMER_H
#define ESP32_NTRIP_RTCM_FRAMER_H

/*
 * Splits the byte stream coming from the GNSS receiver into:
 *   - complete RTCM 3 frames whose CRC-24Q checks out (forwarded to casters)
 *   - ASCII text lines starting with '$' or '#' (NMEA, command replies)
 * Anything else is counted as garbage and dropped.
 *
 * No RTOS dependencies so it can be unit tested on the host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTCM_FRAME_MAX   (3 + 1023 + 3)
#define RTCM_LINE_MAX    480

typedef void (*rtcm_frame_cb_t)(void *ctx, const uint8_t *frame, size_t len);
typedef void (*rtcm_line_cb_t)(void *ctx, const char *line, size_t len);

typedef struct rtcm_framer {
    uint8_t buf[RTCM_FRAME_MAX + 8];
    size_t len;
    uint8_t replay[RTCM_FRAME_MAX + 8]; // scratch for resynchronisation
    uint8_t flush[RTCM_FRAME_MAX + 8];  // scratch for rtcm_framer_flush

    rtcm_frame_cb_t on_frame;
    rtcm_line_cb_t on_line;
    void *ctx;

    uint32_t frames;        // valid RTCM frames
    uint32_t lines;         // text lines
    uint32_t garbage_bytes; // bytes that were neither
} rtcm_framer_t;

void rtcm_framer_init(rtcm_framer_t *f, rtcm_frame_cb_t on_frame, rtcm_line_cb_t on_line, void *ctx);
void rtcm_framer_feed(rtcm_framer_t *f, const uint8_t *data, size_t len);

/*
 * Call when the UART has been idle for a while. A corrupted header can claim a
 * frame longer than what actually arrived; without this, the good frames held
 * behind it would wait for the next burst (~1 s). Receivers send each message
 * contiguously, so after an idle gap nothing held can still be completing.
 */
void rtcm_framer_flush(rtcm_framer_t *f);

uint32_t rtcm_crc24q(const uint8_t *data, size_t len);

#endif // ESP32_NTRIP_RTCM_FRAMER_H
