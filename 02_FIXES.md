# Gong LoRa System — решения

Документ-спутник к `01_AUDIT_REPORT.md`. Нумерация совпадает: C-* — блокирующие, H-* — важные, M-* — средние.
Код приведён под текущий стек (arduino-esp32 2.0.x, RadioLib 6.6, ArduinoJson 6.21, ESP32-audioI2S 2.0.6).

---

## Часть I. Схемотехника

### S-1. Привести README к реальности (сделать первым, до всего остального)

Правки в `README.md`:

```diff
-GPIO2   ──── DIO0
+GPIO4   ──── DIO0

-| Spreading Factor | SF7 |
+| Spreading Factor | SF9 (см. раздел «Выбор SF») |

-| TX Power | 20 дБм |
+| TX Power | 17 дБм |

-3.3V    ──── SD
+SD ──── резистор 1 МОм ──── 3.3V   (режим моно (L+R)/2)
+GAIN ──── резистор 100 кОм ──── GND (усиление 12 дБ; висящий = 9 дБ)
```

Удалить целиком: главу про WiFi STA, всё про NTP и `NTP_UTC_OFFSET`, эндпоинты `/api/wifi/*`, строку `MSG_SCHEDULE` из таблицы протокола, `POST /api/sync`, раздел «Работа без роутера».

Добавить: главу про многодневный курс (12 дней, `dayNN.conf`, автопереход по календарной дате, кнопка Activate), эндпоинты `/api/days`, `/api/day`, `/api/day/activate`, `/api/day/entry`, `/api/tracks`, `/api/time`, `/api/time/source`, `/api/logs`.

Заменить формулировку про DS3231 с «опционально» на **«обязательно»** с объяснением, что NTP в системе нет.

Добавить предупреждение крупно: **«Не подавать питание на LoRa-модуль без подключённой антенны — выйдет из строя выходной каскад»**.

### S-2. Питание — самое важное изменение в железе

```
                 БП 5 В / 3 А
                      │
        ┌─────────────┼─────────────┐
        │             │             │
   [1000 мкФ]    ESP32 5V pin   MAX98357A VDD
   [ 0.1 мкФ]                   [470 мкФ + 0.1 мкФ прямо у чипа]
        │
       GND ──── общая точка (звезда)
```

- `1000 мкФ` low-ESR у входа платы, `470 мкФ` — у VDD усилителя, короткими проводниками.
- `100 мкФ + 0.1 мкФ` у пина 3.3 В ESP32.
- `100–220 мкФ + 0.1 мкФ` у VCC LoRa-модуля.
- Земля — звездой в одной точке, силовая земля усилителя не через плату ESP32.
- Провода к динамику — витая пара, не длиннее 1–2 м, ферритовая бусина на выходе.

Проверка: осциллограф на 3.3 В во время удара гонга при максимальной громкости. Просадка ниже 3.0 В = brownout.

### S-3. Дискретные доработки

| Что | Как | Зачем |
|---|---|---|
| Pull-up 10 кОм на NSS (GPIO5) → 3.3 В | резистор по месту | SX1278 не видит мусор на SPI, пока ESP32 грузится |
| Pull-up 10 кОм на RST (GPIO14) → 3.3 В | резистор по месту | подавить импульсы GPIO14 при загрузке |
| Вывести DIO1 на свободный GPIO (например 27) | провод + `#define LORA_DIO1 27` | понадобится для CAD (см. C-3, вариант B) |
| SD MAX98357A через 1 МОм на VDD | резистор вместо перемычки | режим (L+R)/2 вместо «только левый канал» |
| GAIN через 100 кОм на GND | резистор | 12 дБ; зафиксировать выбор письменно |
| Статусный светодиод клиента на GPIO13 или 32 | светодиод + 1 кОм | см. H-11 ниже |
| DS3231: выпаять зарядный резистор (R5/201) **или** ставить LIR2032 | — | CR2032 в цепи подзарядки вздувается |

---

## Часть II. Блокирующие дефекты

### C-1. Убрать 5-секундные блокировки `getLocalTime()`

Добавить в `server/src/config.h`:

```cpp
#include <time.h>

// Порог «время реально установлено» — 2023-11-14. Всё, что раньше,
// считаем незаданным (ESP32 стартует с 1970).
#define TIME_VALID_EPOCH  1700000000UL

// Неблокирующая замена getLocalTime(). Штатный getLocalTime() имеет
// таймаут 5000 мс по умолчанию и крутит delay(10) до истечения, если
// время не задано — а «время не задано» это состояние по умолчанию
// на устройстве без DS3231. См. C-1 в отчёте.
static inline bool timeIsSet() {
    return (uint32_t)time(nullptr) > TIME_VALID_EPOCH;
}

static inline bool localNow(struct tm& out) {
    time_t t = time(nullptr);
    if ((uint32_t)t <= TIME_VALID_EPOCH) return false;
    localtime_r(&t, &out);
    return true;
}
```

Заменить **все пять** мест вызова:

