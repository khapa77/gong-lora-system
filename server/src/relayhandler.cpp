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
bool      warm    = false;      // реле проработало >= preMs; сбрасывается при OFF
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

// Наблюдение за звуком: единственное место, где вызывается mp3_isPlaying()
// (audioMtx), и не чаще RELAY_POLL_PERIOD_MS. Состояние читают LED, JSON,
// кнопка и hold-таймер — без повторного входа в аудиомьютекс.
bool      lastAudioPlaying = false;
uint32_t  lastPollMs       = 0;

// Каждый запрос звука получает тег; подтверждение старта привязано к тегу,
// поэтому чужой старт (кнопка/веб) не может засчитать отменённый гонг как
// прозвучавший.
uint32_t  tagCounter     = 0;
uint32_t  pendingTag     = 0;   // тег трека, ждущего прогрева
uint32_t  lastStartedTag = 0;   // тег последнего фактически стартовавшего трека

bool      ready = false;

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
    if (lastAudioPlaying)         return "playing";
    if (relayOn)                  return "hold";
    return "idle";
}

void drive(bool on) {
    if (!kRelayPresent) return;
    if (on == relayOn) return;
    relayOn = on;
    warm    = false;            // физическое переключение: прогрев считается заново
    if (on) onSince = millis();
    digitalWrite(RELAY_PIN, (on != (bool)RELAY_ACTIVE_LOW) ? HIGH : LOW);
    logPrintf("[RELAY] %s\n", on ? "ON" : "OFF");
}

bool timingValid(uint32_t pre, uint32_t hold) {
    return pre <= RELAY_MAX_PRE_MS && hold <= RELAY_MAX_HOLD_MS;
}

