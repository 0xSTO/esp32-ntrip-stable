/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * Field layouts follow RTCM 10403.3 (MSM: DF393-DF420; 1005/1006; 1033).
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gnss_monitor.h"

/* ------------------------------------------------------------------ bits */

static uint64_t getbitu(const uint8_t *buf, size_t pos, int len) {
    uint64_t v = 0;
    for (int i = 0; i < len; i++, pos++) {
        v = (v << 1) | ((buf[pos >> 3] >> (7 - (pos & 7))) & 1u);
    }
    return v;
}

static int64_t getbits(const uint8_t *buf, size_t pos, int len) {
    uint64_t v = getbitu(buf, pos, len);
    if (len < 64 && (v & ((uint64_t) 1 << (len - 1)))) v |= ~(((uint64_t) 1 << len) - 1);
    return (int64_t) v;
}

/* ------------------------------------------------------------------ init */

void gnss_monitor_init(gnss_monitor_t *m) {
    memset(m, 0, sizeof(*m));
}

static gnss_sat_t *find_sat(gnss_monitor_t *m, char sys, uint8_t prn, bool create, uint32_t now_ms) {
    gnss_sat_t *free_slot = NULL, *oldest = NULL;
    uint32_t oldest_age = 0;
    for (int i = 0; i < GNSS_MAX_SATS; i++) {
        gnss_sat_t *s = &m->sats[i];
        if (s->sys == sys && s->prn == prn) {
            if (create) s->seen_ms = now_ms;
            return s;
        }
        if (s->sys == 0) {
            if (!free_slot) free_slot = s;
            continue;
        }
        uint32_t age = now_ms - s->seen_ms;
        if (!oldest || age > oldest_age) {
            oldest = s;
            oldest_age = age;
        }
    }
    if (!create) return NULL;
    gnss_sat_t *s = free_slot ? free_slot : oldest;
    memset(s, 0, sizeof(*s));
    s->sys = sys;
    s->prn = prn;
    s->seen_ms = now_ms;
    s->elev = -1;
    s->azim = -1;
    return s;
}

static void note_msg(gnss_monitor_t *m, uint16_t type, uint32_t now_ms) {
    gnss_msg_stat_t *slot = NULL;
    for (int i = 0; i < GNSS_MAX_MSG_TYPES; i++) {
        if (m->msgs[i].type == type) { slot = &m->msgs[i]; break; }
        if (m->msgs[i].type == 0 && !slot) slot = &m->msgs[i];
    }
    if (!slot) return;
    if (slot->type == type && slot->count > 0) {
        uint32_t dt = now_ms - slot->last_ms;
        // Several messages of one type can share an epoch (multiple-message bit):
        // ignore near-zero gaps for the interval estimate.
        if (dt >= 100) {
            slot->interval_ms = slot->interval_ms == 0 ? dt : (slot->interval_ms * 3 + dt) / 4;
        }
    }
    slot->type = type;
    slot->count++;
    slot->last_ms = now_ms;
}

/* ------------------------------------------------------------------ bands */

int gnss_band_of(char sys, int sig) {
    switch (sys) {
        case 'G':
            if ((sig >= 2 && sig <= 4) || (sig >= 30 && sig <= 32)) return 0;
            if ((sig >= 8 && sig <= 10) || (sig >= 15 && sig <= 17)) return 1;
            if (sig >= 22 && sig <= 24) return 2;
            return -1;
        case 'R':
            if (sig == 2 || sig == 3) return 0;
            if (sig == 8 || sig == 9) return 1;
            return -1;
        case 'E':
            if (sig >= 2 && sig <= 6) return 0;
            if (sig >= 8 && sig <= 12) return 3;   // E6
            if (sig >= 14 && sig <= 16) return 1;  // E5b
            if (sig >= 18 && sig <= 24) return 2;  // E5a+b, E5a
            return -1;
        case 'J':
            if (sig == 2 || (sig >= 30 && sig <= 32)) return 0;
            if (sig >= 9 && sig <= 11) return 3;   // L6 (LEX)
            if (sig >= 15 && sig <= 17) return 1;
            if (sig >= 22 && sig <= 24) return 2;
            return -1;
        case 'C':
            if ((sig >= 2 && sig <= 4) || (sig >= 30 && sig <= 32)) return 0; // B1I, B1C
            if (sig >= 8 && sig <= 10) return 3;   // B3I
            if (sig >= 14 && sig <= 16) return 1;  // B2I (7I/7Q/7X)
            if (sig == 25 || sig == 26) return 1;  // B2b
            if (sig >= 22 && sig <= 24) return 2;  // B2a
            return -1;
        case 'I':
            if (sig == 22) return 2;               // L5
            return -1;
        case 'S':
            if (sig == 2) return 0;
            if (sig >= 22 && sig <= 24) return 2;
            return -1;
        default:
            return -1;
    }
}

