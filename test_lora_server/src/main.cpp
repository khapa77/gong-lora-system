// LoRa Ra-02 Simple Test - SERVER
// Sends a ping message every 10 seconds, waits for pong response
// Hardware: ESP32 DevKit + Ra-02 (SX1278) module
// Pins: SS=5, RST=14, DIO0=4

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <esp_task_wdt.h>

// LoRa pins (match config.h)
#define LORA_SS      5
#define LORA_RST     14
#define LORA_DIO0    4

// LoRa parameters
#define LORA_FREQ    433.0
#define LORA_SF      9
#define LORA_BW      125.0
#define LORA_CR      5
#define LORA_SYNC    0xF3
#define LORA_TX_PWR  20

// Module instance
Module mod(LORA_SS, LORA_DIO0, LORA_RST);
SX1278 radio(&mod);

volatile bool dioFlag = false;
bool txBusy = false;

void IRAM_ATTR onDio0() {
  dioFlag = true;
}

void setup() {
  // Disable task watchdog to prevent resets during long LoRa operations
  esp_task_wdt_deinit();

  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== LoRa Ra-02 Test SERVER ===");

  SPI.begin(18, 19, 23, LORA_SS);

  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_TX_PWR);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[ERROR] LoRa init failed: %d\n", state);
    while (true) delay(1000);
  }

  Serial.printf("[OK] LoRa ready @ %.0f MHz SF=%d BW=%.0fk CR=4/%d Sync=0x%02X Pwr=%ddBm\n",
                LORA_FREQ, LORA_SF, LORA_BW, LORA_CR, LORA_SYNC, LORA_TX_PWR);

  pinMode(LORA_DIO0, INPUT);
  attachInterrupt(digitalPinToInterrupt(LORA_DIO0), onDio0, RISING);

  radio.startReceive();
  Serial.println("[SERVER] Listening for pings... sending ping every 10s\n");
}

unsigned long lastPing = 0;
unsigned long pingId = 0;

void loop() {
  // Check for received packet
  if (dioFlag && !txBusy) {
    dioFlag = false;

    size_t len = radio.getPacketLength();
    if (len > 0 && len <= 64) {
      char buf[65];
      if (radio.readData((uint8_t*)buf, len) == RADIOLIB_ERR_NONE) {
        buf[len] = '\0';
        int rssi = radio.getRSSI();
        float snr = radio.getSNR();
        Serial.printf("[RX] RSSI=%d dBm  SNR=%.1f dB  : %s\n", rssi, snr, buf);
      }
    }
    radio.startReceive();
  }

  // Send ping every 10 seconds
  if (!txBusy && millis() - lastPing >= 10000) {
    lastPing = millis();
    pingId++;

    char msg[32];
    snprintf(msg, sizeof(msg), "PING #%lu", pingId);

    dioFlag = false;
    int state = radio.startTransmit(msg);
    if (state == RADIOLIB_ERR_NONE) {
      txBusy = true;
      Serial.printf("[TX] %s\n", msg);
    } else {
      Serial.printf("[ERROR] TX start failed: %d\n", state);
      radio.startReceive();
    }
  }

  // Check TX done
  if (txBusy && dioFlag) {
    dioFlag = false;
    int state = radio.finishTransmit();
    txBusy = false;
    if (state != RADIOLIB_ERR_NONE) {
      Serial.printf("[ERROR] TX finish: %d\n", state);
    } else {
      Serial.println("[TX] Done");
    }
    radio.startReceive();
  }

  delay(10);
}