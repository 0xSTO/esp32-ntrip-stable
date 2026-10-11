#ifndef ESP32_XBEE_UART_H
#define ESP32_XBEE_UART_H

#include <stddef.h>
#include <stdint.h>


#define UART_BUFFER_SIZE 4096

void uart_init();

void uart_inject(void *data, size_t len);
int uart_log(char *buffer, size_t len);
int uart_nmea(const char *fmt, ...);
int uart_write(char *buffer, size_t len);

// Called from the UART task for every chunk read. Must not block.
typedef void (*uart_read_sink_t)(void *ctx, const uint8_t *data, size_t len);
void uart_add_read_sink(uart_read_sink_t sink, void *ctx);

#endif //ESP32_XBEE_UART_H
