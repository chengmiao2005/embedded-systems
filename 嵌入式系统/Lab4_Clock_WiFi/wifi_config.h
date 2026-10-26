#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H
/* UART1: PC4 <- ESP TX, PC5 -> ESP RX. SW1 is PF4, active low.
 * All UART signals are 3.3 V. See docs/开始使用.txt before wiring.
 */
#define LAB4_ESP_BAUD 115200U

/* Empty password: use the Wi-Fi connection remembered by the ESP.
 * The module has already joined your phone hotspot during the serial test.
 * To make the MCU join explicitly, enter your SSID and password locally here.
 * Never send your password in a screenshot. C strings escape " and \\.
 */
#define LAB4_WIFI_SSID "Miles"
#define LAB4_WIFI_PASSWORD ""

/* Public NTP servers; port 123/UDP. No PC TCP Server/IP is required. */
#define LAB4_NTP_PRIMARY "ntp.aliyun.com"
#define LAB4_NTP_BACKUP  "time.cloudflare.com"
#endif
