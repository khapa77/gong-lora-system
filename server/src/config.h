#pragma once
#include <time.h>
#include "logbuffer.h"

// Low: exposed in /api/status so an operator can confirm every client and
// the server were flashed from the same build (no version info existed at all before).
#define FW_VERSION "6.2-max"

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
// К обоим добавляется номер устройства: "GongServer-2", http://gong2.local
// (см. device.h). Номер задаётся в веб-интерфейсе и хранится в NVS.
#define AP_SSID           "GongServer"
#define MDNS_NAME         "gong"
// Номер по умолчанию, пока в NVS его нет: 0 — имя из MAC ("GongServer-A3F2").
// Партию можно прошить сразу с номерами: build_flags = -DDEVICE_NUM=3
#ifndef DEVICE_NUM
#define DEVICE_NUM        0
#endif
#define DEVICE_NUM_MAX    99
#define DEVICE_NAME_MAX   32      // подпись в вебе, байт UTF-8 (~16 кириллических букв)
#define DEVICE_REBOOT_DELAY_MS 2000UL
// Смените перед развёртыванием! Можно передать через build_flags вместо
// правки файла — see platformio.ini:
//   build_flags = -DAP_PASSWORD='"${sysenv.GONG_AP_PASSWORD}"'
#ifndef AP_PASSWORD
#define AP_PASSWORD       "Vipassana7"   // минимум 8 символов для WPA2
#endif
static_assert(sizeof(AP_PASSWORD) - 1 >= 8, "AP_PASSWORD короче 8 символов (минимум для WPA2)");

// ── I2S пины для MAX98357A ────────────────────────────────────────────────
#define I2S_BCLK          26   // Bit Clock
#define I2S_LRC           25   // Left/Right Clock (Word Select)
#define I2S_DOUT          33   // Data Out

// ── Аудио (умолчания) ─────────────────────────────────────────────────────
#define DEFAULT_VOLUME    30   // 0–30 (макс по умолчанию)

// Потолок громкости (0–30): громкость 30 из веба/расписания звучит как
// VOLUME_LIMIT, остальные значения масштабируются пропорционально — шкала
// 0–30 и разница между записями сохраняются. 25 ≈ −3.5 дБ от максимума
// (шаг библиотеки 17 из 21): с треком 0001.mp3 (пик 0 dBFS) пиковый ток
// MAX98357A падает вдвое, и хватает HLK-PM01 (0.6 А) — см. docs/power.md.
// 30 — без ограничения (HLK-10M05 и динамик 4 Ом на полную громкость).
// Переопределить без правки файла: build_flags = -DVOLUME_LIMIT=30
#ifndef VOLUME_LIMIT
#define VOLUME_LIMIT      25
#endif
static_assert(VOLUME_LIMIT >= 1 && VOLUME_LIMIT <= 30, "VOLUME_LIMIT: 1–30");
#define DEFAULT_TRACK     1

// ── Физическая кнопка (ручной гонг + восстановление пароля) ────────────────
// Кнопка между BUTTON_PIN и GND, внутренняя подтяжка включена (INPUT_PULLUP).
// Поведение — только по удержанию, действие выполняется при отпускании:
//   простой, удержание < BUTTON_PLAY_HOLD_MS  → игнорируется;
//   простой, удержание ≥ BUTTON_PLAY_HOLD_MS  → ручной гонг (тот же путь,
//                                               что /api/play);
//   звук играет, удержание ≥ BUTTON_STOP_HOLD_MS → стоп (сразу, не дожидаясь
//                                               отпускания; повтора нет);
//   удержание ≥ BUTTON_AUTH_RESET_HOLD_MS при включении питания → сброс
//                                               пароля админки (см. authreset.h).
//
// ВАЖНО: -1 = кнопка отключена, и это значение ПО УМОЛЧАНИЮ. Плата, на
// которой снята принципиальная схема (footprint/schematic_netlist.md —
// GPIO32 = BUTTON, GPIO13 = STATUS_LED), это классический ESP32
// (ESP32-D0WD-V3), а текущий env собран под esp32-s3-devkitc-1, где GPIO32
// не существует. Пока целевая плата не подтверждена, конфигурация GPIO не
// активируется. Включение — после сверки платы, вместе с нейтральным пином:
//   build_flags = -DBUTTON_PIN=32 -DSTATUS_LED_PIN=13
// (см. docs/BUTTON_LED.md, раздел «Аппаратная конфигурация»).
#ifndef BUTTON_PIN
#define BUTTON_PIN              -1
#endif
#ifndef BUTTON_ACTIVE_LOW
#define BUTTON_ACTIVE_LOW       1      // кнопка замыкает пин на GND
#endif
#ifndef BUTTON_DEBOUNCE_MS
#define BUTTON_DEBOUNCE_MS      50UL   // уровень должен продержаться столько
#endif
#ifndef BUTTON_PLAY_HOLD_MS
#define BUTTON_PLAY_HOLD_MS     3000UL // порог ручного гонга (по отпусканию)
#endif
#ifndef BUTTON_STOP_HOLD_MS
#define BUTTON_STOP_HOLD_MS     1000UL // порог остановки уже играющего звука
#endif
#ifndef BUTTON_AUTH_RESET_HOLD_MS
#define BUTTON_AUTH_RESET_HOLD_MS 10000UL  // удержание при включении питания
#endif
#ifndef BUTTON_STUCK_MS
#define BUTTON_STUCK_MS         60000UL // залипший контакт: нажатие сбрасывается
#endif
// Источник конфигурации ручного гонга с кнопки: те же умолчания, что у
// /api/play без параметров (track=DEFAULT_TRACK, vol=DEFAULT_VOLUME, loop=1).
// Переопределяется build_flags, если на устройстве другой «ручной» трек.
#ifndef BUTTON_TRACK
#define BUTTON_TRACK            DEFAULT_TRACK
#endif
#ifndef BUTTON_VOL
#define BUTTON_VOL              DEFAULT_VOLUME
#endif
#ifndef BUTTON_LOOP
#define BUTTON_LOOP             1
#endif

