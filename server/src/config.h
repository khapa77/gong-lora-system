#pragma once
#include <time.h>
#include "logbuffer.h"
#include "lora_shared.h"

// Low: exposed in /api/status so an operator can confirm every client and
// the server were flashed from the same build (no version info existed at all before).
#define FW_VERSION "5.0"

// ── Время: неблокирующая замена getLocalTime() ─────────────────────────────
// Штатный getLocalTime(tm*, ms=5000) крутит delay(10) до 5 СЕКУНД, если время
// ещё не установлено — а "время не установлено" это состояние ПО УМОЛЧАНИЮ
// на устройстве без DS3231 (и после разряда его батарейки). Это превращало
// каждый вызов /api/status, sched_check() (раз в секунду!) и heartbeat в
// многосекундное зависание всего главного цикла. См. 01_AUDIT_REPORT.md C-1.
#define TIME_VALID_EPOCH 1700000000UL   // 2023-11-14 — всё раньше считаем "не задано"

static inline bool timeIsSet() {
    return (uint32_t)time(nullptr) > TIME_VALID_EPOCH;
}

static inline bool localNow(struct tm& out) {
    time_t t = time(nullptr);
    if ((uint32_t)t <= TIME_VALID_EPOCH) return false;
    localtime_r(&t, &out);
    return true;
}

// ── WiFi (standalone AP only — never joins another network) ───────────────
#define AP_SSID           "GongServer"
#define MDNS_NAME         "gong"        // http://gong.local
// Смените перед развёртыванием! Можно передать через build_flags вместо
// правки файла — see platformio.ini:
//   build_flags = -DAP_PASSWORD='"${sysenv.GONG_AP_PASSWORD}"'
#ifndef AP_PASSWORD
#define AP_PASSWORD       "vipassana"   // минимум 8 символов для WPA2
#endif
static_assert(sizeof(AP_PASSWORD) - 1 >= 8, "AP_PASSWORD короче 8 символов (минимум для WPA2)");

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
// Общие RF-параметры и типы сообщений — см. lora_shared.h (common/).
// Историческая ошибка: конверсия единиц измерения молча ломала radio.begin()
// (см. git history "bug-lora-init-units") — LORA_FREQ/LORA_BW уже в MHz/kHz.

#define LORA_SS           5   // NSS
#define LORA_RST          14  // RST
#define LORA_DIO0         4   // DIO0 — GPIO2 использовался раньше и мешал прошивке, не путать

#define LORA_SPI_SCK      18
#define LORA_SPI_MISO     19
#define LORA_SPI_MOSI     23

// ── LoRa HMAC-подпись ─────────────────────────────────────────────────────
// Секретный ключ — одинаковый на сервере и на ВСЕХ клиентах.
// Смените перед развёртыванием (минимум 16 символов)! H-7: можно передать
// ключ через build_flags вместо правки файла — see platformio.ini:
//   build_flags = -DLORA_HMAC_KEY='"${sysenv.GONG_KEY}"'
#ifndef LORA_HMAC_KEY
#define LORA_HMAC_KEY           "change_me_before_deploy_32chars!"
#endif
static_assert(sizeof(LORA_HMAC_KEY) - 1 >= 16, "LORA_HMAC_KEY короче 16 символов");

// ── LoRa тайминги ─────────────────────────────────────────────────────────
#define HEARTBEAT_INTERVAL_MS   30000UL
#define CLIENT_TIMEOUT_MS       90000UL

// ── M-14: догоняющее срабатывание после перезагрузки ────────────────────────
// Если сервер перезагрузился в узком окне вокруг времени гонга, тот гонг не
// должен пропадать бесследно — и не должен звонить второй раз, если он уже
// успел сработать. См. schedule.cpp: sched_setup()/CATCHUP.
#define CATCHUP_WINDOW_S   120UL   // считаем пропущенным, если ребут был не позже этого окна