// Настройки в NVS (раньше /relay.conf). true — все три поля записаны и
// подтверждены; при ошибке ни одна настройка не считается сохранённой.
// Пишет ВСЕ три ключа из аргументов (не из RAM), чтобы при отказе посередине
// не осталось смеси новых и старых значений: RAM меняется только после успеха.
bool saveConfig(RelayMode m, uint32_t pre, uint32_t hold) {
    if (!kRelayPresent) return true;
    Preferences p;
    if (!p.begin("relay", false)) return false;
    const bool ok = p.putUChar("mode", (uint8_t)m) != 0 &&
                    p.putUInt("pre",  pre) != 0 &&
                    p.putUInt("hold", hold) != 0;
    p.end();
    if (!ok) logPrintf("[RELAY] NVS save FAILED (mode/pre/hold)\n");
    return ok;
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

// Одноразовый импорт /relay.conf (формат 6.1). Файл удаляется только после
// подтверждённой записи в NVS: иначе настройки терялись бы при отказе записи.
void importLegacyConfig() {
    File f = LittleFS.open(RELAY_CONFIG_FILE, "r");
    if (!f) return;
    StaticJsonDocument<128> doc;
    const bool ok = !deserializeJson(doc, f);
    f.close();
    if (!ok) {
        logPrintf("[RELAY] %s is corrupt — using defaults (file kept)\n", RELAY_CONFIG_FILE);
        return;
    }
    RelayMode m = RelayMode::AUTO;
    if (relay_parseMode(String((const char*)(doc["mode"] | "auto")), m)) mode = m;
    applyTiming(doc["pre"]  | RELAY_DEFAULT_PRE_MS,
                doc["hold"] | RELAY_DEFAULT_HOLD_MS, RELAY_CONFIG_FILE);
    if (!saveConfig(mode, preMs, holdMs)) {
        logPrintf("[RELAY] %s import not committed — file kept for the next boot\n",
                  RELAY_CONFIG_FILE);
        return;
    }
    LittleFS.remove(RELAY_CONFIG_FILE);
    logPrintf("[RELAY] Imported %s into NVS\n", RELAY_CONFIG_FILE);
}

void loadConfig() {
    Preferences p;
    bool have = false;
    if (p.begin("relay", true)) {
        // All three keys are written together; a partial record is treated as
        // absent so a half-written store cannot mix new mode with old timings.
        have = p.isKey("mode") && p.isKey("pre") && p.isKey("hold");
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

// true — декодер реально стартовал (файл найден и connecttoFS успешен).
bool startNow(uint8_t track, uint8_t vol, uint8_t loop, uint32_t tag) {
    mp3_setVolume(vol);
    const bool started = mp3_play(track, loop);
    lastActiveMs = millis();
    // Обновляем кэш сразу: иначе busy/relay на мгновение ложно «простаивают»
    // и реле с hold=0 успевает выключиться в момент старта.
    lastAudioPlaying = started;
    lastPollMs       = millis();
    if (started) lastStartedTag = tag;
    return started;
}

}  // namespace

void relay_setup() {
    pending          = false;
    prewarm          = false;
    manualOn         = false;
    relayOn          = false;
    warm             = false;
    lastAudioPlaying = false;
    pendingTag       = 0;
    lastStartedTag   = 0;
    lastActiveMs     = millis();
    lastPollMs       = millis();
    preMs            = RELAY_DEFAULT_PRE_MS;
    holdMs           = RELAY_DEFAULT_HOLD_MS;
    mode             = RelayMode::AUTO;
    ready            = false;

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
        if (!saveConfig(mode, preMs, holdMs))
            logPrintf("[RELAY] Could not persist AUTO after reboot — mode is RAM-only\n");
        logPrintf("[RELAY] Always ON was persisted — not restored after reboot (safe OFF)\n");
    }

    ready = true;
    logPrintf("[RELAY] GPIO%d (active %s), mode=%s pre=%ums hold=%ums\n",
              RELAY_PIN, RELAY_ACTIVE_LOW ? "LOW" : "HIGH", modeName(mode),
              (unsigned)preMs, (unsigned)holdMs);
}

bool relay_play(uint8_t track, uint8_t vol, uint8_t loop, uint32_t* outTag) {
    const uint32_t tag = ++tagCounter;
    if (outTag) *outTag = tag;

    if (!kRelayPresent || !ready) {   // без реле — тот же звук, без задержки
        return startNow(track, vol, loop, tag);
    }

    // ON/OFF — реле от звука не зависит, ждать нечего.
    if (mode != RelayMode::AUTO) return startNow(track, vol, loop, tag);

    drive(true);
    const uint32_t onFor = (uint32_t)(millis() - onSince);
    // `warm` снимает зависимость от переполнения onSince: реле, проработавшее
    // дольше preMs, остаётся готовым до физического выключения.
    if (!pending && (warm || onFor >= preMs)) return startNow(track, vol, loop, tag);

    // Повторный запрос во время ожидания заменяет параметры и тег, но не
    // сдвигает момент старта: таймер идёт от включения реле.
    if (!pending)
        logPrintf("[RELAY] Amp warm-up %ums before track %d\n",
                  (unsigned)(preMs - onFor), track);
    pending = true;
    pendingTag = tag;
    pTrack = track; pVol = vol; pLoop = loop;
    lastActiveMs = millis();
    return false;   // поставлено в очередь, звук ещё не начался
}

void relay_stop() {
    // Отмена pending обязательна: иначе отменённый гонг стартовал бы позже,
    // когда прогрев закончится.
    pending      = false;
    mp3_stop();
    // mp3_stop() is synchronous: refresh the cached observation instead of
    // reporting "busy" for up to one poll period after a stop.
    lastAudioPlaying = false;
    lastPollMs       = millis();
    lastActiveMs     = millis();
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

void relay_cancelPrewarm() {
    // Снимается только предвключение: manual/play/hold живут своей жизнью.
    prewarm = false;
}

void relay_loop() {
    const uint32_t now = millis();

    // Единственное наблюдение за звуком (audioMtx), не чаще RELAY_POLL_PERIOD_MS.
    // Работает и без реле: состояние нужно кнопке и LED.
    if ((uint32_t)(now - lastPollMs) >= RELAY_POLL_PERIOD_MS) {
        lastAudioPlaying = mp3_isPlaying();
        lastPollMs = now;
    }
    if (lastAudioPlaying) lastActiveMs = now;

    if (!kRelayPresent || !ready) return;

    if (pending && (uint32_t)(now - onSince) >= preMs) {
        pending = false;
        warm    = true;
        startNow(pTrack, pVol, pLoop, pendingTag);
    }
    if (prewarm && (int32_t)(now - prewarmUntil) >= 0) prewarm = false;
    if (relayOn && !warm && (uint32_t)(now - onSince) >= preMs) warm = true;

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
        // lastActiveMs уже обновлён выше, если звук играл.
    }

    if (pending || prewarm || lastAudioPlaying) {
        lastActiveMs = now;
        drive(true);
    } else if (relayOn && (uint32_t)(now - lastActiveMs) >= holdMs) {
        drive(false);
    }
}

bool relay_setMode(RelayMode m) {
    if (!kRelayPresent) return false;
    if (!saveConfig(m, preMs, holdMs)) {
        logPrintf("[RELAY] Mode NOT changed: NVS save failed\n");
        return false;
    }
    mode     = m;
    manualOn = false;     // явный выбор режима в вебе отменяет включение кнопкой
    prewarm  = false;     // AUTO снова — следующая секунда расписания включит заново
    // Уход из AUTO во время ожидания — звук запускаем сразу, а не теряем.
    if (pending && m != RelayMode::AUTO) { pending = false; startNow(pTrack, pVol, pLoop, pendingTag); }
    lastActiveMs = millis();   // AUTO после ON — отпустить через holdMs, не мгновенно
    relay_loop();
    logPrintf("[RELAY] Mode = %s\n", modeName(mode));
    return true;
}

bool relay_setTiming(uint32_t pre, uint32_t hold) {
    if (!kRelayPresent) return false;
    if (!timingValid(pre, hold)) return false;
    // RAM не меняется при отказе записи: иначе после перезагрузки получилась бы
    // смесь нового warm-up со старым hold.
    if (!saveConfig(mode, pre, hold)) {
        logPrintf("[RELAY] Timing NOT changed: NVS save failed\n");
        return false;
    }
    const bool longerWarmUp = pre > preMs;
    preMs  = pre;
    holdMs = hold;
    // Увеличенный warm-up применяется и к уже включённому реле: иначе трек
    // стартовал бы по прежнему, более короткому порогу.
    if (longerWarmUp && relayOn && (uint32_t)(millis() - onSince) < preMs) warm = false;
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

bool     relay_isManual()      { return manualOn; }
bool     relay_isOn()          { return relayOn; }
bool     relay_isEnabled()     { return kRelayPresent; }
bool     relay_isPendingStart() { return pending; }
uint32_t relay_pendingTag()    { return pending ? pendingTag : 0; }
uint32_t relay_lastStartedTag() { return lastStartedTag; }
uint32_t relay_preMs()         { return preMs; }
uint32_t relay_holdMs()        { return holdMs; }

bool relay_isBusy() {
    // Кэш, а не прямой mp3_isPlaying(): иначе кнопка/JSON/LED дёргали бы
    // audioMtx на каждом тике.
    return pending || lastAudioPlaying;
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
