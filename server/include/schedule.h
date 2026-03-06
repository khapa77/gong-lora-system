#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

struct ScheduleEntry {
    uint32_t id;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  track;       // which MP3 track to play (1..99)
    uint8_t  loop;        // number of playback repeats (1 = play once)
    bool     enabled;
    String   description;
};

void   sched_setup();
void   sched_check();     // call every second from main loop

bool   sched_add(uint8_t h, uint8_t m, const String& desc, uint8_t track, uint8_t loop);
bool   sched_edit(uint32_t id, uint8_t h, uint8_t m,
                  const String& desc, uint8_t track, uint8_t loop, bool enabled);
bool   sched_del(uint32_t id);

String sched_toJSON();
void   sched_save();
void   sched_load();

// Callback: fired when a scheduled gong triggers
extern void (*onScheduleTrigger)(uint8_t track, uint8_t loop);

