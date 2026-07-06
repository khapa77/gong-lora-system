# Gong LoRa System — схема логики и прерываний

> Составлено по факту чтения исходников: `server/src/*`, `server/include/*`, `client/src/*`.
> Все ссылки вида `file:line` соответствуют состоянию кода на момент анализа.

## 1. Общая архитектура

```
┌─────────────────────────── SERVER (ESP32) ───────────────────────────┐
│                                                                        │
│  Core 1 (Arduino loop, prio 1)        Core 0 (pinned tasks)           │
│  ┌───────────────────────────┐        ┌───────────────────────────┐  │
│  │ loop():                   │        │ loraTask (prio 2)         │  │
│  │  web_loop()  → HTTP API   │◄─TxQ───│  TX state machine (GONG/  │  │
│  │  sched_check() every 1s   │  Sem───►│  HEARTBEAT/STOP/SCHEDULE)│  │
│  │  lora_sendHeartbeat()     │        │  RX: обрабатывает ACK     │  │
│  │  every 30s                │        │  DIO0 IRQ → dioFlag       │  │
│  └───────────────────────────┘        └───────────────────────────┘  │
│                                                                        │
│  Core 1 (audioFeederTask, prio 24 — configMAX_PRIORITIES-1)           │
│  ┌───────────────────────────┐                                       │
│  │ mp3_loop() @ ~1kHz         │  → I2S → MAX98357A                   │
│  └───────────────────────────┘                                       │
│                                                                        │
│  web_setup(): WiFi STA/AP, mDNS, NTP, WebServer(80), auth              │
│  rtc_setup(): DS3231 (NTP / RTC / Manual — источник выбирает юзер)     │
│  schedule.cpp: до 32 записей, multi-day (/dayNN.conf, activeday.conf)  │
└────────────────────────────────────────────────────────────────────┘
                          │  LoRa 433MHz SF9/BW125/CR5, SX1278
                          │  HMAC-SHA256(8 байт) на GONG/HEARTBEAT/STOP
                          ▼
┌─────────────────────────── CLIENT (ESP32) ───────────────────────────┐
│  Core 1 (Arduino loop)                 Core 0 (loraTask, prio 2)      │
│  ┌───────────────────────────┐        ┌───────────────────────────┐  │
│  │ loop():                   │◄──Q────│ RX GONG/HEARTBEAT/STOP    │  │
│  │  lora_poll() — разбирает  │  rxQ   │  verifyMsg (HMAC+replay)  │  │
│  │  rxQueue → mp3_play/stop  │        │  sendAck() (Core0, TX)    │  │
│  └───────────────────────────┘        │  DIO0 IRQ → dioFlag       │  │
│                                        └───────────────────────────┘  │
│  Core 1 (audioFeederTask, prio 24)                                    │
│  ┌───────────────────────────┐                                       │
│  │ mp3_loop() @ ~1kHz → I2S   │                                       │
│  └───────────────────────────┘                                       │
└────────────────────────────────────────────────────────────────────┘
```

## 2. Прерывания (DIO0)

Оба устройства используют один и тот же паттерн:

```cpp
static volatile bool dioFlag = false;
static void IRAM_ATTR onDio0() { dioFlag = true; }
attachInterrupt(digitalPinToInterrupt(LORA_DIO0), onDio0, RISING);
```

- DIO0 у SX1278 сигнализирует **и** `RxDone`, **и** `TxDone` (в зависимости от режима, выставляемого RadioLib в `startReceive()`/`startTransmit()`).
- ISR не делает ничего, кроме установки флага — корректно (без SPI/heap внутри ISR).
- `loraTask` (Core 0) поллит `dioFlag` в цикле с `vTaskDelay`:
  - сервер: `vTaskDelay(1)` (и в TX-ветке, и в RX-ветке) — задержка отклика ~1мс.
  - клиент: `vTaskDelay(20)` **только когда флag==false и нет активности** (`client/src/lorahandler.cpp:132-135`) — комментарий поясняет, что это осознанно увеличено с 1мс до 20мс, чтобы не морить watchdog IDLE0.
