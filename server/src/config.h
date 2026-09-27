#pragma once
#include <time.h>
#include "logbuffer.h"

// Low: exposed in /api/status so an operator can confirm every client and
// the server were flashed from the same build (no version info existed at all before).
#define FW_VERSION "6.2-solo"

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

// ── Часовой пояс и NTP (см. timesync.h) ──────────────────────────────────
// Системные часы и DS3231 хранят UTC, локальное время — через POSIX TZ.
// "MSK-3" = UTC+3 без перехода на летнее время. Другой пояс — например
// "<+05>-5" (Екатеринбург) или "EET-2EEST,M3.5.0/3,M10.5.0/4" (Киев).
#ifndef TIME_TZ
#define TIME_TZ                "MSK-3"
#endif
#define NTP_SERVER1            "ru.pool.ntp.org"
#define NTP_SERVER2            "pool.ntp.org"
#define NTP_SERVER3            "time.google.com"
#define NTP_SYNC_INTERVAL_MS   3600000UL       // опрос NTP раз в час, пока есть STA
#define NTP_STALE_MS           (2 * NTP_SYNC_INTERVAL_MS)  // дольше без NTP — "интернета нет"
#define RTC_RELOAD_MS          3600000UL       // тогда раз в час подтягиваем DS3231

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
#define SCHEDULE_FILE     "/gong.conf"   // только первый запуск: засев Дня 00 (дальше — dayNN.conf)
#define LOOP_MAX          7              // повторов трека за одно срабатывание
// Описание записи ограничено в байтах UTF-8 (кириллица — 2 байта/символ, т.е.
// ~48 букв). Без лимита 32 длинных описания переполняли JSON-пул: API отвечал
// "ok", а сохранение молча отменялось. 32 × 96 Б укладываются в пул с запасом.
#define DESC_MAX_BYTES    96

// ── Многодневный курс ──────────────────────────────────────────────────────
#define DAY_COUNT         12   // day00.conf .. day11.conf

// ── Аутентификация веб-админки ────────────────────────────────────────────
#define AUTH_CONFIG_FILE  "/auth.conf"   // устаревшее: с 6.2 — в NVS, файл импортируется один раз
#define AUTH_REALM        "Gong Server"
#define AUTH_MIN_PASSWORD 8
// Сброс пароля админки без перепрошивки: держать кнопку гонга (BUTTON_PIN)
// нажатой при включении питания AUTH_RESET_HOLD_MS — светодиод горит, пока
// держите; после сброса вход снова открыт. Нужен, потому что пока пароль не
// задан, его может задать любой подключившийся к AP и запереть владельца.
#define AUTH_RESET_HOLD_MS 10000UL

// ── WiFi STA (подключение к существующей сети) ─────────────────────────────
#define WIFI_CONFIG_FILE        "/wifi.conf"   // устаревшее: с 6.2 — в NVS
// Пока STA не подключён, ESP32 сканирует каналы — это рвёт связь с клиентами
// собственной AP. Поэтому никакого непрерывного auto-reconnect: одна попытка
// раз в WIFI_RETRY_MS, между попытками AP работает спокойно.
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
#define WIFI_RETRY_MS           60000UL

// ── Реле (питание внешнего усилителя / трансляционной линии) ───────────────
// GPIO27: не strapping-пин, не input-only, при загрузке не дёргается.
// Освободившиеся после LoRa пины (4, 14, 18, 19, 23; на печатной плате они
// разведены на разъём Ra-02 и свободны, только пока модуль не вставлен) тоже
// подойдут; GPIO5 — нет, он strapping. Большинство китайских модулей реле с оптроном
// включаются НИЗКИМ уровнем — тогда соберите с -DRELAY_ACTIVE_LOW=1.
// До первого digitalWrite() пин висит в воздухе: на плате нужна подтяжка к
// «неактивному» уровню (10 кОм), иначе реле может щёлкнуть при старте.
#ifndef RELAY_PIN
#define RELAY_PIN               27
#endif
#ifndef RELAY_ACTIVE_LOW
#define RELAY_ACTIVE_LOW        0
#endif
#define RELAY_CONFIG_FILE       "/relay.conf"  // устаревшее: с 6.2 — в NVS
// Авто-режим: реле включается ДО звука (усилителю нужно время выйти на режим,
// иначе начало удара гонга срезается) и держится после окончания трека.
#define RELAY_DEFAULT_PRE_MS    800UL
#define RELAY_DEFAULT_HOLD_MS   3000UL
#define RELAY_MAX_PRE_MS        5000UL    // прогрев усилителя до гонга
#define RELAY_MAX_HOLD_MS       20000UL   // удержание ПОСЛЕ окончания гонга

// ── Физическая кнопка (запуск гонга мимо веб-интерфейса) ─────────────────
// Кнопка между BUTTON_PIN и GND, внутренняя подтяжка к 3.3V включена.
// Срабатывает по УДЕРЖАНИЮ, а не по касанию — случайное нажатие (задели
// плечом, ребёнок, наводка) гонг не запустит:
//   в тишине          — держать BUTTON_PLAY_HOLD_MS → гонг (BUTTON_TRACK/VOL/LOOP)
//   во время звучания — держать BUTTON_STOP_HOLD_MS → стоп
// Срабатывает один раз за нажатие, ещё пока кнопка зажата; дальше ждёт отпускания.
// GPIO32: не strapping, есть внутренний pull-up (у GPIO34–39 его нет).
// Раньше была GPIO4 — на печатной плате она занята LoRa DIO0.
// При проводе к кнопке длиннее ~30 см добавьте внешний 10 кОм к 3.3V и
// 100 нФ к GND у пина — наводки от сети/усилителя дают ложные срабатывания.
#ifndef BUTTON_PIN
#define BUTTON_PIN              32
#endif
#define BUTTON_DEBOUNCE_MS      50UL
#define BUTTON_PLAY_HOLD_MS     3000UL
#define BUTTON_STOP_HOLD_MS     1000UL
#define BUTTON_TRACK            DEFAULT_TRACK
#define BUTTON_VOL              DEFAULT_VOLUME
#define BUTTON_LOOP             1

// ── Светодиод состояния (statusled.h) ───────────────────────────────────────
// GPIO13 — LED2 на печатной плате (как STATUS_LED у клиента). Раньше был
// GPIO2 (встроенный синий DevKit) — это strapping-пин, на плате он NC.
// Для макета без платы: -DSTATUS_LED_PIN=2 — уровень GPIO2
// читается только в момент сброса — выход после загрузки прошивке не мешает.
// -1 — отключить.
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN          13
#endif
#ifndef STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW   0
#endif

// ── M-14: догоняющее срабатывание после перезагрузки ────────────────────────
// Если сервер перезагрузился в узком окне вокруг времени гонга, тот гонг не
// должен пропадать бесследно — и не должен звонить второй раз, если он уже
// успел сработать. См. schedule.cpp: sched_setup()/CATCHUP.
#define CATCHUP_WINDOW_S   120UL   // считаем пропущенным, если ребут был не позже этого окна
