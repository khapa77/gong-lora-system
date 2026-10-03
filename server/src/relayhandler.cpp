#include "relayhandler.h"
#include "config.h"
#include "mp3handler.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>

static RelayMode mode    = RelayMode::AUTO;
static uint32_t  preMs   = RELAY_DEFAULT_PRE_MS;
static uint32_t  holdMs  = RELAY_DEFAULT_HOLD_MS;
static bool      relayOn = false;
static uint32_t  onSince = 0;        // когда реле последний раз включилось — от него считается прогрев

// Отложенный старт: реле уже включено, ждём, пока с onSince пройдёт preMs.
static bool      pending      = false;
static uint8_t   pTrack, pVol, pLoop;

// Предвключение перед гонгом по расписанию (relay_prewarm).
static bool      prewarm      = false;
static uint32_t  prewarmUntil = 0;

static uint32_t  lastActiveMs = 0;   // последний момент, когда звук играл или ждал старта

// Ручное включение кнопкой (relay_setManual) — не сохраняется в NVS.
static bool      manualOn     = false;
static uint32_t  manualSince  = 0;

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
    if (on) onSince = millis();
    digitalWrite(RELAY_PIN, (on != (bool)RELAY_ACTIVE_LOW) ? HIGH : LOW);
    logPrintf("[RELAY] %s\n", on ? "ON" : "OFF");
}

static bool timingValid(uint32_t pre, uint32_t hold) {
    return pre <= RELAY_MAX_PRE_MS && hold <= RELAY_MAX_HOLD_MS;
}

// Settings live in NVS (were /relay.conf): uploadfs rewrites the whole
// LittleFS partition and used to reset them along with everything else.
static void saveConfig() {
    Preferences p;
    p.begin("relay", false);
    p.putUChar("mode", (uint8_t)mode);
    p.putUInt("pre",  preMs);
    p.putUInt("hold", holdMs);
    p.end();
}

// Same limits as relay_setTiming(): values saved by an older firmware (which
// allowed 10 s / 10 min) or a damaged store must not bring back e.g. a
// 10-minute hold. Out of range → clamped to the new maximum.
static void applyTiming(uint32_t pre, uint32_t hold, const char* from) {
    if (!timingValid(pre, hold))
        logPrintf("[RELAY] %s: pre=%u hold=%u out of range — clamped to %u/%u ms\n", from,
                  (unsigned)pre, (unsigned)hold, (unsigned)RELAY_MAX_PRE_MS, (unsigned)RELAY_MAX_HOLD_MS);
    preMs  = min(pre,  (uint32_t)RELAY_MAX_PRE_MS);
    holdMs = min(hold, (uint32_t)RELAY_MAX_HOLD_MS);
}

