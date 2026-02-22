#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

struct ScheduleEntry {
    uint32_t id;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  track;       // which MP3 track to play (1..99)
    bool     enabled;
    String   description;
};

void   sched_setup();
void   sched_check();     // call every second from main loop

bool   sched_add(uint8_t h, uint8_t m, const String& desc, uint8_t track);
bool   sched_edit(uint32_t id, uint8_t h, uint8_t m,
                  const String& desc, uint8_t track, bool enabled);
bool   sched_del(uint32_t id);

String sched_toJSON();
void   sched_save();
void   sched_load();

// Callback: fired when a scheduled gong triggers
// Argument: track number to play
extern void (*onScheduleTrigger)(uint8_t track);
