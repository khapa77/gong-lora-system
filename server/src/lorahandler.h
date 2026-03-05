#pragma once

#include <Arduino.h>

void   lora_setup();
void   lora_loop();
void   lora_sendGong(uint8_t track, uint8_t vol);
void   lora_sendHeartbeat();
void   lora_sendSchedule(const String& scheduleJson);
String lora_clientsJSON();
int    lora_clientCount();
