/*
 * Host driver for rtcm_framer + gnss_monitor + line_log.
 *
 *   receiver_driver unit
 *       built-in unit tests (framer edge cases, line log, geodesy)
 *   receiver_driver stream <stream.bin> <outdir> <chunk_seed>
 *       feeds the recorded stream in random-sized chunks and writes
 *       outdir/frames.bin, outdir/lines.txt and outdir/live.json for
 *       check_receiver.py to compare against pyrtcm's decode.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtcm_framer.h"
#include "gnss_monitor.h"
#include "line_log.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ------------------------------------------------------------- collectors */

typedef struct {
    uint8_t frames[200000];
    size_t frames_len;
    int nframes;
    size_t frame_off[2000];
    size_t frame_len[2000];
    char lines[64][RTCM_LINE_MAX + 1];
    int nlines;
} sink_t;

static void on_frame(void *ctx, const uint8_t *f, size_t len) {
    sink_t *s = ctx;
    if (s->frames_len + len > sizeof(s->frames) || s->nframes >= 2000) { failures++; return; }
    s->frame_off[s->nframes] = s->frames_len;
    s->frame_len[s->nframes] = len;
    s->nframes++;
    memcpy(s->frames + s->frames_len, f, len);
    s->frames_len += len;
}

static void on_line(void *ctx, const char *line, size_t len) {
    sink_t *s = ctx;
    if (s->nlines < 64) {
        memcpy(s->lines[s->nlines], line, len);
        s->lines[s->nlines][len] = '\0';
        s->nlines++;
    }
}

static size_t make_frame(uint8_t *out, const uint8_t *payload, size_t n) {
    out[0] = 0xD3;
    out[1] = (uint8_t) ((n >> 8) & 3);
    out[2] = (uint8_t) (n & 0xFF);
    memcpy(out + 3, payload, n);
    uint32_t crc = rtcm_crc24q(out, n + 3);
    out[n + 3] = (uint8_t) (crc >> 16);
    out[n + 4] = (uint8_t) (crc >> 8);
    out[n + 5] = (uint8_t) crc;
    return n + 6;
}

/* ------------------------------------------------------------- unit tests */

