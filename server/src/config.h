#pragma once
#include "logbuffer.h"

// ── WiFi (standalone AP only — never joins another network) ───────────────
#define AP_SSID           "GongServer"
#define MDNS_NAME         "gong"        // http://gong.local
#define AP_PASSWORD       "vipassana"   // минимум 8 символов для WPA2

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

// ── Многодневный курс ──────────────────────────────────────────────────────
#define DAY_COUNT         12   // day00.conf .. day11.conf

// ── Аутентификация веб-админки ────────────────────────────────────────────
#define AUTH_CONFIG_FILE  "/auth.conf"
#define AUTH_REALM        "Gong Server"

// ── LoRa (Ra-02 / SX1278, внешняя SMA-антенна) ─────────────────────────────
// ВАЖНО: LORA_FREQ и LORA_BW уже в MHz/kHz — передавать в radio.begin()
// БЕЗ деления на 1e6/1e3. Историческая ошибка: конверсия единиц измерения
// молча ломала radio.begin() (см. git history "bug-lora-init-units").
#define LORA_FREQ      433.0     // MHz
#define LORA_SF        9         // Spreading factor 7..12 — SF9 = "сбалансированный" режим
#define LORA_BW        125.0     // kHz
#define LORA_CR        5         // Coding rate 5..8 (4/5)
#define LORA_SYNC_WORD 0xF3      // Приватное слово синхронизации (не публичное LoRaWAN 0x34)
#define LORA_TX_POWER  17        // dBm — стартовое значение для SMA-антенны Ra-02

#define LORA_SS           5   // NSS
#define LORA_RST          14  // RST
#define LORA_DIO0         4   // DIO0 — GPIO2 использовался раньше и мешал прошивке, не путать

#define LORA_SPI_SCK      18
#define LORA_SPI_MISO     19
#define LORA_SPI_MOSI     23

// ── LoRa типы сообщений ───────────────────────────────────────────────────
#define MSG_GONG          0x01
#define MSG_HEARTBEAT     0x02
#define MSG_SCHEDULE      0x03
#define MSG_ACK           0x04
#define MSG_STOP          0x05

// ── LoRa HMAC-подпись ─────────────────────────────────────────────────────
// Секретный ключ — одинаковый на сервере и на ВСЕХ клиентах.
// Смените перед развёртыванием (минимум 16 символов)!
#define LORA_HMAC_KEY           "change_me_before_deploy_32chars!"

// ── LoRa тайминги ─────────────────────────────────────────────────────────
#define HEARTBEAT_INTERVAL_MS   30000UL
#define CLIENT_TIMEOUT_MS       90000UL
