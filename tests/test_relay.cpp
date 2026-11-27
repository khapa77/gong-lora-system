// Host tests for the relay handler (relayhandler.cpp) with a fake clock, fake
// GPIO and a fake audio backend. No hardware, NVS or LittleFS access.
//
// The production translation unit is compiled as-is with -DRELAY_PIN=27; the
// "not fitted" build (RELAY_PIN=-1) is covered by test_relay_disabled.cpp.
#include "host/Arduino.h"
#include "host/LittleFS.h"
#include "host/Preferences.h"

#include "../server/src/config.h"
#include "../server/src/relayhandler.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

void logPrintf(const char*, ...) {}

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::cout << "  FAIL line " << __LINE__ << ": " << #cond << "\n";    \
        }                                                                        \
    } while (0)

#define CHECK_EQ(actual, expected)                                               \
    do {                                                                         \
        ++checks;                                                                \
        const auto a_ = (actual);                                                \
        const auto e_ = (expected);                                              \
        if (a_ == e_) break;                                                     \
        ++failures;                                                              \
        std::cout << "  FAIL line " << __LINE__ << ": " << #actual              \
                  << " == " << (long long)a_ << ", expected "                    \
                  << (long long)e_ << "\n";                                      \
    } while (0)

// ── Fake audio backend ─────────────────────────────────────────────────────
static int     starts = 0, stops = 0;
static uint8_t lastTrack = 0, lastVol = 0, lastLoop = 0;
static bool    playing = false;

void mp3_setVolume(uint8_t vol) { lastVol = vol; }
static bool fileMissing = false;   // simulate a missing/corrupt track
bool mp3_play(uint8_t track, uint8_t loops) {
    ++starts;
    lastTrack = track;
    lastLoop  = loops;
    if (fileMissing) return false;
    playing   = true;
    return true;
}
void mp3_stop() { ++stops; playing = false; }
bool mp3_isPlaying() { return playing; }

// ── Rig ────────────────────────────────────────────────────────────────────
static constexpr int kInactiveLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;

static void resetDevice(uint32_t startMs = 1000) {
    starts = stops = 0;
    lastTrack = lastVol = lastLoop = 0;
    playing = false;
    host::pins().reset();
    host::nvs().visible.clear();
    host::nvs().durable.clear();
    host::nvs().handles.clear();
    host::nvs().nextHandle = 1;
    host::nvs().fault = host::Fault();
    host::nvs().namespacePresent = false;
    host::files().clear();
    host::fs().removeErrorPath.clear();
    host::fs().path.clear();
    host::ticks() = startMs;
}

static void seedRelay(uint8_t mode, uint32_t pre, uint32_t hold) {
    Preferences p;
    CHECK(p.begin("relay", false));
    p.putUChar("mode", mode);
    p.putUInt("pre", pre);
    p.putUInt("hold", hold);
    p.end();
}

static void boot(uint32_t startMs = 1000) {
    host::ticks() = startMs;
    relay_setup();
}

// Advances the fake clock in 10 ms steps, ticking relay_loop like controlTask.
static void advance(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i += 10) {
        host::ticks() = (uint32_t)host::ticks() + 10;
        relay_loop();
    }
}

static int relayLevel() { return host::pins().outLevel[RELAY_PIN]; }
static bool relayActive() { return relayLevel() != kInactiveLevel; }

// ── Cases ──────────────────────────────────────────────────────────────────

static void test_setup_defaults() {
    resetDevice();
    boot();
    CHECK(relay_isEnabled());
    CHECK(!relay_isOn());
    CHECK_EQ((int)host::pins().mode[RELAY_PIN], (int)OUTPUT);
    CHECK_EQ(relayLevel(), kInactiveLevel);      // safe level from the first write
    CHECK_EQ((int)relay_preMs(), (int)RELAY_DEFAULT_PRE_MS);
    CHECK_EQ((int)relay_holdMs(), (int)RELAY_DEFAULT_HOLD_MS);
    CHECK(!relay_isManual());
    CHECK(!relay_isBusy());
}

