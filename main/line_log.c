/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#include <stdio.h>
#include <string.h>

#include "line_log.h"

void line_log_init(line_log_t *log) {
    memset(log, 0, sizeof(*log));
    log->next_seq = 1;
}

void line_log_add(line_log_t *log, char dir, const char *text, uint32_t now_ms) {
    line_log_entry_t *e = &log->entries[log->next_seq % LINE_LOG_LINES];
    e->seq = log->next_seq++;
    e->t_ms = now_ms;
    e->dir = dir;
    size_t w = 0;
    for (const char *c = text; *c && w + 1 < sizeof(e->text); c++) {
        unsigned char ch = (unsigned char) *c;
        e->text[w++] = (ch >= 0x20 && ch <= 0x7E) ? (char) ch : '?';
    }
    e->text[w] = '\0';
}

int line_log_json(const line_log_t *log, uint32_t since, uint32_t now_ms, char *out, size_t size) {
    size_t len = 0;
    int n = snprintf(out, size, "{\"seq\":%lu,\"lines\":[", (unsigned long) (log->next_seq - 1));
    if (n < 0 || (size_t) n >= size) return -1;
    len = (size_t) n;

    uint32_t first = log->next_seq > LINE_LOG_LINES ? log->next_seq - LINE_LOG_LINES : 1;
    if (since + 1 > first) first = since + 1;
    int count = 0;
    for (uint32_t seq = first; seq < log->next_seq; seq++) {
        const line_log_entry_t *e = &log->entries[seq % LINE_LOG_LINES];
        if (e->seq != seq) continue;
        // JSON-escape text (only " and \ can occur after sanitising)
        char esc[LINE_LOG_WIDTH * 2];
        size_t w = 0;
        for (const char *c = e->text; *c; c++) {
            if (*c == '"' || *c == '\\') esc[w++] = '\\';
            esc[w++] = *c;
        }
        esc[w] = '\0';
        n = snprintf(out + len, size - len, "%s{\"seq\":%lu,\"age_ms\":%lu,\"dir\":\"%c\",\"text\":\"%s\"}",
                     count ? "," : "", (unsigned long) e->seq, (unsigned long) (now_ms - e->t_ms), e->dir, esc);
        if (n < 0 || (size_t) n >= size - len) return -1;
        len += (size_t) n;
        count++;
    }
    n = snprintf(out + len, size - len, "]}");
    if (n < 0 || (size_t) n >= size - len) return -1;
    return (int) (len + (size_t) n);
}