static void test_framer_units(void) {
    static sink_t s;
    static rtcm_framer_t f;
    uint8_t payload[200], fr[300], stream[2000];

    for (int i = 0; i < 200; i++) payload[i] = (uint8_t) (i * 7 + 3);
    payload[0] = 0x3E; payload[1] = 0xD0;  // message 1005-ish header bits
    size_t flen = make_frame(fr, payload, 100);

    // CRC known value: CRC-24Q of "123456789" is 0xCDE703
    CHECK(rtcm_crc24q((const uint8_t *) "123456789", 9) == 0xCDE703);

    // 1. single frame, byte by byte
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    for (size_t i = 0; i < flen; i++) rtcm_framer_feed(&f, fr + i, 1);
    CHECK(s.nframes == 1 && s.frame_len[0] == flen && memcmp(s.frames, fr, flen) == 0);

    // 2. corrupted frame between two good ones: only the bad one is lost
    size_t n = 0;
    memcpy(stream + n, fr, flen); n += flen;
    memcpy(stream + n, fr, flen); stream[n + 50] ^= 0x10; n += flen;
    memcpy(stream + n, fr, flen); n += flen;
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    rtcm_framer_feed(&f, stream, n);
    CHECK(s.nframes == 2);
    CHECK(f.garbage_bytes == flen);

    // 3. truncated frame (length says 100, only 40 bytes) followed by a good frame:
    //    the good frame starts inside what the framer is holding and must be found
    n = 0;
    memcpy(stream + n, fr, 40); n += 40;
    memcpy(stream + n, fr, flen); n += flen;
    memcpy(stream + n, fr, flen); n += flen;
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    rtcm_framer_feed(&f, stream, n);
    CHECK(s.nframes == 2);
    CHECK(s.nframes >= 1 && memcmp(s.frames, fr, flen) == 0);

    // 4. text lines, CRLF stripped, interleaved with frames
    n = 0;
    const char *l1 = "$command,VERSION,response: OK*45\r\n";
    const char *l2 = "#VERSION,97,GPS,FINE;UM980*abcd\n";
    memcpy(stream + n, l1, strlen(l1)); n += strlen(l1);
    memcpy(stream + n, fr, flen); n += flen;
    memcpy(stream + n, l2, strlen(l2)); n += strlen(l2);
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    rtcm_framer_feed(&f, stream, n);
    CHECK(s.nframes == 1 && s.nlines == 2);
    CHECK(strcmp(s.lines[0], "$command,VERSION,response: OK*45") == 0);
    CHECK(strcmp(s.lines[1], "#VERSION,97,GPS,FINE;UM980*abcd") == 0);

    // 5. binary junk containing '$' right before a real line: line must survive intact
    n = 0;
    const uint8_t junk[] = {0x01, '$', 'A', 'B', 0x7F, '$', 'x', 'y'};
    memcpy(stream + n, junk, sizeof(junk)); n += sizeof(junk);
    memcpy(stream + n, l1, strlen(l1)); n += strlen(l1);
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    rtcm_framer_feed(&f, stream, n);
    CHECK(s.nlines == 1 && strcmp(s.lines[0], "$command,VERSION,response: OK*45") == 0);

    // 6. overlong line is dropped, following frame still found
    n = 0;
    stream[n++] = '$';
    for (int i = 0; i < RTCM_LINE_MAX + 20; i++) stream[n++] = 'A';
    memcpy(stream + n, fr, flen); n += flen;
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    rtcm_framer_feed(&f, stream, n);
    CHECK(s.nlines == 0 && s.nframes == 1);

    // 7. maximum length frame (1023 byte payload)
    static uint8_t big[1100], bigp[1023];
    for (int i = 0; i < 1023; i++) bigp[i] = (uint8_t) (i ^ 0x5A);
    size_t blen = make_frame(big, bigp, 1023);
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    rtcm_framer_feed(&f, big, blen);
    CHECK(s.nframes == 1 && s.frame_len[0] == 1029);
}

static void test_framer_flush(void) {
    static sink_t s;
    static rtcm_framer_t f;
    uint8_t payload[60], fr[100], stream[300];
    for (int i = 0; i < 60; i++) payload[i] = (uint8_t) (i + 1);
    size_t flen = make_frame(fr, payload, 60);
    // A junk header claiming a 255-byte frame, then a complete good frame, then silence.
    size_t n = 0;
    stream[n++] = 0xD3; stream[n++] = 0x00; stream[n++] = 0xFF;
    memcpy(stream + n, fr, flen); n += flen;
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    rtcm_framer_feed(&f, stream, n);
    CHECK(s.nframes == 0);          // held behind the bogus header...
    rtcm_framer_flush(&f);
    CHECK(s.nframes == 1);          // ...released by the idle flush
    CHECK(s.nframes == 1 && memcmp(s.frames, fr, flen) == 0);
    CHECK(f.len == 0);
    // Flushing an empty or idle framer is harmless
    rtcm_framer_flush(&f);
    CHECK(s.nframes == 1);
    // A pending text line without newline is discarded by flush, next data fine
    rtcm_framer_feed(&f, (const uint8_t *) "$GPGGA,123", 10);
    rtcm_framer_flush(&f);
    CHECK(f.len == 0 && s.nlines == 0);
    rtcm_framer_feed(&f, fr, flen);
    CHECK(s.nframes == 2);
}

