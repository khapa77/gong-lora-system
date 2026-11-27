#include "buttonhandler.h"
#include "config.h"
#include "statusled.h"
#include "logbuffer.h"

#if BUTTON_PIN >= 0

// Уровни пина. BUTTON_ACTIVE_LOW = 1 — кнопка замыкает пин на GND.
static constexpr uint8_t LEVEL_PRESSED = BUTTON_ACTIVE_LOW ? LOW : HIGH;
static constexpr uint8_t LEVEL_IDLE    = BUTTON_ACTIVE_LOW ? HIGH : LOW;

// Дребезг без прерываний: новый уровень принимается, только если продержался
// BUTTON_DEBOUNCE_MS. Время фронта запоминается по СЫРОМУ фронту, поэтому
// длительность нажатия не включает в себя задержку подавления дребезга.
namespace {

struct Debounce {
    uint8_t  raw       = LEVEL_IDLE;
    uint8_t  stable    = LEVEL_IDLE;
    uint32_t edgeAtMs  = 0;

    // true — stable только что изменился. Дельты беззнаковые: переполнение
    // millis() не влияет на результат.
    bool feed(uint8_t level, uint32_t now) {
        if (level != raw) {
            raw      = level;
            edgeAtMs = now;
            return false;
        }
        if (level != stable && (uint32_t)(now - edgeAtMs) >= BUTTON_DEBOUNCE_MS) {
            stable = level;
            return true;
        }
        return false;
    }
};

Debounce debounce;

bool     pressed      = false;  // идёт обработанное нажатие
bool     fired        = false;  // это нажатие уже выполнило действие
bool     pressWasStop = false;  // действие этого нажатия — стоп (решено при нажатии)
uint32_t pressedAtMs  = 0;      // время сырого фронта нажатия
ButtonHooks hooks{};
bool     hooksValid   = false;

}  // namespace

void button_setup(const ButtonHooks& port) {
    hooks      = port;
    hooksValid = port.audioPlaying && port.playManual && port.stopAudio;

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    const uint32_t now   = millis();
    const uint8_t  level = (uint8_t)digitalRead(BUTTON_PIN);
    debounce.raw      = level;
    debounce.stable   = level;
    debounce.edgeAtMs = now;

    pressed      = false;
    fired        = false;
    pressWasStop = false;
    pressedAtMs  = now;
    led_hint(LedHint::None);

    // Кнопка, зажатая к моменту старта (обрыв, залипание, удержание после
    // сброса пароля), не должна сработать сама: ждём отпускания. button_loop()
    // ничего не сделает, пока не увидит сначала стабильное отпускание.
    if (level == LEVEL_PRESSED) {
        pressed = true;
        fired   = true;
        logPrintf("[BTN] GPIO%d reads PRESSED at start — waiting for release, no action\n",
                  BUTTON_PIN);
    } else {
        logPrintf("[BTN] GPIO%d ready (hold %lus = manual gong, %lus = stop)\n",
                  BUTTON_PIN,
                  (unsigned long)(BUTTON_PLAY_HOLD_MS / 1000),
                  (unsigned long)(BUTTON_STOP_HOLD_MS / 1000));
    }
}

namespace {

void doManualGong(uint32_t heldMs) {
    if (!hooksValid) {
        logPrintf("[BTN] Ignored: no playback hooks installed\n");
        return;
    }
    // Гонг не должен накладываться на уже играющий звук: веб-кнопка или
    // событие расписания могли начать воспроизведение, пока кнопку держали.
    if (hooks.audioPlaying()) {
        logPrintf("[BTN] Held %lums — audio already playing, manual gong skipped\n",
                  (unsigned long)heldMs);
        return;
    }
    logPrintf("[BTN] Held %lums — manual gong (track=%d vol=%d loop=%d)\n",
              (unsigned long)heldMs, BUTTON_TRACK, BUTTON_VOL, BUTTON_LOOP);
    hooks.playManual((uint8_t)BUTTON_TRACK, (uint8_t)BUTTON_VOL, (uint8_t)BUTTON_LOOP);
}

void onStablePress() {
    pressed      = true;
    fired        = false;
    pressedAtMs  = debounce.edgeAtMs;   // сырой фронт нажатия, без задержки дребезга
    pressWasStop = hooksValid && hooks.audioPlaying();
    led_hint(LedHint::None);
}

void onStableRelease() {
    if (!pressed) return;   // отпускание контакта, который мы не считали нажатием

    pressed = false;
    led_hint(LedHint::None);

    const uint32_t heldMs = (uint32_t)(debounce.edgeAtMs - pressedAtMs);

    if (fired) {            // стоп уже выполнен, либо нажатие сброшено как залипшее
        fired = false;
        return;
    }
    if (pressWasStop) {
        // Звук играл в момент нажатия и стих сам, пока кнопку держали:
        // превращать это нажатие в запуск гонга нельзя.
        logPrintf("[BTN] Released after %lums — audio ended while held, no action\n",
                  (unsigned long)heldMs);
        pressWasStop = false;
        return;
    }
    if (heldMs < BUTTON_PLAY_HOLD_MS) {
        logPrintf("[BTN] Released after %lums — below %lums threshold, ignored\n",
                  (unsigned long)heldMs, (unsigned long)BUTTON_PLAY_HOLD_MS);
        return;
    }
    doManualGong(heldMs);
}

}  // namespace

