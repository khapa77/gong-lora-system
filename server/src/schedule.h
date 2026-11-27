#pragma once

#include <Arduino.h>
#include "config.h"
#include <ArduinoJson.h>
#include "active_day_state.h"

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

// Milliseconds to the next enabled event of the ACTIVE day, or -1 when nothing
// will fire (not ready, suspended, time not set, no upcoming event). Used only
// for relay pre-warming: the schedule itself is untouched.
long   sched_msToNext();

bool   sched_add(uint8_t h, uint8_t m, const String& desc, uint8_t track, uint8_t loop, uint8_t vol = DEFAULT_VOLUME);
bool   sched_edit(uint32_t id, uint8_t h, uint8_t m,
                  const String& desc, uint8_t track, uint8_t loop, bool enabled, uint8_t vol = DEFAULT_VOLUME);
bool   sched_del(uint32_t id);

String sched_toJSON();
void   sched_save();
// Explicit reload of the accepted day's current file, never /gong.conf.
// File BLOCKED is cleared only after successful preparation; publication
// then stays paused until Resume. UNKNOWN/INVALID and pending edits refuse it.
bool   sched_load();

// Multi-day support. The active day lives in NVS; its /dayNN.conf is the
// live schedule's only on-disk copy.
String sched_dayJSON(uint8_t day);      // read /dayNN.conf → JSON (no memory change)
bool   sched_activateDay(uint8_t day);  // load /dayNN.conf as active schedule
int    sched_getActiveDay();            // -1 if not set
uint8_t sched_dayCount();               // active course length (DAY_COUNT in legacy mode)
bool   sched_courseEnded();             // true if active day is the last day (DAY_COUNT-1)
bool   sched_hasError();                // file error or persistence failure
active_day::Status sched_persistenceStatus();

bool    sched_courseRetry();            // manual recovery of a SUSPENDED course
bool   sched_isRunning();
// Internal only: no HTTP/UI/serial caller in A02.1. Recovery preserves the
// exact expected record and leaves execution paused; Resume performs no I/O.
bool   sched_recover(uint64_t expected, bool discardPending = false);
bool   sched_resume();

// Template editing — modifies /dayNN.conf directly on disk WITHOUT activating
// it or touching the live in-memory schedule. If `day` happens to already be
// the active day, these transparently delegate to sched_add/edit/del so there
// is a single source of truth for the live schedule.
bool   sched_addToDay(uint8_t day, uint8_t h, uint8_t m,
                      const String& desc, uint8_t track, uint8_t loop, uint8_t vol = DEFAULT_VOLUME);
bool   sched_editInDay(uint8_t day, uint32_t id, uint8_t h, uint8_t m,
                       const String& desc, uint8_t track, uint8_t loop, bool enabled, uint8_t vol = DEFAULT_VOLUME);
bool   sched_delFromDay(uint8_t day, uint32_t id);

// Whole-course import/export (web "Course" card). Every day is first staged
// into /dayNN.new; sched_commitCourse() then renames all of them over the
// day files sequentially. An interrupted staging upload leaves the old course;
// an interrupted commit can leave a mixed course (A03 is not implemented).
// Staging Day 0 starts a new upload and drops leftovers of an earlier one.
// The active day and its date are kept; only the entries change. A failed
// active-file reload returns false and latches file BLOCKED, without invalidating NVS.
bool   sched_stageDay(uint8_t day, JsonArrayConst entries, String& err);
bool   sched_commitCourse(String& err);
String sched_courseJSON();   // {"days":[[...], ...]}; String() on out-of-memory

// Callback: fired when a scheduled gong triggers
// true — звук начался; false — трек поставлен в очередь на прогрев реле.
// Расписание отмечает событие прозвучавшим только после фактического старта,
// поэтому перезагрузка во время прогрева не теряет гонг.
extern bool (*onScheduleTrigger)(uint8_t track, uint8_t loop, uint8_t vol, uint32_t* outTag);

// ── Course editor facade (stage 5) ────────────────────────────────────────
// The draft course is edited through course_editor; these wrappers keep the
// scheduler's in-RAM calendar copy in sync after every change. Errors are
// reported through sched_courseError().
bool sched_courseInfoJSON(String& out);
bool sched_courseDraftDayJSON(uint8_t day, String& out);
bool sched_courseDraftSaveDay(uint8_t day, const String& json);
bool sched_courseDraftClearDay(uint8_t day);
bool sched_courseInsertDay(uint8_t day, bool copyEvents);
bool sched_courseDeleteDay(uint8_t day);
bool sched_coursePreset(uint8_t days);
bool sched_courseRename(const String& name);
bool sched_courseImport(const String& json);
// Pure validation of a course JSON (v1/v2) without storing anything.
bool sched_courseInspect(const String& json, uint8_t& days, uint16_t& events,
                         uint8_t& version, String& name);
bool sched_courseApply();
bool sched_courseDiscardDraft();
// activeOnly exports the published course instead of the draft being edited.
bool sched_courseExportBegin(bool activeOnly);
bool sched_courseExportNext(String& chunk, bool& done);
const String& sched_courseError();
bool sched_courseEnabled();   // a published course owns execution
// "course" | "legacy" | "waiting" — the mode the device is actually executing.
const char* sched_runMode();
bool sched_waitingForCourse();