static size_t make_1005(uint8_t *out, double x, double y, double z) {
    uint8_t p[19] = {0};
    // msg 1005, station 0, flags 0; coordinates written bit by bit
    uint64_t fields[][2] = {{1005, 12}, {0, 12}, {0, 6}, {0, 4}, {0, 0}};
    size_t pos = 0;
    #define PUT(v, nb) do { uint64_t _v = (uint64_t) (v); for (int _i = (nb) - 1; _i >= 0; _i--, pos++) \
        if ((_v >> _i) & 1) p[pos >> 3] |= (uint8_t) (0x80 >> (pos & 7)); } while (0)
    (void) fields;
    PUT(1005, 12); PUT(0, 12); PUT(0, 6); PUT(0, 4);
    PUT((int64_t) llround(x * 10000) & ((1LL << 38) - 1), 38); PUT(0, 2);
    PUT((int64_t) llround(y * 10000) & ((1LL << 38) - 1), 38); PUT(0, 2);
    PUT((int64_t) llround(z * 10000) & ((1LL << 38) - 1), 38);
    #undef PUT
    return make_frame(out, p, 19);
}

static void test_monitor_position_and_slots(void) {
    static gnss_monitor_t m;
    static char out[32768];
    uint8_t fr[64];
    gnss_monitor_init(&m);
    // All-zero position (receiver not surveyed yet) must not produce NaN or a position
    gnss_monitor_rtcm(&m, fr, make_1005(fr, 0, 0, 0), 1000);
    int n = gnss_monitor_json(&m, 1100, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "nan") == NULL && strstr(out, "\"position\"") == NULL);
    gnss_monitor_rtcm(&m, fr, make_1005(fr, -4173994.6057, 2773222.0951, -3932511.6382), 2000);
    n = gnss_monitor_json(&m, 2100, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "\"lat\":-38.30942616") != NULL);
    // A later zero position must not overwrite the good one
    gnss_monitor_rtcm(&m, fr, make_1005(fr, 0, 0, 0), 3000);
    n = gnss_monitor_json(&m, 3100, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "\"lat\":-38.30942616") != NULL);

    // Slot reuse: fill the table via GSV, then add two new satellites at once
    char line[128];
    for (int i = 0; i < GNSS_MAX_SATS; i++) {
        // GN talker with PRN ranges: use BeiDou 201-263 and GPS 1-32... simplest: write sats directly
        m.sats[i].sys = 'C'; m.sats[i].prn = (uint8_t) (i + 1); m.sats[i].seen_ms = 4000 + (uint32_t) i;
    }
    snprintf(line, sizeof(line), "$GPGSV,1,1,02,30,10,100,40,31,20,200,41,1*");
    unsigned cs = 0;
    for (char *c = line + 1; *c != '*'; c++) cs ^= (unsigned char) *c;
    snprintf(line + strlen(line), 4, "%02X", cs);
    CHECK(gnss_monitor_line(&m, line, 10000));
    int found30 = 0, found31 = 0;
    for (int i = 0; i < GNSS_MAX_SATS; i++) {
        if (m.sats[i].sys == 'G' && m.sats[i].prn == 30 && m.sats[i].elev == 10) found30 = 1;
        if (m.sats[i].sys == 'G' && m.sats[i].prn == 31 && m.sats[i].elev == 20) found31 = 1;
    }
    CHECK(found30 && found31);
}

static void test_json_worst_case(void) {
    static gnss_monitor_t m;
    static char out[16384];
    gnss_monitor_init(&m);
    const char sys[] = "GREJC";
    for (int i = 0; i < GNSS_MAX_SATS; i++) {
        gnss_sat_t *s = &m.sats[i];
        s->sys = sys[i % 5]; s->prn = (uint8_t) (10 + i % 54);
        s->msm_ms = s->gsv_ms = s->seen_ms = 1000; s->elev = -1 + 90; s->azim = 359;
        for (int b = 0; b < GNSS_BANDS; b++) { s->sig_count[b] = 9; s->cnr_sum[b] = 9 * 16 * 55; }
        s->cnr_max16 = 16 * 55;
    }
    for (int i = 0; i < GNSS_MAX_MSG_TYPES; i++) { m.msgs[i].type = (uint16_t) (1000 + i); m.msgs[i].count = 4000000000u;
        m.msgs[i].interval_ms = 30000; m.msgs[i].last_ms = 1000; }
    m.have_position = true; m.ecef[0] = -4173994.6057; m.ecef[1] = 2773222.0951; m.ecef[2] = -3932511.6382;
    memset(m.receiver, 'X', 31); memset(m.firmware, 'X', 31); memset(m.receiver_serial, 'X', 31);
    memset(m.antenna, 'X', 31); memset(m.antenna_serial, 'X', 31);
    int n = gnss_monitor_json(&m, 2000, out, sizeof(out));
    printf("worst-case live JSON: %d bytes (firmware buffer 10240, ~400 reserved for extras)\n", n);
    CHECK(n > 0 && n < 10240 - 450);
}

