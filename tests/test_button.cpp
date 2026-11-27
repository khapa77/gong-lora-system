// Host tests for the physical button, the status LED and the credential reset.
// Production translation units are compiled as-is; Arduino GPIO/millis, NVS and
// LittleFS are replaced by the host fakes. No hardware is touched.
#include "host/Arduino.h"
#include "host/LittleFS.h"
#include "host/Preferences.h"

#include "../server/src/buttonhandler.h"
#include "../server/src/statusled.h"
#include "../server/src/authreset.h"
#include "../server/src/mp3handler.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

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
        if (!(a_ == e_)) {                                                       \
            ++failures;                                                          \
            std::cout << "  FAIL line " << __LINE__ << ": " << #actual          \
                      << " == " << (long long)a_ << ", expected "               \
                      << (long long)e_ << "\n";                                 \
        }                                                                        \
    } while (0)

// ── Rig ────────────────────────────────────────────────────────────────────
// Button thresholds are fixed by config.h; the tests assert those numbers so a
// silent change of a default is caught here.
static_assert(BUTTON_PLAY_HOLD_MS == 3000, "tests assume a 3 s manual-gong hold");
static_assert(BUTTON_STOP_HOLD_MS == 1000, "tests assume a 1 s stop hold");
static_assert(BUTTON_AUTH_RESET_HOLD_MS == 10000, "tests assume a 10 s reset hold");

struct Rig {
    int     plays = 0;
    int     stops = 0;
    uint8_t track = 0;
    uint8_t vol   = 0;
    uint8_t loop  = 0;
    bool    audio = false;
};

static Rig rig;

static bool hookPlaying() { return rig.audio; }

static void hookPlay(uint8_t track, uint8_t vol, uint8_t loop) {
    ++rig.plays;
    rig.track = track;
    rig.vol   = vol;
    rig.loop  = loop;
    rig.audio = true;
}

static void hookStop() {
    ++rig.stops;
    rig.audio = false;
}

static const ButtonHooks kHooks = { hookPlaying, hookPlay, hookStop };

// Clock/GPIO helpers: every sample is taken at an exact absolute millisecond so
// the 2999/3000/3001 boundaries are not blurred by the loop cadence.
static void tickAt(uint32_t at) {
    host::ticks() = at;
    button_loop();
}

static void pressAt(uint32_t at) {
    host::pins().inLevel[BUTTON_PIN] = LOW;
    tickAt(at);
}

static void releaseAt(uint32_t at) {
    host::pins().inLevel[BUTTON_PIN] = HIGH;
    tickAt(at);
}

static void freshButton(uint32_t startMs = 1000) {
    rig = Rig();
    host::pins().reset();
    host::ticks() = startMs;
    button_setup(kHooks);
}

// A complete hold: press, hold for `holdMs`, release, let the debounce settle.
static void doHold(uint32_t holdMs, uint32_t startMs = 1000) {
    pressAt(startMs);
    tickAt(startMs + BUTTON_DEBOUNCE_MS + 1);          // debounced press
    if (holdMs > BUTTON_DEBOUNCE_MS + 1) {
        tickAt(startMs + holdMs - 1);
        tickAt(startMs + holdMs);
    }
    releaseAt(startMs + holdMs);
    tickAt(startMs + holdMs + BUTTON_DEBOUNCE_MS + 1);
}

static uint8_t ledLevel() {
#if STATUS_LED_PIN >= 0
    return host::pins().outLevel[STATUS_LED_PIN];
#else
    return 0;
#endif
}

static bool ledOn() {
#if STATUS_LED_ACTIVE_LOW
    return ledLevel() == LOW;
#else
    return ledLevel() == HIGH;
#endif
}

// ── BUTTON ─────────────────────────────────────────────────────────────────

static void test_hold_thresholds() {
    freshButton();
    doHold(2999);
    CHECK_EQ(rig.plays, 0);

    freshButton();
    doHold(3000);
    CHECK_EQ(rig.plays, 1);

    freshButton();
    doHold(3001);
    CHECK_EQ(rig.plays, 1);
}

