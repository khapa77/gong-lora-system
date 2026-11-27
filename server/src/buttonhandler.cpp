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

    void reset(uint8_t level, uint32_t now) {
        raw = stable = level;
        edgeAtMs = now;
    }

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
bool     pressWasStop = false;  // действие этого нажатия — стоп
bool     rawWasStop   = false;  // то же решение, снятое на СЫРОМ фронте нажатия
uint32_t pressedAtMs  = 0;      // время сырого фронта нажатия
ButtonHooks hooks{};
bool     hooksValid   = false;

}  // namespace

void button_setup(const ButtonHooks& port) {
    hooks      = port;
    hooksValid = port.audioPlaying && port.busy && port.playManual &&
                 port.stopAudio && port.relayToggle;

    // Идемпотентно: button_bootProbe() уже настроил вход, но button_setup()
    // обязан работать и без него (host-тесты, повторная инициализация).
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    const uint32_t now   = millis();
    const uint8_t  level = (uint8_t)digitalRead(BUTTON_PIN);
    debounce.reset(level, now);

    pressed      = false;
    fired        = false;
    pressWasStop = false;
    rawWasStop   = false;
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
        logPrintf("[BTN] GPIO%d ready (%lus = manual gong, %lus = relay toggle, %lus = stop)\n",
                  BUTTON_PIN,
                  (unsigned long)(BUTTON_PLAY_HOLD_MS / 1000),
                  (unsigned long)(BUTTON_RELAY_HOLD_MS / 1000),
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
    pressWasStop = rawWasStop;          // решение, снятое на сыром фронте
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

    // Отпускание ровно на границе залипания: нажатие уже нельзя считать
    // намеренным (иначе зажатый на минуту контакт дал бы гонг при отпускании).
    if (heldMs >= BUTTON_STUCK_MS) {
        logPrintf("[BTN] Released after %lums — abandoned (stuck contact?)\n",
                  (unsigned long)heldMs);
        pressWasStop = false;
        return;
    }

    if (pressWasStop) {
        pressWasStop = false;
        if (heldMs >= BUTTON_STOP_HOLD_MS && hooksValid && hooks.busy()) {
            // Отпускание пришлось ровно на порог: живой путь не увидел закрытый
            // контакт на 1000-й мс, но жест был именно «стоп». Стоп выдаём,
            // только если ещё есть что останавливать (звук или прогрев).
            logPrintf("[BTN] Released at %lums — stop\n", (unsigned long)heldMs);
            hooks.stopAudio();
            return;
        }
        // Звук играл в момент нажатия и стих сам, пока кнопку держали:
        // превращать это нажатие в запуск гонга нельзя.
        logPrintf("[BTN] Released after %lums — audio ended while held, no action\n",
                  (unsigned long)heldMs);
        return;
    }

    if (heldMs >= BUTTON_RELAY_HOLD_MS) {
        // Переключение реле без звука (только в режиме AUTO; режим ON/OFF из
        // веба главнее — модуль сам вернёт false).
        const bool ok = hooksValid && hooks.relayToggle();
        logPrintf("[BTN] Held %lums — relay toggle %s\n",
                  (unsigned long)heldMs, ok ? "applied" : "ignored (mode is not AUTO)");
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

    const uint8_t previousRaw = debounce.raw;
    const bool    changed     = debounce.feed(level, now);

    // Решение «стоп или действие» принимается на СЫРОМ фронте нажатия, до
    // дребезга: если трек закончился в первые 50 мс удержания, жест всё равно
    // остаётся стопом, а не превращается в запуск.
    if (previousRaw != LEVEL_PRESSED && debounce.raw == LEVEL_PRESSED)
        rawWasStop = hooksValid && hooks.busy();

    if (changed) {
        if (debounce.stable == LEVEL_PRESSED) onStablePress();
        else                                  onStableRelease();
        // onStableRelease() мог только что выполнить действие — дальше в этом
        // тике делать нечего.
        return;
    }

    // Живой путь: пороги считаются только пока контакт ЗАКРЫТ. В окне
    // дребезга отпускания stable ещё «нажата», а raw уже «отпущена» — иначе
    // стоп срабатывал бы после того, как оператор отпустил кнопку.
    if (!pressed || fired || debounce.raw != LEVEL_PRESSED) return;

    const uint32_t heldMs = (uint32_t)(now - pressedAtMs);

    if (pressWasStop) {
        // Стоп срабатывает сразу, не дожидаясь отпускания: оператору нужно
        // немедленно прекратить звук. Если трек уже закончился сам — стопить
        // нечего, нажатие останется без последствий.
        if (heldMs >= BUTTON_STOP_HOLD_MS && hooksValid && hooks.busy()) {
            fired = true;
            led_hint(LedHint::None);
            logPrintf("[BTN] Held %lums — stop\n", (unsigned long)heldMs);
            hooks.stopAudio();
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

    // Подсказки: 3…8 с — ровный свет («будет гонг»), ≥8 с — 2 Гц («реле»).
    if (heldMs >= BUTTON_RELAY_HOLD_MS)      led_hint(LedHint::RelayReady);
    else if (heldMs >= BUTTON_PLAY_HOLD_MS)  led_hint(LedHint::ButtonReady);
    else                                     led_hint(LedHint::None);
}

// ── Автомат восстановления пароля при включении питания ────────────────────

namespace {

Debounce bootDebounce;
bool     bootHeld      = false;  // кнопка зажата в первом сэмпле
bool     bootGranted   = false;  // ResetGrant уже выдан (фронт, не уровень)
bool     bootProbed    = false;
uint32_t bootPressedAt = 0;      // момент подачи питания (первая строка setup)

}  // namespace

void button_bootProbe() {
    // Вход обязан быть настроен ДО первого чтения: у ненастроенного вывода
    // входной буфер может быть выключен, и digitalRead() вернёт 0 — при
    // active-low это выглядело бы как вечно зажатая кнопка.
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    const uint32_t now   = millis();
    const uint8_t  level = (uint8_t)digitalRead(BUTTON_PIN);

    bootDebounce.reset(level, now);
    bootHeld      = (level == LEVEL_PRESSED);
    bootGranted   = false;
    bootProbed    = true;
    bootPressedAt = now;   // отсчёт удержания идёт от подачи питания

    if (bootHeld)
        logPrintf("[BTN] GPIO%d held at power-on — keep holding %lus to reset the admin password\n",
                  BUTTON_PIN, (unsigned long)(BUTTON_AUTH_RESET_HOLD_MS / 1000));
}

bool button_bootHeldAtPowerOn() { return bootHeld; }

BootGate button_bootTick() {
    if (!bootProbed || !bootHeld) return BootGate::Settled;

    const uint32_t now = millis();
    bootDebounce.feed((uint8_t)digitalRead(BUTTON_PIN), now);

    // Стабильное отпускание — обычная загрузка.
    if (bootDebounce.stable == LEVEL_IDLE) return BootGate::Settled;

    // Контакт уже разомкнут, но дребезг ещё не подтвердил отпускание: порог
    // считать нельзя, иначе отпускание на 9990 мс дало бы сброс на 10000-й.
    if (bootDebounce.raw != LEVEL_PRESSED)
        return bootGranted ? BootGate::WaitRelease : BootGate::Holding;

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

void     button_bootProbe() {}

bool     button_bootHeldAtPowerOn() { return false; }

BootGate button_bootTick() { return BootGate::Settled; }

#endif
