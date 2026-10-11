#ifndef ESP32_XBEE_TASKS_H
#define ESP32_XBEE_TASKS_H

#define TASK_PRIORITY_STATUS_LED 0
#define TASK_PRIORITY_RESET_BUTTON 0
#define TASK_PRIORITY_WIFI_STATUS 0
#define TASK_PRIORITY_STATS 0
#define TASK_PRIORITY_INTERFACE 5
#define TASK_PRIORITY_HEALTH 12
#define TASK_PRIORITY_UART 10
// Must stay below configMAX_PRIORITIES (25). It was 100, which made
// config_restart() hit a FreeRTOS assert (PANIC reboot) on every settings save.
#define TASK_PRIORITY_MAX 20

#endif //ESP32_XBEE_TASKS_H
