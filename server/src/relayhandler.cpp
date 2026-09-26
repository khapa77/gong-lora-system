#include "relayhandler.h"
#include "config.h"
#include "mp3handler.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

static RelayMode mode    = RelayMode::AUTO;
static uint32_t  preMs   = RELAY_DEFAULT_PRE_MS;
static uint32_t  holdMs  = RELAY_DEFAULT_HOLD_MS;
static bool      relayOn = false;

// Отложенный старт: реле уже включено, ждём preMs до mp3_play().
static bool      pending      = false;
static uint32_t  pendingSince = 0;
static uint8_t   pTrack, pVol, pLoop;

static uint32_t  lastActiveMs = 0;   // последний момент, когда звук играл или ждал старта

static const char* modeName(RelayMode m) {
    switch (m) {
        case RelayMode::ON:  return "on";
        case RelayMode::OFF: return "off";
        default:             return "auto";
    }
}

bool relay_parseMode(const String& s, RelayMode& out) {
    if (s == "auto") { out = RelayMode::AUTO; return true; }
    if (s == "on")   { out = RelayMode::ON;   return true; }
    if (s == "off")  { out = RelayMode::OFF;  return true; }
    return false;
}

static void drive(bool on) {
    if (on == relayOn) return;
    relayOn = on;
    digitalWrite(RELAY_PIN, (on != (bool)RELAY_ACTIVE_LOW) ? HIGH : LOW);
    logPrintf("[RELAY] %s\n", on ? "ON" : "OFF");
}

static void saveConfig() {
    File f = LittleFS.open(RELAY_CONFIG_FILE, "w");
    if (!f) { logPrintf("[RELAY] Failed to save %s\n", RELAY_CONFIG_FILE); return; }
    StaticJsonDocument<96> doc;
    doc["mode"] = modeName(mode);
    doc["pre"]  = preMs;
    doc["hold"] = holdMs;
    serializeJson(doc, f);
    f.close();
}

static void loadConfig() {
    File f = LittleFS.open(RELAY_CONFIG_FILE, "r");
    if (!f) return;
    StaticJsonDocument<96> doc;
    bool ok = !deserializeJson(doc, f);
    f.close();
    if (!ok) { logPrintf("[RELAY] %s is corrupt — using defaults\n", RELAY_CONFIG_FILE); return; }
    RelayMode m;
    if (relay_parseMode(String((const char*)(doc["mode"] | "auto")), m)) mode = m;
    preMs  = doc["pre"]  | RELAY_DEFAULT_PRE_MS;
    holdMs = doc["hold"] | RELAY_DEFAULT_HOLD_MS;
}

void relay_setup() {
    // Уровень выставляется ДО pinMode(OUTPUT) — иначе пин на мгновение
    // выходит в LOW, и модуль с активным низким уровнем щёлкает при загрузке.
    digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(RELAY_PIN, OUTPUT);
    relayOn = false;

    loadConfig();
    if (mode == RelayMode::ON) drive(true);
    logPrintf("[RELAY] GPIO%d (active %s), mode=%s pre=%ums hold=%ums\n",
              RELAY_PIN, RELAY_ACTIVE_LOW ? "LOW" : "HIGH", modeName(mode),
              (unsigned)preMs, (unsigned)holdMs);
}

static void startNow(uint8_t track, uint8_t vol, uint8_t loop) {
    mp3_setVolume(vol);
    mp3_play(track, loop);
    lastActiveMs = millis();
}

void relay_play(uint8_t track, uint8_t vol, uint8_t loop) {
    // Реле уже включено (идёт гонг / удержание / режим ON) или вообще не
    // участвует (OFF, preMs=0) — усилитель готов, ждать нечего.
    if (mode != RelayMode::AUTO || (relayOn && !pending) || preMs == 0) {
        if (mode == RelayMode::AUTO) drive(true);
        startNow(track, vol, loop);
        return;
    }
    // Повторный запрос во время ожидания заменяет параметры, но не сдвигает
    // момент старта — таймер отсчитывается от включения реле.
    if (!pending) {
        drive(true);
        pendingSince = millis();
        logPrintf("[RELAY] Amp warm-up %ums before track %d\n", (unsigned)preMs, track);
    }
    pending = true;
    pTrack = track; pVol = vol; pLoop = loop;
    lastActiveMs = millis();
}

void relay_stop() {
    pending = false;
    mp3_stop();
    // Реле отпустит relay_loop() через holdMs — как после обычного окончания.
    lastActiveMs = millis();
}

void relay_loop() {
    uint32_t now = millis();

    if (pending && now - pendingSince >= preMs) {
        pending = false;
        startNow(pTrack, pVol, pLoop);
    }

    switch (mode) {
        case RelayMode::ON:  drive(true);  return;
        case RelayMode::OFF: drive(false); return;
        case RelayMode::AUTO: break;
    }

    // mp3_isPlaying() takes audioMtx, which the decoder holds for whole
    // audio.loop() passes — polling it every 1ms loop() pass would keep
    // loopTask (web + scheduler) queued behind the decoder. 50ms is far
    // below any hold time anyone would configure.
    static uint32_t lastPollMs = 0;
    if (!pending && now - lastPollMs < 50) return;
    lastPollMs = now;

    if (pending || mp3_isPlaying()) {
        lastActiveMs = now;
        drive(true);
    } else if (relayOn && now - lastActiveMs >= holdMs) {
        drive(false);
    }
}

bool relay_setMode(RelayMode m) {
    mode = m;
    // Уход из AUTO во время ожидания — звук запускаем сразу, а не теряем.
    if (pending && m != RelayMode::AUTO) { pending = false; startNow(pTrack, pVol, pLoop); }
    lastActiveMs = millis();   // AUTO после ON — отпустить через holdMs, а не мгновенно
    saveConfig();
    relay_loop();
    logPrintf("[RELAY] Mode = %s\n", modeName(m));
    return true;
}

bool relay_setTiming(uint32_t pre, uint32_t hold) {
    if (pre > 10000 || hold > 600000) return false;
    preMs  = pre;
    holdMs = hold;
    saveConfig();
    logPrintf("[RELAY] Timing: pre=%ums hold=%ums\n", (unsigned)preMs, (unsigned)holdMs);
    return true;
}

bool     relay_isOn()   { return relayOn; }
bool     relay_isBusy() { return pending || mp3_isPlaying(); }
uint32_t relay_preMs()  { return preMs; }
uint32_t relay_holdMs() { return holdMs; }

String relay_toJSON() {
    StaticJsonDocument<192> doc;
    doc["mode"]    = modeName(mode);
    doc["on"]      = relayOn;
    doc["pending"] = pending;
    doc["pre"]     = preMs;
    doc["hold"]    = holdMs;
    doc["pin"]     = RELAY_PIN;
    String s;
    serializeJson(doc, s);
    return s;
}