static void test_manual_gong_configuration() {
    freshButton();
    doHold(3500);
    CHECK_EQ(rig.plays, 1);
    CHECK_EQ(rig.track, (uint8_t)BUTTON_TRACK);
    CHECK_EQ(rig.vol, (uint8_t)BUTTON_VOL);
    CHECK_EQ(rig.loop, (uint8_t)BUTTON_LOOP);
    // The manual request must obey the same rules as /api/play.
    CHECK_EQ((int)mp3_manualVolume(200), 30);
    CHECK_EQ((int)mp3_manualVolume(-5), 0);
    CHECK_EQ((int)mp3_manualLoops(255), (int)LOOP_MAX);
    CHECK_EQ((int)mp3_manualLoops(0), 1);
    CHECK(mp3_manualTrackValid(1));
    CHECK(mp3_manualTrackValid(99));
    CHECK(!mp3_manualTrackValid(0));
    CHECK(!mp3_manualTrackValid(100));
}

static void test_stop_thresholds() {
    freshButton();
    rig.audio = true;                       // a gong is playing when pressed
    doHold(999);
    CHECK_EQ(rig.stops, 0);
    CHECK_EQ(rig.plays, 0);

    rig = Rig();
    rig.audio = true;
    doHold(1000);
    CHECK_EQ(rig.stops, 1);
    CHECK_EQ(rig.plays, 0);

    rig = Rig();
    rig.audio = true;
    doHold(1001);
    CHECK_EQ(rig.stops, 1);
    CHECK_EQ(rig.plays, 0);
}

static void test_stop_is_edge_and_does_not_restart() {
    freshButton();
    rig.audio = true;
    const uint32_t t = 5000;
    pressAt(t);
    tickAt(t + BUTTON_DEBOUNCE_MS + 1);
    tickAt(t + 1000);                       // stop fires here
    CHECK_EQ(rig.stops, 1);
    tickAt(t + 2000);                       // still held: must not stop again
    CHECK_EQ(rig.stops, 1);
    releaseAt(t + 4000);                    // long hold after the stop
    tickAt(t + 4000 + BUTTON_DEBOUNCE_MS + 1);
    CHECK_EQ(rig.plays, 0);                 // release must not launch a gong
    CHECK_EQ(rig.stops, 1);
}

static void test_audio_ends_during_hold() {
    // Stop was armed at press time, but the track ended by itself: the release
    // must not turn into a manual gong.
    freshButton();
    rig.audio = true;
    pressAt(1000);
    tickAt(1000 + BUTTON_DEBOUNCE_MS + 1);
    rig.audio = false;                      // decoder finished before the hold
    releaseAt(1600);
    tickAt(1600 + BUTTON_DEBOUNCE_MS + 1);
    CHECK_EQ(rig.plays, 0);
    CHECK_EQ(rig.stops, 0);

    // Same, but the hold also passes the stop threshold without audio playing.
    freshButton();
    rig.audio = true;
    pressAt(1000);
    tickAt(1000 + BUTTON_DEBOUNCE_MS + 1);
    rig.audio = false;
    releaseAt(2500);
    tickAt(2500 + BUTTON_DEBOUNCE_MS + 1);
    CHECK_EQ(rig.plays, 0);
    CHECK_EQ(rig.stops, 0);
}

static void test_debounce() {
    freshButton();
    // 40 ms of contact bounce never becomes a stable press.
    const uint32_t t = 1000;
    for (uint32_t i = 0; i < 40; i += 4) {
        pressAt(t + i);
        releaseAt(t + i + 2);
    }
    tickAt(t + 200);
    CHECK_EQ(rig.plays, 0);

    // A short 20 ms press is ignored even after the debounce settles.
    pressAt(t + 1000);
    releaseAt(t + 1020);
    tickAt(t + 1080);
    CHECK_EQ(rig.plays, 0);

    // A 3 s hold containing a 5 ms glitch still fires exactly once: the glitch
    // is shorter than the debounce window, so the press is not restarted.
    pressAt(t + 2000);
    tickAt(t + 2100);
    releaseAt(t + 3000);                    // glitch (5 ms)
    pressAt(t + 3005);
    tickAt(t + 4000);
    releaseAt(t + 5100);                    // held 3100 ms from the real edge
    tickAt(t + 5200);
    CHECK_EQ(rig.plays, 1);
}