static void test_setup_never_restores_always_on() {
    resetDevice();
    seedRelay(1 /*ON*/, 800, 3000);
    boot();
    CHECK(!relay_isOn());                        // Q2: OFF at boot
    CHECK_EQ(relayLevel(), kInactiveLevel);
    // The persisted mode was corrected to AUTO.
    Preferences p;
    CHECK(p.begin("relay", true));
    CHECK_EQ((int)p.getUChar("mode", 9), 0);
    p.end();
}

static void test_setup_clamps_stored_timing() {
    resetDevice();
    seedRelay(0, 999999, 999999);
    boot();
    CHECK_EQ((int)relay_preMs(), (int)RELAY_MAX_PRE_MS);
    CHECK_EQ((int)relay_holdMs(), (int)RELAY_MAX_HOLD_MS);
}

static void test_legacy_config_import() {
    resetDevice();
    host::files()["/relay.conf"] = "{\"mode\":\"off\",\"pre\":1200,\"hold\":4000}";
    boot();
    CHECK_EQ((int)relay_preMs(), 1200);
    CHECK_EQ((int)relay_holdMs(), 4000);
    CHECK(host::files().count("/relay.conf") == 0);
    Preferences p;
    CHECK(p.begin("relay", true));
    CHECK_EQ((int)p.getUChar("mode", 9), 2 /*OFF*/);
    p.end();
}

static void test_play_cold_relay_waits_for_warmup() {
    resetDevice();
    boot();
    relay_play(3, 20, 2);
    CHECK(relayActive());                        // relay closed first
    CHECK_EQ(starts, 0);                         // no sound yet
    advance(790);
    CHECK_EQ(starts, 0);
    advance(20);                                 // crosses 800 ms
    CHECK_EQ(starts, 1);
    CHECK_EQ((int)lastTrack, 3);
    CHECK_EQ((int)lastVol, 20);
    CHECK_EQ((int)lastLoop, 2);
}

static void test_play_on_warm_relay_starts_immediately() {
    resetDevice();
    boot();
    CHECK(relay_setManual(true));
    advance(1000);                       // the relay has been closed long enough
    relay_play(1, 30, 1);
    CHECK_EQ(starts, 1);                 // already warm → no extra delay
    CHECK(relayActive());
}

static void test_repeat_play_during_warmup_replaces_request() {
    resetDevice();
    boot();
    relay_play(1, 30, 1);
    advance(300);
    relay_play(2, 25, 3);
    advance(490);                                // total 790 from relay ON
    CHECK_EQ(starts, 0);
    advance(20);
    CHECK_EQ(starts, 1);
    CHECK_EQ((int)lastTrack, 2);                 // the later request wins
    CHECK_EQ((int)lastVol, 25);
    CHECK_EQ((int)lastLoop, 3);
}

static void test_audio_end_holds_then_releases() {
    resetDevice();
    boot();
    relay_play(1, 30, 1);
    advance(800);
    CHECK_EQ(starts, 1);
    playing = false;                             // track finished
    advance((uint32_t)RELAY_DEFAULT_HOLD_MS - 100);
    CHECK(relayActive());                        // still inside hold
    advance(200);
    CHECK(!relayActive());
}

static void test_stop_cancels_pending_gong() {
    resetDevice();
    boot();
    relay_play(1, 30, 1);
    advance(300);
    relay_stop();
    advance((uint32_t)RELAY_DEFAULT_HOLD_MS + 200);
    CHECK_EQ(starts, 0);                         // cancelled gong never starts
    CHECK_EQ(stops, 1);
    CHECK(!relayActive());                       // released after hold
}

static void test_stop_during_playback() {
    resetDevice();
    boot();
    relay_play(1, 30, 1);
    advance(800);
    CHECK_EQ(starts, 1);
    relay_stop();
    CHECK_EQ(stops, 1);
    CHECK(!playing);
    advance((uint32_t)RELAY_DEFAULT_HOLD_MS + 100);
    CHECK(!relayActive());
}

static void test_prewarm_holds_until_gong_then_releases() {
    resetDevice();
    boot();
    relay_prewarm(20000);
    CHECK(relayActive());
    CHECK_EQ(starts, 0);
    advance(20000);
    CHECK(relayActive());                        // grace window
    advance((uint32_t)RELAY_PREWARM_GRACE_MS + (uint32_t)RELAY_DEFAULT_HOLD_MS + 200);
    CHECK(!relayActive());
}

