#include "buttonhandler.h"
#include "config.h"
#include "relayhandler.h"
#include "statusled.h"

static bool     stableLevel = HIGH;   // HIGH = отпущена (pull-up)
static bool     lastRaw     = HIGH;
static uint32_t rawSinceMs  = 0;

static uint32_t pressedAtMs = 0;
static bool     pressForStop = false;  // что делает ЭТО нажатие — решается в момент нажатия
static bool     fired        = false;  // уже сработало (или зажата с включения) — ждём отпускания

void button_setup() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    stableLevel = lastRaw = digitalRead(BUTTON_PIN);
    rawSinceMs  = millis();
    // Зажатая (или замкнутая) при старте кнопка не должна сработать сама:
    // считаем, что это нажатие уже "отработано", и ждём отпускания.
    fired = (stableLevel == LOW);
    logPrintf("[BTN] GPIO%d ready (release after %us = gong, %us = relay on/off; hold %us = stop)%s\n",
              BUTTON_PIN, (unsigned)(BUTTON_PLAY_HOLD_MS / 1000), (unsigned)(BUTTON_RELAY_HOLD_MS / 1000),
              (unsigned)(BUTTON_STOP_HOLD_MS / 1000),
              fired ? " — WARNING: reads pressed at boot (shorted / wiring?)" : "");
}

// Нажатие в тишине отпущено после heldMs — выполнить выбранное действие.
static void onRelease(uint32_t heldMs) {
    if (heldMs >= BUTTON_RELAY_HOLD_MS) {
        bool on = !relay_isManual();
        if (relay_setManual(on))
            logPrintf("[BTN] Held %us — relay %s\n", (unsigned)(heldMs / 1000),
                      on ? "ON without sound" : "OFF");
        else
            logPrintf("[BTN] Held %us — relay is forced ON/OFF in the web UI, button ignored\n",
                      (unsigned)(heldMs / 1000));
    } else if (heldMs >= BUTTON_PLAY_HOLD_MS) {
        logPrintf("[BTN] Held %us — gong (track=%d vol=%d loop=%d)\n",
                  (unsigned)(heldMs / 1000), BUTTON_TRACK, BUTTON_VOL, BUTTON_LOOP);
        relay_play(BUTTON_TRACK, BUTTON_VOL, BUTTON_LOOP);
    } else {
        logPrintf("[BTN] Released after %ums — too short, ignored\n", (unsigned)heldMs);
    }
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
        } else {
            led_hint(LedHint::NONE);
            // Длительность — до момента, когда контакт разомкнулся (rawSinceMs),
            // так же, как её мерила подсказка светодиода.
            if (!fired && !pressForStop) onRelease(rawSinceMs - pressedAtMs);
            else if (!fired) logPrintf("[BTN] Released after %ums — too short, ignored\n",
                                       (unsigned)(rawSinceMs - pressedAtMs));
        }
    }

    if (stableLevel != LOW || fired) return;
    uint32_t held = now - pressedAtMs;

    if (!pressForStop) {
        led_hint(held >= BUTTON_RELAY_HOLD_MS ? LedHint::RELAY
               : held >= BUTTON_PLAY_HOLD_MS  ? LedHint::GONG : LedHint::NONE);
        return;
    }

    if (held < BUTTON_STOP_HOLD_MS) return;
    fired = true;
    logPrintf("[BTN] Held %us — stop\n", (unsigned)(BUTTON_STOP_HOLD_MS / 1000));
    relay_stop();
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