static void test_repeated_presses() {
    freshButton();
    doHold(3000, 1000);
    CHECK_EQ(rig.plays, 1);
    rig.audio = false;                      // the gong is over
    doHold(3000, 10000);
    CHECK_EQ(rig.plays, 2);
    doHold(2999, 20000);                    // too short: no third gong
    CHECK_EQ(rig.plays, 2);
}

static void test_long_hold_fires_once() {
    freshButton();
    const uint32_t t = 1000;
    pressAt(t);
    tickAt(t + BUTTON_DEBOUNCE_MS + 1);
    for (uint32_t at = t + 1000; at <= t + 20000; at += 1000) tickAt(at);
    CHECK_EQ(rig.plays, 0);                 // nothing happens while held
    releaseAt(t + 20000);
    tickAt(t + 20000 + BUTTON_DEBOUNCE_MS + 1);
    CHECK_EQ(rig.plays, 1);
    tickAt(t + 25000);                      // and not again afterwards
    CHECK_EQ(rig.plays, 1);
}

static void test_stuck_contact() {
    freshButton();
    const uint32_t t = 1000;
    pressAt(t);
    tickAt(t + BUTTON_DEBOUNCE_MS + 1);
    tickAt(t + BUTTON_STUCK_MS);            // abandoned here, LED hint cleared
    CHECK_EQ(rig.plays, 0);
    releaseAt(t + BUTTON_STUCK_MS + 5000);
    tickAt(t + BUTTON_STUCK_MS + 5000 + BUTTON_DEBOUNCE_MS + 1);
    CHECK_EQ(rig.plays, 0);                 // release after a stuck press is inert

    // The button re-arms after a real release.
    doHold(3000, t + BUTTON_STUCK_MS + 10000);
    CHECK_EQ(rig.plays, 1);
}

static void test_pressed_at_setup() {
    // A button already closed when the firmware starts must not ring on release.
    rig = Rig();
    host::pins().reset();
    host::pins().inLevel[BUTTON_PIN] = LOW;
    host::ticks() = 1000;
    button_setup(kHooks);
    tickAt(1100);
    releaseAt(6000);
    tickAt(6100);
    CHECK_EQ(rig.plays, 0);

    doHold(3000, 7000);                     // subsequent real press works
    CHECK_EQ(rig.plays, 1);
}

static void test_web_starts_audio_while_held() {
    // /api/play (or a scheduled event) starts audio during the hold: the release
    // must not restart it.
    freshButton();
    pressAt(1000);
    tickAt(1000 + BUTTON_DEBOUNCE_MS + 1);
    tickAt(4000);
    rig.audio = true;                       // web started a gong meanwhile
    releaseAt(5000);
    tickAt(5000 + BUTTON_DEBOUNCE_MS + 1);
    CHECK_EQ(rig.plays, 0);
    CHECK_EQ(rig.stops, 0);
}

static void test_millis_wrap() {
    const uint32_t start = 0xFFFFFFFFu - 1500u;   // press before the wrap
    freshButton(start);
    const uint32_t t = start;
    pressAt(t);
    tickAt(t + BUTTON_DEBOUNCE_MS + 1);
    tickAt(t + 3100);                             // crossed the 2^32 boundary
    releaseAt(t + 3100);
    tickAt(t + 3100 + BUTTON_DEBOUNCE_MS + 1);
    CHECK_EQ(rig.plays, 1);

    // Stop threshold across the wrap as well.
    const uint32_t s2 = 0xFFFFFFFFu - 500u;
    freshButton(s2);
    rig.audio = true;
    pressAt(s2);
    tickAt(s2 + BUTTON_DEBOUNCE_MS + 1);
    tickAt(s2 + 1100);
    CHECK_EQ(rig.stops, 1);
    releaseAt(s2 + 2000);
    tickAt(s2 + 2000 + BUTTON_DEBOUNCE_MS + 1);
    CHECK_EQ(rig.plays, 0);
}

// ── LED ────────────────────────────────────────────────────────────────────

static bool problemFlag = false;
static bool problemSource() { return problemFlag; }