static void test_prewarm_gong_starts_without_delay() {
    resetDevice();
    boot();
    relay_prewarm(2000);
    advance(1000);
    relay_play(1, 30, 1);                        // gong arrives while pre-warmed
    CHECK_EQ(starts, 1);                         // already warm → immediate
}

static void test_manual_mode_lifecycle() {
    resetDevice();
    boot();
    CHECK(relay_setManual(true));
    CHECK(relay_isManual());
    CHECK(relay_isOn());
    CHECK(!relay_isBusy());                      // on without sound
    advance((uint32_t)RELAY_MANUAL_MAX_MS - 1000);
    CHECK(relay_isManual());
    advance(2000);                              // past 30 minutes
    CHECK(!relay_isManual());
    CHECK(!relayActive());
}

static void test_manual_rejected_outside_auto() {
    resetDevice();
    boot();
    CHECK(relay_setMode(RelayMode::OFF));
    CHECK(!relay_setManual(true));
    CHECK(!relay_isManual());
    CHECK(!relayActive());
    CHECK(relay_setMode(RelayMode::ON));
    CHECK(!relay_setManual(true));
    CHECK(relayActive());                        // Always ON drives the output
}

static void test_mode_switch_during_warmup_keeps_sound() {
    resetDevice();
    boot();
    relay_play(1, 30, 1);
    advance(200);
    CHECK_EQ(starts, 0);
    CHECK(relay_setMode(RelayMode::OFF));        // operator picks Always OFF
    CHECK_EQ(starts, 1);                         // the pending gong is not lost
    CHECK(!relayActive());                       // and the output follows OFF
}

static void test_always_off_does_not_stop_audio() {
    resetDevice();
    boot();
    relay_setMode(RelayMode::ON);
    relay_play(1, 30, 1);
    CHECK_EQ(starts, 1);
    const int stopsBefore = stops;
    relay_setMode(RelayMode::OFF);
    CHECK_EQ(stops, stopsBefore);                // Q3: only the relay is affected
    CHECK(!relayActive());
}

static void test_timing_validation() {
    resetDevice();
    boot();
    CHECK(!relay_setTiming((uint32_t)RELAY_MAX_PRE_MS + 1, 1000));
    CHECK(!relay_setTiming(1000, (uint32_t)RELAY_MAX_HOLD_MS + 1));
    CHECK(relay_setTiming(2000, 5000));
    CHECK_EQ((int)relay_preMs(), 2000);
    CHECK_EQ((int)relay_holdMs(), 5000);
    Preferences p;
    CHECK(p.begin("relay", true));
    CHECK_EQ((int)p.getUInt("pre", 0), 2000);
    CHECK_EQ((int)p.getUInt("hold", 0), 5000);
    p.end();
}

static void test_mode_parsing() {
    RelayMode m;
    CHECK(relay_parseMode(String("auto"), m));
    CHECK(m == RelayMode::AUTO);
    CHECK(relay_parseMode(String("on"), m));
    CHECK(m == RelayMode::ON);
    CHECK(relay_parseMode(String("off"), m));
    CHECK(m == RelayMode::OFF);
    CHECK(!relay_parseMode(String("sometimes"), m));
}

static void test_busy_semantics() {
    resetDevice();
    boot();
    CHECK(!relay_isBusy());
    relay_play(1, 30, 1);                        // pending warm-up
    CHECK(relay_isBusy());
    advance(800);
    advance(60);                                 // let the 50 ms audio poll see it
    CHECK(relay_isBusy());                       // playing
    playing = false;
    advance(3100 + 100);                         // hold expired
    CHECK(!relay_isBusy());
}

static void test_millis_wrap() {
    const uint32_t start = 0xFFFFFFFFu - 400u;
    resetDevice(start);
    boot(start);
    relay_play(1, 30, 1);
    CHECK(relayActive());
    advance(300);                                // crosses 2^32
    CHECK_EQ(starts, 0);
    advance(520);                                // 820 ms total
    CHECK_EQ(starts, 1);
    playing = false;
    advance((uint32_t)RELAY_DEFAULT_HOLD_MS + 100);
    CHECK(!relayActive());
}

