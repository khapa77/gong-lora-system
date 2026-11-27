#include "relayhandler.h"
#include "config.h"
#include "mp3handler.h"
#include "logbuffer.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// RELAY_PIN = -1 (по умолчанию) — реле на этой сборке нет. Воспроизведение
// работает напрямую, без прогрева и без таймеров; все функции становятся
// безопасными no-op, чтобы один и тот же код вызывающих не обрастал #if.
static constexpr bool kRelayPresent = RELAY_PIN >= 0;

namespace {

RelayMode mode    = RelayMode::AUTO;
uint32_t  preMs   = RELAY_DEFAULT_PRE_MS;
uint32_t  holdMs  = RELAY_DEFAULT_HOLD_MS;
bool      relayOn = false;
uint32_t  onSince = 0;          // от него считается прогрев

// Отложенный старт: реле включено, ждём preMs от onSince.
bool      pending = false;
uint8_t   pTrack  = 0;
uint8_t   pVol    = 0;
uint8_t   pLoop   = 1;

// Предвключение к гонгу расписания.
bool      prewarm      = false;
uint32_t  prewarmUntil = 0;

uint32_t  lastActiveMs = 0;     // последний момент, когда звук играл или ждал старта

// Ручное включение (кнопка) — не сохраняется, живёт RELAY_MANUAL_MAX_MS.
bool      manualOn    = false;
uint32_t  manualSince = 0;

uint32_t  lastPollMs  = 0;
bool      ready       = false;

const char* modeName(RelayMode m) {
    switch (m) {
        case RelayMode::ON:  return "on";
        case RelayMode::OFF: return "off";
        default:             return "auto";
    }
}

// Производное состояние только для индикации; логика от него не зависит.
const char* stateName() {
    if (!kRelayPresent || !ready) return "disabled";
    if (mode == RelayMode::OFF)   return "forced_off";
    if (mode == RelayMode::ON)    return "forced_on";
    if (manualOn)                 return "manual";
    if (pending)                  return "warmup";
    if (prewarm)                  return "prewarm";
    if (mp3_isPlaying())          return "playing";
    if (relayOn)                  return "hold";
    return "idle";
}

void drive(bool on) {
    if (!kRelayPresent) return;
    if (on == relayOn) return;
    relayOn = on;
    if (on) onSince = millis();
    digitalWrite(RELAY_PIN, (on != (bool)RELAY_ACTIVE_LOW) ? HIGH : LOW);
    logPrintf("[RELAY] %s\n", on ? "ON" : "OFF");
}

bool timingValid(uint32_t pre, uint32_t hold) {
    return pre <= RELAY_MAX_PRE_MS && hold <= RELAY_MAX_HOLD_MS;
}

// Настройки в NVS (раньше /relay.conf): uploadfs перезаписывает весь раздел
// LittleFS и раньше сбрасывал их вместе со всем остальным.
void saveConfig() {
    if (!kRelayPresent) return;
    Preferences p;
    if (!p.begin("relay", false)) return;
    p.putUChar("mode", (uint8_t)mode);
    p.putUInt("pre",  preMs);
    p.putUInt("hold", holdMs);
    p.end();
}

void applyTiming(uint32_t pre, uint32_t hold, const char* from) {
    if (!timingValid(pre, hold))
        logPrintf("[RELAY] %s: pre=%u hold=%u out of range — clamped to %u/%u ms\n", from,
                  (unsigned)pre, (unsigned)hold,
                  (unsigned)RELAY_MAX_PRE_MS, (unsigned)RELAY_MAX_HOLD_MS);
    // No Arduino min() macro: it is not available in the host build and a
    // macro would also break a std::min spelling on the device.
    preMs  = pre  <= (uint32_t)RELAY_MAX_PRE_MS  ? pre  : (uint32_t)RELAY_MAX_PRE_MS;
    holdMs = hold <= (uint32_t)RELAY_MAX_HOLD_MS ? hold : (uint32_t)RELAY_MAX_HOLD_MS;
}

// Одноразовый импорт /relay.conf (формат 6.1). Ошибки не фатальны.
void importLegacyConfig() {
    File f = LittleFS.open(RELAY_CONFIG_FILE, "r");
    if (!f) return;
    StaticJsonDocument<128> doc;
    const bool ok = !deserializeJson(doc, f);
    f.close();
    if (!ok) {
        logPrintf("[RELAY] %s is corrupt — using defaults\n", RELAY_CONFIG_FILE);
        LittleFS.remove(RELAY_CONFIG_FILE);
        return;
    }
    RelayMode m = RelayMode::AUTO;
    if (relay_parseMode(String((const char*)(doc["mode"] | "auto")), m)) mode = m;
    applyTiming(doc["pre"]  | RELAY_DEFAULT_PRE_MS,
                doc["hold"] | RELAY_DEFAULT_HOLD_MS, RELAY_CONFIG_FILE);
    saveConfig();
    LittleFS.remove(RELAY_CONFIG_FILE);
    logPrintf("[RELAY] Imported %s into NVS\n", RELAY_CONFIG_FILE);
}

void loadConfig() {
    Preferences p;
    bool have = false;
    if (p.begin("relay", true)) {
        have = p.isKey("mode");
        if (have) {
            const uint8_t m = p.getUChar("mode", (uint8_t)RelayMode::AUTO);
            if (m <= (uint8_t)RelayMode::OFF) mode = (RelayMode)m;
            applyTiming(p.getUInt("pre",  RELAY_DEFAULT_PRE_MS),
                        p.getUInt("hold", RELAY_DEFAULT_HOLD_MS), "NVS");
        }
        p.end();
    }
    if (!have) importLegacyConfig();
}

void startNow(uint8_t track, uint8_t vol, uint8_t loop) {
    mp3_setVolume(vol);
    mp3_play(track, loop);
    lastActiveMs = millis();
}

}  // namespace

