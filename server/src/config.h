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

// ── Аутентификация веб-админки ────────────────────────────────────────────
#define AUTH_CONFIG_FILE  "/auth.conf"
#define AUTH_REALM        "Gong Server"