```diff
--- server/src/schedule.cpp:47  (currentDateStr)
-    if (!getLocalTime(&ti) || ti.tm_year < 124) return "";
+    if (!localNow(ti) || ti.tm_year < 124) return "";

--- server/src/schedule.cpp:95  (sched_check)
-    if (!getLocalTime(&ti)) {
+    if (!localNow(ti)) {

--- server/src/lorahandler.cpp:326  (lora_sendHeartbeat)
-    if (getLocalTime(&ti)) {
+    if (localNow(ti)) {

--- server/src/webhandler.cpp:168  (handleTimeSet)
-    bool haveCur = getLocalTime(&cur) && cur.tm_year >= 124;
+    bool haveCur = localNow(cur) && cur.tm_year >= 124;

--- server/src/webhandler.cpp:222  (handleStatus)
-    if (getLocalTime(&ti) && ti.tm_year >= 124) {
+    if (localNow(ti) && ti.tm_year >= 124) {
```

Затраты: 15 минут. Эффект: интерфейс перестаёт «зависать» в состоянии без установленного времени; кнопка «Set time» начинает срабатывать мгновенно.

Проверка после правки: загрузить прошивку на плату **без** DS3231, открыть веб-интерфейс — `/api/status` должен отвечать за миллисекунды, часы показывать `--:--:--`, а не подвисать.

### C-2. Монотонная метка времени `ts`

**Сервер** — `server/src/lorahandler.cpp`:

```cpp
#include <Preferences.h>

static Preferences tsPrefs;
static uint32_t    tsBase        = 0;   // база для случая «время не задано»
static uint32_t    tsPersisted   = 0;   // последнее сохранённое в NVS значение

void ts_setup() {                        // вызывать в начале lora_setup()
    tsPrefs.begin("gong", false);
    tsBase      = tsPrefs.getUInt("tsbase", 0);
    tsPersisted = tsBase;
}

// Никогда не идёт назад — ни при перезагрузке, ни при потере RTC,
// ни при переключении источника времени. См. C-2 в отчёте.
static uint32_t nowTs() {
    uint32_t real = (uint32_t)time(nullptr);
    uint32_t up   = tsBase + (uint32_t)(millis() / 1000);
    uint32_t v    = (real > TIME_VALID_EPOCH && real > up) ? real : up;

    // Раз в минуту фиксируем «водяной знак» с запасом 120 с,
    // чтобы неучтённое окно между записями не дало отката.
    if (v > tsPersisted + 60) {
        tsPersisted = v;
        tsPrefs.putUInt("tsbase", v + 120);
    }
    return v;
}
```

**Клиент** — `client/src/lorahandler.cpp`, заменить блок anti-replay целиком:

```cpp
#include <Preferences.h>
static Preferences rpPrefs;
static uint32_t    lastServerTs = 0;

static void replay_setup() {              // вызывать в начале lora_setup()
    rpPrefs.begin("gong", false);
    lastServerTs = rpPrefs.getUInt("lastts", 0);
}

static bool verifyMsg(uint8_t type, DynamicJsonDocument& doc) {
    /* … проверка подписи без изменений … */

    uint32_t ts = doc["ts"] | 0;
    if (ts == 0) {
        Serial.printf("[LORA] No ts on 0x%02X — rejected\n", type);
        return false;                     // теперь ts обязателен
    }
    // Сервер гарантирует монотонность (см. nowTs выше), поэтому
    // никаких «окон ресинхронизации» больше не нужно — строгий >.
    if (ts <= lastServerTs) {
        Serial.printf("[LORA] Replay/dup ts=%u last=%u — rejected\n", ts, lastServerTs);
        return false;
    }
    lastServerTs = ts;
    // Пишем в NVS не чаще раза в минуту — беречь ресурс flash
    static uint32_t lastWrite = 0;
    if (ts > lastWrite + 60) { lastWrite = ts; rpPrefs.putUInt("lastts", ts); }
    return true;
}
```

Удалить `REPLAY_RESYNC_GAP_S` и `lastServerSig` — они больше не нужны.

**Процедура ввода в строй после этой правки:** если сервер уже работал со старой прошивкой, у клиентов в NVS может лежать большое `lastServerTs`. Один раз выполнить `nvs erase` на клиентах (или залить прошивку с `rpPrefs.clear()` на один цикл), чтобы стартовать с чистого листа.

### C-3. Коллизии ACK

Три меры, применять вместе.

**Мера 1 — снизить SF.** Самое дешёвое действие с самым большим эффектом:

| SF | Airtime ACK | Airtime GONG | Чувствительность @125 кГц | Бюджет линка при +17 dBm |
|---|---|---|---|---|
| SF9 (сейчас) | 288 мс | 431 мс | −129 дБм | 146 дБ |
| SF8 | 158 мс | 236 мс | −126 дБм | 143 дБ |
| **SF7** | **87 мс** | **133 мс** | −123 дБм | **140 дБ** |

