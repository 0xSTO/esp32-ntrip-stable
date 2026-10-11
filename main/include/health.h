/*
 * This file is part of esp32-ntrip-stable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 */

#ifndef ESP32_NTRIP_HEALTH_H
#define ESP32_NTRIP_HEALTH_H

/*
 * Progress watchdog and restart-cause record.
 *
 * Long-running tasks register and call health_kick() every loop. If a task
 * stops kicking for its timeout, the device records the cause in RTC RAM
 * (survives a software reset) and reboots. The cause is printed at the next
 * boot as "Restart cause: ...".
 */

#include <stdint.h>

#define HEALTH_DEFAULT_TIMEOUT_MS (3u * 60u * 1000u)

typedef int health_handle_t;

void health_init();

/* Returns the cause recorded before the last reboot, or NULL. Valid until reboot. */
const char *health_restart_cause();

health_handle_t health_register(const char *name, uint32_t timeout_ms);
void health_kick(health_handle_t handle);

/* Record a cause and reboot. Does not return. */
void health_restart(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#endif //ESP32_NTRIP_HEALTH_H
