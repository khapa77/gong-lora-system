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
    led_set(problem ? (now % 200) < 100      // 5 Гц
                    : (now % 3000) < 60);    // сердцебиение
#endif
}