- Порядок `attachInterrupt()` → `radio.startReceive()` соблюдён на обеих сторонах — гонки на старте нет.

## 3. Протокол сообщений

| Тип | Значение | HMAC подпись | Кто шлёт | Кто обрабатывает |
|---|---|---|---|---|
| `MSG_GONG` (0x01) | play track/vol/loop | да (`sig`+`ts`) | сервер | клиент → `mp3_play`, ACK назад |
| `MSG_HEARTBEAT` (0x02) | time+clients | да | сервер (каждые 30с) | клиент → ACK назад (не проигрывает) |
| `MSG_SCHEDULE` (0x03) | JSON расписания | **нет** | сервер (`/api/sync`) | клиент **только логирует**, не парсит и не применяет |
| `MSG_ACK` (0x04) | id+rssi | **нет** | клиент | сервер → `upsertClient()` (RSSI/RTT таблица) |
| `MSG_STOP` (0x05) | ts | да | сервер | клиент → `mp3_stop()`, **без ACK** |

Подпись: `HMAC-SHA256(key, msgType‖payload)`, обрезана до первых 8 байт (16 hex).
Ключ `LORA_HMAC_KEY = "REDACTED_HMAC_KEY"` (8 символов) — совпадает на сервере и клиенте, но короче рекомендованных в комментарии 16+ символов.

### Anti-replay (клиент, `verifyMsg`)

```
ts <= lastServerTs
  ├─ drop < 3600s  → REJECT как replay
  └─ drop >= 3600s → считаем, что сервер перезагрузился без RTC/NTP → resync (принять)
ts > lastServerTs → OK, lastServerTs = ts
```
`ts` — секунды (`nowTs()` = `time()` если валидно, иначе `millis()/1000`), **общий счётчик на все три подписанных типа** (GONG/HEARTBEAT/STOP используют один и тот же `lastServerTs`).

## 4. Синхронизация "гонга" сервер↔клиент

Задумано (комментарий в `server/src/main.cpp:20-21`):
> "Send LoRa FIRST (blocking TX ~170ms), then start local playback. Both server and client will begin audio after TX completes → in sync."

Фактический путь:
1. `onGongFire()` → `lora_sendGong()` кладёт `TxReq` в `txQueue`, блокируется на `xSemaphoreTake(txGongDone, 30000ms)`.
2. `loraTask` (Core0, сервер) реально передаёт пакет, по завершении TX (`dioFlag`) вызывает `finishTransmit()`, отдаёт семафор `txGongDone`.
3. Сервер: семафор получен → `mp3_setVolume`+`mp3_play()` **сразу**.
4. Клиент: `loraTask` получает RX, проверяет HMAC, **сначала `sendAck(rssi)`** (случайная задержка 10-70мс + время эфира ACK-пакета), и только **после** этого кладёт команду в `rxQueue` для Core1 → `mp3_play()`.

Итог: клиент стартует звук позже сервера на время своего ACK (десятки–сотни мс) — расходится с заявленной целью "оба начинают одновременно". См. таблицу §6, пункт 4.

## 5. Аудио (mp3handler, идентичен на сервере и клиенте)

- `mp3_play(track, loops)`: `loopRemain = loops-1`, запускает `_startPlay()`.
- `_startPlay()`: стоп текущего, `SPIFFS.exists()`, `audio.setVolume(0)` → `connecttoFS()` → `ramping=true`.
- `mp3_loop()` (таск на Core1, приоритет 24, ~1кГц):
  - пока `ramping` и `millis()-rampStart<40ms` — тихо; после 40мс — `applyVolume()` (анти-щелчок).
  - если `audio.isRunning()==false` 20 тиков подряд (~20мс) — считаем трек завершённым:
    - `loopRemain>0` → `_startPlay(loopTrack)` (повтор).
    - иначе → `audio.setVolume(0)` (тишина, ждём новую команду).