Для здания или кампуса 140 дБ — избыточный запас. **Рекомендую SF7**, при жалобах на дальность — SF8. Менять надо **одновременно** в обоих `config.h` (см. M-21 про защиту от рассинхронизации).

**Мера 2 — убрать ACK на GONG.** Сервер его всё равно игнорирует для RTT, а в эфир он уходит ровно в момент запуска звука.

```diff
--- client/src/lorahandler.cpp:187
             xQueueSend(rxQueue, &cmd, 0);
-            sendAck(rssi);  // no "hb": GONG ACKs must not affect RTT stats
+            radio.startReceive();   // ACK на GONG не нужен: сервер его
+                                    // не использует, а эфир он занимает
+                                    // ровно тогда, когда клиент запускает звук
             continue;
```

**Мера 3 — детерминированные слоты вместо случайного джиттера.** Каждый клиент отвечает в своё окно, вычисленное из хеша его `CLIENT_ID`:

```cpp
// client/src/config.h
#define ACK_SLOT_COUNT   16
#define ACK_SLOT_MS     120     // > airtime ACK при SF7 (87 мс) + запас
#define ACK_GUARD_MS    150     // пауза после heartbeat, пока сервер уходит в RX

// client/src/lorahandler.cpp
static uint16_t ackSlot() {
    uint32_t h = 2166136261u;               // FNV-1a
    for (const char* p = CLIENT_ID; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
    return h % ACK_SLOT_COUNT;
}

static void sendAck(int rxRssi, uint32_t hbSeq) {
    /* … сборка payload … */
    vTaskDelay(pdMS_TO_TICKS(ACK_GUARD_MS + ackSlot() * ACK_SLOT_MS));
    /* … передача … */
}
```

Окно ответов: `150 + 16 × 120 = 2.07 с` после каждого heartbeat. При интервале heartbeat 30 с эфир занят на 7 % — приемлемо. Коллизии исчезают полностью для ≤ 16 клиентов с уникальными ID (о дубликатах — H-6).

Сервер при этом не должен ничего передавать в течение 2.1 с после heartbeat — добавить в `loraTask`:

```cpp
static uint32_t ackWindowUntil = 0;
// в ветке TX-done для MSG_HEARTBEAT:
if (txType == MSG_HEARTBEAT) {
    hbTxDoneMs     = millis();
    ackWindowUntil = millis() + ACK_GUARD_MS + ACK_SLOT_COUNT * ACK_SLOT_MS;
}
// перед стартом очередного TX:
if (millis() < ackWindowUntil && req.type != MSG_GONG && req.type != MSG_STOP) {
    /* вернуть в очередь / подождать */
}
```
GONG и STOP пропускаем всегда — они важнее статистики.

**Вариант B (если позже разведёте DIO1):** LBT/CAD перед передачей ACK — `radio.scanChannel()` в цикле с отступом. Это правильнее в принципе, но требует DIO1 и заметно больше кода.

### C-4. Заменить RTT на RSSI + SNR

**Сервер** — `lorahandler.cpp`:

```diff
 struct ClientInfo {
     String   id;
     int      rssi;
+    float    snr;
     uint32_t lastSeenMs;
-    uint32_t rttMs;
-    uint32_t oneWayMs;
+    uint32_t respMs;      // время ответа, честно названное: airtime + слот
 };
```
```diff
-        int rssi = (int)radio.getRSSI();
+        int   rssi = (int)radio.getRSSI();
+        float snr  = radio.getSNR();
```

В `lora_clientsJSON()` отдавать `snr` и переименовать `rtt_ms` → `resp_ms`, `one_way_ms` убрать.

**UI** — `index.html`, `loadClients()`:

```diff
-          <span class="${rssiClass(c.rssi)}">RSSI: ${c.rssi} dBm</span><br>
-          RTT: ${c.rtt_ms || 0} ms<br>
+          <span class="${rssiClass(c.rssi)}">RSSI: ${c.rssi} dBm</span><br>
+          <span class="${snrClass(c.snr)}">SNR: ${(c.snr ?? 0).toFixed(1)} dB</span><br>
           Seen: ${Math.round((c.seen_ms || 0) / 1000)}s ago
```
```js
// Порог демодуляции LoRa: SF7 ≈ −7.5 дБ, SF9 ≈ −12.5 дБ.
// Запас меньше 3 дБ — линк на грани.
function snrClass(snr) {
  if (snr >= 0)   return 'rssi-good';
  if (snr >= -7)  return 'rssi-ok';
  return 'rssi-bad';
}
```

Из README убрать формулу `OneWay = (RTT − avg_ack_delay) / 2` и утверждение про оценку расстояния.

---

## Часть III. Важные дефекты

### H-1. Не блокировать `loop()` на время передачи GONG

Вместо ожидания семафора — передать флаг «после TX-done запустить локально» внутрь запроса:

