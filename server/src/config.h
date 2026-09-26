#pragma once
#include <time.h>
#include "logbuffer.h"

// Low: exposed in /api/status so an operator can confirm every client and
// the server were flashed from the same build (no version info existed at all before).
#define FW_VERSION "6.0-solo"

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

// ── WiFi ──────────────────────────────────────────────────────────────────
// Своя точка доступа поднимается ВСЕГДА — это аварийный вход в админку, даже
// если домашняя сеть недоступна или пароль к ней неверный. Дополнительно
// устройство может подключиться к существующей сети (STA) — SSID/пароль
// задаются в веб-интерфейсе и хранятся в WIFI_CONFIG_FILE (в .gitignore).
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

// ── WiFi STA (подключение к существующей сети) ─────────────────────────────
#define WIFI_CONFIG_FILE        "/wifi.conf"
// Пока STA не подключён, ESP32 сканирует каналы — это рвёт связь с клиентами
// собственной AP. Поэтому никакого непрерывного auto-reconnect: одна попытка
// раз в WIFI_RETRY_MS, между попытками AP работает спокойно.
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
#define WIFI_RETRY_MS           60000UL

// ── Реле (питание внешнего усилителя / трансляционной линии) ───────────────
// GPIO27: не strapping-пин, не input-only, при загрузке не дёргается.
// Освободившиеся после LoRa пины (4, 5, 14, 18, 19, 23) тоже подойдут, кроме
// GPIO5 — он strapping. Большинство китайских модулей реле с оптроном
// включаются НИЗКИМ уровнем — тогда соберите с -DRELAY_ACTIVE_LOW=1.
// До первого digitalWrite() пин висит в воздухе: на плате нужна подтяжка к
// «неактивному» уровню (10 кОм), иначе реле может щёлкнуть при старте.
#ifndef RELAY_PIN
#define RELAY_PIN               27
#endif
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW        0
#endif
#define RELAY_CONFIG_FILE       "/relay.conf"
// Авто-режим: реле включается ДО звука (усилителю нужно время выйти на режим,
// иначе начало удара гонга срезается) и держится после окончания трека.
#define RELAY_DEFAULT_PRE_MS    800UL
#define RELAY_DEFAULT_HOLD_MS   3000UL

// ── M-14: догоняющее срабатывание после перезагрузки ────────────────────────
// Если сервер перезагрузился в узком окне вокруг времени гонга, тот гонг не
// должен пропадать бесследно — и не должен звонить второй раз, если он уже
// успел сработать. См. schedule.cpp: sched_setup()/CATCHUP.
#define CATCHUP_WINDOW_S   120UL   // считаем пропущенным, если ребут был не позже этого окна
