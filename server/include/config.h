#pragma once

// =============================================================
//  SERVER CONFIG — edit this file to match your hardware
// =============================================================

// LoRa module pins (SPI VSPI: SCK=18 MISO=19 MOSI=23)
#define LORA_SS        5
#define LORA_RST       14
#define LORA_DIO0      2

// LoRa RF settings — must match clients exactly
#define LORA_FREQ      433E6     // 433 MHz
#define LORA_SF        7         // Spreading factor 7..12
#define LORA_BW        125E3     // Bandwidth Hz
#define LORA_CR        5         // Coding rate 5..8
#define LORA_SYNC_WORD 0x12      // Private network word
#define LORA_TX_POWER  20        // dBm (max 20)

// LoRa message types
#define MSG_GONG       0x01      // Server -> clients: play gong
#define MSG_HEARTBEAT  0x02      // Server -> clients: I'm alive + time
#define MSG_SCHEDULE   0x03      // Server -> clients: schedule dump
#define MSG_ACK        0x04      // Client -> server: acknowledgment

// MP3 module (DFPlayer Mini / MP3-TF-16P) pins
#define MP3_RX         16        // ESP32 RX  <-- MP3 TX
#define MP3_TX         17        // ESP32 TX  --> MP3 RX
#define MP3_BUSY       4         // MP3 BUSY  --> ESP32 (LOW = playing)

// WiFi fallback AP credentials
#define AP_SSID        "GongServer"
#define AP_PASSWORD    "vipassana"

// WiFi STA connection timeout
#define WIFI_TIMEOUT_MS 30000

// NTP
#define NTP_SERVER       "pool.ntp.org"
#define NTP_UTC_OFFSET   0       // seconds (e.g. 10800 = UTC+3)
#define NTP_INTERVAL_MS  60000

// SPIFFS file paths
#define SCHEDULE_FILE    "/schedule.json"
#define WIFI_CONFIG_FILE "/wifi.conf"

// Schedule
#define MAX_SCHEDULES   30

// Defaults
#define DEFAULT_TRACK   1
#define DEFAULT_VOLUME  25

// How often server broadcasts heartbeat to clients
#define HEARTBEAT_INTERVAL_MS 30000
