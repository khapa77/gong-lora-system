#pragma once
#include <Arduino.h>
#include <cstdint>
#include <string>

// Stage 4: built-in emergency schedule and reboot-limited recovery.
//
// The emergency schedule is compiled into the firmware and never reads the
// user course JSON or LittleFS. It is active while the persisted emergency
// state says so (survives reboots) and fires through onScheduleTrigger, the
// same callback the normal scheduler uses.
//
// Recovery from EMERGENCY is a separate operation (checkRecovery), driven by a
// periodic timer or a manual GUI retry. It never runs while an emergency event
// is being fired.

namespace emergency {

enum Reason : uint8_t {
    kNone = 0,
    kActivateFailed = 1,
    kMidnightFailed = 2,
    kBootFailed = 3,
    kCalendarCorrupt = 4,
    kGenerationInvalid = 5,
    kManualRetryFailed = 6
};

struct Entry {
    uint8_t hour;
    uint8_t minute;
    uint8_t track;
    uint8_t plays;   // total plays, 1 = once
    uint8_t volume;  // 0..30
};

const Entry* table(size_t& count);

struct State {
    uint8_t  active;
    uint8_t  reason;
    uint8_t  attempts;      // failed load attempts for the current error
    uint32_t lastFireDate;  // YYYYMMDD, 0 = none
    uint16_t lastFireMinute;  // minute of day, 0..1439
    State();
};

// Persistence in the "course" NVS namespace under a separate key.
bool loadState(State& out, std::string& error);
bool saveState(const State& state, std::string& error);
bool clearState(std::string& error);

// Runtime.
void begin();                 // load persisted state into RAM
bool isActive();
uint8_t reason();
const char* reasonText(uint8_t r);
uint8_t attempts();
const State& state();

// Records a failed day load. After the third failure it enters EMERGENCY and
// stops rebooting; before that it arms a delayed reboot.
void recordFailure(uint8_t reason);
// Clears the failure counter and leaves EMERGENCY. No-op when already clean.
void clearFailure();
bool enter(uint8_t reason);
bool leave();

// Fires an emergency event for the current minute. Never touches the course.
bool tick();
// Runs the separate recovery check; true when the normal course is running.
bool checkRecovery();

void requestReboot(uint32_t delayMs);
bool rebootPending();
void cancelReboot();
void tickReboot();

// Host tests only.
void resetRuntime();

// Hooks installed by the scheduler (and by host tests).
extern bool (*onLocalNow)(struct tm& out);
extern bool (*onRecoveryCheck)();
extern void (*onReboot)();

}  // namespace emergency
