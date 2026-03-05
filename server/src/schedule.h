#pragma once

#include <Arduino.h>
#include "config.h"

struct ScheduleEntry {
    uint32_t id;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  track;
    bool     enabled;
    String   description;
};

// Колбэк, вызывается при срабатывании расписания (назначается в main.cpp)
extern void (*onScheduleTrigger)(uint8_t track);

void   sched_setup();
void   sched_check();
bool   sched_add(uint8_t h, uint8_t m, const String& desc, uint8_t track);
bool   sched_edit(uint32_t id, uint8_t h, uint8_t m,
                  const String& desc, uint8_t track, bool enabled);
bool   sched_del(uint32_t id);
String sched_toJSON();
void   sched_save();
void   sched_load();
