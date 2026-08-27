#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mbedtls/md.h"

// ── Общий заголовок протокола LoRa — подключается сервером и клиентом ──────
// Вынесено из обоих config.h, т.к. эти константы ОБЯЗАНЫ совпадать на всех
// устройствах — расхождение не даёт ошибки, просто устройства перестают
// слышать друг друга (см. 01_AUDIT_REPORT.md, раздел 6).

// Версия протокола кадра — растёт при любом несовместимом изменении формата.
// Позволяет диагностировать рассинхронизацию парка устройств вместо молчания
// без единой ошибки в логе.
#define LORA_PROTO_VERSION 5   // 4: fixed-offset signed frame (H-3), binary schedule (H-5)
                                // 5: "n" sub-second replay tie-breaker added to every signed
                                //    frame incl. MSG_SCHEDULE's wire layout (code_review.md C2)

// ── LoRa радио-параметры (Ra-02 / SX1278) ───────────────────────────────────
// ВАЖНО: LORA_FREQ и LORA_BW уже в MHz/kHz — передавать в radio.begin()
// БЕЗ деления на 1e6/1e3.
#define LORA_FREQ      433.0     // MHz
#define LORA_SF        7         // Spreading factor — SF7: короче airtime,
                                  // меньше окно коллизий ACK (см. C-3)
#define LORA_BW        125.0     // kHz
#define LORA_CR        5         // Coding rate 5..8 (4/5)
#define LORA_SYNC_WORD 0xF3      // Приватное слово синхронизации
#define LORA_TX_POWER  17        // dBm

// Максимум полезной нагрузки LoRa-кадра — физический лимит модема 255 байт,
// минус 1 байт типа сообщения (см. M-4).
#define LORA_PAYLOAD_MAX 254

// ── LoRa типы сообщений ──────────────────────────────────────────────────
#define MSG_GONG          0x01
#define MSG_HEARTBEAT     0x02
#define MSG_SCHEDULE      0x03
#define MSG_ACK            0x04
#define MSG_STOP           0x05

// ── Формат кадра (H-3 + M-5 + M-6) ─────────────────────────────────────────
// [1 байт: тип][8 байт: HMAC-тег][payload]
// Тег — на ФИКСИРОВАННОМ смещении и считается по (тип || payload) напрямую из
// принятых байт, без ре-сериализации JSON. Раньше подпись дописывалась
// строковой хирургией в конец JSON (`substring(0,len-1)+",\"sig\":...}"`,
// ломалось на "{}") и проверялась через parse→remove("sig")→ре-serialize —
// работало только пока ArduinoJson сохраняет порядок/типы полей побайтово.
// Кадр подписывается для MSG_GONG/HEARTBEAT/STOP/SCHEDULE. MSG_ACK не
// подписывается — сервер не выполняет по нему никаких действий.
#define LORA_TAG_LEN 8

static inline void lora_hmacTag(const char* key, uint8_t type,
                                 const uint8_t* data, size_t len,
                                 uint8_t out[LORA_TAG_LEN]) {
    uint8_t full[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, &type, 1);
    if (len) mbedtls_md_hmac_update(&ctx, data, len);
    mbedtls_md_hmac_finish(&ctx, full);
    mbedtls_md_free(&ctx);
    memcpy(out, full, LORA_TAG_LEN);
}

// Constant-time compare — a signature check must not leak timing info about
// how many leading bytes matched.
static inline bool lora_tagEqual(const uint8_t* a, const uint8_t* b) {
    uint8_t d = 0;
    for (int i = 0; i < LORA_TAG_LEN; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

// ── C-3: детерминированные слоты ACK ────────────────────────────────────────
// Заменяют случайный джиттер 0-70мс (в 4 раза короче airtime ACK при SF9,
// откуда и коллизии при 2+ клиентах). Слот вычисляется из CLIENT_ID, поэтому
// клиенты никогда не отвечают одновременно. При SF7 airtime ACK ~87 мс.
#define ACK_SLOT_COUNT   16
#define ACK_SLOT_MS      120     // > airtime ACK на SF7 (87 мс) + запас
#define ACK_GUARD_MS     150     // пауза после heartbeat, пока сервер уходит в RX
// Полное окно ответов после каждого heartbeat/GONG:
#define ACK_WINDOW_MS    (ACK_GUARD_MS + ACK_SLOT_COUNT * ACK_SLOT_MS)

// ── H-5: автономное расписание на клиентах ──────────────────────────────────
// Сервер — единая точка отказа: если он умирает ночью, ни один клиент не
// прозвонит утренний подъём. Решение: сервер рассылает расписание активного
// дня в бинарном виде (влезает в один кадр — 3.5 КБ JSON не влезли бы), а
// клиент хранит его в NVS и берёт на себя расписание, если heartbeat не
// приходил дольше HEARTBEAT_LOST_MS (см. client/src/lorahandler.cpp).
#define SCHED_BIN_MAX 16   // сколько записей активного дня влезает в кадр

struct __attribute__((packed)) SchedBinHeader {
    uint8_t day;     // какой день курса (0..DAY_COUNT-1)
    uint8_t count;   // сколько SchedBin следует за заголовком
};

struct __attribute__((packed)) SchedBin {
    uint8_t hour;    // 0-23
    uint8_t minute;  // 0-59
    uint8_t track;   // 1-99
    uint8_t vol;     // 0-30 (M-12)
    uint8_t loopEn;  // биты 0-6: повторы 1-7, бит 7: enabled
};

static inline uint8_t schedbin_pack(uint8_t loopCount, bool enabled) {
    if (loopCount < 1) loopCount = 1;
    if (loopCount > 7) loopCount = 7;
    return (uint8_t)(loopCount | (enabled ? 0x80 : 0));
}
static inline uint8_t schedbin_loop(uint8_t loopEn)    { return loopEn & 0x7F; }
static inline bool    schedbin_enabled(uint8_t loopEn) { return (loopEn & 0x80) != 0; }
