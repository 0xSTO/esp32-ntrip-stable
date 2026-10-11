/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_GNSS_MONITOR_H
#define ESP32_NTRIP_GNSS_MONITOR_H

/*
 * Builds a "live view" of the base station from what the receiver sends:
 *   - RTCM MSM4..7 observations: satellites, signals, C/N0 per band
 *   - RTCM 1005/1006: announced base position
 *   - RTCM 1033 / 1007 / 1008: receiver and antenna descriptions
 *   - NMEA GSV: satellite elevation/azimuth for the sky view
 *
 * Pure C, no RTOS: callers pass the current time and provide their own lock.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GNSS_MAX_SATS      160
#define GNSS_MAX_MSG_TYPES 24
#define GNSS_BANDS         4   // 0: L1/E1/B1  1: L2/E5b/B2I  2: L5/E5a/B2a  3: L6/E6/B3
#define GNSS_SAT_TIMEOUT_MS 5000
#define GNSS_GSV_TIMEOUT_MS 30000

typedef struct {
    char sys;                // 'G' GPS, 'R' GLONASS, 'E' Galileo, 'J' QZSS, 'C' BeiDou, 'I' NavIC, 'S' SBAS
    uint8_t prn;
    uint32_t msm_ms;         // last time seen in an MSM message
    uint32_t gsv_ms;         // last time seen in GSV
    uint32_t seen_ms;        // last time touched at all (for slot reuse)
    int8_t elev;             // degrees, from GSV
    int16_t azim;            // degrees, from GSV
    uint8_t gsv_snr;         // dB-Hz, from GSV
    uint8_t sig_count[GNSS_BANDS];   // signals tracked per band (last MSM)
    uint16_t cnr_sum[GNSS_BANDS];    // sum of C/N0 per band, 1/16 dB-Hz units
    uint16_t cnr_max16;              // best C/N0 of any signal, 1/16 dB-Hz
} gnss_sat_t;

typedef struct {
    uint16_t type;
    uint32_t count;
    uint32_t last_ms;
    uint32_t interval_ms;    // smoothed time between messages
} gnss_msg_stat_t;

typedef struct gnss_monitor {
    gnss_sat_t sats[GNSS_MAX_SATS];
    gnss_msg_stat_t msgs[GNSS_MAX_MSG_TYPES];

    bool have_position;
    double ecef[3];          // metres
    double ant_height;       // metres (1006)
    uint16_t station_id;
    uint32_t position_ms;

    char antenna[32];
    char antenna_serial[32];
    char receiver[32];
    char firmware[32];
    char receiver_serial[32];

    uint32_t rtcm_frames;
    uint32_t rtcm_bytes;
    uint32_t nmea_lines;
    uint32_t msm_decode_errors;
    uint32_t last_rtcm_ms;
} gnss_monitor_t;

void gnss_monitor_init(gnss_monitor_t *m);

/* A complete RTCM 3 frame (0xD3 header ... CRC), already CRC-checked. */
void gnss_monitor_rtcm(gnss_monitor_t *m, const uint8_t *frame, size_t len, uint32_t now_ms);

/* One text line (no CR/LF). Returns true if it was a valid NMEA sentence. */
bool gnss_monitor_line(gnss_monitor_t *m, const char *line, uint32_t now_ms);

/* Live view as JSON. Returns bytes written (excluding NUL), or -1 if out was too small. */
int gnss_monitor_json(const gnss_monitor_t *m, uint32_t now_ms, char *out, size_t size);

/* Helpers exposed for tests. */
int gnss_band_of(char sys, int signal_id);
void gnss_ecef_to_llh(const double ecef[3], double *lat_deg, double *lon_deg, double *h_m);
bool gnss_nmea_checksum_ok(const char *line);

#endif // ESP32_NTRIP_GNSS_MONITOR_H