/* ------------------------------------------------------------------ MSM */

static char msm_system(uint16_t type, int *msm) {
    if (type < 1071 || type > 1137) return 0;
    int sub = type % 10;
    if (sub < 1 || sub > 7) return 0;
    *msm = sub;
    switch (type / 10) {
        case 107: return 'G';
        case 108: return 'R';
        case 109: return 'E';
        case 110: return 'S';
        case 111: return 'J';
        case 112: return 'C';
        case 113: return 'I';
        default: return 0;
    }
}

static void decode_msm(gnss_monitor_t *m, const uint8_t *p, size_t bits, char sys, int msm, uint32_t now_ms) {
    if (msm < 4) return;  // MSM1-3 carry no C/N0
    const size_t hdr = 169;
    if (bits < hdr) { m->msm_decode_errors++; return; }

    uint64_t sat_mask = getbitu(p, 73, 64);
    uint32_t sig_mask = (uint32_t) getbitu(p, 137, 32);

    int sats[64], nsat = 0;
    for (int i = 0; i < 64; i++) if (sat_mask & ((uint64_t) 1 << (63 - i))) sats[nsat++] = i + 1;
    int sigs[32], nsig = 0;
    for (int i = 0; i < 32; i++) if (sig_mask & (1u << (31 - i))) sigs[nsig++] = i + 1;
    if (nsat * nsig > 64) { m->msm_decode_errors++; return; }
    if (bits < hdr + (size_t) (nsat * nsig)) { m->msm_decode_errors++; return; }

    bool cell[64];
    int ncell = 0;
    for (int i = 0; i < nsat * nsig; i++) {
        cell[i] = getbitu(p, hdr + i, 1) != 0;
        if (cell[i]) ncell++;
    }

    // Satellite block size and the cell fields that precede C/N0.
    int sat_bits, before_cnr, cnr_bits, cell_bits;
    switch (msm) {
        case 4: sat_bits = 18; before_cnr = 15 + 22 + 4 + 1; cnr_bits = 6;  cell_bits = before_cnr + 6;        break;
        case 5: sat_bits = 36; before_cnr = 15 + 22 + 4 + 1; cnr_bits = 6;  cell_bits = before_cnr + 6 + 15;   break;
        case 6: sat_bits = 18; before_cnr = 20 + 24 + 10 + 1; cnr_bits = 10; cell_bits = before_cnr + 10;      break;
        default: sat_bits = 36; before_cnr = 20 + 24 + 10 + 1; cnr_bits = 10; cell_bits = before_cnr + 10 + 15; break;
    }
    size_t cell_start = hdr + (size_t) (nsat * nsig) + (size_t) nsat * sat_bits;
    if (bits < cell_start + (size_t) ncell * cell_bits) { m->msm_decode_errors++; return; }
    size_t cnr_pos = cell_start + (size_t) ncell * before_cnr;

    // Reset per-band data for every satellite in this message, then fill.
    gnss_sat_t *sp[64];
    for (int s = 0; s < nsat; s++) {
        uint8_t prn = (uint8_t) sats[s];
        sp[s] = find_sat(m, sys, prn, true, now_ms);
        memset(sp[s]->sig_count, 0, sizeof(sp[s]->sig_count));
        memset(sp[s]->cnr_sum, 0, sizeof(sp[s]->cnr_sum));
        sp[s]->cnr_max16 = 0;
    }

    int k = 0;
    for (int s = 0; s < nsat; s++) {
        bool any = false;
        for (int g = 0; g < nsig; g++) {
            if (!cell[s * nsig + g]) continue;
            uint32_t raw = (uint32_t) getbitu(p, cnr_pos + (size_t) k * cnr_bits, cnr_bits);
            k++;
            uint32_t cnr16 = cnr_bits == 6 ? raw * 16 : raw;  // MSM4/5: 1 dB-Hz; MSM6/7: 1/16 dB-Hz
            if (cnr16 == 0) continue;                       // not tracked
            any = true;
            int band = gnss_band_of(sys, sigs[g]);
            if (band >= 0) {
                sp[s]->sig_count[band]++;
                sp[s]->cnr_sum[band] += (uint16_t) cnr16;
            }
            if (cnr16 > sp[s]->cnr_max16) sp[s]->cnr_max16 = (uint16_t) cnr16;
        }
        if (any) sp[s]->msm_ms = now_ms;
    }
}

