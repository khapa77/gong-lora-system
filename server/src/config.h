#pragma once

// ── WiFi ──────────────────────────────────────────────────────────────────
#define AP_SSID           "GongServer"
#define AP_PASSWORD       "vipassana"   // минимум 8 символов для WPA2
#define WIFI_TIMEOUT_MS   10000
#define WIFI_CONFIG_FILE  "/wifi.conf"

// ── NTP ───────────────────────────────────────────────────────────────────
#define NTP_SERVER        "pool.ntp.org"
#define NTP_UTC_OFFSET    10800      // секунды; UTC+3 (Москва)

// ── LoRa (SX1276 / SX1278) ────────────────────────────────────────────────
#define LORA_SS           5
#define LORA_RST          14
#define LORA_DIO0         2
#define LORA_FREQ         433E6      // 433 МГц; меняй на 868E6 / 915E6
#define LORA_SYNC_WORD    0xF3
#define LORA_SF           7
#define LORA_BW           125E3
#define LORA_CR           5
#define LORA_TX_POWER     20

// ── LoRa типы сообщений ───────────────────────────────────────────────────
#define MSG_GONG          0x01
#define MSG_HEARTBEAT     0x02
#define MSG_SCHEDULE      0x03
#define MSG_ACK           0x04

// ── I2S пины для MAX98357A ────────────────────────────────────────────────
#define I2S_BCLK          26   // Bit Clock
#define I2S_LRC           25   // Left/Right Clock (Word Select)
#define I2S_DOUT          33   // Data Out

// ── Аудио (умолчания) ─────────────────────────────────────────────────────
#define DEFAULT_VOLUME    30   // 0–30 (макс по умолчанию)
#define DEFAULT_TRACK     1

// ── Расписание ────────────────────────────────────────────────────────────
#define MAX_SCHEDULES     32
#define SCHEDULE_FILE     "/gong.conf"

// ── Аутентификация веб-админки ────────────────────────────────────────────
#define AUTH_CONFIG_FILE  "/auth.conf"
#define AUTH_REALM        "Gong Server"

// ── Тайминги ──────────────────────────────────────────────────────────────
#define HEARTBEAT_INTERVAL_MS   30000UL
#define CLIENT_TIMEOUT_MS       90000UL
