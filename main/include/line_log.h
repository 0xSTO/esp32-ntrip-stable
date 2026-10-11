/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_LINE_LOG_H
#define ESP32_NTRIP_LINE_LOG_H

/*
 * Small ring of recent console lines (receiver replies and commands sent),
 * each with a sequence number so the web page can fetch only new lines.
 * Pure C; callers provide locking.
 */

#include <stddef.h>
#include <stdint.h>

#define LINE_LOG_LINES 48
#define LINE_LOG_WIDTH 180

typedef struct {
    uint32_t seq;
    uint32_t t_ms;
    char dir;       // '<' from receiver, '>' sent by us, '!' note
    char text[LINE_LOG_WIDTH];
} line_log_entry_t;

typedef struct line_log {
    line_log_entry_t entries[LINE_LOG_LINES];
    uint32_t next_seq;   // seq of the next line to be added (starts at 1)
} line_log_t;

void line_log_init(line_log_t *log);
void line_log_add(line_log_t *log, char dir, const char *text, uint32_t now_ms);

/* JSON {"seq":N,"lines":[...]} with lines whose seq > since. Returns length or -1. */
int line_log_json(const line_log_t *log, uint32_t since, uint32_t now_ms, char *out, size_t size);

#endif // ESP32_NTRIP_LINE_LOG_H