```cpp
// server/src/lorahandler.cpp
struct TxReq {
    uint8_t type;
    uint8_t buf[LORA_PAYLOAD_MAX + 1];
    size_t  len;
    bool    playLocal;      // ← новое
    uint8_t track, vol, loop;
};

static QueueHandle_t localPlayQueue = nullptr;   // Core 0 → Core 1

// в loraTask, ветка TX-done:
if (txType == MSG_GONG && txPlayLocal) {
    LocalPlay lp = { txTrack, txVol, txLoop };
    xQueueSend(localPlayQueue, &lp, 0);
}
```
```cpp
// server/src/main.cpp
void loop() {
    web_loop();
    lora_drainLocalPlay();      // ← новое, отрабатывает за микросекунды
    /* … */
}
```

`lora_sendGong()` становится неблокирующим, `txGongDone` удаляется целиком. Синхронность старта звука сохраняется — она обеспечивается тем, что TX-done на сервере и RX-done на клиенте происходят почти одновременно, а не тем, что кто-то ждал в цикле.

Заодно поправить комментарий: `~100-150ms` → фактическое значение для выбранного SF из таблицы в C-3.

### H-2. Не терять пакет в окне ожидания перед ACK

```diff
--- client/src/lorahandler.cpp, sendAck()
     vTaskDelay(pdMS_TO_TICKS(ACK_GUARD_MS + ackSlot() * ACK_SLOT_MS));
 
+    // За время ожидания мог прийти пакет (например STOP сразу после GONG).
+    // Слепой dioFlag = false уничтожал его без следа. Обработаем сначала его.
+    if (dioFlag) {
+        Serial.println("[LORA] RX during ACK slot — handling packet first");
+        return;   // ACK пропускаем; loraTask на следующей итерации разберёт пакет
+    }
+
     uint8_t buf[LORA_PAYLOAD_MAX + 1];
     buf[0] = MSG_ACK;
```
Пропущенный ACK ничего не ломает — сервер увидит клиента на следующем heartbeat. Потерянный STOP ломает.

### H-3 + M-5 + M-6. Бинарный кадр с подписью на фиксированном смещении

Текущая схема (строковая хирургия при подписании + ре-сериализация при проверке) работает случайно. Заменить на формат с фиксированным смещением:

```
кадр = [1 байт: тип][8 байт: HMAC-тег][N байт: payload]
тег  = HMAC-SHA256(key, type || payload)[0..7]
```

```cpp
// общая функция для обоих устройств
static void hmacTag(uint8_t type, const uint8_t* data, size_t len, uint8_t out[8]) {
    uint8_t full[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t*)LORA_HMAC_KEY, strlen(LORA_HMAC_KEY));
    mbedtls_md_hmac_update(&ctx, &type, 1);
    mbedtls_md_hmac_update(&ctx, data, len);
    mbedtls_md_hmac_finish(&ctx, full);
    mbedtls_md_free(&ctx);
    memcpy(out, full, 8);
}

// сравнение — константное по времени
static bool tagEqual(const uint8_t* a, const uint8_t* b) {
    uint8_t d = 0;
    for (int i = 0; i < 8; i++) d |= a[i] ^ b[i];
    return d == 0;
}
```

Плюсы: нет зависимости от порядка ключей JSON, нет ре-сериализации, экономия 8 байт на кадр (двоичный тег вместо hex), `{}` больше не ломает формат, сравнение защищено от timing-атак.

### H-4 + H-5. Резервное расписание на клиентах — снятие единой точки отказа

Вывод «расписание не влезает в LoRa-кадр» верен только для JSON. В бинарном виде:

```cpp
// 4 байта на запись
struct __attribute__((packed)) SchedBin {
    uint8_t hour;    // 0–23
    uint8_t minute;  // 0–59
    uint8_t track;   // 1–99
    uint8_t loop;    // 1–7, бит 7 = enabled
};
// 16 записей = 64 байта + тип + тег(8) + счётчик дня(1) = 74 байта.
// Airtime при SF7 ≈ 136 мс. Влезает в один кадр с большим запасом.
```

Схема работы:
1. Сервер шлёт `MSG_SCHEDULE` (подписанный, с `ts`) при активации дня и раз в час.
2. Клиент сохраняет его в NVS вместе с номером дня.
3. Heartbeat каждые 30 с несёт поле `time` — клиент подводит по нему свои локальные часы. Дрейф кварца ESP32 ±20 ppm ≈ ±1.7 с/сутки, а с подстройкой раз в 30 секунд — единицы миллисекунд.
4. **Если heartbeat не приходил дольше 10 минут** — клиент переключается на автономный режим и звонит по собственному расписанию.
5. Как только heartbeat вернулся — обратно в ведомый режим (сервер главный, чтобы не было двойных ударов).

Это устраняет самый тяжёлый риск проекта: смерть сервера ночью больше не срывает утренний подъём.

### H-6. `CLIENT_ID` по умолчанию из MAC-адреса

