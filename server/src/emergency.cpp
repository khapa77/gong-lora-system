#include "emergency.h"

#include "config.h"
#include "course_store.h"
#include "schedule.h"

#include <Preferences.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace emergency {

// Built-in table. Track 1 / volume 30 are the confirmed standard-gong settings
// from config.h. 06:31 is the quiet gong: volume 5/30 and exactly 10 plays.
static const Entry kTable[] = {
    {4, 0, DEFAULT_TRACK, 1, DEFAULT_VOLUME},
    {4, 20, DEFAULT_TRACK, 1, DEFAULT_VOLUME},
    {6, 30, DEFAULT_TRACK, 1, DEFAULT_VOLUME},
    {6, 31, DEFAULT_TRACK, 10, 5},
};

static const char* kNamespace = "course";
static const char* kKey = "emg";
static const uint32_t kRecoveryIntervalMs = 300000UL;  // 5 minutes
static const uint32_t kRebootDelayMs = 10000UL;        // 10 seconds

bool (*onLocalNow)(struct tm& out) = nullptr;
bool (*onRecoveryCheck)() = nullptr;
void (*onReboot)() = nullptr;

static State gState;
static bool gLoaded = false;
static uint32_t gRebootAt = 0;
static bool gRebootArmed = false;
static uint32_t gLastRecoveryCheck = 0;

const Entry* table(size_t& count) {
    count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

State::State()
    : active(0), reason(kNone), attempts(0), lastFireDate(0), lastFireMinute(0) {}

// ---------------------------------------------------------------- persistence

static bool splitFields(const std::string& blob, std::vector<std::string>& out) {
    out.clear();
    std::string current;
    for (char c : blob) {
        if (c == '|') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return true;
}

static bool parseU32(const std::string& s, uint32_t& out) {
    if (s.empty()) return false;
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<uint32_t>(c - '0');
    }
    out = v;
    return true;
}

static bool parseHex32(const std::string& s, uint32_t& out) {
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    for (char c : s) {
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

static std::string hex32(uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(v));
    return std::string(buf);
}

static std::string makeBlob(const State& s) {
    std::string payload = "v1|";
    payload += std::to_string(static_cast<unsigned>(s.active));
    payload += '|';
    payload += std::to_string(static_cast<unsigned>(s.reason));
    payload += '|';
    payload += std::to_string(static_cast<unsigned>(s.attempts));
    payload += '|';
    payload += std::to_string(s.lastFireDate);
    payload += '|';
    payload += std::to_string(static_cast<unsigned>(s.lastFireMinute));
    return payload + "|" + hex32(course_store::CourseStore::crc32(payload));
}

static bool readBlob(const std::string& blob, State& out, std::string& error) {
    std::vector<std::string> parts;
    splitFields(blob, parts);
    if (parts.size() != 7) {
        error = "emergency field count";
        return false;
    }
    if (parts[0] != "v1") {
        error = "emergency version";
        return false;
    }
    uint32_t active = 0, reason = 0, attempts = 0, date = 0, minute = 0, crc = 0;
    if (!parseU32(parts[1], active) || !parseU32(parts[2], reason) ||
        !parseU32(parts[3], attempts) || !parseU32(parts[4], date) ||
        !parseU32(parts[5], minute) || !parseHex32(parts[6], crc)) {
        error = "emergency numeric parse";
        return false;
    }
    std::string payload;
    for (size_t i = 0; i < 6; ++i) {
        if (i) payload += '|';
        payload += parts[i];
    }
    if (course_store::CourseStore::crc32(payload) != crc) {
        error = "emergency CRC mismatch";
        return false;
    }
    if (active > 1 || reason > 255 || attempts > 255 || minute > 1439) {
        error = "emergency value range";
        return false;
    }
    out.active = static_cast<uint8_t>(active);
    out.reason = static_cast<uint8_t>(reason);
    out.attempts = static_cast<uint8_t>(attempts);
    out.lastFireDate = date;
    out.lastFireMinute = static_cast<uint16_t>(minute);
    return true;
}

bool loadState(State& out, std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kKey)) {
        prefs.end();
        error = "no emergency state";
        return false;
    }
    const String raw = prefs.getString(kKey, "");
    prefs.end();
    return readBlob(std::string(raw.c_str()), out, error);
}