- Приоритет audio-таска (24) выше `loraTask` (2) и Arduino `loop()` (1) — гарантирует бесперебойную выдачу I2S, ценой того что вся остальная логика получает CPU только между `vTaskDelay(1)` тиками audio-таска.

## 6. Планировщик (schedule.cpp)

- `sched_check()` каждую секунду из `main.cpp loop()`.
- Требует `getLocalTime()` **и** `tm_year>=124` (реальная дата) — иначе просто "Skip" (защита от ложного срабатывания по времени с момента загрузки).
- Guard от повторного срабатывания в ту же минуту: `lastFiredKey==key && millis()-lastFiredMillis<65000`.
- Если несколько записей совпадают по `hour:min` — сработает **только первая** в массиве (`break`), остальные молча игнорируются в эту минуту.
- Полночь (00:00): автопереход на `/dayNN.conf` следующего дня, если файл существует (отдельный таймер `lastAutoAdvanceMs`, независимый от `lastFiredKey`).
- `sched_save()` дублирует запись в активный `/dayNN.conf`, чтобы правки не терялись при смене дня.

## 7. Время (NTP / RTC / Manual)

- Источник выбирается явно пользователем через `/api/time/source`, никакого автоприоритета нет (осознанное архитектурное решение, см. комментарий в `rtchandler.h`).
- `wifi_connect()` при успешном STA: `ntp.begin()+update()`, `configTime()`, затем `rtc_syncFromSystem()` — пишет NTP-время в DS3231.
- `handleTimeSet()` (ручной ввод): `settimeofday()` с датой-заглушкой 2024-01-01 (важны только HH:MM), `esp_sntp_stop()`, затем тоже пишет в RTC.

## 8. Веб-API (webhandler.cpp)

CRUD `/api/schedule` (GET/POST/PUT/DELETE), `/api/play`, `/api/play/lora`, `/api/play/all`, `/api/stop`, `/api/sync`, `/api/time`, `/api/time/source`, `/api/clients`, `/api/status`, `/api/wifi/*`, `/api/auth/*`, `/api/days`, `/api/day`, `/api/day/activate`, `/api/logs`. Basic Auth опционален (`authEnabled` в `/auth.conf`), проверяется в каждом хендлере через `checkAuth()`.

---

## 9. Самопроверка диаграммы

Перечитал каждый пункт выше против исходников ещё раз:

- Core pinning: `xTaskCreatePinnedToCore(loraTask, ..., 0)` на обеих сторонах (Core 0) — подтверждено (`server/src/lorahandler.cpp:251`, `client/src/lorahandler.cpp:234`). Audio task — `..., 1)` (Core 1) — подтверждено (`server/src/mp3handler.cpp:128`, `client/src/mp3handler.cpp:133`).
- Приоритет audio-таска `configMAX_PRIORITIES-1` — подтверждено в обоих `mp3handler.cpp`.
- HMAC подписываются только GONG/HEARTBEAT/STOP (`loraSend()` условие `type==MSG_GONG||MSG_HEARTBEAT||MSG_STOP`, `server/src/lorahandler.cpp:118`) — SCHEDULE и ACK вне этого списка, подтверждено — на клиенте `verifyMsg()` вызывается только для `type==MSG_GONG||MSG_HEARTBEAT||MSG_STOP` (`client/src/lorahandler.cpp:161-169`), SCHEDULE и ACK не проверяются — подтверждено.
- Клиент действительно не парсит SCHEDULE (`client/src/lorahandler.cpp:198-201` — только `Serial.printf` + `startReceive()`) — подтверждено, поиском `deserializeJson.*schedule` в клиенте ничего не найдено, `sched_*` API в клиентском коде вообще отсутствует.
- `sendAck()` вызывается до `xQueueSend(rxQueue,...)` для GONG (`client/src/lorahandler.cpp:192-193`) — подтверждено, порядок именно такой.
- `txGongDone` подтверждено как двоичный семафор, отдаваемый только в двух местах `loraTask` сервера (успешный/неуспешный старт TX) — при этом создание таска `loraTask` пропускается, если `radio.begin()` вернул ошибку (`return` на `server/src/lorahandler.cpp:244` до `xTaskCreatePinnedToCore`) — подтверждено, см. баг №1 в разделе 10.
- RTT считается только от `lastHeartbeatSentMs`, устанавливаемого исключительно в `lora_sendHeartbeat()` — в `upsertClient()` нет никакого способа отличить ACK-на-GONG от ACK-на-HEARTBEAT — подтверждено (баг №3).
- `ts <= lastServerTs` (строгое `<=`, не `<`) — подтверждено дословно (`client/src/lorahandler.cpp:62`) — баг №2.