```cpp
// client/src/config.h
// #define CLIENT_ID_OVERRIDE "room_A"   // раскомментировать для говорящего имени

// client/src/lorahandler.cpp
#include <esp_system.h>
static String clientId() {
#ifdef CLIENT_ID_OVERRIDE
    return String(CLIENT_ID_OVERRIDE);
#else
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char b[16];
    snprintf(b, sizeof(b), "cli_%02X%02X%02X", mac[3], mac[4], mac[5]);
    return String(b);
#endif
}
```
Уникальность гарантирована аппаратно, «забыть поменять» больше нельзя. `ackSlot()` считать от того же значения.

### H-7. Ключ по умолчанию должен ломать сборку

Убрать ключ из `config.h`, передавать через `platformio.ini`:

```ini
build_flags =
    -DCORE_DEBUG_LEVEL=3
    -DLORA_HMAC_KEY='"${sysenv.GONG_KEY}"'
```
```cpp
// config.h
#ifndef LORA_HMAC_KEY
#error "LORA_HMAC_KEY не задан. Экспортируйте GONG_KEY перед сборкой:  export GONG_KEY='...'"
#endif
static_assert(sizeof(LORA_HMAC_KEY) >= 17, "LORA_HMAC_KEY короче 16 символов");
```

Если менять сборочный процесс не хочется — минимум, проверка в рантайме:

```cpp
if (strcmp(LORA_HMAC_KEY, "change_me_before_deploy_32chars!") == 0)
    logPrintf("[SEC] !!! ИСПОЛЬЗУЕТСЯ КЛЮЧ ПО УМОЛЧАНИЮ — ЛЮБОЙ МОЖЕТ УПРАВЛЯТЬ СИСТЕМОЙ !!!\n");
```
и красный баннер в веб-интерфейсе (флаг `default_key` в `/api/status`).

### H-8. Убрать мёртвую кнопку Sync Schedule

```diff
--- server/data/index.html:345
-        <button class="btn btn-yellow"  onclick="syncSchedule()">&#8635; Sync Schedule</button>
```
и удалить `syncSchedule()`, обработчик `/api/sync` и его OPTIONS-маршрут.

Либо — если делаете H-5 — наполнить её смыслом: рассылка бинарного расписания клиентам.

### H-9. Убрать CORS

CORS не нужен: интерфейс отдаётся с того же origin, что и API.

```diff
-static void cors(WebServer& s) { … }
-static void handleOptions() { … }
 static void sendJSON(int code, const String& body) {
-    cors(server);
     server.send(code, "application/json", body);
 }
```
Удалить все 17 строк `server.on(..., HTTP_OPTIONS, handleOptions)`.

Дополнительно — простая защита от CSRF: требовать нестандартный заголовок на всех изменяющих запросах.

```cpp
static bool checkOrigin() {
    if (server.header("X-Gong-Request") != "1") {
        server.send(403, "text/plain", "Forbidden");
        return false;
    }
    return true;
}
// в web_setup(): const char* hdrs[] = {"X-Gong-Request"}; server.collectHeaders(hdrs, 1);
```
В `postJSON()` добавить `headers: {'Content-Type':'application/json', 'X-Gong-Request':'1'}`. Без CORS-ответа на preflight сторонняя страница этот заголовок поставить не сможет.

### H-10. Хэшировать пароль админки

```cpp
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include <esp_random.h>

// /auth.conf → {"enabled":true,"salt":"<32 hex>","hash":"<64 hex>","iter":20000}
static bool pbkdf2(const String& pwd, const uint8_t salt[16], uint32_t iter, uint8_t out[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) != 0) return false;
    int rc = mbedtls_pkcs5_pkcs2_pbkdf2_hmac(&ctx, (const uint8_t*)pwd.c_str(), pwd.length(),
                                             salt, 16, iter, 32, out);
    mbedtls_md_free(&ctx);
    return rc == 0;
}
```
На ESP32 20 000 итераций PBKDF2-SHA256 ≈ 150 мс — приемлемо для входа раз в сессию.

`server.authenticate()` придётся заменить: разобрать заголовок `Authorization` вручную, декодировать base64, посчитать PBKDF2 и сравнить константно по времени.

Минимум усилий при отказе от PBKDF2: `SHA-256(salt || password)` со случайной 16-байтовой солью — это уже принципиально лучше открытого текста.

### H-11. Вернуть индикацию на клиенте

```cpp
// client/src/config.h
#define STATUS_LED  13     // свободный GPIO, не strapping
```

| Состояние | Индикация |
|---|---|
| Загрузка | 4 коротких мигания |
| Радио не инициализировалось | быстрое непрерывное мигание 5 Гц |
| Норма, heartbeat получен < 90 с назад | одно короткое мигание раз в 3 с |
| Heartbeat потерян (> 90 с) | двойное мигание раз в 3 с |
| Играет трек | горит непрерывно |

Ставит диагноз без ноутбука. Для устройств, разбросанных по зданию, это экономит часы.

---

## Часть IV. Средние дефекты

### M-1 + M-2. Приоритеты задач и блокировки

```diff
--- mp3handler.cpp (оба устройства)
     xTaskCreatePinnedToCore(audioFeederTask, "audio_feed", 8192, nullptr,
-                            configMAX_PRIORITIES - 1, &audioTaskHandle, 1);
+                            8, &audioTaskHandle, 1);
```
Приоритет 8 всё ещё выше loopTask (1) и веб-задач, но ниже системных. Приоритет 24 не давал ничего, кроме риска.

