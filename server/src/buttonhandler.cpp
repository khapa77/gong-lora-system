#include "buttonhandler.h"
#include "config.h"
#include "relayhandler.h"
#include "statusled.h"

static bool     stableLevel = HIGH;   // HIGH = отпущена (pull-up)
static bool     lastRaw     = HIGH;
static uint32_t rawSinceMs  = 0;

static uint32_t pressedAtMs = 0;
static bool     pressForStop = false;  // что делает ЭТО нажатие — решается в момент нажатия
static bool     fired        = false;  // уже сработало — ждём отпускания

void button_setup() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    stableLevel = lastRaw = digitalRead(BUTTON_PIN);
    rawSinceMs  = millis();
    // Зажатая (или замкнутая) при старте кнопка не должна сработать сама:
    // считаем, что это нажатие уже "отработано", и ждём отпускания.
    fired = (stableLevel == LOW);
    logPrintf("[BTN] GPIO%d ready (hold %us = gong, %us = stop)%s\n", BUTTON_PIN,
              (unsigned)(BUTTON_PLAY_HOLD_MS / 1000), (unsigned)(BUTTON_STOP_HOLD_MS / 1000),
              fired ? " — WARNING: reads pressed at boot (shorted / wiring?)" : "");
}

void button_loop() {
    bool raw = digitalRead(BUTTON_PIN);
    uint32_t now = millis();

    // Дребезг: уровень должен продержаться BUTTON_DEBOUNCE_MS.
    if (raw != lastRaw) { lastRaw = raw; rawSinceMs = now; }
    else if (raw != stableLevel && now - rawSinceMs >= BUTTON_DEBOUNCE_MS) {
        stableLevel = raw;
        if (stableLevel == LOW) {
            pressedAtMs  = now;
            pressForStop = relay_isBusy();
            fired        = false;
        } else if (!fired) {
            logPrintf("[BTN] Released after %ums — too short, ignored\n",
                      (unsigned)(now - pressedAtMs));
        }
    }

    if (stableLevel != LOW || fired) return;

    uint32_t need = pressForStop ? BUTTON_STOP_HOLD_MS : BUTTON_PLAY_HOLD_MS;
    if (now - pressedAtMs < need) return;
    fired = true;

    if (pressForStop) {
        logPrintf("[BTN] Held %us — stop\n", (unsigned)(need / 1000));
        relay_stop();
    } else {
        logPrintf("[BTN] Held %us — gong (track=%d vol=%d loop=%d)\n",
                  (unsigned)(need / 1000), BUTTON_TRACK, BUTTON_VOL, BUTTON_LOOP);
        relay_play(BUTTON_TRACK, BUTTON_VOL, BUTTON_LOOP);
    }
}

// Сброс пароля (config.h, AUTH_RESET_HOLD_MS): кнопка зажата с самого
// включения и держится всё это время. Светодиод горит, пока держите, и
// гаснет при отпускании. Отпустили раньше — ничего не происходит.
bool button_resetHeldAtBoot(uint32_t holdMs) {
    if (digitalRead(BUTTON_PIN) != LOW) return false;
    logPrintf("[BTN] Held at boot — keep holding %us to reset the admin password\n",
              (unsigned)(holdMs / 1000));
    uint32_t start = millis();
    uint32_t lastHigh = 0;
    led_set(true);
    while (millis() - start < holdMs) {
        if (digitalRead(BUTTON_PIN) != LOW) {
            if (!lastHigh) lastHigh = millis();
            if (millis() - lastHigh >= BUTTON_DEBOUNCE_MS) {
                led_set(false);
                logPrintf("[BTN] Released — password NOT reset\n");
                return false;
            }
        } else {
            lastHigh = 0;
        }
        delay(10);
    }
    led_set(false);
    return true;
}
