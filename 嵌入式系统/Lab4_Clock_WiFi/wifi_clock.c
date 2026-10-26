/* ESP AT + UDP NTP client, polled without long waits in the display loop.
 * Uses the basic CIPSTART/CIPSEND commands, not firmware-specific SNTP ATs.
 * NTP packet layout: RFC 5905. 100 Hz tick source must keep running in IRQs.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "wifi_clock.h"
#include "uart_log.h"
#include "wifi_config.h"
#include "inc/hw_memmap.h"
#include "driverlib/sysctl.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/uart.h"
#include "driverlib/interrupt.h"

#define RX_SIZE 1024U
#define RX_MASK (RX_SIZE - 1U)
#define LEDS (GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3)
#define AT_OK 1U
#define AT_ERROR 2U
#define AT_PROMPT 4U
#define DAY_CS 8640000UL

enum {
    S_IDLE, S_GUARD1, S_GUARD2, S_AT, S_ECHO, S_NORMAL,
    S_CLOSE, S_SINGLE, S_STATION, S_INFO, S_ACTIVE, S_AUTO,
    S_WIFI, S_WIFI_PAUSE, S_JOIN, S_OPEN, S_PROMPT, S_REPLY,
    S_NEXT_CLOSE, S_FINISH
};
volatile uint32_t g_wifi_status = WIFI_IDLE;
volatile uint32_t g_wifi_last_error = WIFI_ERR_NONE;
volatile uint32_t g_wifi_sync_count = 0U;
volatile uint32_t g_wifi_bad_packets = 0U;
static volatile uint8_t rx[RX_SIZE];
static volatile uint16_t rx_head, rx_tail;
static volatile bool rx_error;
static uint8_t state, server_index, events;
static uint32_t now_tick, state_tick, state_timeout, wifi_tick;
static uint32_t last_start, sent_tick, request_sequence;
static bool ever_started, wifi_seen, time_pending;
static WifiClockTime pending_time;
static char line[192], command[256];
static uint16_t line_length, payload_left, payload_length, payload_pos;
static uint8_t packet[48], request[48];
static bool discard_line;
static bool button_raw, button_stable;
static uint32_t button_changed;

static void Led(uint32_t pin) { GPIOPinWrite(GPIO_PORTF_BASE, LEDS, pin); }
static void WriteBytes(const uint8_t *data, uint32_t size)
{
    uint32_t i;
    /* UART transmission is bounded to one short command/48-byte packet.
     * IRQs remain enabled, so the clock and RX ring keep running. */
    for (i = 0U; i < size; ++i) UARTCharPut(UART1_BASE, data[i]);
}
static void WriteText(const char *text)
{
    WriteBytes((const uint8_t *)text, (uint32_t)strlen(text));
}
static void WaitState(uint8_t next, uint32_t timeout)
{
    state = next; state_tick = now_tick; state_timeout = timeout;
    events = 0U;
}
static void Command(uint8_t next, uint32_t timeout, const char *text)
{
    WaitState(next, timeout);
    WriteText(text);
    WriteText("\r\n");
}
static void ParserReset(void)
{
    line_length = 0U; payload_left = 0U; payload_length = 0U;
    payload_pos = 0U; discard_line = false;
}
static void Fail(uint32_t error)
{
    state = S_IDLE;
    g_wifi_last_error = error;
    g_wifi_status = WIFI_FAILED;
    Led(GPIO_PIN_1);
    UartLogValue("[SYNC] FAILED; error code: ", error);
    switch (error) {
    case WIFI_ERR_NO_AT: UartLogLine("[ERROR] ESP did not answer AT."); break;
    case WIFI_ERR_COMMAND: UartLogLine("[ERROR] ESP command failed or timed out."); break;
    case WIFI_ERR_NOT_CONNECTED: UartLogLine("[ERROR] No remembered Wi-Fi connection."); break;
    case WIFI_ERR_JOIN: UartLogLine("[ERROR] Could not join the configured hotspot."); break;
    case WIFI_ERR_NTP: UartLogLine("[ERROR] No valid NTP reply from either server."); break;
    case WIFI_ERR_UART: UartLogLine("[ERROR] ESP UART receive error or overflow."); break;
    case WIFI_ERR_CONFIG: UartLogLine("[ERROR] Invalid Wi-Fi/server configuration."); break;
    default: UartLogLine("[ERROR] Unknown error."); break;
    }
    UartLogValue("[NTP] Rejected replies (total): ", g_wifi_bad_packets);
    UartLogLine("[CLOCK] No network time applied; clock keeps running.");
}
static uint32_t ReadBE(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void WriteBE(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}
static void ReceiveNtp(void)
{
    uint32_t version, seconds, fraction, rtt, cs;
    if (state != S_REPLY) return;
    version = (packet[0] >> 3) & 7U;
    rtt = now_tick - sent_tick;
    if (payload_length < 48U || payload_length > 512U ||
        (packet[0] >> 6) == 3U || (packet[0] & 7U) != 4U ||
        (version != 3U && version != 4U) ||
        packet[1] == 0U || packet[1] > 15U ||
        memcmp(packet + 24, request + 40, 8U) != 0 ||
        ReadBE(packet + 40) == 0U || rtt > 200U) {
        ++g_wifi_bad_packets; return;
    }
    /* Unsigned subtraction also handles the NTP rollover in 2036.
     * Unix seconds are valid until 2106. Only time-of-day is displayed. */
    seconds = ReadBE(packet + 40) - 2208988800UL;
    if (seconds < 1577836800UL) { ++g_wifi_bad_packets; return; }
    fraction = ReadBE(packet + 44);
    cs = (((seconds % 86400UL) + 28800UL) % 86400UL) * 100UL;
    cs += ((fraction >> 16) * 100UL) >> 16;
    /* These values come from the validated reply, never from the preset
     * clock. UART0 logs are queued; UART1 still carries only ESP traffic. */
    UartLogValue("[NTP] Valid reply bytes: ", payload_length);
    UartLogValue("[NTP] Stratum: ", packet[1]);
    UartLogValue("[NTP] Transmit seconds (since 1900): ", ReadBE(packet + 40));
    UartLogValue("[NTP] Transmit fraction (2^-32 s): ", fraction);
    UartLogValue("[PARSE] Unix seconds (UTC): ", seconds);
    UartLogTime("[PARSE] UTC: ",
                (seconds % 86400UL) * 100UL + (((fraction >> 16) * 100UL) >> 16));
    UartLogTime("[PARSE] Beijing (UTC+8): ", cs);
    UartLogValue("[NTP] RTT milliseconds: ", rtt * 10U);
    /* Approximate return delay with half of the bounded request round trip. */
    cs = (cs + rtt / 2U) % DAY_CS;
    pending_time.centiseconds = cs;
    pending_time.sample_tick = now_tick;
    time_pending = true;
    ++g_wifi_sync_count;
    g_wifi_last_error = WIFI_ERR_NONE;
    g_wifi_status = WIFI_SYNCED;
    Led(GPIO_PIN_3);
    Command(S_FINISH, 200U, "AT+CIPCLOSE");
}
static void ParseLine(void)
{
    line[line_length] = '\0';
    if (strcmp(line, "OK") == 0) events |= AT_OK;
    if (strcmp(line, "ERROR") == 0 || strcmp(line, "FAIL") == 0 ||
        strcmp(line, "SEND FAIL") == 0 || strncmp(line, "busy", 4U) == 0)
        events |= AT_ERROR;
    if (strncmp(line, "+CWJAP:", 7U) == 0 && strchr(line, '"') != 0)
        wifi_seen = true;
    line_length = 0U;
}
static void ParseByte(uint8_t byte)
{
    uint32_t n;
    uint16_t i;
    if (payload_left != 0U) {
        if (payload_pos < sizeof(packet)) packet[payload_pos] = byte;
        ++payload_pos;
        if (--payload_left == 0U) ReceiveNtp();
        return;
    }
    if (byte == '>' && state == S_PROMPT) {
        events |= AT_PROMPT; line_length = 0U; return;
    }
    if (byte == '\r') return;
    if (byte == '\n') {
        if (!discard_line && line_length != 0U) ParseLine();
        line_length = 0U; discard_line = false; return;
    }
    if (discard_line) return;
    if (line_length == 0U && (byte == ' ' || byte == '\t')) return;
    if (line_length + 1U >= sizeof(line)) {
        line_length = 0U; discard_line = true; return;
    }
    line[line_length++] = (char)byte;
    /* Single-connection active receive: +IPD,<length>:<binary payload>.
     * CIPDINFO=0 and CIPMUX=0 remove source/link-id fields. */
    if (byte == ':' && line_length > 6U && memcmp(line, "+IPD,", 5U) == 0) {
        n = 0U;
        for (i = 5U; i + 1U < line_length; ++i) {
            if (line[i] < '0' || line[i] > '9') {
                discard_line = true; line_length = 0U; return;
            }
            n = n * 10U + (uint32_t)(line[i] - '0');
            if (n > 2048U) {
                discard_line = true; line_length = 0U; return;
            }
        }
        payload_length = (uint16_t)n; payload_left = (uint16_t)n;
        payload_pos = 0U; line_length = 0U;
        memset(packet, 0, sizeof(packet));
    }
}
static void UartHandler(void)
{
    uint32_t flags = UARTIntStatus(UART1_BASE, true);
    uint16_t next;
    int32_t c;
    UARTIntClear(UART1_BASE, flags);
    if ((flags & (UART_INT_OE | UART_INT_BE | UART_INT_PE | UART_INT_FE)) != 0U) {
        UARTRxErrorClear(UART1_BASE); rx_error = true;
    }
    while (UARTCharsAvail(UART1_BASE)) {
        c = UARTCharGetNonBlocking(UART1_BASE);
        next = (uint16_t)((rx_head + 1U) & RX_MASK);
        if (next == rx_tail) rx_error = true;
        else { rx[rx_head] = (uint8_t)c; rx_head = next; }
    }
}
static bool Append(char **out, const char *text, bool escape)
{
    const char *limit = command + sizeof(command) - 3U;
    while (*text != '\0') {
        if (*text == '\r' || *text == '\n') return false;
        if (escape && (*text == '"' || *text == '\\' || *text == ',')) {
            if (*out >= limit) return false;
            *(*out)++ = '\\';
        }
        if (*out >= limit) return false;
        *(*out)++ = *text++;
    }
    **out = '\0'; return true;
}
static void JoinOrQuery(void)
{
    char *out = command;
    wifi_tick = now_tick;
    wifi_seen = false;
    if (LAB4_WIFI_PASSWORD[0] == '\0') {
        UartLogLine("[WIFI] Checking the remembered hotspot connection.");
        Command(S_WIFI, 300U, "AT+CWJAP?"); return;
    }
    if (!Append(&out, "AT+CWJAP=\"", false) ||
        !Append(&out, LAB4_WIFI_SSID, true) || !Append(&out, "\",\"", false) ||
        !Append(&out, LAB4_WIFI_PASSWORD, true) || !Append(&out, "\"", false)) {
        Fail(WIFI_ERR_CONFIG); return;
    }
    UartLogLine("[WIFI] Joining the configured hotspot (credentials hidden).");
    Command(S_JOIN, 3000U, command);
}
static void OpenServer(void)
{
    char *out = command;
    const char *server = server_index == 0U ? LAB4_NTP_PRIMARY : LAB4_NTP_BACKUP;
    if (!Append(&out, "AT+CIPSTART=\"UDP\",\"", false) ||
        !Append(&out, server, true) || !Append(&out, "\",123", false)) {
        Fail(WIFI_ERR_CONFIG); return;
    }
    UartLogText("[NTP] Opening UDP port 123 on: ", server);
    Command(S_OPEN, 2000U, command);
}
static void NextServer(void)
{
    if (server_index != 0U) { Fail(WIFI_ERR_NTP); return; }
    server_index = 1U;
    UartLogLine("[NTP] Primary attempt failed; trying the backup server.");
    Command(S_NEXT_CLOSE, 200U, "AT+CIPCLOSE");
}
static void SendRequest(void)
{
    memset(request, 0, sizeof(request));
    request[0] = 0x23U; /* LI=0, version=4, mode=client. */
    request[2] = 6U;
    ++request_sequence;
    UartLogValue("[NTP] Sending 48-byte request; sequence: ", request_sequence);
    /* Request correlation token. The server echoes it in Origin Timestamp;
     * no assumption about the board's default wall clock is needed. */
    WriteBE(request + 40, 0xE0000000UL + now_tick / 100U);
    WriteBE(request + 44, (now_tick * 2654435761UL) ^ request_sequence);
    sent_tick = now_tick;
    WaitState(S_REPLY, 500U);
    WriteBytes(request, sizeof(request));
}
static void Start(void)
{
    bool disabled;
    if (state != S_IDLE) {
        UartLogLine("[SYNC] SW1 ignored: a request is already running.");
        return;
    }
    if (ever_started && now_tick - last_start < 1000U) {
        UartLogLine("[SYNC] SW1 ignored: wait 10 s between request starts.");
        return;
    }
    ever_started = true; last_start = now_tick;
    disabled = IntMasterDisable();
    rx_tail = rx_head; rx_error = false;
    if (!disabled) IntMasterEnable();
    ParserReset(); time_pending = false; server_index = 0U;
    g_wifi_status = WIFI_SYNCING; g_wifi_last_error = WIFI_ERR_NONE;
    Led(GPIO_PIN_2);
    WaitState(S_GUARD1, 110U);
    UartLogLine("");
    UartLogLine("[SYNC] SW1 pressed; starting a new network time request.");
}
void WifiClockInit(uint32_t system_clock)
{
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART1);
    while (!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOC) ||
           !SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOF) ||
           !SysCtlPeripheralReady(SYSCTL_PERIPH_UART1)) { }
    GPIOPinConfigure(GPIO_PC4_U1RX);
    GPIOPinConfigure(GPIO_PC5_U1TX);
    GPIOPinTypeUART(GPIO_PORTC_BASE, GPIO_PIN_4 | GPIO_PIN_5);
    UARTConfigSetExpClk(UART1_BASE, system_clock, LAB4_ESP_BAUD,
                       UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTFIFOEnable(UART1_BASE);
    UARTFIFOLevelSet(UART1_BASE, UART_FIFO_TX1_8, UART_FIFO_RX1_8);
    UARTIntRegister(UART1_BASE, UartHandler);
    UARTIntClear(UART1_BASE, 0x7FFU);
    UARTIntEnable(UART1_BASE, UART_INT_RX | UART_INT_RT | UART_INT_OE |
                  UART_INT_BE | UART_INT_PE | UART_INT_FE);
    GPIOPinTypeGPIOInput(GPIO_PORTF_BASE, GPIO_PIN_4);
    GPIOPadConfigSet(GPIO_PORTF_BASE, GPIO_PIN_4, GPIO_STRENGTH_2MA,
                     GPIO_PIN_TYPE_STD_WPU);
    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, LEDS);
    Led(0U);
}
void WifiClockPoll(uint32_t tick100hz)
{
    uint8_t c;
    uint16_t budget = RX_SIZE;
    bool raw, expired, ok, error;
    now_tick = tick100hz;
    raw = (GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_4) == 0U);
    if (raw != button_raw) { button_raw = raw; button_changed = now_tick; }
    if (button_stable != raw && now_tick - button_changed >= 3U) {
        button_stable = raw;
        if (raw) Start(); /* Holding SW1 never causes repeat requests. */
    }
    while (rx_tail != rx_head && budget-- != 0U) {
        c = rx[rx_tail]; rx_tail = (uint16_t)((rx_tail + 1U) & RX_MASK);
        ParseByte(c);
    }
    if (state == S_IDLE) return;
    if (rx_error) { Fail(WIFI_ERR_UART); return; }
    expired = now_tick - state_tick >= state_timeout;
    ok = (events & AT_OK) != 0U;
    error = (events & AT_ERROR) != 0U;
    switch (state) {
    case S_GUARD1:
        if (expired) { WriteText("+++"); WaitState(S_GUARD2, 110U); }
        break;
    case S_GUARD2:
        if (expired) Command(S_AT, 300U, "AT");
        break;
    case S_AT:
        if (ok) Command(S_ECHO, 200U, "ATE0");
        else if (error || expired) Fail(WIFI_ERR_NO_AT);
        break;
    case S_ECHO:
        if (ok) Command(S_NORMAL, 200U, "AT+CIPMODE=0");
        else if (error || expired) Fail(WIFI_ERR_COMMAND);
        break;
    case S_NORMAL:
        if (ok) Command(S_CLOSE, 200U, "AT+CIPCLOSE");
        else if (error || expired) Fail(WIFI_ERR_COMMAND);
        break;
    case S_CLOSE:
        if (ok || error || expired) Command(S_SINGLE, 200U, "AT+CIPMUX=0");
        break;
    case S_SINGLE:
        if (ok) Command(S_STATION, 300U, "AT+CWMODE=1");
        else if (error || expired) Fail(WIFI_ERR_COMMAND);
        break;
    case S_STATION:
        if (ok) Command(S_INFO, 200U, "AT+CIPDINFO=0");
        else if (error || expired) Fail(WIFI_ERR_COMMAND);
        break;
    case S_INFO:
        /* Old firmware may lack this optional command; its default is 0. */
        if (ok || error) Command(S_ACTIVE, 200U, "AT+CIPRECVMODE=0");
        else if (expired) Fail(WIFI_ERR_COMMAND);
        break;
    case S_ACTIVE:
        if (ok || error) Command(S_AUTO, 200U, "AT+CWAUTOCONN=1");
        else if (expired) Fail(WIFI_ERR_COMMAND);
        break;
    case S_AUTO:
        if (ok || error) JoinOrQuery();
        else if (expired) Fail(WIFI_ERR_COMMAND);
        break;
    case S_WIFI:
        if (ok && wifi_seen) OpenServer();
        else if (ok || error || expired) {
            if (now_tick - wifi_tick >= 3000U) Fail(WIFI_ERR_NOT_CONNECTED);
            else WaitState(S_WIFI_PAUSE, 100U);
        }
        break;
    case S_WIFI_PAUSE:
        if (expired) { wifi_seen = false; Command(S_WIFI, 300U, "AT+CWJAP?"); }
        break;
    case S_JOIN:
        if (ok) OpenServer();
        else if (error || expired) Fail(WIFI_ERR_JOIN);
        break;
    case S_OPEN:
        if (ok) Command(S_PROMPT, 300U, "AT+CIPSEND=48");
        else if (error || expired) NextServer();
        break;
    case S_PROMPT:
        if ((events & AT_PROMPT) != 0U) SendRequest();
        else if (error) NextServer();
        else if (expired) Fail(WIFI_ERR_COMMAND);
        break;
    case S_REPLY:
        if (error || expired) NextServer();
        break;
    case S_NEXT_CLOSE:
        if (ok || error || expired) OpenServer();
        break;
    case S_FINISH:
        if (ok || error || expired) state = S_IDLE;
        break;
    default: break;
    }
}
bool WifiClockTakeTime(WifiClockTime *time)
{
    if (!time_pending) return false;
    *time = pending_time; time_pending = false; return true;
}