void relay_setup() {
    pending      = false;
    prewarm      = false;
    manualOn     = false;
    relayOn      = false;
    lastActiveMs = millis();
    lastPollMs   = millis();
    preMs        = RELAY_DEFAULT_PRE_MS;
    holdMs       = RELAY_DEFAULT_HOLD_MS;
    mode         = RelayMode::AUTO;
    ready        = false;

    if (!kRelayPresent) {
        ready = true;   // все вызовы ниже работают как no-op
        logPrintf("[RELAY] Disabled (RELAY_PIN = -1): playback without relay, no warm-up\n");
        return;
    }

    // Уровень выставляется ДО pinMode(OUTPUT): иначе пин на мгновение уходит в
    // LOW и модуль с активным низким уровнем щёлкает при загрузке.
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(RELAY_PIN, OUTPUT);
    relayOn = false;

    loadConfig();

    // Q2: после перезагрузки активный выход не восстанавливается. Always ON —
    // только явная команда оператора после старта.
    if (mode == RelayMode::ON) {
        mode = RelayMode::AUTO;
        saveConfig();
        logPrintf("[RELAY] Always ON was persisted — not restored after reboot (safe OFF)\n");
    }

    ready = true;
    logPrintf("[RELAY] GPIO%d (active %s), mode=%s pre=%ums hold=%ums\n",
              RELAY_PIN, RELAY_ACTIVE_LOW ? "LOW" : "HIGH", modeName(mode),
              (unsigned)preMs, (unsigned)holdMs);
}

void relay_play(uint8_t track, uint8_t vol, uint8_t loop) {
    if (!kRelayPresent) {          // без реле — ровно тот же звук, без задержки
        startNow(track, vol, loop);
        return;
    }
    if (!ready) { startNow(track, vol, loop); return; }

    // ON/OFF — реле от звука не зависит, ждать нечего.
    if (mode != RelayMode::AUTO) { startNow(track, vol, loop); return; }

    drive(true);
    const uint32_t onFor = (uint32_t)(millis() - onSince);
    if (!pending && onFor >= preMs) { startNow(track, vol, loop); return; }

    // Повторный запрос во время ожидания заменяет параметры, но не сдвигает
    // момент старта: таймер идёт от включения реле.
    if (!pending)
        logPrintf("[RELAY] Amp warm-up %ums before track %d\n",
                  (unsigned)(preMs - onFor), track);
    pending = true;
    pTrack = track; pVol = vol; pLoop = loop;
    lastActiveMs = millis();
}

void relay_stop() {
    // Отмена pending обязательна: иначе отменённый гонг стартовал бы позже,
    // когда прогрев закончится.
    pending      = false;
    mp3_stop();
    lastActiveMs = millis();
}

void relay_prewarm(uint32_t msUntilGong) {
    if (!kRelayPresent || !ready) return;
    if (mode != RelayMode::AUTO || preMs == 0) return;
    const uint32_t now = millis();
    if (!prewarm)
        logPrintf("[RELAY] Pre-warm: scheduled gong in %ums (warm-up %ums)\n",
                  (unsigned)msUntilGong, (unsigned)preMs);
    prewarm      = true;
    prewarmUntil = now + msUntilGong + RELAY_PREWARM_GRACE_MS;
    lastActiveMs = now;
    drive(true);
}

