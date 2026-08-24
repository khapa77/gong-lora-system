#pragma once
#include "lora_shared.h"

// =============================================================
//  CLIENT CONFIG — change CLIENT_ID for EVERY device before flashing!
// =============================================================
// Раскомментировать для говорящего имени; иначе CLIENT_ID выводится из MAC
// (см. clientId() в lorahandler.cpp — H-6: дефолт больше нельзя "забыть поменять").
// #define CLIENT_ID_OVERRIDE "room_A"

// ── LoRa module pins — same wiring as the server (Ra-02, SMA antenna) ─────
#define LORA_SS        5   // NSS
#define LORA_RST       14  // RST
#define LORA_DIO0      4   // DIO0

#define LORA_SPI_SCK   18
#define LORA_SPI_MISO  19
#define LORA_SPI_MOSI  23

// RF-параметры и типы сообщений — см. lora_shared.h (common/). ДОЛЖНЫ
// совпадать с сервером, иначе устройства просто не услышат друг друга.

// ── LoRa HMAC-подпись — должен совпадать с сервером! ───────────────────────
// H-7: можно передать через build_flags вместо правки файла — see
// platformio.ini: build_flags = -DLORA_HMAC_KEY='"${sysenv.GONG_KEY}"'
#ifndef LORA_HMAC_KEY
#define LORA_HMAC_KEY  "change_me_before_deploy_32chars!"
#endif
static_assert(sizeof(LORA_HMAC_KEY) - 1 >= 16, "LORA_HMAC_KEY короче 16 символов");

// ── I2S pins for MAX98357A ──────────────────────────────────────────────
#define I2S_BCLK       26   // Bit Clock
#define I2S_LRC        25   // Left/Right Clock (Word Select)
#define I2S_DOUT       33   // Data Out

// Status LED (H-11) — без экрана, сети и кнопок это единственный способ
// понять "жив ли клиент, слышит ли он сервер" без ноутбука по USB.
// GPIO13/32 свободны (см. 01_AUDIT_REPORT.md 1.2); set to -1 to disable.
#define STATUS_LED     13

#define DEFAULT_VOLUME 25

// ── H-5: автономный режим при потере связи с сервером ───────────────────────
#define HEARTBEAT_LOST_MS   (10UL * 60UL * 1000UL)   // 10 минут без heartbeat → автономно