static void loadConfig() {
    Preferences p;
    p.begin("relay", true);
    bool have = p.isKey("mode");
    if (have) {
        uint8_t m = p.getUChar("mode", (uint8_t)RelayMode::AUTO);
        if (m <= (uint8_t)RelayMode::OFF) mode = (RelayMode)m;
        applyTiming(p.getUInt("pre", RELAY_DEFAULT_PRE_MS), p.getUInt("hold", RELAY_DEFAULT_HOLD_MS), "NVS");
    }
    p.end();
    if (have) return;

    // One-time import of the pre-6.2 /relay.conf.
    File f = LittleFS.open(RELAY_CONFIG_FILE, "r");
    if (!f) return;
    StaticJsonDocument<96> doc;
    bool ok = !deserializeJson(doc, f);
    f.close();
    if (ok) {
        RelayMode m;
        if (relay_parseMode(String((const char*)(doc["mode"] | "auto")), m)) mode = m;
        applyTiming(doc["pre"] | RELAY_DEFAULT_PRE_MS, doc["hold"] | RELAY_DEFAULT_HOLD_MS, RELAY_CONFIG_FILE);
        saveConfig();
        logPrintf("[RELAY] Imported %s into NVS\n", RELAY_CONFIG_FILE);
    } else {
        logPrintf("[RELAY] %s is corrupt — using defaults\n", RELAY_CONFIG_FILE);
    }
    LittleFS.remove(RELAY_CONFIG_FILE);
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
    // Режимы ON/OFF — реле от звука не зависит, ждать нечего.
    if (mode != RelayMode::AUTO) { startNow(track, vol, loop); return; }

    drive(true);
    // Реле включено достаточно давно (предвключение, удержание после прошлого
    // гонга, кнопка) или прогрев не нужен (preMs=0) — усилитель готов.
    uint32_t onFor = millis() - onSince;
    if (!pending && onFor >= preMs) { startNow(track, vol, loop); return; }

    // Повторный запрос во время ожидания заменяет параметры, но не сдвигает
    // момент старта — таймер отсчитывается от включения реле.
    if (!pending) {
        logPrintf("[RELAY] Amp warm-up %ums before track %d\n", (unsigned)(preMs - onFor), track);
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

void relay_prewarm(uint32_t msUntilGong) {
    if (mode != RelayMode::AUTO || preMs == 0) return;
    uint32_t now = millis();
    if (!prewarm) {
        logPrintf("[RELAY] Pre-warm: scheduled gong in %ums (warm-up %ums)\n",
                  (unsigned)msUntilGong, (unsigned)preMs);
    }
    prewarm      = true;
    prewarmUntil = now + msUntilGong + RELAY_PREWARM_GRACE_MS;
    lastActiveMs = now;
    drive(true);
}

void relay_loop() {
    uint32_t now = millis();

    if (pending && now - onSince >= preMs) {
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
        if (now - manualSince < RELAY_MANUAL_MAX_MS) { drive(true); return; }
        manualOn = false;
        logPrintf("[RELAY] Manual ON expired after %u min — back to auto\n",
                  (unsigned)(RELAY_MANUAL_MAX_MS / 60000));
        // lastActiveMs не трогаем: если звука давно не было — отпустит сразу.
    }

    // mp3_isPlaying() takes audioMtx, which the decoder holds for whole
    // audio.loop() passes — polling it every 1ms loop() pass would keep
    // loopTask (web + scheduler) queued behind the decoder. 50ms is far
    // below any hold time anyone would configure.
    static uint32_t lastPollMs = 0;
    if (!pending && now - lastPollMs < 50) return;
    lastPollMs = now;

    if (pending || prewarm || mp3_isPlaying()) {
        lastActiveMs = now;
        drive(true);
    } else if (relayOn && now - lastActiveMs >= holdMs) {
        drive(false);
    }
}

bool relay_setMode(RelayMode m) {
    mode = m;
    manualOn = false;          // выбор режима в вебе отменяет включение кнопкой
    prewarm  = false;          // AUTO снова — следующая секунда расписания включит заново
    // Уход из AUTO во время ожидания — звук запускаем сразу, а не теряем.
    if (pending && m != RelayMode::AUTO) { pending = false; startNow(pTrack, pVol, pLoop); }
    lastActiveMs = millis();   // AUTO после ON — отпустить через holdMs, а не мгновенно
    saveConfig();
    relay_loop();
    logPrintf("[RELAY] Mode = %s\n", modeName(m));
    return true;
}

bool relay_setTiming(uint32_t pre, uint32_t hold) {
    if (!timingValid(pre, hold)) return false;
    preMs  = pre;
    holdMs = hold;
    saveConfig();
    logPrintf("[RELAY] Timing: pre=%ums hold=%ums\n", (unsigned)preMs, (unsigned)holdMs);
    return true;
}

bool relay_setManual(bool on) {
    if (mode != RelayMode::AUTO) return false;
    if (on == manualOn) return true;
    manualOn = on;
    if (on) manualSince = millis();
    logPrintf("[RELAY] Manual %s\n", on ? "ON (no sound)" : "OFF");
    relay_loop();              // выключение — сразу, если звука нет и hold истёк
    return true;
}

bool     relay_isManual() { return manualOn; }
bool     relay_isOn()   { return relayOn; }
bool     relay_isBusy() { return pending || mp3_isPlaying(); }
uint32_t relay_preMs()  { return preMs; }
uint32_t relay_holdMs() { return holdMs; }

String relay_toJSON() {
    StaticJsonDocument<256> doc;
    doc["mode"]    = modeName(mode);
    doc["on"]      = relayOn;
    doc["pending"] = pending;
    doc["prewarm"] = prewarm;
    doc["manual"]  = manualOn;
    if (manualOn) {
        uint32_t el = millis() - manualSince;   // может чуть перешагнуть до следующего relay_loop()
        doc["manual_left"] = el < RELAY_MANUAL_MAX_MS ? (RELAY_MANUAL_MAX_MS - el) / 1000 : 0;
    }
    doc["pre"]     = preMs;
    doc["hold"]    = holdMs;
    doc["pin"]     = RELAY_PIN;
    String s;
    serializeJson(doc, s);
    return s;
}