Все пункты диаграммы соответствуют коду на момент чтения. Расхождений между записанной схемой и фактическим кодом не найдено — переходим к списку ошибок.

## 10. Найденные ошибки и задачи на исправление

См. итоговую таблицу в ответе в чате (раздел "Сравнение логики и ошибок"). Кратко, по приоритету:

1. **[Критично]** Сервер зависает на 30с (блокируется весь `loop()`, включая веб-сервер) в `lora_sendGong()`, если LoRa модуль не инициализировался ИЛИ `txQueue` был полон >200мс — семафор `txGongDone` в этом случае никогда не будет отдан, т.к. `loraTask` не создаётся / TxReq не поставлен в очередь.
2. **[Критично]** Anti-replay на клиенте отклоняет легитимные сообщения с тем же `ts` (секундная гранулярность) как replay (`<=` вместо `<` с доп. проверкой типа/содержимого) — может "проглотить" STOP или GONG, посланные в ту же секунду, что и предыдущее сообщение.
3. **[Высоко]** RTT/one-way расчёт на сервере ломается всякий раз, когда ACK приходит в ответ на GONG (не Heartbeat) — `rtt = millis()-lastHeartbeatSentMs` может дать выброс до 30с, отравляя экспоненциальное скользящее среднее, которое показывается в UI.
4. **[Средне]** Клиент стартует воспроизведение позже сервера на время отправки собственного ACK (10-70мс случайная задержка + время эфира) — нарушает заявленную синхронность "оба начинают сразу после TX".
5. **[Средне]** `MSG_ACK` и `MSG_SCHEDULE` не подписаны HMAC и не проверяются — можно подделать ACK с произвольным `id`, засоряя таблицу клиентов на сервере.
6. **[Средне]** `MSG_SCHEDULE` на клиенте ничего не делает (нет парсинга/применения) — функция синхронизации расписания на клиент фактически не реализована.
7. **[Низко]** `main.cpp` (сервер) продвигает `lastHeartbeat=now` даже если `lora_sendHeartbeat()` вышел раньше времени (очередь занята) — реальные heartbeat могут пропускаться дольше расчётного интервала.
8. **[Низко]** `lora_getAvgOneWayMs()` объявлена и реализована, но нигде не вызывается (мёртвый код).
9. **[Низко]** Баннер лога дублируется дважды подряд в `main.cpp setup()`.
10. **[Низко]** Закомментированный старый `checkAuth()`/`requestAuthentication` оставлен мёртвым кодом в `webhandler.cpp`.
11. **[Информационно]** `LORA_HMAC_KEY` короче рекомендованных в собственном комментарии 16+ символов (сейчас 8), плюс HMAC обрезан до 8 байт — ослабленная подпись для private LoRa сети (уже отмечено авторами как "сменить перед деплоем", но всё ещё дефолтное значение).
12. **[Информационно]** Если две записи расписания совпадают по времени `hour:min`, сработает только первая (`break`) — не баг, но неочевидное для пользователя ограничение.
