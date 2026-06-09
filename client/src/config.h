#pragma once

// =============================================================
//  CLIENT CONFIG — change CLIENT_ID for every device!
// =============================================================
#define CLIENT_ID  "tester"   // unique name, e.g. "room_A"

// LoRa module pins — same wiring as server
#define LORA_SS        5
#define LORA_RST       14
#define LORA_DIO0      4

// LoRa RF settings — must match server exactly

#define LORA_FREQ      433.0     // 433 MHz
#define LORA_SF        7         // Spreading factor 7..12
#define LORA_BW        125.0     // Bandwidth kHz
#define LORA_CR        5         // Coding rate 5..8
#define LORA_SYNC_WORD 0x12      // Private network word
#define LORA_TX_POWER  20        // dBm (max 20)

// Message types
#define MSG_GONG       0x01
#define MSG_HEARTBEAT  0x02
#define MSG_SCHEDULE   0x03
#define MSG_ACK        0x04
#define MSG_STOP       0x05

// I2S pins for MAX98357A
#define I2S_BCLK       26   // Bit Clock
#define I2S_LRC        25   // Left/Right Clock (Word Select)
#define I2S_DOUT       33   // Data Out

// Status LED; set to -1 to disable (GPIO33 used by I2S_DOUT)
#define STATUS_LED     -1

#define DEFAULT_VOLUME 25

// LoRa HMAC-подпись — должен совпадать с сервером!
// #define LORA_HMAC_KEY  "change_me_before_deploy_32chars!"
#define LORA_HMAC_KEY           "REDACTED_HMAC_KEY"