static void ledSampleAt(uint32_t at) {
    host::ticks() = at;
    led_loop();
}

static void freshLed(uint32_t startMs = 1000, bool problem = false) {
    host::pins().reset();
    host::ticks() = startMs;
    problemFlag = problem;
    led_setProblemSource(problemSource);
    led_setup();
}

static void test_led_heartbeat() {
    freshLed();
    const uint32_t t = 1000;
    ledSampleAt(t);                       CHECK(ledOn());
    ledSampleAt(t + LED_HEARTBEAT_ON_MS); CHECK(!ledOn());
    ledSampleAt(t + 1000);                CHECK(!ledOn());
    ledSampleAt(t + LED_HEARTBEAT_MS);    CHECK(ledOn());   // next flash
    ledSampleAt(t + LED_HEARTBEAT_MS + LED_HEARTBEAT_ON_MS + 1);
    CHECK(!ledOn());
}

static void test_led_error_blink() {
    freshLed(1000, true);
    const uint32_t t = 1000;
    // One monotonic pass of one second at 10 ms resolution. The expected level is
    // derived from the same elapsed phase, so a skipped sample cannot distort it.
    int transitions = 0;
    bool previous = false;
    bool first = true;
    for (uint32_t at = t; at <= t + 1000; at += 10) {
        ledSampleAt(at);
        const uint32_t elapsed = at - t;
        const bool expected = (elapsed % LED_ERROR_PERIOD_MS) < (LED_ERROR_PERIOD_MS / 2);
        CHECK_EQ((int)ledOn(), (int)expected);
        if (!first && ledOn() != previous) ++transitions;
        previous = ledOn();
        first = false;
    }
    CHECK_EQ(transitions, 10);   // 5 Hz → ten level changes per second
}

static void test_led_error_recovers() {
    freshLed(1000, true);
    ledSampleAt(1100);
    CHECK(ledOn() || !ledOn());          // any phase is fine while blinking
    problemFlag = false;                 // time set / FS mounted / course runs
    bool sawOn = false;
    for (uint32_t at = 1200; at < 1200 + LED_HEARTBEAT_MS; at += 10) {
        ledSampleAt(at);
        if (ledOn()) sawOn = true;
    }
    CHECK(sawOn);                        // heartbeat is back within one period
    problemFlag = true;                  // and the error blink returns at once
    ledSampleAt(5000);
    CHECK(ledOn());                      // phase 4000 % 200 == 0 → on
}

static void test_led_hint_priority() {
    freshLed(1000, true);                // error state
    led_hint(LedHint::ButtonReady);
    ledSampleAt(1050);
    CHECK(ledOn());                      // steady: the operator must see the threshold
    ledSampleAt(1100);
    CHECK(ledOn());                      // even where the blink would be off
    led_hint(LedHint::None);
    ledSampleAt(1150);
    CHECK(!ledOn());                     // error blink resumes immediately (150 % 200)
}

static void test_led_no_false_ready() {
    // Below the 3 s threshold the LED must not show the steady ready light.
    freshButton(1000);
    problemFlag = false;
    led_setProblemSource(problemSource);
    led_setup();
    pressAt(2000);
    tickAt(2000 + BUTTON_DEBOUNCE_MS + 1);
    tickAt(2000 + 2999);
    ledSampleAt(2000 + 2999);
    CHECK(!ledOn());                     // heartbeat phase 3999 % 3000 = 999 > 60
    tickAt(2000 + 3000);                 // threshold reached
    ledSampleAt(2000 + 3000);
    CHECK(ledOn());                      // steady ready
    releaseAt(2000 + 3500);
    tickAt(2000 + 3500 + BUTTON_DEBOUNCE_MS + 1);
    ledSampleAt(2000 + 3600);
    CHECK(!ledOn());                     // hint cleared, heartbeat resumes
}

static void test_led_wrap() {
    const uint32_t start = 0xFFFFFFFFu - 1000u;
    freshLed(start, false);
    bool sawOn = false;
    // `start + i` wraps in uint32 arithmetic exactly like millis() does.
    for (uint32_t i = 0; i <= 4000; i += 20) {
        ledSampleAt(start + i);
        if (ledOn()) sawOn = true;
    }
    CHECK(sawOn);                        // the heartbeat survived the wrap
}

