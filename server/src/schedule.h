#pragma once

#include <Arduino.h>
#include "config.h"

struct ScheduleEntry {
    uint32_t id;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  track;       // which MP3 track to play (1..99)
    uint8_t  loop;        // number of playback repeats (1 = play once)
    uint8_t  vol;         // 0-30 (M-12) — lets e.g. a quiet 04:00 wake-up differ from a loud 08:00 gathering
    bool     enabled;
    String   description;
};

void   sched_setup();
void   sched_check();     // call every second from main loop

bool   sched_add(uint8_t h, uint8_t m, const String& desc, uint8_t track, uint8_t loop, uint8_t vol = DEFAULT_VOLUME);
bool   sched_edit(uint32_t id, uint8_t h, uint8_t m,
                  const String& desc, uint8_t track, uint8_t loop, bool enabled, uint8_t vol = DEFAULT_VOLUME);
bool   sched_del(uint32_t id);

String sched_toJSON();
void   sched_save();
void   sched_load();

// Multi-day support
String sched_dayJSON(uint8_t day);      // read /dayNN.conf → JSON (no memory change)
bool   sched_activateDay(uint8_t day);  // load /dayNN.conf as active schedule
int    sched_getActiveDay();            // -1 if not set
bool   sched_courseEnded();             // true if active day is the last day (DAY_COUNT-1)

// Template editing — modifies /dayNN.conf directly on disk WITHOUT activating
// it or touching the live in-memory schedule. If `day` happens to already be
// the active day, these transparently delegate to sched_add/edit/del so there
// is a single source of truth for the live schedule.
bool   sched_addToDay(uint8_t day, uint8_t h, uint8_t m,
                      const String& desc, uint8_t track, uint8_t loop, uint8_t vol = DEFAULT_VOLUME);
bool   sched_editInDay(uint8_t day, uint32_t id, uint8_t h, uint8_t m,
                       const String& desc, uint8_t track, uint8_t loop, bool enabled, uint8_t vol = DEFAULT_VOLUME);
bool   sched_delFromDay(uint8_t day, uint32_t id);

// H-5: binary snapshot of the ACTIVE day's enabled entries (capped at
// SCHED_BIN_MAX) for LoRa broadcast — see lora_broadcastSchedule().
uint8_t sched_activeBinSnapshot(SchedBin* out, uint8_t maxCount);
// True once (and only once) since the last call, if the active schedule
// changed (add/edit/del/activate) — lets main.cpp broadcast promptly instead
// of only on the hourly timer.
bool    sched_consumeChanged();

// Callback: fired when a scheduled gong triggers
extern void (*onScheduleTrigger)(uint8_t track, uint8_t loop, uint8_t vol);
