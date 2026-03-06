#pragma once
#include <Arduino.h>

void   lora_setup();
void   lora_loop();

// Broadcast GONG command to all listening clients
void   lora_sendGong(uint8_t track, uint8_t vol, uint8_t loop = 1);

// Broadcast STOP command to all listening clients
void   lora_sendStop();

// Broadcast heartbeat with current server time
void   lora_sendHeartbeat();

// Broadcast full schedule JSON to all clients
void   lora_sendSchedule(const String& scheduleJson);

// Get list of known clients as JSON array
String lora_clientsJSON();
int    lora_clientCount();

