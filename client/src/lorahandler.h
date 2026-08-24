#pragma once

#include <Arduino.h>
#include "lora_shared.h"

void lora_setup();
bool lora_isReady();
void lora_poll();   // call every loop() from Core 1 — drains the GONG/STOP command queue
const char* lora_clientId();   // resolved in lora_setup(): CLIENT_ID_OVERRIDE or MAC-derived (H-6)

// H-5: call once a second from loop(). Runs the autonomous fallback schedule
// when the server has been silent longer than HEARTBEAT_LOST_MS — see
// client/src/lorahandler.cpp for the full rationale.
void lora_autonomousTick();

// H-11: status for the LED state machine in main.cpp.
bool     lora_heartbeatLost();     // true once HEARTBEAT_LOST_MS has elapsed since the last one
uint32_t lora_msSinceHeartbeat();