bool saveState(const State& state, std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    const std::string blob = makeBlob(state);
    const size_t written = prefs.putString(kKey, String(blob.c_str()));
    prefs.end();
    if (written != blob.size()) {
        error = "emergency NVS write";
        return false;
    }
    return true;
}

bool clearState(std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    prefs.remove(kKey);
    prefs.end();
    return true;
}

// -------------------------------------------------------------------- runtime

void resetRuntime() {
    gState = State();
    gLoaded = false;
    gRebootAt = 0;
    gRebootArmed = false;
    gLastRecoveryCheck = 0;
}

static void refresh() {
    if (gLoaded) return;
    gLoaded = true;
    State loaded;
    std::string error;
    if (loadState(loaded, error)) {
        gState = loaded;
    } else {
        gState = State();
    }
}

void begin() {
    gLoaded = false;
    refresh();
    gRebootArmed = false;
    gRebootAt = 0;
    gLastRecoveryCheck = 0;
    if (gState.active) {
        logPrintf("[EMG] emergency active, reason=%u attempts=%u\n",
                  static_cast<unsigned>(gState.reason),
                  static_cast<unsigned>(gState.attempts));
    }
}

bool isActive() {
    refresh();
    return gState.active != 0;
}

uint8_t reason() {
    refresh();
    return gState.reason;
}

uint8_t attempts() {
    refresh();
    return gState.attempts;
}

const State& state() {
    refresh();
    return gState;
}

const char* reasonText(uint8_t r) {
    switch (r) {
        case kNone: return "none";
        case kActivateFailed: return "activate load failed";
        case kMidnightFailed: return "midnight load failed";
        case kBootFailed: return "boot load failed";
        case kCalendarCorrupt: return "calendar state corrupt";
        case kGenerationInvalid: return "course generation invalid";
        case kManualRetryFailed: return "manual retry failed";
        default: return "unknown";
    }
}

static bool nowLocal(struct tm& out) {
    // The scheduler installs localNow() so host tests can drive the clock.
    // There is deliberately no fallback: an unset hook must not read the
    // real-time clock of the host.
    if (!onLocalNow) return false;
    return onLocalNow(out);
}

static uint32_t dateNumber(const struct tm& ti) {
    return static_cast<uint32_t>(ti.tm_year + 1900) * 10000u +
           static_cast<uint32_t>(ti.tm_mon + 1) * 100u +
           static_cast<uint32_t>(ti.tm_mday);
}

void requestReboot(uint32_t delayMs) {
    gRebootArmed = true;
    gRebootAt = millis() + delayMs;
}

bool rebootPending() {
    return gRebootArmed;
}

void cancelReboot() {
    gRebootArmed = false;
    gRebootAt = 0;
}

void tickReboot() {
    if (!gRebootArmed) return;
    if (static_cast<int32_t>(millis() - gRebootAt) < 0) return;
    gRebootArmed = false;
    logPrintf("[EMG] reboot requested after failed recovery\n");
    if (onReboot) onReboot();
}

bool enter(uint8_t r) {
    refresh();
    gState.active = 1;
    gState.reason = r;
    std::string error;
    const bool saved = saveState(gState, error);
    if (!saved) logPrintf("[EMG] cannot persist emergency state: %s\n", error.c_str());
    cancelReboot();
    logPrintf("[EMG] entering EMERGENCY, reason=%s\n", reasonText(r));
    return saved;
}