Не логировать под мьютексом аудио:

```diff
--- mp3_loop(), ветка повтора трека
             if (loopRemain > 0) {
                 loopRemain--;
                 idleCount = 0;
-                logPrintf("[MP3] Loop repeat — remaining=%d\n", loopRemain);
                 _startPlay(loopTrack);
+                pendingLoopLog = loopRemain;   // залогируем после unlock
             }
     audioUnlock();
+    if (pendingLoopLog >= 0) { logPrintf("[MP3] Loop repeat — remaining=%d\n", pendingLoopLog); pendingLoopLog = -1; }
```

И убрать бессмысленный вызов раз в миллисекунду:
```diff
-            } else {
-                audio.setVolume(0);
-            }
+            } else if (!muted) {
+                audio.setVolume(0);
+                muted = true;
+            }
```

Зафиксировать в комментарии порядок блокировок: **`audioMtx` → `logMtx`, никогда наоборот**.

### M-3. Не резать вывод в Serial

```diff
 void logPrintf(const char* fmt, ...) {
-    char tmp[LOG_LINE_LEN];
+    char tmp[256];                    // Serial получает полную строку
     …
-    Serial.write((const uint8_t*)tmp, len);
+    Serial.write((const uint8_t*)tmp, len);
+    // в кольцевой буфер — усечённая копия
+    if (slen >= LOG_LINE_LEN) slen = LOG_LINE_LEN - 1;
```

### M-4. Правильный лимит payload

```diff
-#define LORA_PAYLOAD_MAX 256
+#define LORA_PAYLOAD_MAX 254     // максимум LoRa payload 255, минус 1 байт типа
```
В обоих файлах.

### M-7. Не терять heartbeat

```diff
--- server/src/main.cpp
     if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
-        lora_sendHeartbeat();
-        lastHeartbeat = now;
+        if (lora_sendHeartbeat()) lastHeartbeat = now;
+        else                      lastHeartbeat = now - HEARTBEAT_INTERVAL_MS + 2000;  // повтор через 2 с
     }
```
`lora_sendHeartbeat()` вернуть `bool`.

### M-8. Самовосстановление радио

```cpp
// в loraTask, в ветке простоя
static uint32_t lastRadioOk = 0;
if (millis() - lastRadioOk > 300000UL) {          // 5 минут без успешных TX/RX
    logPrintf("[LORA] No activity 5 min — reinitialising radio\n");
    radio.reset();
    if (radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR,
                    LORA_SYNC_WORD, LORA_TX_POWER, 8, 0) == RADIOLIB_ERR_NONE) {
        radio.startReceive();
        loraReady = true;
        logPrintf("[LORA] Reinit OK\n");
    } else {
        loraReady = false;
    }
    lastRadioOk = millis();
}
```
`lastRadioOk = millis()` обновлять при каждом успешном TX-done и RX. Плюс: если `lora_setup()` не удался при старте, повторять попытку раз в 30 секунд, а не сдаваться навсегда.

### M-9. Watchdog

```cpp
// server/src/main.cpp и client/src/main.cpp
#include <esp_task_wdt.h>

void setup() {
    …
    esp_task_wdt_init(30, true);   // 30 с, паника → перезагрузка
    esp_task_wdt_add(NULL);        // главный цикл
}

void loop() {
    esp_task_wdt_reset();
    …
}
```
30 секунд — с запасом на `sched_activateDay()` с несколькими записями в SPIFFS. `loraTask` тоже стоит подписать на WDT, но с бо́льшим таймаутом.

### M-10. Один эндпоинт вместо пяти

```cpp
// GET /api/state — всё, что нужно UI, одним запросом
static void handleState() {
    if (!checkAuth()) return;
    String s = "{";
    s += "\"status\":";   s += statusJSON();
    s += ",\"schedule\":";s += sched_toJSON();
    s += ",\"days\":";    s += daysJSON();
    s += ",\"clients\":"; s += lora_clientsJSON();
    s += ",\"auth\":{\"enabled\":"; s += authEnabled ? "true" : "false"; s += "}";
    s += "}";
    sendJSON(200, s);
}
```
```diff
--- index.html
 async function refreshAll() {
   if (refreshInFlight) return;
   refreshInFlight = true;
   try {
-    await Promise.all([loadStatus(), loadSchedules(), loadAuthStatus(), loadDaysStatus(), loadClients()]);
+    const r = await fetch('/api/state');
+    const d = await r.json();
+    applyStatus(d.status); applySchedule(d.schedule);
+    applyDays(d.days);     applyClients(d.clients);  applyAuth(d.auth);
   } finally { refreshInFlight = false; }
 }
```
Пять последовательных запросов к однопоточному серверу → один. Отзывчивость интерфейса вырастет заметно.

### M-11. Громкость 0

