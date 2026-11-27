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

// Фаза индикации. Хранится как остаток по модулю LED_HEARTBEAT_MS (он кратен
// LED_ERROR_PERIOD_MS), поэтому и heartbeat, и 5 Гц считаются из одного числа,
// а накопитель не может переполниться вовсе: разница millis() складывается уже
// приведённой к периоду. Ни 32-, ни 64-битного «счётчика с начала работы» нет.
static_assert(LED_HEARTBEAT_MS % LED_ERROR_PERIOD_MS == 0,
              "LED_HEARTBEAT_MS must be a multiple of LED_ERROR_PERIOD_MS");
static uint32_t phaseMs = 0;   // всегда < LED_HEARTBEAT_MS
static uint32_t lastMs  = 0;

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
    const uint32_t now   = millis();
    const uint32_t delta = (uint32_t)(now - lastMs);
    lastMs = now;
    phaseMs = (uint32_t)((phaseMs + (delta % LED_HEARTBEAT_MS)) % LED_HEARTBEAT_MS);

    if (hint == LedHint::ButtonReady) {
        writeLed(true);
        return;
    }
    if (hint == LedHint::RelayReady) {
        // 2 Гц: период вдвое короче фазового, 250 мс вкл / 250 мс выкл.
        writeLed((phaseMs % LED_RELAY_PERIOD_MS) < (LED_RELAY_PERIOD_MS / 2));
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