static void test_json_fields() {
    resetDevice();
    boot();
    relay_play(1, 30, 1);
    const String j = relay_toJSON();
    CHECK(j.find("\"enabled\":true") != std::string::npos);
    CHECK(j.find("\"mode\":\"auto\"") != std::string::npos);
    CHECK(j.find("\"pending\":true") != std::string::npos);
    CHECK(j.find("\"pre\":800") != std::string::npos);
    CHECK(j.find("\"hold\":3000") != std::string::npos);
    CHECK(j.find("\"pin\":27") != std::string::npos);
    CHECK(j.find("\"state\":\"warmup\"") != std::string::npos);
}

static void test_no_audio_still_releases_relay() {
    // Missing MP3 file / failed decoder: mp3_play() starts nothing, but the
    // relay must still fall back to OFF after the hold window.
    resetDevice();
    boot();
    relay_play(9, 30, 1);
    advance(800);                                // startNow called
    playing = false;                             // decoder never ran
    advance((uint32_t)RELAY_DEFAULT_HOLD_MS + 200);
    CHECK(!relayActive());
}

// ── Review regressions ─────────────────────────────────────────────────────

static void test_save_error_is_reported() {
    resetDevice();
    boot();
    // The "pre" write fails: the setter must not claim success.
    host::nvs().inject("set", "pre", host::Action::ErrorBefore);
    CHECK(!relay_setTiming(2000, 4000));
    // RAM is NOT updated on a failed save: otherwise the device would run with a
    // new warm-up and, after reboot, silently fall back to a mixed configuration.
    CHECK_EQ((int)relay_preMs(), RELAY_DEFAULT_PRE_MS);
    CHECK_EQ((int)relay_holdMs(), RELAY_DEFAULT_HOLD_MS);

    // A failure on the second key must not leave 2000/3000 behind either.
    resetDevice();
    boot();
    host::nvs().inject("set", "hold", host::Action::ErrorBefore);
    CHECK(!relay_setTiming(2000, 4000));
    CHECK_EQ((int)relay_preMs(), RELAY_DEFAULT_PRE_MS);
    CHECK_EQ((int)relay_holdMs(), RELAY_DEFAULT_HOLD_MS);
    resetDevice();                       // reboot: nothing half-written is loaded
    boot();
    CHECK_EQ((int)relay_preMs(), RELAY_DEFAULT_PRE_MS);
    CHECK_EQ((int)relay_holdMs(), RELAY_DEFAULT_HOLD_MS);
}

static void test_failed_start_is_not_confirmed() {
    resetDevice();
    boot();
    relay_play(1, 30, 1);                // second, queued track
    advance(800);
    fileMissing = true;                  // decoder cannot start the file
    const uint32_t before = relay_lastStartedTag();
    CHECK(!relay_play(4, 30, 1));        // synchronous path reports the failure
    CHECK(relay_lastStartedTag() == before);    // nothing was confirmed
    fileMissing = false;
}

static void test_start_sets_busy_and_holds_relay() {
    resetDevice();
    boot();
    CHECK(relay_setTiming(RELAY_DEFAULT_PRE_MS, 0));   // hold = 0
    relay_play(1, 30, 1);
    advance(800);                        // pending -> playing inside this tick
    CHECK(playing);
    CHECK(relay_isBusy());               // busy is true at once, not 50 ms later
    CHECK(relayActive());                // the relay must not drop at start
    advance(10);
    CHECK(relayActive());
}

static void test_longer_warmup_reapplies_to_warm_relay() {
    resetDevice();
    boot();
    CHECK(relay_setManual(true));
    advance(900);                        // relay warm under the old 800 ms rule
    CHECK(relay_setTiming(30000, 3000)); // new warm-up is much longer
    CHECK(!relay_play(3, 30, 1));        // must queue, not start immediately
    advance(30000);
    CHECK_EQ((int)lastTrack, 3);
}