static void test_line_log(void) {
    static line_log_t log;
    static char out[32768];
    line_log_init(&log);
    line_log_add(&log, '>', "VERSION", 1000);
    line_log_add(&log, '<', "#VERSION,\"quoted\"\\x\x01", 1500);
    int n = line_log_json(&log, 0, 2000, out, sizeof(out));
    CHECK(n > 0);
    CHECK(strstr(out, "\"seq\":2") != NULL);
    CHECK(strstr(out, "#VERSION,\\\"quoted\\\"\\\\x?") != NULL);
    n = line_log_json(&log, 1, 2000, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "VERSION\"") == NULL && strstr(out, "#VERSION") != NULL);
    for (int i = 0; i < 200; i++) line_log_add(&log, '<', "x", 3000);
    n = line_log_json(&log, 0, 4000, out, sizeof(out));
    CHECK(n > 0 && strstr(out, "\"seq\":202,") != NULL);
    // only the last LINE_LOG_LINES are kept
    int count = 0;
    for (char *p = out; (p = strstr(p, "\"dir\"")) != NULL; p++) count++;
    CHECK(count == LINE_LOG_LINES);
    // too small buffer -> -1
    CHECK(line_log_json(&log, 0, 4000, out, 50) == -1);
}

static void test_geodesy(void) {
    double r[3] = {-4173994.6057, 2773222.0951, -3932511.6382}, lat, lon, h;
    gnss_ecef_to_llh(r, &lat, &lon, &h);
    CHECK(fabs(lat - -38.30942616) < 1e-7);
    CHECK(fabs(lon - 146.39974564) < 1e-7);
    CHECK(fabs(h - 96.447) < 0.002);
    CHECK(gnss_nmea_checksum_ok("$GPGSV,2,1,07,02,45,123,44,05,30,045,40,13,10,300,35,15,60,210,47,1*64"));
    CHECK(!gnss_nmea_checksum_ok("$GPGSV,2,1,07,02,45,123,44,05,30,045,40,13,10,300,35,15,60,210,47,1*65"));
    CHECK(!gnss_nmea_checksum_ok("$command,VERSION,response: OK"));
    CHECK(gnss_band_of('E', 8) == 3 && gnss_band_of('C', 2) == 0 && gnss_band_of('C', 22) == 2);
}

static void test_monitor_robustness(void) {
    // Random / truncated MSM payloads must never crash or read out of bounds (ASan).
    static gnss_monitor_t m;
    static char out[16384];
    gnss_monitor_init(&m);
    uint8_t payload[1023], fr[1100];
    srand(1);
    for (int iter = 0; iter < 20000; iter++) {
        size_t n = 2 + (size_t) (rand() % 300);
        for (size_t i = 0; i < n; i++) payload[i] = (uint8_t) rand();
        static const uint16_t types[] = {1074, 1075, 1076, 1077, 1087, 1097, 1127, 1005, 1006, 1033, 1007, 1008};
        uint16_t t = types[rand() % 12];
        payload[0] = (uint8_t) (t >> 4);
        payload[1] = (uint8_t) ((t << 4) | (payload[1] & 0x0F));
        size_t flen = make_frame(fr, payload, n);
        gnss_monitor_rtcm(&m, fr, flen, (uint32_t) iter * 10);
    }
    CHECK(gnss_monitor_json(&m, 200000, out, sizeof(out)) > 0);
    CHECK(gnss_monitor_json(&m, 200000, out, 30) == -1);
    // garbage NMEA
    CHECK(!gnss_monitor_line(&m, "$GPGSV,,,,*00", 1));
    CHECK(gnss_monitor_line(&m, "$GPGSV,1,1,00*79", 1));
}