void relay_loop() {
    if (!kRelayPresent || !ready) return;

    const uint32_t now = millis();

    if (pending && (uint32_t)(now - onSince) >= preMs) {
        pending = false;
        startNow(pTrack, pVol, pLoop);
    }
    if (prewarm && (int32_t)(now - prewarmUntil) >= 0) prewarm = false;

    switch (mode) {
        case RelayMode::ON:  drive(true);  return;
        case RelayMode::OFF: drive(false); return;
        case RelayMode::AUTO: break;
    }

    if (manualOn) {
        if ((uint32_t)(now - manualSince) < RELAY_MANUAL_MAX_MS) { drive(true); return; }
        manualOn = false;
        logPrintf("[RELAY] Manual ON expired after %u min — back to auto\n",
                  (unsigned)(RELAY_MANUAL_MAX_MS / 60000));
        // lastActiveMs не трогаем: если звука давно не было — отпустит сразу.
    }

    if (!pending && (uint32_t)(now - lastPollMs) < RELAY_POLL_PERIOD_MS) return;
    lastPollMs = now;

    if (pending || prewarm || mp3_isPlaying()) {
        lastActiveMs = now;
        drive(true);
    } else if (relayOn && (uint32_t)(now - lastActiveMs) >= holdMs) {
        drive(false);
    }
}

bool relay_setMode(RelayMode m) {
    if (!kRelayPresent) return false;
    mode     = m;
    manualOn = false;     // явный выбор режима в вебе отменяет включение кнопкой
    prewarm  = false;     // AUTO снова — следующая секунда расписания включит заново
    // Уход из AUTO во время ожидания — звук запускаем сразу, а не теряем.
    if (pending && m != RelayMode::AUTO) { pending = false; startNow(pTrack, pVol, pLoop); }
    lastActiveMs = millis();   // AUTO после ON — отпустить через holdMs, не мгновенно
    saveConfig();
    relay_loop();
    logPrintf("[RELAY] Mode = %s\n", modeName(mode));
    return true;
}

bool relay_setTiming(uint32_t pre, uint32_t hold) {
    if (!kRelayPresent) return false;
    if (!timingValid(pre, hold)) return false;
    preMs  = pre;
    holdMs = hold;
    saveConfig();
    logPrintf("[RELAY] Timing: pre=%ums hold=%ums\n", (unsigned)preMs, (unsigned)holdMs);
    return true;
}

bool relay_setManual(bool on) {
    if (!kRelayPresent) return false;
    if (mode != RelayMode::AUTO) return false;
    if (on == manualOn) return true;
    manualOn = on;
    if (on) manualSince = millis();
    logPrintf("[RELAY] Manual %s\n", on ? "ON (no sound)" : "OFF");
    relay_loop();              // выключение — сразу, если звука нет и hold истёк
    return true;
}

bool relay_parseMode(const String& s, RelayMode& out) {
    if (s == "auto") { out = RelayMode::AUTO; return true; }
    if (s == "on")   { out = RelayMode::ON;   return true; }
    if (s == "off")  { out = RelayMode::OFF;  return true; }
    return false;
}

bool     relay_isManual()  { return manualOn; }
bool     relay_isOn()      { return relayOn; }
bool     relay_isEnabled() { return kRelayPresent; }
uint32_t relay_preMs()     { return preMs; }
uint32_t relay_holdMs()    { return holdMs; }

bool relay_isBusy() {
    if (!kRelayPresent) return mp3_isPlaying();
    return pending || mp3_isPlaying();
}

String relay_toJSON() {
    // Numbers are formatted explicitly: String (Arduino or the host mock) has no
    // numeric operator+=, and appending an int used to emit one raw byte.
    char num[16];
    String s;
    s.reserve(224);
    s += "{\"enabled\":";  s += kRelayPresent ? "true" : "false";
    s += ",\"state\":\"";  s += stateName(); s += "\"";
    s += ",\"mode\":\"";   s += modeName(mode); s += "\"";
    s += ",\"on\":";       s += relayOn ? "true" : "false";
    s += ",\"pending\":";  s += pending ? "true" : "false";
    s += ",\"prewarm\":";  s += prewarm ? "true" : "false";
    s += ",\"manual\":";   s += manualOn ? "true" : "false";
    if (manualOn) {
        const uint32_t el = (uint32_t)(millis() - manualSince);
        snprintf(num, sizeof(num), "%lu",
                 (unsigned long)(el < RELAY_MANUAL_MAX_MS ? (RELAY_MANUAL_MAX_MS - el) / 1000 : 0));
        s += ",\"manual_left\":"; s += num;
    }
    snprintf(num, sizeof(num), "%lu", (unsigned long)holdMs);
    s += ",\"hold\":"; s += num;
    snprintf(num, sizeof(num), "%lu", (unsigned long)preMs);
    s += ",\"pre\":"; s += num;
    snprintf(num, sizeof(num), "%d", (int)RELAY_PIN);
    s += ",\"pin\":"; s += num;
    s += ",\"active_low\":"; s += RELAY_ACTIVE_LOW ? "true" : "false";
    s += "}";
    return s;
}