/* ------------------------------------------------------------------ 1005/1006, 1033/1007/1008 */

static void decode_position(gnss_monitor_t *m, const uint8_t *p, size_t bits, uint16_t type, uint32_t now_ms) {
    if (bits < (type == 1006 ? 168u : 152u)) return;
    double x = (double) getbits(p, 34, 38) * 0.0001;
    double y = (double) getbits(p, 74, 38) * 0.0001;
    double z = (double) getbits(p, 114, 38) * 0.0001;
    // Receivers send all-zero (or junk) positions before a survey-in completes.
    // Only accept something near the Earth's surface; it also keeps NaN out of the JSON.
    double r = sqrt(x * x + y * y + z * z);
    if (!(r > 6.2e6 && r < 6.5e6) || sqrt(x * x + y * y) < 1.0) return;
    m->station_id = (uint16_t) getbitu(p, 12, 12);
    m->ecef[0] = x;
    m->ecef[1] = y;
    m->ecef[2] = z;
    m->ant_height = type == 1006 ? (double) getbitu(p, 152, 16) * 0.0001 : 0;
    m->have_position = true;
    m->position_ms = now_ms;
}

/* Read an 8-bit counted string; returns new bit position or 0 on overrun. */
static size_t read_string(const uint8_t *p, size_t bits, size_t pos, char *out, size_t out_size) {
    if (pos + 8 > bits) return 0;
    size_t n = (size_t) getbitu(p, pos, 8);
    pos += 8;
    if (pos + n * 8 > bits) return 0;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        char c = (char) getbitu(p, pos + i * 8, 8);
        if (w + 1 < out_size && c >= 0x20 && c <= 0x7E && c != '"' && c != '\\') out[w++] = c;
    }
    out[w] = '\0';
    return pos + n * 8;
}

static void decode_descriptor(gnss_monitor_t *m, const uint8_t *p, size_t bits, uint16_t type) {
    size_t pos = 24;
    char tmp[32];
    pos = read_string(p, bits, pos, m->antenna, sizeof(m->antenna));   // antenna descriptor
    if (!pos) return;
    pos += 8;                                                          // setup ID
    if (type == 1007) return;
    pos = read_string(p, bits, pos, m->antenna_serial, sizeof(m->antenna_serial));
    if (!pos || type == 1008) return;
    pos = read_string(p, bits, pos, tmp, sizeof(tmp));
    if (!pos) return;
    memcpy(m->receiver, tmp, sizeof(tmp));
    pos = read_string(p, bits, pos, m->firmware, sizeof(m->firmware));
    if (!pos) return;
    read_string(p, bits, pos, m->receiver_serial, sizeof(m->receiver_serial));
}

