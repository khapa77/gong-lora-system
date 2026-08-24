#pragma once

#include <Arduino.h>
#include "lora_shared.h"

void     lora_setup();
bool     lora_isReady();     // true once the radio is initialised and loraTask can TX/RX
bool     lora_usesDefaultKey();   // H-7: true if LORA_HMAC_KEY was never changed from the repo default

// playLocal: if true, the local speaker plays once TX actually completes (or
// times out) — see lora_pollLocalPlay(). Non-blocking (H-1): the caller's
// thread is never stalled waiting for airtime.
void     lora_sendGong(uint8_t track, uint8_t vol, uint8_t loop = 1, bool playLocal = false);
void     lora_sendStop();
bool     lora_sendHeartbeat();   // M-7: false if it couldn't even be queued (caller should retry sooner)

// H-5: broadcast the active day's schedule (binary, signed) so clients can
// fall back to it if the server goes silent. `entries`/`count` come from
// sched_activeBinSnapshot().
void     lora_broadcastSchedule(uint8_t day, const SchedBin* entries, uint8_t count);

// H-1: call every loop() from Core 1. Drains at most one queued local-play
// request per call; returns true and fills track/vol/loop if one was ready.
bool     lora_pollLocalPlay(uint8_t& track, uint8_t& vol, uint8_t& loop);

String   lora_clientsJSON();
int      lora_clientCount();