static void test_led_overrides_direct() {
    freshLed(1000, false);
    led_set(true);                       // recovery-hold indication
    CHECK(ledOn());
    led_set(false);
    CHECK(!ledOn());
}

// ── Power-on recovery hold ─────────────────────────────────────────────────

static void freshBoot(bool pressed) {
    rig = Rig();
    host::pins().reset();
    host::pins().inLevel[BUTTON_PIN] = pressed ? LOW : HIGH;
    host::ticks() = 500;
    button_bootProbe();
}

// Drives the gate like main.cpp does and returns true when a reset was granted.
static bool runGate(uint32_t holdMs, uint32_t* settledAt = nullptr) {
    bool granted = false;
    for (uint32_t at = 500; at <= 500 + holdMs + BUTTON_DEBOUNCE_MS + 10; at += 10) {
        host::ticks() = at;
        const BootGate g = button_bootTick();
        if (g == BootGate::ResetGrant) granted = true;
        if (g == BootGate::Settled) {
            if (settledAt) *settledAt = at;
            break;
        }
    }
    return granted;
}

static void test_boot_no_press() {
    freshBoot(false);
    CHECK(!button_bootHeldAtPowerOn());
    host::ticks() = 500;
    CHECK(button_bootTick() == BootGate::Settled);
    CHECK_EQ(rig.plays, 0);
}

static void test_boot_thresholds() {
    // 9999 ms: released before the threshold.
    freshBoot(true);
    bool granted = false;
    for (uint32_t at = 500; at <= 500 + 9999; at += 10) {
        host::ticks() = at;
        if (button_bootTick() == BootGate::ResetGrant) granted = true;
    }
    releaseAt(500 + 9999);
    uint32_t settled = 0;
    for (uint32_t at = 500 + 9999; at < 500 + 9999 + 500; at += 10) {
        host::ticks() = at;
        if (button_bootTick() == BootGate::Settled) { settled = at; break; }
    }
    CHECK(!granted);
    CHECK(settled != 0);

    // 10000 ms exactly: grant fires at the boundary.
    freshBoot(true);
    granted = false;
    uint32_t grantAt = 0;
    for (uint32_t at = 500; at <= 500 + 10000; at += 10) {
        host::ticks() = at;
        if (button_bootTick() == BootGate::ResetGrant) { granted = true; grantAt = at; break; }
    }
    CHECK(granted);
    CHECK_EQ(grantAt, 500u + 10000u);

    // 10001 ms: the grant happened at 10000, then the gate waits for release.
    freshBoot(true);
    bool grantSeenTwice = false;
    int grants = 0;
    for (uint32_t at = 500; at <= 500 + 10001; at += 10) {
        host::ticks() = at;
        if (button_bootTick() == BootGate::ResetGrant) ++grants;
    }
    if (grants > 1) grantSeenTwice = true;
    CHECK(!grantSeenTwice);              // edge, not level: exactly one request
    CHECK_EQ(grants, 1);
    host::ticks() = 500 + 10001;
    CHECK(button_bootTick() == BootGate::WaitRelease);   // still held after grant
}

static void test_boot_early_release_and_bounce() {
    freshBoot(true);
    releaseAt(500 + 3000);
    uint32_t settled = 0;
    for (uint32_t at = 500 + 3000; at < 500 + 3000 + 200; at += 10) {
        host::ticks() = at;
        if (button_bootTick() == BootGate::Settled) { settled = at; break; }
    }
    CHECK(settled >= 500u + 3000u + BUTTON_DEBOUNCE_MS);
    CHECK_EQ(rig.plays, 0);              // no gong from the boot press

    // A 10 ms contact glitch at power-on is not a recovery hold.
    freshBoot(true);
    host::pins().inLevel[BUTTON_PIN] = HIGH;
    host::ticks() = 505;
    button_bootTick();
    bool granted = false;
    for (uint32_t at = 505; at < 500 + 10000; at += 10) {
        host::ticks() = at;
        if (button_bootTick() == BootGate::ResetGrant) granted = true;
    }
    CHECK(!granted);
}

