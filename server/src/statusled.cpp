#include "statusled.h"
#include "config.h"

#if STATUS_LED_PIN >= 0

// Активный уровень: STATUS_LED_ACTIVE_LOW = 1 → светодиод горит при LOW.
static inline void writeLed(bool on) {
    digitalWrite(STATUS_LED_PIN,
                 (on != (bool)STATUS_LED_ACTIVE_LOW) ? HIGH : LOW);
}

#endif

static bool (*problemSource)() = nullptr;
static LedHint hint = LedHint::None;

// Накопленное время работы модуля. Считается разницей (now - last), поэтому
// покрывает полный оборот millis() без скачков фазы.
static uint32_t phaseMs  = 0;
static uint32_t lastMs   = 0;

void led_set(bool on) {
#if STATUS_LED_PIN >= 0
    writeLed(on);
#else
    (void)on;
#endif
}

void led_hint(LedHint value) { hint = value; }

void led_setProblemSource(bool (*source)()) { problemSource = source; }

void led_setup() {
#if STATUS_LED_PIN >= 0
    pinMode(STATUS_LED_PIN, OUTPUT);
    writeLed(false);
#endif
    hint     = LedHint::None;
    phaseMs  = 0;
    lastMs   = millis();
}

void led_loop() {
#if STATUS_LED_PIN >= 0
    const uint32_t now = millis();
    phaseMs += (uint32_t)(now - lastMs);
    lastMs = now;

    if (hint == LedHint::ButtonReady) {
        writeLed(true);
        return;
    }

    const bool problem = problemSource ? problemSource() : true;
    if (problem) {
        writeLed((phaseMs % LED_ERROR_PERIOD_MS) < (LED_ERROR_PERIOD_MS / 2));
    } else {
        writeLed((phaseMs % LED_HEARTBEAT_MS) < LED_HEARTBEAT_ON_MS);
    }
#endif
}