void gnss_monitor_rtcm(gnss_monitor_t *m, const uint8_t *frame, size_t len, uint32_t now_ms) {
    if (len < 6 + 2) return;
    size_t payload_len = ((size_t) (frame[1] & 0x03) << 8) | frame[2];
    if (payload_len + 6 != len || payload_len < 2) return;
    const uint8_t *p = frame + 3;
    size_t bits = payload_len * 8;
    uint16_t type = (uint16_t) getbitu(p, 0, 12);

    m->rtcm_frames++;
    m->rtcm_bytes += (uint32_t) len;
    m->last_rtcm_ms = now_ms;
    note_msg(m, type, now_ms);

    int msm;
    char sys = msm_system(type, &msm);
    if (sys) {
        decode_msm(m, p, bits, sys, msm, now_ms);
    } else if (type == 1005 || type == 1006) {
        decode_position(m, p, bits, type, now_ms);
    } else if (type == 1033 || type == 1007 || type == 1008) {
        decode_descriptor(m, p, bits, type);
    }
}

/* ------------------------------------------------------------------ NMEA GSV */

bool gnss_nmea_checksum_ok(const char *line) {
    if (line[0] != '$') return false;
    const char *star = strrchr(line, '*');
    if (!star || strlen(star) < 3) return false;
    uint8_t sum = 0;
    for (const char *c = line + 1; c < star; c++) sum ^= (uint8_t) *c;
    char *end;
    long want = strtol(star + 1, &end, 16);
    return end == star + 3 && (uint8_t) want == sum;
}

/* Map a GSV talker + PRN to (system, number). Returns 0 if unknown. */
static char gsv_system(const char *talker, int prn, int *num) {
    char sys = 0;
    if (!strncmp(talker, "GP", 2)) sys = 'G';
    else if (!strncmp(talker, "GL", 2)) sys = 'R';
    else if (!strncmp(talker, "GA", 2)) sys = 'E';
    else if (!strncmp(talker, "GB", 2) || !strncmp(talker, "BD", 2)) sys = 'C';
    else if (!strncmp(talker, "GQ", 2) || !strncmp(talker, "QZ", 2)) sys = 'J';
    else if (!strncmp(talker, "GI", 2)) sys = 'I';
    else if (!strncmp(talker, "GN", 2)) {
        // Combined talker: fall back to the NMEA PRN ranges.
        if (prn >= 1 && prn <= 32) sys = 'G';
        else if (prn >= 65 && prn <= 96) sys = 'R';
        else if (prn >= 193 && prn <= 202) sys = 'J';
        else if (prn >= 201 && prn <= 263) sys = 'C';
        else if (prn >= 301 && prn <= 336) sys = 'E';
    }
    if (!sys) return 0;

    int n = prn;
    if (sys == 'R' && n > 64) n -= 64;
    if (sys == 'J' && n > 192) n -= 192;
    if (sys == 'C' && n > 400) n -= 400;
    if (sys == 'C' && n > 200) n -= 200;
    if (sys == 'E' && n > 300) n -= 300;
    if (sys == 'G' && n > 32) return 0;   // SBAS etc.
    if (n < 1 || n > 64) return 0;
    *num = n;
    return sys;
}

bool gnss_monitor_line(gnss_monitor_t *m, const char *line, uint32_t now_ms) {
    if (!gnss_nmea_checksum_ok(line)) return false;
    m->nmea_lines++;

    // $xxGSV,total,num,inview,(prn,elev,azim,snr)*[,signal]*hh
    if (strlen(line) < 7 || strncmp(line + 3, "GSV,", 4) != 0) return true;

    char talker[3] = {line[1], line[2], 0};
    char buf[512];
    size_t n = strcspn(line, "*");
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';

    // Split on commas, keeping empty fields.
    char *fields[32];
    int nf = 0;
    char *s = buf;
    while (nf < 32) {
        fields[nf++] = s;
        char *comma = strchr(s, ',');
        if (!comma) break;
        *comma = '\0';
        s = comma + 1;
    }
    // fields[0]=$xxGSV, 1=total, 2=num, 3=inview, then groups of 4
    for (int i = 4; i + 3 < nf; i += 4) {
        if (fields[i][0] == '\0') continue;
        int prn = atoi(fields[i]);
        int num;
        char sys = gsv_system(talker, prn, &num);
        if (!sys) continue;
        gnss_sat_t *sat = find_sat(m, sys, (uint8_t) num, true, now_ms);
        uint32_t prev = sat->gsv_ms;
        sat->gsv_ms = now_ms;
        sat->elev = fields[i + 1][0] ? (int8_t) atoi(fields[i + 1]) : -1;
        sat->azim = fields[i + 2][0] ? (int16_t) atoi(fields[i + 2]) : -1;
        int snr = fields[i + 3][0] ? atoi(fields[i + 3]) : 0;
        // A satellite appears once per signal in NMEA 4.11: keep the best SNR of this burst.
        if (prev == 0 || now_ms - prev > 1000 || snr > sat->gsv_snr) sat->gsv_snr = (uint8_t) snr;
    }
    return true;
}