static void test_no_gong_after_reset() {
    // Full sequence: reset granted while held, released, firmware arms the
    // button, and the same physical press must not ring.
    freshBoot(true);
    CHECK(runGate(12000));
    CHECK_EQ(rig.plays, 0);
    // Still held after the grant: main continues to tick until release.
    host::ticks() = 500 + 12000;
    CHECK(button_bootTick() == BootGate::WaitRelease);

    // Release, then arm the runtime machine while the contact is still closed
    // (worst case: release not yet debounced when setup() continues).
    host::pins().inLevel[BUTTON_PIN] = HIGH;
    host::ticks() = 500 + 12500;
    CHECK(button_bootTick() == BootGate::WaitRelease);    // released, not yet debounced
    host::ticks() = 500 + 12500 + BUTTON_DEBOUNCE_MS + 1;
    CHECK(button_bootTick() == BootGate::Settled);

    button_setup(kHooks);
    tickAt(500 + 13000);
    CHECK_EQ(rig.plays, 0);
    CHECK_EQ(rig.stops, 0);
}

// ── Credential reset ───────────────────────────────────────────────────────

static void seedNvs() {
    host::nvs().visible.clear();
    host::nvs().durable.clear();
    host::nvs().handles.clear();
    host::nvs().nextHandle = 1;
    host::nvs().fault = host::Fault();
    host::nvs().namespacePresent = true;

    Preferences auth;
    CHECK(auth.begin(AUTH_NVS_NAMESPACE, false));
    CHECK(auth.putBool("enabled", true) != 0);
    CHECK(auth.putString("salt", "00112233") != 0);
    CHECK(auth.putString("hash", "deadbeef") != 0);
    CHECK(auth.putUInt("iter", 20000) != 0);
    auth.end();

    Preferences wifi;
    CHECK(wifi.begin("wifi", false));
    CHECK(wifi.putString("ssid", "home") != 0);
    CHECK(wifi.putString("pass", "secret") != 0);
    wifi.end();

    Preferences course;
    CHECK(course.begin("course", false));
    CHECK(course.putUInt("course_state", 3) != 0);
    course.end();

    Preferences device;
    CHECK(device.begin("device", false));
    CHECK(device.putUInt("num", 7) != 0);
    device.end();
}

static bool nvsHas(const char* key) {
    return host::nvs().visible.count(key) != 0;
}

static bool nvsDurableHas(const char* key) {
    return host::nvs().durable.count(key) != 0;
}

static void test_authreset_scope() {
    seedNvs();
    host::files().clear();
    host::fs().removeErrorPath.clear();
    host::fs().path.clear();
    host::files()["/auth.conf"] = "{\"enabled\":true}";
    host::files()["/wifi.conf"] = "{\"ssid\":\"home\"}";
    host::files()["/course/g3/meta.json"] = "{}";
    host::files()["/0001.mp3"] = "ID3";

    CHECK(authreset_eraseCredentials());

    // Auth state is gone, in RAM-visible and durable NVS.
    CHECK(!nvsHas("auth/enabled"));
    CHECK(!nvsHas("auth/salt"));
    CHECK(!nvsHas("auth/hash"));
    CHECK(!nvsHas("auth/iter"));
    CHECK(!nvsDurableHas("auth/hash"));

    // Everything else survives, both in NVS and on LittleFS.
    CHECK(nvsHas("wifi/ssid"));
    CHECK(nvsHas("wifi/pass"));
    CHECK(nvsHas("course/course_state"));
    CHECK(nvsHas("device/num"));
    CHECK(host::files().count("/auth.conf") == 0);
    CHECK(host::files().count("/wifi.conf") == 1);
    CHECK(host::files().count("/course/g3/meta.json") == 1);
    CHECK(host::files().count("/0001.mp3") == 1);
}

static void test_authreset_idempotent() {
    seedNvs();
    host::files().clear();
    CHECK(authreset_eraseCredentials());
    const size_t before = host::files().size();
    CHECK(authreset_eraseCredentials());       // second call is a no-op, not an error
    CHECK_EQ(host::files().size(), before);
    CHECK(!nvsHas("auth/hash"));
}

