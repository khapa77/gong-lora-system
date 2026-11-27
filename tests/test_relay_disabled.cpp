// Host tests for the "no relay fitted" build (RELAY_PIN = -1).
//
// The same production relayhandler.cpp is compiled with -DRELAY_PIN=-1; the
// point is that playback must stay on the direct path: no warm-up delay, no
// timers, no GPIO writes, and all relay API calls are harmless no-ops.
#include "host/Arduino.h"

#include "../server/src/config.h"
#include "../server/src/relayhandler.h"

#include <iostream>

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

static int  starts = 0, stops = 0;
static bool playing = false;

void mp3_setVolume(uint8_t) {}
bool mp3_play(uint8_t, uint8_t) { ++starts; playing = true; return true; }
void mp3_stop() { ++stops; playing = false; }
bool mp3_isPlaying() { return playing; }

int main() {
    host::pins().reset();
    host::ticks() = 1000;
    relay_setup();

    CHECK(!relay_isEnabled());
    CHECK(!relay_isOn());

    // No delay: the audio backend is called synchronously.
    relay_play(4, 18, 2);
    CHECK(starts == 1);              // started inside relay_play()
    CHECK(playing);
    CHECK(!relay_isOn());            // no relay state machines ran

    // Timers must not turn into a delayed start.
    for (uint32_t i = 0; i < 2000; i += 10) {
        host::ticks() = (uint32_t)host::ticks() + 10;
        relay_loop();
    }
    CHECK(starts == 1);

    relay_prewarm(5000);             // no-op
    CHECK(!relay_isOn());
    CHECK(!relay_setManual(true));   // refuses: no relay hardware
    CHECK(!relay_isManual());
    CHECK(!relay_setMode(RelayMode::OFF));
    CHECK(!relay_setTiming(1000, 1000));

    CHECK(relay_isBusy());           // busy == audio only when relay is absent
    relay_stop();
    CHECK(stops == 1);
    CHECK(!relay_isBusy());

    const String j = relay_toJSON();
    CHECK(j.find("\"enabled\":false") != std::string::npos);
    CHECK(j.find("\"state\":\"disabled\"") != std::string::npos);

    // No GPIO was configured for the relay.
    CHECK(host::pins().mode[27] == 0);

    std::cout << "SUMMARY checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