#if BUTTON_PIN >= 0 && defined(CONFIG_IDF_TARGET_ESP32S3) && BUTTON_PIN > 21
#error "BUTTON_PIN does not exist on ESP32-S3: confirm the board in server/platformio.ini first"
#endif

// ── Светодиод состояния (statusled.h) ─────────────────────────────────────
// Отключён по умолчанию по той же причине, что и кнопка (см. выше).
//   норма   — короткая вспышка LED_HEARTBEAT_ON_MS раз в LED_HEARTBEAT_MS;
//   ошибка  — мигание 5 Гц (период LED_ERROR_PERIOD_MS), расписание не
//             выполнится (нет времени / нет LittleFS / ошибка расписания /
//             курс не опубликован);
//   подсказка кнопки — ровный свет, пока удержание ≥ BUTTON_PLAY_HOLD_MS.
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN          -1
#endif
#ifndef STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW   0      // LED2 на плате подключён анодом к GPIO13
#endif
#ifndef LED_HEARTBEAT_MS
#define LED_HEARTBEAT_MS        3000UL
#endif
#ifndef LED_HEARTBEAT_ON_MS
#define LED_HEARTBEAT_ON_MS     60UL
#endif
#ifndef LED_ERROR_PERIOD_MS
#define LED_ERROR_PERIOD_MS     200UL  // половина периода = 100 мс → 5 Гц
#endif
static_assert(LED_HEARTBEAT_ON_MS > 0 && LED_HEARTBEAT_ON_MS < LED_HEARTBEAT_MS,
              "LED_HEARTBEAT_ON_MS must be a short flash inside the period");
static_assert(LED_ERROR_PERIOD_MS >= 20, "LED_ERROR_PERIOD_MS: too fast to see");

#if STATUS_LED_PIN >= 0 && defined(CONFIG_IDF_TARGET_ESP32S3) && STATUS_LED_PIN > 21
#error "STATUS_LED_PIN does not exist on ESP32-S3: confirm the board in server/platformio.ini first"
#endif

// ── Расписание ────────────────────────────────────────────────────────────
#define MAX_SCHEDULES     32
#define SCHEDULE_FILE     "/gong.conf"   // только первый запуск: засев Дня 00 (дальше — dayNN.conf)
#define LOOP_MAX          7              // повторов трека за одно срабатывание
// Описание записи ограничено в байтах UTF-8 (кириллица — 2 байта/символ, т.е.
// ~48 букв). Без лимита 32 длинных описания переполняли JSON-пул: API отвечал
// "ok", а сохранение молча отменялось. 32 × 96 Б укладываются в пул с запасом.
#define DESC_MAX_BYTES    96

// ── Многодневный курс ──────────────────────────────────────────────────────
#define DAY_COUNT         13   // day00.conf .. day12.conf (day12 = changeover)

// ── Аутентификация веб-админки ────────────────────────────────────────────
#define AUTH_CONFIG_FILE  "/auth.conf"   // устаревшее: с 6.2 — в NVS, файл импортируется один раз
#define AUTH_NVS_NAMESPACE "auth"        // единственное состояние, которое стирает сброс пароля
// NVS ограничивает имена 15 символами (NVS_KEY_NAME_MAX_SIZE = 16 с '\0');
// проверяется здесь, потому что на устройстве ошибка выглядит как «write failed».
static_assert(sizeof(AUTH_NVS_NAMESPACE) - 1 <= 15, "NVS namespace longer than 15 chars");
#define AUTH_REALM        "Gong Server"
#define AUTH_MIN_PASSWORD 8
// Сброс пароля админки без перепрошивки — два независимых пути, оба вызывают
// web_resetAuth(): физическая кнопка (удержание BUTTON_AUTH_RESET_HOLD_MS при
// включении питания, см. buttonhandler.h) и команда AUTH_RESET_CMD в Serial по
// USB (115200). Пароль стирается только в NVS-неймспейсе "auth" и в
// устаревшем /auth.conf; WiFi, курсы, расписания, номер устройства, RTC и
// громкость не затрагиваются (см. authreset.h). Стереть NVS целиком нельзя:
// пропадёт отметка миграции DS3231, и время сдвинется.
#define AUTH_RESET_CMD    "reset-auth"

// ── WiFi STA (подключение к существующей сети) ─────────────────────────────
#define WIFI_CONFIG_FILE        "/wifi.conf"   // устаревшее: с 6.2 — в NVS
// Пока STA не подключён, ESP32 сканирует каналы — это рвёт связь с клиентами
// собственной AP. Поэтому никакого непрерывного auto-reconnect: одна попытка
// раз в WIFI_RETRY_MS, между попытками AP работает спокойно.
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
#define WIFI_RETRY_MS           60000UL

// ── M-14: догоняющее срабатывание после перезагрузки ────────────────────────
// Если сервер перезагрузился в узком окне вокруг времени гонга, тот гонг не
// должен пропадать бесследно — и не должен звонить второй раз, если он уже
// успел сработать. См. schedule.cpp: sched_setup()/CATCHUP.
#define CATCHUP_WINDOW_S   120UL   // считаем пропущенным, если ребут был не позже этого окна