```diff
-    vol:   parseInt(document.getElementById('cVol').value)   || 30,
+    vol:   (v => Number.isFinite(v) ? v : 30)(parseInt(document.getElementById('cVol').value)),
```

### M-12. Громкость в записи расписания

```diff
 struct ScheduleEntry {
     uint32_t id;
     uint8_t  hour, minute, track, loop;
+    uint8_t  vol;          // 0–30
     bool     enabled;
     String   description;
 };
```
```diff
-static void onGongFire(uint8_t track, uint8_t loop) {
-    lora_sendGong(track, DEFAULT_VOLUME, loop);
-    mp3_setVolume(DEFAULT_VOLUME);
+static void onGongFire(uint8_t track, uint8_t loop, uint8_t vol) {
+    lora_sendGong(track, vol, loop);
+    mp3_setVolume(vol);
```
Плюс поле `vol` в JSON, в форме модального окна и в бинарной структуре из H-5 (влезает: 5 байт на запись, 16 записей = 80 байт).

При загрузке старых файлов `o["vol"] | DEFAULT_VOLUME` — обратная совместимость сохраняется.

### M-13. Объяснить ограничение «одна запись на минуту»

Текст подсказки под таблицей расписания:
> На одну минуту можно поставить только один гонг. Нужно два подряд — используйте поле «повторы» (loop) или разнесите на соседние минуты.

### M-14. Догоняющее срабатывание после перезагрузки

```cpp
// сохранять при каждом срабатывании
prefs.putUInt("lastfire", (uint32_t)time(nullptr));

// в sched_setup(), после загрузки расписания:
uint32_t lastFire = prefs.getUInt("lastfire", 0);
time_t   now      = time(nullptr);
if (timeIsSet() && now - lastFire < 3600) {
    // найти записи, которые должны были сработать в промежутке
    // (lastFire, now] и не сработали. Если пропуск < CATCHUP_WINDOW_S,
    // отзвонить один раз; иначе только записать в лог как пропущенный.
}
```
`CATCHUP_WINDOW_S` сделать настраиваемым (по умолчанию 120 с). Пропущенные гонги показывать в интерфейсе — оператор должен знать.

### M-15. Переход дня на несколько суток

```cpp
static int daysBetween(const String& from, const String& to) {
    struct tm a = {}, b = {};
    if (sscanf(from.c_str(), "%d-%d-%d", &a.tm_year, &a.tm_mon, &a.tm_mday) != 3) return 0;
    if (sscanf(to.c_str(),   "%d-%d-%d", &b.tm_year, &b.tm_mon, &b.tm_mday) != 3) return 0;
    a.tm_year -= 1900; a.tm_mon -= 1; a.tm_hour = 12;
    b.tm_year -= 1900; b.tm_mon -= 1; b.tm_hour = 12;
    return (int)((mktime(&b) - mktime(&a)) / 86400);
}
```
```diff
-                uint8_t nextDay = (uint8_t)(activeDay + 1);
+                int diff    = daysBetween(stored, today);
+                if (diff <= 0) diff = 1;
+                int nextDay = activeDay + diff;
+                if (nextDay >= DAY_COUNT) nextDay = DAY_COUNT - 1;   // курс окончен
+                logPrintf("[SCHED] Пропущено суток: %d\n", diff);
```

### M-16. Не писать в SPIFFS во время воспроизведения

```cpp
// перед sched_save() / sched_activateDay()
if (mp3_isPlaying()) {
    pendingSave = true;      // отложить
    return;
}
// в loop(): if (pendingSave && !mp3_isPlaying()) { pendingSave = false; sched_save(); }
```
Для веб-обработчиков ответ остаётся `200` — операция принята, просто применится через несколько секунд.

### M-17. Не собирать строку посимвольно

```diff
-        String payload = "";
-        for (size_t i = 1; i < len; i++) payload += (char)buf[i];
+        buf[len] = '\0';                        // буфер на 1 байт больше
+        String payload((const char*)(buf + 1));
```
Один вызов вместо 256 реаллокаций, в горячем пути обработки каждого пакета.

### M-18. Переход на LittleFS

```diff
--- platformio.ini
-board_build.filesystem = spiffs
+board_build.filesystem = littlefs
+lib_deps =
+    lorol/LittleFS_esp32 @ ^1.0.6     # для arduino-esp32 2.0.x; в 3.x встроен
```
```diff
--- везде
-#include <SPIFFS.h>
-SPIFFS.begin(true)
-audio.connecttoFS(SPIFFS, path)
+#include <LittleFS.h>
+LittleFS.begin(true)
+audio.connecttoFS(LittleFS, path)
```
`connecttoFS()` принимает `fs::FS&`, так что менять больше ничего не нужно.

**Важно:** после перехода обязателен `pio run -t uploadfs` — старая SPIFFS-разметка не читается. Заранее сохранить `gong.conf` и `dayNN.conf` копированием.

### M-19. Чистить `data/` перед заливкой

