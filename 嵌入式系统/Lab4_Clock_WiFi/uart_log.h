#ifndef UART_LOG_H
#define UART_LOG_H
#include <stdint.h>

/* UART0 PA0/PA1 -> LaunchPad ICDI USB virtual COM, 115200 8-N-1.
 * Call only from the main loop, never from an interrupt handler.
 * Logging is queued and never waits for the PC or the UART transmit FIFO. */
extern volatile uint32_t g_uart_log_dropped;
void UartLogInit(uint32_t system_clock);
void UartLogPoll(void);
void UartLogLine(const char *text);
void UartLogText(const char *label, const char *value);
void UartLogValue(const char *label, uint32_t value);
void UartLogTime(const char *label, uint32_t centiseconds);
#endif