/* ------------------------------------------------------------------ geodesy */

void gnss_ecef_to_llh(const double r[3], double *lat_deg, double *lon_deg, double *h_m) {
    const double a = 6378137.0, f = 1.0 / 298.257223563;
    const double e2 = f * (2 - f);
    double p = sqrt(r[0] * r[0] + r[1] * r[1]);
    double lon = atan2(r[1], r[0]);
    double lat = atan2(r[2], p * (1 - e2));
    double h = 0;
    for (int i = 0; i < 10; i++) {
        double sl = sin(lat);
        double N = a / sqrt(1 - e2 * sl * sl);
        h = p / cos(lat) - N;
        lat = atan2(r[2], p * (1 - e2 * N / (N + h)));
    }
    *lat_deg = lat * 180.0 / M_PI;
    *lon_deg = lon * 180.0 / M_PI;
    *h_m = h;
}

/* ------------------------------------------------------------------ JSON */

typedef struct {
    char *out;
    size_t size;
    size_t len;
    bool overflow;
} jbuf_t;

static void jprintf(jbuf_t *j, const char *fmt, ...) {
    if (j->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->out + j->len, j->size - j->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t) n >= j->size - j->len) {
        j->overflow = true;
        return;
    }
    j->len += (size_t) n;
}

static const char SYSTEMS[] = "GREJCIS";

