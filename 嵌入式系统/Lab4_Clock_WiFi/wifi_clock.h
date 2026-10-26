#ifndef WIFI_CLOCK_H
#define WIFI_CLOCK_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t centiseconds; /* Beijing time since midnight, 0..8639999. */
    uint32_t sample_tick;  /* Local 100 Hz tick when the sample was received. */
} WifiClockTime;

enum {
    WIFI_IDLE = 0, WIFI_SYNCING, WIFI_SYNCED, WIFI_FAILED
};
enum {
    WIFI_ERR_NONE = 0, WIFI_ERR_NO_AT, WIFI_ERR_COMMAND,
    WIFI_ERR_NOT_CONNECTED, WIFI_ERR_JOIN, WIFI_ERR_NTP,
    WIFI_ERR_UART, WIFI_ERR_CONFIG
};
/* Useful CCS Expressions; no secrets are written to a debug log. */
extern volatile uint32_t g_wifi_status;
extern volatile uint32_t g_wifi_last_error;
extern volatile uint32_t g_wifi_sync_count;
extern volatile uint32_t g_wifi_bad_packets;
void WifiClockInit(uint32_t system_clock);
void WifiClockPoll(uint32_t tick100hz);
bool WifiClockTakeTime(WifiClockTime *time);
#endif