void button_loop() {
    const uint32_t now   = millis();
    const uint8_t  level = (uint8_t)digitalRead(BUTTON_PIN);

    if (debounce.feed(level, now)) {
        if (debounce.stable == LEVEL_PRESSED) onStablePress();
        else                                  onStableRelease();
    }

    // The action threshold is only evaluated while the contact is CLOSED. In the
    // release debounce window (up to BUTTON_DEBOUNCE_MS) the stable level is
    // still "pressed" while the raw level is already idle — evaluating here
    // would fire a stop after the operator let go. Found by the
    // audio_ends_during_hold host test.
    if (!pressed || fired || debounce.raw != LEVEL_PRESSED) return;

    const uint32_t heldMs = (uint32_t)(now - pressedAtMs);

    if (pressWasStop) {
        // Стоп срабатывает сразу, не дожидаясь отпускания: оператору нужно
        // немедленно прекратить звук.
        if (heldMs >= BUTTON_STOP_HOLD_MS) {
            fired = true;
            led_hint(LedHint::None);
            logPrintf("[BTN] Held %lums — stop\n", (unsigned long)heldMs);
            if (hooksValid) hooks.stopAudio();
        }
        return;
    }

    if (heldMs >= BUTTON_STUCK_MS) {
        // Залипший контакт: нажатие сбрасывается, чтобы отпускание через
        // минуты не превратилось в неожиданный гонг.
        fired = true;
        led_hint(LedHint::None);
        logPrintf("[BTN] Held %lums — abandoned (stuck contact?), release to re-arm\n",
                  (unsigned long)heldMs);
        return;
    }

    // Подсказка «отпустите — будет гонг»: ровный свет после порога.
    led_hint(heldMs >= BUTTON_PLAY_HOLD_MS ? LedHint::ButtonReady : LedHint::None);
}

// ── Автомат восстановления пароля при включении питания ────────────────────

namespace {

Debounce bootDebounce;
bool     bootHeld     = false;  // кнопка зажата в первом сэмпле
bool     bootGranted  = false;  // ResetGrant уже выдан (фронт, не уровень)
uint32_t bootPressedAt = 0;

}  // namespace

void button_bootBegin() {
    const uint32_t now   = millis();
    const uint8_t  level = (uint8_t)digitalRead(BUTTON_PIN);

    bootDebounce.raw      = level;
    bootDebounce.stable   = level;
    bootDebounce.edgeAtMs = now;
    bootHeld     = (level == LEVEL_PRESSED);
    bootGranted  = false;
    bootPressedAt = now;

    if (bootHeld)
        logPrintf("[BTN] GPIO%d held at power-on — keep holding %lus to reset the admin password\n",
                  BUTTON_PIN, (unsigned long)(BUTTON_AUTH_RESET_HOLD_MS / 1000));
}

bool button_bootHeldAtPowerOn() { return bootHeld; }

BootGate button_bootTick() {
    if (!bootHeld) return BootGate::Settled;

    const uint32_t now = millis();
    bootDebounce.feed((uint8_t)digitalRead(BUTTON_PIN), now);

    // Отпустили (в том числе раньше порога) — обычная загрузка.
    if (bootDebounce.stable == LEVEL_IDLE) return BootGate::Settled;

    if (!bootGranted &&
        (uint32_t)(now - bootPressedAt) >= BUTTON_AUTH_RESET_HOLD_MS) {
        bootGranted = true;
        return BootGate::ResetGrant;
    }
    return bootGranted ? BootGate::WaitRelease : BootGate::Holding;
}

#else  // BUTTON_PIN < 0 — кнопки на этой сборке нет

void button_setup(const ButtonHooks& port) {
    (void)port;
    logPrintf("[BTN] Disabled (BUTTON_PIN = -1): no physical button on this build\n");
}

void button_loop() {}

void     button_bootBegin() {}

bool     button_bootHeldAtPowerOn() { return false; }

BootGate button_bootTick() { return BootGate::Settled; }

#endif