```ini
extra_scripts = pre:scripts/clean_data.py
```
```python
# scripts/clean_data.py
import os, glob
for junk in ('.DS_Store', 'Thumbs.db', '._*'):
    for p in glob.glob(os.path.join('data', '**', junk), recursive=True):
        os.remove(p)
```

### M-20. Статика и gzip

```cpp
server.serveStatic("/", LittleFS, "/index.html");     // покрывает и /index.html
```
Пожать интерфейс: `gzip -9 index.html` → `index.html.gz` (41 КБ → ~9 КБ), отдавать с `Content-Encoding: gzip`. Загрузка страницы по AP ускорится в разы.

### M-21 (новое). Защита от расхождения RF-параметров

Вынести общие константы в один файл, включаемый обоими проектами:

```
gong-lora-system/
├── common/
│   └── lora_shared.h     ← FREQ, SF, BW, CR, SYNC_WORD, типы сообщений, версия протокола
├── server/
└── client/
```
```ini
; в обоих platformio.ini
build_flags = -I../common
```
```cpp
// common/lora_shared.h
#define LORA_PROTO_VERSION 3
#define LORA_FREQ      433.0
#define LORA_SF        7
#define LORA_BW        125.0
#define LORA_CR        5
#define LORA_SYNC_WORD 0xF3
```

Плюс — версия протокола в каждом кадре и проверка на клиенте:

```cpp
if (protoVer != LORA_PROTO_VERSION) {
    Serial.printf("[LORA] Несовместимая версия протокола: сервер %u, клиент %u — "
                  "перепрошейте устройство\n", protoVer, LORA_PROTO_VERSION);
    // мигнуть светодиодом характерным узором
    return;
}
```
Теперь рассинхронизация парка **видна**, а не проявляется как молчание.

Туда же вынести `mp3handler.cpp/h` и HMAC — сейчас это скопированные файлы, которые расходятся при любой правке.

---

## Часть V. Проверка после исправлений

Минимальный набор, который надо прогнать перед курсом.

**Стенд**
1. Сервер **без** DS3231, свежая прошивка → веб-интерфейс отвечает мгновенно, часы `--:--:--`, «Set time» срабатывает за доли секунды. *(проверяет C-1)*
2. Установить время → расписание оживает, автопереход дня стоит на сегодняшней дате.
3. Клиент включён, сервер перезагружается через 10 минут работы → **первый же гонг после ребута доходит**. *(проверяет C-2 — на текущей прошивке этот тест падает)*
4. Три клиента одновременно → все три стабильно видны в панели, не мигают, RSSI и SNR разумные. *(проверяет C-3)*
5. Осциллограф на 3.3 В, максимальная громкость, трек с сильной атакой → просадка не ниже 3.1 В. *(проверяет S-2)*
6. Заблокировать питание LoRa-модуля на работающем сервере → в логе TX-таймаут, через 5 минут автоматический reinit, после возврата питания связь восстанавливается сама. *(проверяет M-8)*
7. Клиент вне зоны, потом внесён в зону → появляется в панели не позже чем через 30 с.
8. Отключить питание сервера на 30 секунд в момент, когда до гонга 40 секунд → после загрузки гонг либо отрабатывает, либо отмечен как пропущенный в интерфейсе. *(проверяет M-14)*

**Полевая проверка**
9. Пройти с клиентом по всем точкам, где он будет стоять, записать RSSI/SNR. Запас по SNR меньше 3 дБ до порога — переставить или поднять SF.
10. Сутки непрерывной работы с полным расписанием, все 16 гонгов отработали на всех клиентах, heap в `/api/status` не убывает монотонно.

**Регулярно**
11. Раз в год — замена батарейки DS3231 и сверка часов.

---

## Порядок работ

| Этап | Пункты | Оценка | Результат |
|---|---|---|---|
| **1. Срочные правки кода** | C-1, C-2, S-1 (README), H-7 | 1 день | Уходят «зависания» и пропадание гонгов после ребута |
| **2. Радиоканал** | C-3 (SF7 + слоты + убрать ACK на GONG), C-4, M-4, M-21 | 2–3 дня | Система работает с любым числом клиентов, метрики честные |
| **3. Железо** | S-2 (питание), S-3 (резисторы, LED), DS3231 на все серверы | 1–2 дня | Уходят brownout-перезагрузки, появляется диагностика на месте |
| **4. Надёжность** | H-5 (автономное расписание), M-8, M-9, M-14, M-15 | 3–4 дня | Смерть сервера больше не срывает распорядок |
| **5. Качество** | H-1, H-2, H-3, M-1, M-2, M-7, M-10, M-16, M-17 | 2–3 дня | Отзывчивость, отсутствие редких сбоев |
| **6. Безопасность и гигиена** | H-9, H-10, H-6, H-8, M-11…M-13, M-18…M-20, Low | 2–3 дня | — |

Этапы 1 и 3 дают наибольший эффект на вложенное время. Этап 4 — единственный, который меняет архитектуру, но именно он снимает риск, из-за которого систему нельзя оставить без присмотра на ночь.