static void test_authreset_file_absent() {
    seedNvs();
    host::files().clear();                     // no legacy /auth.conf at all
    CHECK(authreset_eraseCredentials());
    CHECK(!nvsHas("auth/enabled"));
}

static void test_authreset_unremovable_legacy_file() {
    seedNvs();
    host::files().clear();
    host::files()["/auth.conf"] = "{\"enabled\":true,\"hash\":\"deadbeef\"}";
    host::fs().removeErrorPath = "/auth.conf"; // LittleFS refuses the remove
    // Clearing NVS is not enough: loadAuth() would re-import this file and the
    // password would come back. The call must report failure.
    CHECK(!authreset_eraseCredentials());
    CHECK(!nvsHas("auth/enabled"));            // NVS part still cleared
    CHECK(host::files().count("/auth.conf") == 1);
    host::fs().removeErrorPath.clear();
}

static void test_authreset_nvs_failure() {
    seedNvs();
    host::files().clear();
    // The commit after the erase fails: the visible state must roll back and the
    // function must report failure instead of pretending the password is gone.
    host::nvs().lastKey = "auth/enabled";
    host::nvs().inject("commit", "auth/enabled", host::Action::ErrorBefore);
    CHECK(!authreset_eraseCredentials());
    CHECK(nvsHas("auth/enabled"));
    CHECK(nvsHas("auth/hash"));
}

static void test_led_phase_rollover() {
    // More than 2^32 ms of uptime must not reset the phase (the reviewer's
    // original accumulator was uint32 and wrapped after 49.7 days).
    freshLed(1000, false);
    uint64_t elapsed = 0;
    uint32_t t = 1000;
    for (int i = 0; i < 220; ++i) {
        t += 21474836u;                // ~2^32 / 200
        elapsed += 21474836u;
        ledSampleAt(t);
        CHECK(ledOn() == ((elapsed % LED_HEARTBEAT_MS) < LED_HEARTBEAT_ON_MS));
    }
    CHECK(elapsed > 0xFFFFFFFFull);
    for (int i = 0; i < 300; ++i) {
        t += 10;
        elapsed += 10;
        ledSampleAt(t);
        if (ledOn() != ((elapsed % LED_HEARTBEAT_MS) < LED_HEARTBEAT_ON_MS)) {
            CHECK(false);
            return;
        }
    }
    CHECK(true);
}

// ── Boundary cases found by the independent review ─────────────────────────

static void test_probe_configures_pin() {
    freshBoot(false);
    // The input must be configured before the first digitalRead(): an
    // unconfigured pad can read 0 regardless of the real level.
    CHECK_EQ((int)host::pins().mode[BUTTON_PIN], (int)INPUT_PULLUP);
}

static void test_boot_released_just_before_threshold() {
    // Released at 9990 ms, first gate sample at 10000 ms: the debounced level is
    // still "pressed", but the contact is open — no reset may be granted.
    freshBoot(true);
    host::ticks() = 500 + 9990;
    releaseAt(500 + 9990);
    bool granted = false;
    for (uint32_t at = 500 + 10000; at <= 500 + 10100; at += 10) {
        host::ticks() = at;
        if (button_bootTick() == BootGate::ResetGrant) granted = true;
    }
    CHECK(!granted);
}

static void test_stop_release_exactly_at_threshold() {
    freshButton();
    rig.audio = true;
    pressAt(1000);
    tickAt(1051);                     // debounced press, stop gesture armed
    tickAt(1990);                     // 990 ms held, still no stop
    CHECK_EQ(rig.stops, 0);
    releaseAt(2000);                  // release exactly at the 1000 ms boundary
    tickAt(2051);                     // debounced release
    CHECK_EQ(rig.stops, 1);
    CHECK_EQ(rig.plays, 0);
}

static void test_release_before_stop_threshold() {
    freshButton();
    rig.audio = true;
    pressAt(1000);
    tickAt(1051);
    releaseAt(1900);                  // only 900 ms
    tickAt(1951);
    CHECK_EQ(rig.stops, 0);
    CHECK_EQ(rig.plays, 0);
}