/* ------------------------------------------------------------- stream mode */

static int run_stream(const char *path, const char *outdir, unsigned seed) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 2; }
    static uint8_t data[400000];
    size_t total = fread(data, 1, sizeof(data), fp);
    fclose(fp);

    static sink_t s;
    static rtcm_framer_t f;
    memset(&s, 0, sizeof(s));
    rtcm_framer_init(&f, on_frame, on_line, &s);
    srand(seed);
    size_t pos = 0;
    while (pos < total) {
        size_t n = 1 + (size_t) (rand() % 700);
        if (n > total - pos) n = total - pos;
        rtcm_framer_feed(&f, data + pos, n);
        pos += n;
    }

    char p[512];
    snprintf(p, sizeof(p), "%s/frames.bin", outdir);
    fp = fopen(p, "wb");
    for (int i = 0; i < s.nframes; i++) {
        uint32_t len = (uint32_t) s.frame_len[i];
        fwrite(&len, 4, 1, fp);
        fwrite(s.frames + s.frame_off[i], 1, len, fp);
    }
    fclose(fp);
    snprintf(p, sizeof(p), "%s/lines.txt", outdir);
    fp = fopen(p, "w");
    for (int i = 0; i < s.nlines; i++) fprintf(fp, "%s\n", s.lines[i]);
    fclose(fp);

    // Live view: feed everything with simulated time, then replay the last
    // message of each type (plus GSV lines) 10 s later so only those count.
    static gnss_monitor_t m;
    gnss_monitor_init(&m);
    uint32_t t = 1000;
    for (int i = 0; i < s.nframes; i++, t += 250) gnss_monitor_rtcm(&m, s.frames + s.frame_off[i], s.frame_len[i], t);
    t += 10000;
    uint16_t seen[32];
    int nseen = 0;
    for (int i = s.nframes - 1; i >= 0; i--) {
        const uint8_t *fr = s.frames + s.frame_off[i];
        uint16_t type = (uint16_t) ((fr[3] << 4) | (fr[4] >> 4));
        int dup = 0;
        for (int k = 0; k < nseen; k++) if (seen[k] == type) dup = 1;
        if (dup || nseen >= 32) continue;
        seen[nseen++] = type;
        gnss_monitor_rtcm(&m, fr, s.frame_len[i], t);
    }
    for (int i = 0; i < s.nlines; i++) gnss_monitor_line(&m, s.lines[i], t);

    static char json[32768];
    int n = gnss_monitor_json(&m, t + 100, json, sizeof(json));
    if (n < 0) { fprintf(stderr, "json overflow\n"); return 1; }
    snprintf(p, sizeof(p), "%s/live.json", outdir);
    fp = fopen(p, "w");
    fwrite(json, 1, (size_t) n, fp);
    fclose(fp);
    printf("stream: %d frames, %d lines, %u garbage bytes, json %d bytes\n",
           s.nframes, s.nlines, (unsigned) f.garbage_bytes, n);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "stream") == 0 && argc == 5) {
        return run_stream(argv[2], argv[3], (unsigned) atoi(argv[4]));
    }
    test_framer_units();
    test_framer_flush();
    test_monitor_position_and_slots();
    test_line_log();
    test_json_worst_case();
    test_geodesy();
    test_monitor_robustness();
    if (failures) {
        fprintf(stderr, "%d receiver unit check(s) failed\n", failures);
        return 1;
    }
    printf("receiver unit tests passed\n");
    return 0;
}
