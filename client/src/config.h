#pragma once

// =============================================================
//  CLIENT CONFIG — change CLIENT_ID for EVERY device before flashing!
// =============================================================
#define CLIENT_ID      "tester"   // unique per device, e.g. "room_A"

// ── LoRa module pins — same wiring as the server (Ra-02, SMA antenna) ─────
#define LORA_SS        5   // NSS
#define LORA_RST       14  // RST
#define LORA_DIO0      4   // DIO0

#define LORA_SPI_SCK   18
#define LORA_SPI_MISO  19
#define LORA_SPI_MOSI  23

// ── LoRa RF settings — MUST match the server's config.h exactly, or the
// devices will simply never hear each other (no error, just silence) ──────
#define LORA_FREQ      433.0     // MHz — already MHz, pass unconverted to radio.begin()
#define LORA_SF        9         // Spreading factor 7..12
#define LORA_BW        125.0     // kHz — already kHz, pass unconverted to radio.begin()
#define LORA_CR        5         // Coding rate 5..8 (4/5)
#define LORA_SYNC_WORD 0xF3      // Private network word
#define LORA_TX_POWER  17        // dBm

// ── LoRa message types (must match server) ─────────────────────────────────
#define MSG_GONG       0x01
#define MSG_HEARTBEAT  0x02
#define MSG_SCHEDULE   0x03
#define MSG_ACK        0x04
#define MSG_STOP       0x05

// ── LoRa HMAC-подпись — должен совпадать с сервером! ───────────────────────
#define LORA_HMAC_KEY  "change_me_before_deploy_32chars!"

// ── I2S pins for MAX98357A ──────────────────────────────────────────────
#define I2S_BCLK       26   // Bit Clock
#define I2S_LRC        25   // Left/Right Clock (Word Select)
#define I2S_DOUT       33   // Data Out

// Status LED; set to -1 to disable (GPIO33 is used by I2S_DOUT)
#define STATUS_LED     -1

#define DEFAULT_VOLUME 25