int gnss_monitor_json(const gnss_monitor_t *m, uint32_t now_ms, char *out, size_t size) {
    jbuf_t j = {out, size, 0, false};
    if (size == 0) return -1;
    out[0] = '\0';

    // Aggregate
    int total_sats = 0, total_sigs = 0;
    uint32_t band_sum[GNSS_BANDS] = {0}, band_n[GNSS_BANDS] = {0};
    struct { int sats; int sigs[GNSS_BANDS]; } per_sys[sizeof(SYSTEMS) - 1];
    memset(per_sys, 0, sizeof(per_sys));

    for (int i = 0; i < GNSS_MAX_SATS; i++) {
        const gnss_sat_t *s = &m->sats[i];
        if (!s->sys || s->msm_ms == 0 || now_ms - s->msm_ms > GNSS_SAT_TIMEOUT_MS) continue;
        const char *pos = strchr(SYSTEMS, s->sys);
        int si = pos ? (int) (pos - SYSTEMS) : -1;
        total_sats++;
        if (si >= 0) per_sys[si].sats++;
        for (int b = 0; b < GNSS_BANDS; b++) {
            total_sigs += s->sig_count[b];
            band_sum[b] += s->cnr_sum[b];
            band_n[b] += s->sig_count[b];
            if (si >= 0) per_sys[si].sigs[b] += s->sig_count[b];
        }
    }
    uint32_t all_sum = 0, all_n = 0;
    for (int b = 0; b < GNSS_BANDS; b++) { all_sum += band_sum[b]; all_n += band_n[b]; }

    bool live = m->last_rtcm_ms != 0 && now_ms - m->last_rtcm_ms < GNSS_SAT_TIMEOUT_MS;
    jprintf(&j, "{\"live\":%s,\"rtcm_age_ms\":%lu,\"satellites\":%d,\"signals\":%d,\"mean_cnr\":%.1f",
            live ? "true" : "false",
            (unsigned long) (m->last_rtcm_ms ? now_ms - m->last_rtcm_ms : 0),
            total_sats, total_sigs, all_n ? all_sum / 16.0 / all_n : 0.0);

    jprintf(&j, ",\"bands\":[");
    for (int b = 0; b < GNSS_BANDS; b++) {
        jprintf(&j, "%s{\"signals\":%lu,\"mean_cnr\":%.1f}", b ? "," : "",
                (unsigned long) band_n[b], band_n[b] ? band_sum[b] / 16.0 / band_n[b] : 0.0);
    }
    jprintf(&j, "],\"systems\":[");
    bool first = true;
    for (size_t si = 0; si < sizeof(SYSTEMS) - 1; si++) {
        if (per_sys[si].sats == 0) continue;
        jprintf(&j, "%s{\"sys\":\"%c\",\"sats\":%d,\"sigs\":[%d,%d,%d,%d]}", first ? "" : ",", SYSTEMS[si],
                per_sys[si].sats, per_sys[si].sigs[0], per_sys[si].sigs[1], per_sys[si].sigs[2], per_sys[si].sigs[3]);
        first = false;
    }

    // Satellites: tracked (MSM) or recently in view (GSV)
    jprintf(&j, "],\"sats\":[");
    first = true;
    for (int i = 0; i < GNSS_MAX_SATS; i++) {
        const gnss_sat_t *s = &m->sats[i];
        if (!s->sys) continue;
        bool tracked = s->msm_ms != 0 && now_ms - s->msm_ms <= GNSS_SAT_TIMEOUT_MS;
        bool in_view = s->gsv_ms != 0 && now_ms - s->gsv_ms <= GNSS_GSV_TIMEOUT_MS;
        if (!tracked && !in_view) continue;
        double cnr = tracked ? s->cnr_max16 / 16.0 : (double) s->gsv_snr;
        jprintf(&j, "%s{\"id\":\"%c%02u\",\"t\":%d,\"cnr\":%.1f,\"el\":%d,\"az\":%d,\"b\":[%u,%u,%u,%u]}",
                first ? "" : ",", s->sys, s->prn, tracked ? 1 : 0, cnr,
                in_view ? s->elev : -1, in_view ? s->azim : -1,
                tracked ? s->sig_count[0] : 0, tracked ? s->sig_count[1] : 0,
                tracked ? s->sig_count[2] : 0, tracked ? s->sig_count[3] : 0);
        first = false;
    }

    // Message types
    jprintf(&j, "],\"messages\":[");
    first = true;
    for (int i = 0; i < GNSS_MAX_MSG_TYPES; i++) {
        const gnss_msg_stat_t *ms = &m->msgs[i];
        if (!ms->type) continue;
        jprintf(&j, "%s{\"type\":%u,\"count\":%lu,\"interval_ms\":%lu,\"age_ms\":%lu}", first ? "" : ",",
                ms->type, (unsigned long) ms->count, (unsigned long) ms->interval_ms,
                (unsigned long) (now_ms - ms->last_ms));
        first = false;
    }
    jprintf(&j, "]");

    // Base position
    if (m->have_position) {
        double lat, lon, h;
        gnss_ecef_to_llh(m->ecef, &lat, &lon, &h);
        jprintf(&j, ",\"position\":{\"station\":%u,\"lat\":%.8f,\"lon\":%.8f,\"height\":%.3f,"
                    "\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"antenna_height\":%.4f,\"age_ms\":%lu}",
                m->station_id, lat, lon, h, m->ecef[0], m->ecef[1], m->ecef[2], m->ant_height,
                (unsigned long) (now_ms - m->position_ms));
    }

    jprintf(&j, ",\"receiver\":{\"type\":\"%s\",\"firmware\":\"%s\",\"serial\":\"%s\","
                "\"antenna\":\"%s\",\"antenna_serial\":\"%s\"}",
            m->receiver, m->firmware, m->receiver_serial, m->antenna, m->antenna_serial);
    jprintf(&j, ",\"counters\":{\"rtcm_frames\":%lu,\"rtcm_bytes\":%lu,\"nmea_lines\":%lu,\"msm_errors\":%lu}}",
            (unsigned long) m->rtcm_frames, (unsigned long) m->rtcm_bytes,
            (unsigned long) m->nmea_lines, (unsigned long) m->msm_decode_errors);

    return j.overflow ? -1 : (int) j.len;
}