static void test_stop_not_fired_after_release() {
    // The original defect: a tick during the release debounce window saw the
    // stable level still "pressed" and fired a stop after the operator let go.
    freshButton();
    rig.audio = true;
    pressAt(1000);
    tickAt(1051);
    releaseAt(1055);                  // let go immediately
    tickAt(1105);                     // debounced release
    CHECK_EQ(rig.stops, 0);
    CHECK_EQ(rig.plays, 0);
}

static void test_stuck_release_exactly_at_limit() {
    freshButton();
    const uint32_t t = 1000;
    pressAt(t);
    tickAt(t + 51);
    releaseAt(t + BUTTON_STUCK_MS);   // exactly 60 s
    tickAt(t + BUTTON_STUCK_MS + 51);
    CHECK_EQ(rig.plays, 0);           // abandoned, not a gong
    CHECK_EQ(rig.stops, 0);
}

static void test_eof_during_debounce_stays_stop() {
    // Audio was playing at the raw press edge but finished inside the 50 ms
    // debounce window: the gesture must stay a stop, never become a gong.
    freshButton();
    rig.audio = true;
    pressAt(1000);
    rig.audio = false;                // decoder finished before the debounce
    tickAt(1051);                     // stable press; rawWasStop was captured at 1000
    tickAt(4000);
    releaseAt(4010);
    tickAt(4061);
    CHECK_EQ(rig.plays, 0);
    CHECK_EQ(rig.stops, 0);           // nothing is playing → no bogus stop either
}

// ── main ───────────────────────────────────────────────────────────────────

int main() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        { "hold_thresholds_2999_3000_3001", test_hold_thresholds },
        { "manual_gong_configuration",      test_manual_gong_configuration },
        { "stop_thresholds_999_1000_1001",  test_stop_thresholds },
        { "stop_is_edge_no_restart",        test_stop_is_edge_and_does_not_restart },
        { "audio_ends_during_hold",         test_audio_ends_during_hold },
        { "debounce",                       test_debounce },
        { "repeated_presses",               test_repeated_presses },
        { "long_hold_fires_once",           test_long_hold_fires_once },
        { "stuck_contact",                  test_stuck_contact },
        { "pressed_at_setup",               test_pressed_at_setup },
        { "web_starts_audio_while_held",    test_web_starts_audio_while_held },
        { "millis_wrap_button",             test_millis_wrap },
        { "led_heartbeat",                  test_led_heartbeat },
        { "led_error_blink",                test_led_error_blink },
        { "led_error_recovers",             test_led_error_recovers },
        { "led_hint_priority",              test_led_hint_priority },
        { "led_no_false_ready",             test_led_no_false_ready },
        { "led_wrap",                       test_led_wrap },
        { "led_phase_rollover",             test_led_phase_rollover },
        { "led_direct_override",            test_led_overrides_direct },
        { "probe_configures_pin",           test_probe_configures_pin },
        { "boot_no_press",                  test_boot_no_press },
        { "boot_thresholds",                test_boot_thresholds },
        { "boot_release_just_before",       test_boot_released_just_before_threshold },
        { "boot_early_release_and_bounce",  test_boot_early_release_and_bounce },
        { "no_gong_after_reset",            test_no_gong_after_reset },
        { "stop_release_at_threshold",      test_stop_release_exactly_at_threshold },
        { "release_before_stop_threshold",  test_release_before_stop_threshold },
        { "stop_not_fired_after_release",   test_stop_not_fired_after_release },
        { "stuck_release_at_limit",         test_stuck_release_exactly_at_limit },
        { "eof_during_debounce",            test_eof_during_debounce_stays_stop },
        { "authreset_scope",                test_authreset_scope },
        { "authreset_idempotent",           test_authreset_idempotent },
        { "authreset_file_absent",          test_authreset_file_absent },
        { "authreset_unremovable_file",     test_authreset_unremovable_legacy_file },
        { "authreset_nvs_failure",          test_authreset_nvs_failure },
    };
    for (const Case& c : cases) {
        const int before = failures;
        c.fn();
        std::cout << (failures == before ? "PASS " : "FAIL ") << c.name << "\n";
    }
    std::cout << "SUMMARY checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