static void test_legacy_import_keeps_file_when_save_fails() {
    resetDevice();
    host::files()["/relay.conf"] = "{\"mode\":\"auto\",\"pre\":1200,\"hold\":4000}";
    host::nvs().inject("set", "pre", host::Action::ErrorBefore);
    boot();
    CHECK_EQ((int)relay_preMs(), 1200);              // RAM has the imported value
    CHECK(host::files().count("/relay.conf") == 1);  // but the file was NOT deleted

    // Next boot (fault cleared) imports it again and only then drops the file.
    resetDevice();
    host::files()["/relay.conf"] = "{\"mode\":\"auto\",\"pre\":1200,\"hold\":4000}";
    boot();
    CHECK_EQ((int)relay_preMs(), 1200);
    CHECK(host::files().count("/relay.conf") == 0);
}

static void test_manual_does_not_hide_audio_hold() {
    resetDevice();
    boot();
    CHECK(relay_setManual(true));
    advance(1000);                       // relay is warm
    relay_play(1, 30, 1);
    CHECK_EQ(starts, 1);
    playing = true;
    advance(5000);                       // audio plays while manual is on
    playing = false;
    advance(60);                         // poll sees the end of the track
    CHECK(relay_setManual(false));       // manual released right after EOF
    advance(10);
    CHECK(relayActive());                // hold window still protects the amp
    advance((uint32_t)RELAY_DEFAULT_HOLD_MS + 200);
    CHECK(!relayActive());
}

static void test_prewarm_can_be_cancelled() {
    resetDevice();
    boot();
    relay_prewarm(20000);
    CHECK(relayActive());
    relay_cancelPrewarm();               // target disappeared
    advance((uint32_t)RELAY_DEFAULT_HOLD_MS + 200);
    CHECK(!relayActive());
}

static void test_play_reports_deferral() {
    resetDevice();
    boot();
    CHECK(!relay_play(1, 30, 1));        // queued for warm-up
    advance(800);
    CHECK(relay_play(2, 30, 1));         // warm now: starts synchronously
    CHECK_EQ((int)lastTrack, 2);
}

int main() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "setup_defaults",                  test_setup_defaults },
        { "setup_never_restores_on",         test_setup_never_restores_always_on },
        { "setup_clamps_timing",             test_setup_clamps_stored_timing },
        { "legacy_config_import",            test_legacy_config_import },
        { "play_cold_relay_warmup",          test_play_cold_relay_waits_for_warmup },
        { "play_warm_relay_immediate",       test_play_on_warm_relay_starts_immediately },
        { "repeat_play_replaces_request",    test_repeat_play_during_warmup_replaces_request },
        { "audio_end_hold_then_off",         test_audio_end_holds_then_releases },
        { "stop_cancels_pending",            test_stop_cancels_pending_gong },
        { "stop_during_playback",            test_stop_during_playback },
        { "prewarm_then_release",            test_prewarm_holds_until_gong_then_releases },
        { "prewarm_gong_immediate",          test_prewarm_gong_starts_without_delay },
        { "manual_lifecycle",                test_manual_mode_lifecycle },
        { "manual_rejected_outside_auto",    test_manual_rejected_outside_auto },
        { "mode_switch_keeps_sound",         test_mode_switch_during_warmup_keeps_sound },
        { "always_off_keeps_audio",          test_always_off_does_not_stop_audio },
        { "timing_validation",               test_timing_validation },
        { "mode_parsing",                    test_mode_parsing },
        { "busy_semantics",                  test_busy_semantics },
        { "millis_wrap",                     test_millis_wrap },
        { "json_fields",                     test_json_fields },
        { "no_audio_releases_relay",         test_no_audio_still_releases_relay },
        { "save_error_reported",             test_save_error_is_reported },
        { "legacy_import_keeps_file",        test_legacy_import_keeps_file_when_save_fails },
        { "manual_keeps_audio_hold",         test_manual_does_not_hide_audio_hold },
        { "prewarm_cancel",                  test_prewarm_can_be_cancelled },
        { "play_reports_deferral",           test_play_reports_deferral },
        { "longer_warmup_reapplies",         test_longer_warmup_reapplies_to_warm_relay },
        { "failed_start_not_confirmed",      test_failed_start_is_not_confirmed },
        { "start_sets_busy_and_holds",       test_start_sets_busy_and_holds_relay },
    };
    for (const Case& c : cases) {
        const int before = failures;
        c.fn();
        std::cout << (failures == before ? "PASS " : "FAIL ") << c.name << "\n";
    }
    std::cout << "SUMMARY checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
