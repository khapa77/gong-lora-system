#include "statusled.h"
#include "config.h"
#include "sysstate.h"
#include "schedule.h"

void led_set(bool on) {
#if STATUS_LED_PIN >= 0
    digitalWrite(STATUS_LED_PIN, (on != (bool)STATUS_LED_ACTIVE_LOW) ? HIGH : LOW);
#else
    (void)on;
#endif
}

static LedHint hint = LedHint::NONE;

void led_hint(LedHint h) { hint = h; }

void led_setup() {
#if STATUS_LED_PIN >= 0
    pinMode(STATUS_LED_PIN, OUTPUT);
    led_set(false);
#endif
}

void led_loop() {
#if STATUS_LED_PIN >= 0
    bool problem = !timeIsSet() || !sys_fsOk() || sched_hasError();
    uint32_t now = millis();
    if (hint == LedHint::GONG)  { led_set(true); return; }
    if (hint == LedHint::RELAY) { led_set((now % 500) < 250); return; }   // 2 Гц, не путать с 5 Гц «проблемы»
    led_set(problem ? (now % 200) < 100      // 5 Гц
                    : (now % 3000) < 60);    // сердцебиение
#endif
}
