#pragma once

// =============================================================
//  CLIENT CONFIG — change CLIENT_ID for every device!
// =============================================================
#define CLIENT_ID  "client_001"   // unique name, e.g. "room_A"

// LoRa module pins — same wiring as server
#define LORA_SS        5
#define LORA_RST       14
#define LORA_DIO0      2

// LoRa RF settings — must match server exactly
#define LORA_FREQ      433E6
#define LORA_SF        7
#define LORA_BW        125E3
#define LORA_CR        5
#define LORA_SYNC_WORD 0xF3
#define LORA_TX_POWER  20

// Message types
#define MSG_GONG       0x01
#define MSG_HEARTBEAT  0x02
#define MSG_SCHEDULE   0x03
#define MSG_ACK        0x04

// MP3 module pins
#define MP3_RX         16
#define MP3_TX         17
#define MP3_BUSY       4

// Status LED (onboard LED on many boards is GPIO2, but that's DIO0)
// Use GPIO33 or any free GPIO; set to -1 to disable
#define STATUS_LED     33

#define DEFAULT_VOLUME 25
