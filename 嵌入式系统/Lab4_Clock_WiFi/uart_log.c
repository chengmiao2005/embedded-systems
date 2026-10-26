/* Human-readable assessment log on UART0. ESP AT traffic stays on UART1.
 * Fixed storage, no printf/heap, and bounded nonblocking FIFO service. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "uart_log.h"
#include "inc/hw_memmap.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/uart.h"

#define LOG_SIZE 2048U
#define LOG_MASK (LOG_SIZE - 1U)
#define LOG_BAUD 115200U
#define LOG_POLL_BUDGET 64U

static char queue[LOG_SIZE];
static uint16_t head, tail;
static uint32_t reported_drops;
volatile uint32_t g_uart_log_dropped = 0U;

static void PutQueued(char byte)
{
    queue[head] = byte;
    head = (uint16_t)((head + 1U) & LOG_MASK);
}

void UartLogText(const char *label, const char *value)
{
    uint32_t a = (uint32_t)strlen(label);
    uint32_t b = (uint32_t)strlen(value);
    uint32_t free_bytes = (tail - head - 1U) & LOG_MASK;
    /* Reserve the whole CRLF-terminated line, so overload never emits a
     * partial time or splices two unrelated messages together. */
    if (a > LOG_SIZE - 3U || b > LOG_SIZE - 3U ||
        a + b + 2U > free_bytes) {
        ++g_uart_log_dropped;
        return;
    }
    while (*label != '\0') PutQueued(*label++);
    while (*value != '\0') PutQueued(*value++);
    PutQueued('\r');
    PutQueued('\n');
}

void UartLogLine(const char *text) { UartLogText(text, ""); }

void UartLogValue(const char *label, uint32_t value)
{
    char digits[11];
    char *out = digits + sizeof(digits) - 1U;
    *out = '\0';
    do {
        *--out = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    UartLogText(label, out);
}

void UartLogTime(const char *label, uint32_t centiseconds)
{
    char text[] = "00:00:00.00";
    uint32_t h, m, s, cs;
    centiseconds %= 8640000UL;
    h = centiseconds / 360000UL;
    m = (centiseconds / 6000UL) % 60UL;
    s = (centiseconds / 100UL) % 60UL;
    cs = centiseconds % 100UL;
    text[0] = (char)('0' + h / 10U); text[1] = (char)('0' + h % 10U);
    text[3] = (char)('0' + m / 10U); text[4] = (char)('0' + m % 10U);
    text[6] = (char)('0' + s / 10U); text[7] = (char)('0' + s % 10U);
    text[9] = (char)('0' + cs / 10U); text[10] = (char)('0' + cs % 10U);
    UartLogText(label, text);
}

void UartLogInit(uint32_t system_clock)
{
    head = 0U; tail = 0U;
    g_uart_log_dropped = 0U; reported_drops = 0U;
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOA) ||
           !SysCtlPeripheralReady(SYSCTL_PERIPH_UART0)) { }
    GPIOPinConfigure(GPIO_PA0_U0RX);
    GPIOPinConfigure(GPIO_PA1_U0TX);
    GPIOPinTypeUART(GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);
    UARTClockSourceSet(UART0_BASE, UART_CLOCK_SYSTEM);
    UARTConfigSetExpClk(UART0_BASE, system_clock, LOG_BAUD,
                       UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTFlowControlSet(UART0_BASE, UART_FLOWCONTROL_NONE);
    UARTIntDisable(UART0_BASE, 0x7FFU);
    UARTIntClear(UART0_BASE, 0x7FFU);
    UARTFIFOEnable(UART0_BASE);
    UARTEnable(UART0_BASE);
}

void UartLogPoll(void)
{
    uint32_t budget = LOG_POLL_BUDGET;
    while (tail != head && budget-- != 0U) {
        if (!UARTCharPutNonBlocking(UART0_BASE, (unsigned char)queue[tail]))
            break;
        tail = (uint16_t)((tail + 1U) & LOG_MASK);
    }
    if (tail == head && reported_drops != g_uart_log_dropped) {
        reported_drops = g_uart_log_dropped;
        UartLogValue("[LOG] Dropped complete lines (total): ", reported_drops);
    }
}
