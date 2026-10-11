/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_RECORDER_H
#define ESP32_NTRIP_RECORDER_H

/*
 * RTCM log download, like RTKLIB STRSVR's file output: the browser downloads
 * a file that the ESP32 keeps streaming RTCM into until the chosen duration
 * has passed or Stop is pressed. Runs in its own task (async HTTP request), so
 * the rest of the web interface keeps working while a recording is running.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_http_server.h>

/* Called from the UART task with every valid RTCM frame. Never blocks. */
void recorder_feed(const uint8_t *frame, size_t len);

/* HTTP handlers: GET /receiver/record?minutes=N and POST /receiver/record/stop */
esp_err_t recorder_start_handler(httpd_req_t *req);
esp_err_t recorder_stop_handler(httpd_req_t *req);

/* Appends ,"recording":{...} status to a JSON object being built. Returns chars written or -1. */
int recorder_status_json(char *out, size_t size);

#endif // ESP32_NTRIP_RECORDER_H
