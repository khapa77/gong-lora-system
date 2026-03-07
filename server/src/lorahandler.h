#pragma once

#include <Arduino.h>

void   lora_setup();
void   lora_loop();
void   lora_sendGong(uint8_t track, uint8_t vol, uint8_t loop = 1);
void   lora_sendStop();
void   lora_sendHeartbeat();
void   lora_sendSchedule(const String& scheduleJson);
String   lora_clientsJSON();
int      lora_clientCount();
uint32_t lora_getAvgOneWayMs();  // estimated one-way delay to clients (ms)