bool leave() {
    refresh();
    gState.active = 0;
    gState.reason = kNone;
    gState.attempts = 0;
    gState.lastFireDate = 0;
    gState.lastFireMinute = 0;
    std::string error;
    const bool saved = saveState(gState, error);
    if (!saved) logPrintf("[EMG] cannot clear emergency state: %s\n", error.c_str());
    cancelReboot();
    return saved;
}

void recordFailure(uint8_t r) {
    refresh();
    gState.reason = r;
    if (gState.active) {
        // Already in EMERGENCY: never arm another reboot.
        std::string error;
        if (!saveState(gState, error)) {
            logPrintf("[EMG] cannot persist emergency state: %s\n", error.c_str());
        }
        return;
    }
    if (gState.attempts < 255) gState.attempts++;
    if (gState.attempts >= 3) {
        // Third failure: stop rebooting and run the built-in schedule.
        gState.active = 1;
        cancelReboot();
        logPrintf("[EMG] third failure, entering EMERGENCY (%s)\n", reasonText(r));
    } else {
        // 1st/2nd failure: persist the counter and request a delayed reboot.
        requestReboot(kRebootDelayMs);
        logPrintf("[EMG] failure %u (%s), reboot in %u ms\n",
                  static_cast<unsigned>(gState.attempts), reasonText(r),
                  static_cast<unsigned>(kRebootDelayMs));
    }
    std::string error;
    if (!saveState(gState, error)) {
        logPrintf("[EMG] cannot persist failure state: %s\n", error.c_str());
    }
}

void clearFailure() {
    refresh();
    if (gState.attempts == 0 && !gState.active) return;
    gState.active = 0;
    gState.reason = kNone;
    gState.attempts = 0;
    gState.lastFireDate = 0;
    gState.lastFireMinute = 0;
    cancelReboot();
    std::string error;
    if (!saveState(gState, error)) {
        logPrintf("[EMG] cannot clear failure state: %s\n", error.c_str());
    }
}

bool tick() {
    refresh();
    if (!gState.active) return false;

    struct tm ti;
    if (!nowLocal(ti)) return false;

    // Separate recovery check on its own timer. It never runs inside an event.
    if (gLastRecoveryCheck == 0 ||
        millis() - gLastRecoveryCheck >= kRecoveryIntervalMs) {
        gLastRecoveryCheck = millis();
        if (onRecoveryCheck && onRecoveryCheck()) return false;
    }

    const uint32_t today = dateNumber(ti);
    const uint16_t minute = static_cast<uint16_t>(ti.tm_hour * 60 + ti.tm_min);
    size_t count = 0;
    const Entry* entries = table(count);
    for (size_t i = 0; i < count; ++i) {
        const Entry& e = entries[i];
        if (e.hour != ti.tm_hour || e.minute != ti.tm_min) continue;
        if (gState.lastFireDate == today && gState.lastFireMinute == minute) {
            return false;
        }
        if (onScheduleTrigger) {
            onScheduleTrigger(e.track, e.plays, e.volume);
        }
        gState.lastFireDate = today;
        gState.lastFireMinute = minute;
        std::string error;
        if (!saveState(gState, error)) {
            logPrintf("[EMG] cannot persist last fire: %s\n", error.c_str());
        }
        logPrintf("[EMG] fired %02u:%02u track=%u plays=%u vol=%u\n",
                  static_cast<unsigned>(e.hour), static_cast<unsigned>(e.minute),
                  static_cast<unsigned>(e.track), static_cast<unsigned>(e.plays),
                  static_cast<unsigned>(e.volume));
        return true;
    }
    return false;
}

bool checkRecovery() {
    refresh();
    if (!gState.active) return false;
    if (!onRecoveryCheck) return false;
    if (!onRecoveryCheck()) return false;
    // The callback has already restored the normal course binding and set the
    // calendar state to RUNNING. Clear the emergency state here.
    leave();
    return true;
}

}  // namespace emergency
