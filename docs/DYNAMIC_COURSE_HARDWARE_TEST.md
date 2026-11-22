# Аппаратные испытания JSON-курсов (этапы A–E)

Цель: проверить на ESP32-D0WD-V3 без PSRAM, что курс с уникальными шаблонами
(`templates` + `days`) корректно проверяется, импортируется, публикуется, активируется,
переживает перезагрузку и смену суток, экспортируется обратно и что при любом сбое нет
неверных звонков и частичной публикации.

Документ не заменяет host-тесты: он описывает, что и как проверить руками.

---

## 0. Подготовка

- Прошивка: ветка `standalone-max`, коммит `b416318` (этап E) или новее.
- Плата: ESP32-D0WD-V3 без PSRAM, IP в примерах `192.168.1.197`.
- Serial Monitor 115200 (PlatformIO: `pio device monitor -b 115200`).
- Файл курса: операторский JSON v1 с `templates` (14 дней, повторяющиеся дни ссылками).
- Инструменты на ПК: `curl`, `jq` (опционально).

Все запросы требуют авторизации, как и остальной API. Примеры ниже показывают только путь и
тело; добавьте свои заголовки авторизации.

Переменные для удобства:

```bash
DEV=192.168.1.197
COURSE=./gong-course-14d.json      # операторский файл
BIG=./gong-course-47d.json         # курс на 47 дней
```

---

## 1. Базовая точка: что показывает устройство

```bash
curl -s http://$DEV/api/course/status | jq .
```

Ожидаемые поля (путь `has_course=false` — нормальное состояние новой установки):

| Поле | Ожидание |
|---|---|
| `has_course` | `false` до первого Apply |
| `has_draft` | `false` |
| `active_generation` | `0` |
| `draft_generation` | `0` |
| `course_mode` | `false`, пока курс не опубликован |
| `running` | `false` |
| `day_index` | `-1` |
| `run_mode` | `waiting` или `legacy` (не `course`) |
| `scheduler_ready` | `false` в `waiting` |

```bash
curl -s http://$DEV/api/status | jq '.run_mode, .waiting_for_course, .free_heap'
```

`free_heap` запомните — это ваша базовая точка для сравнения.

Serial Monitor в этот момент не должен показывать ни одного `[COURSE] activated day`.

---

## 2. Тест 1 — импорт операторского 14-дневного курса

```bash
curl -s -X POST --data-binary @$COURSE -H 'Content-Type: application/json' \
     http://$DEV/api/course/validate | jq .
```

Ожидается:

```json
{ "ok": true, "version": 1, "name": "Gong #2", "days": 14, "events": <число вхождений> }
```

- `events` — число календарных вхождений, не уникальных шаблонов. Для 14 дней и 136
  вхождений ожидается `136`.
- Ошибка `not enough memory to validate this file (...)` означает нехватку памяти:
  запишите `free`/`largest` из сообщения — это FAIL по памяти, а не по формату.
- Serial: `[WEB] validate heap free A->B, largest C->D, stack hw E`.
  `B` не должен упасть ниже ~40 КБ, `D` — ниже ~30 КБ.

```bash
curl -s -X POST --data-binary @$COURSE -H 'Content-Type: application/json' \
     http://$DEV/api/course/import | jq .
```

Ожидается `{"ok":true}`; Serial: `[WEB] import heap ...` и запись о черновике.
После импорта (до Apply):

```bash
curl -s http://$DEV/api/course/status | jq '{has_course, has_draft, draft_generation, running, day_index}'
```

Ожидается `has_course=false` (или `true`, если курс уже был опубликован ранее),
`has_draft=true`, `running=false`, `day_index=-1`.

---

## 3. Тест 2 — Apply без автоматической активации

```bash
curl -s -X POST http://$DEV/api/course/apply | jq .
curl -s http://$DEV/api/course/status | jq '{has_course, active_generation, length, flags, running, activation_date, day_index, run_mode, scheduler_ready}'
```

Критерии PASS:

- `has_course=true`, `running=false`, `activation_date=0`, `day_index=-1`, `flags` = APPLIED (младший бит).
- `run_mode="course"`, `scheduler_ready=false` (курс опубликован, но не запущен).
- Serial: `[COURSE] draft applied: generation N, 14 days`.
- За 2–3 минуты **ни одного** звонка, даже если время совпадает с событием курса.

Зафиксируйте время события из курса (например, 04:00) и проверьте тишину в этот момент —
это ключевой критерий «нет автозапуска после Apply».

---

## 4. Тест 3 — Activate day 07

```bash
curl -s -X POST "http://$DEV/api/course/activate?i=7" | jq .
curl -s http://$DEV/api/course/status | jq '{running, day_index, flags, activation_date, scheduler_ready}'
```

Проверить (вручную, не в тесте): `running=true`, `day_index=7`, `activation_date=YYYYMMDD`,
`flags` содержит APPLIED|RUNNING, `scheduler_ready=true`.

Serial: `[COURSE] activated day 7`.

```bash
# Список событий активного дня
curl -s "http://$DEV/api/days" | jq '.day_count'
curl -s "http://$DEV/api/day?n=7" | jq 'length'
```

Число событий должно совпадать с содержимым шаблона, на который ссылается день 07.

---

## 5. Тест 4 — перезагрузка и восстановление дня 07

Перезагрузите плату (кнопка EN/RST, не стирая flash). После старта:

```bash
curl -s http://$DEV/api/course/status | jq '{running, day_index, active_generation, run_mode, failure_count}'
```

PASS: `running=true`, `day_index=7`, `active_generation` тот же, `failure_count=0`.
Serial при старте: `[COURSE] ...` без `suspended`.

---

## 6. Тест 5 — просмотр черновика во время RUNNING

1. Импортируйте **другой** курс (или тот же с изменённым именем) без Apply:

```bash
curl -s -X POST --data-binary @$BIG -H 'Content-Type: application/json' \
     http://$DEV/api/course/import | jq .
```

2. Посмотрите дни черновика:

```bash
curl -s http://$DEV/api/course/status | jq '{has_course, draft_generation, draft_dirty}'
curl -s "http://$DEV/api/course/draft?i=0" | jq 'length'
```

3. Убедитесь, что активный курс продолжает работать: дождитесь события и услышьте звонок,
   либо проверьте `curl -s http://$DEV/api/status | jq '.scheduler_ready'` → `true`.

PASS: активный курс не прервался, `day_index` не изменился, звонок прозвучал.
FAIL: звонок пропал или прозвучало событие из черновика.

Serial при просмотре не должен содержать `templates unavailable`.

---

## 7. Тест 6 — изменение одного дня без изменения остальных

```bash
# добавить событие в день 03 (не активный): тело запроса - JSON события
curl -s -X POST "http://$DEV/api/day/entry?day=3" \
     -H 'Content-Type: application/json' \
     -d '{"hour":21,"min":30,"track":1,"loop":1,"vol":10,"en":true,"desc":"test"}' | jq .
curl -s "http://$DEV/api/course/draft?i=3" | jq 'length'    # должно увеличиться на 1
curl -s "http://$DEV/api/course/draft?i=4" | jq 'length'    # не должно измениться
```

Если день 03 использует тот же шаблон, что и 04, размер дня 04 обязан остаться прежним.

Активный день:

```bash
curl -s "http://$DEV/api/day?n=7" | jq 'length'
curl -s -X POST "http://$DEV/api/day/entry?day=7" \
     -H 'Content-Type: application/json' \
     -d '{"hour":22,"min":0,"track":1,"loop":1,"vol":10,"en":true,"desc":"live"}' | jq .
curl -s "http://$DEV/api/day?n=7" | jq 'length'   # +1, без повторного Activate
```

PASS: изменение активного дня применилось без Activate.

---

## 8. Тест 7 — Export → Import → Apply

```bash
curl -s "http://$DEV/api/course/export" -o exported.json
jq '.format, .version, (.templates|keys), (.days|length)' exported.json
```

Для компактного поколения ожидается `"gong-course"`, `version=1`, дни — строки-ссылки
(`"t1"`) и/или массивы событий для индивидуально изменённых дней; размер файла заметно
меньше развёрнутого.

```bash
curl -s -X POST --data-binary @exported.json http://$DEV/api/course/validate | jq .
curl -s -X POST --data-binary @exported.json http://$DEV/api/course/import | jq .
curl -s -X POST http://$DEV/api/course/apply | jq .
curl -s -X POST "http://$DEV/api/course/activate?i=0" | jq .
```

PASS: `validate.ok=true`, число дней и событий совпадает с исходным, после Activate события
те же (сравните `api/day?n=N` для нескольких дней).

---

## 9. Тест 8 — переход 13→00

Курс на 14 дней, активирован день 13 (или активируйте его вручную, если сейчас другой):

```bash
curl -s -X POST "http://$DEV/api/course/activate?i=13" | jq .
```

Не переводите часы: дождитесь реальной полуночи (или, если RTC/время управляемо, установите
23:59 и подождите). После смены суток:

```bash
curl -s http://$DEV/api/course/status | jq '{day_index, running, failure_count}'
```

PASS: `day_index=0`, `running=true`, `failure_count=0`, звонки идут по дню 00.
Serial: без `cannot load next day` и без `suspended`.

---

## 10. Тест 9 — курс на 47 дней

```bash
curl -s -X POST --data-binary @$BIG http://$DEV/api/course/validate | jq '{ok, days, events}'
curl -s -X POST --data-binary @$BIG http://$DEV/api/course/import | jq .
curl -s -X POST http://$DEV/api/course/apply | jq .
curl -s -X POST "http://$DEV/api/course/activate?i=46" | jq .
curl -s -X POST "http://$DEV/api/course/activate?i=99" | jq .    # ожидается ошибка
```

PASS:

- `days=47`, импорт и Apply успешны;
- активация дня 46 работает, индекс 99 отвергнут (`invalid day`);
- после перезагрузки день 46 сохраняется;
- память: `[WEB] validate heap ... largest ...` не падает ниже ~30 КБ.

---

## 11. Тест 10 — контролируемая нехватка памяти

Самый простой способ — занять heap запросами и попробовать большой файл:

1. Откройте несколько вкладок GUI (или параллельные `curl` к `/api/status` в цикле), чтобы
   фрагментировать кучу.
2. Загрузите файл, близкий к лимиту 48 КБ:

```bash
curl -s -X POST --data-binary @huge.json http://$DEV/api/course/validate | jq .
```

PASS (любой из двух исходов, но обязательно без перезагрузки):

- `{"ok":true, ...}` — памяти хватило;
- `{"ok":false,"error":"not enough memory to validate this file (...)"}` — контролируемый
  отказ, устройство отвечает дальше, курс не изменился.

FAIL: перезагрузка, `abort`, `Guru Meditation`, зависание веб-сервера, или изменение
опубликованного курса после отказа.

Проверьте сразу после отказа:

```bash
curl -s http://$DEV/api/course/status | jq '{active_generation, running, day_index}'
```

Значения должны совпадать с состоянием до попытки.

---

## 12. Как измерять память

**Свободная куча и крупнейший блок** — в каждом запросе Validate/Import/Apply:

```
[WEB] validate heap free A->B, largest C->D, stack hw E
[WEB] import   heap free A->B, largest C->D, stack hw E
[WEB] apply    heap free A->B, largest C->D, stack hw E
```

- `A`/`B` — свободная куча до/после; `C`/`D` — largest free block до/после.
- `E` — `uxTaskGetStackHighWaterMark(loopTask)` в словах: чем больше, тем лучше.
  Меньше ~1200 слов — повод уменьшить размер курса/проверить стек.

Снимок в любой момент:

```bash
curl -s http://$DEV/api/status | jq '{free_heap, min_free_heap}'
```

Ориентиры для ESP32 без PSRAM (по результатам предыдущих замеров на этом устройстве):
свободно ~125 КБ при простое, largest ~65–73 КБ. Import 27-КБ JSON на host требует ~60 КБ;
если на устройстве `largest` падает ниже ~30 КБ во время импорта — считайте это риском.

Не используйте процент статической RAM из PlatformIO как показатель: он не отражает
динамическую кучу, фрагментацию и реальные пики.

---

## 13. Сообщения Serial, которые нужно фиксировать

| Сообщение | Значение |
|---|---|
| `[WEB] validate heap ...` | пик при проверке файла |
| `[WEB] import heap ...` | пик при импорте |
| `[WEB] apply heap ...` | пик при публикации |
| `[COURSE] draft applied: generation N, M days` | Apply прошёл |
| `[COURSE] activated day N` | ручная активация |
| `[COURSE] suspended: <причина>` | безопасная остановка |
| `[COURSE] templates unavailable: <ошибка>` | не удалось загрузить набор шаблонов |
| `[COURSE] load day N failed: <ошибка>` | день не читается |
| `[COURSE] save day N failed: <ошибка>` | день не записался |
| `[WEB] course export failed: ...` | ошибка экспорта |

Любой `suspended` в тестах 1–9 — FAIL, кроме теста 10 (там допустим контролируемый отказ
без смены состояния).

---

## 14. Безопасное восстановление после сбоя

Не стирайте flash и NVS: курс можно вернуть, не трогая файловую систему.

1. Если устройство в SUSPENDED:

```bash
curl -s http://$DEV/api/course/status | jq '{flags, failure_count, emergency}'
curl -s -X POST http://$DEV/api/course/emergency/retry | jq .
```

2. Если курс повреждён (например, после обрыва питания при записи):

```bash
# Проверить, читается ли опубликованный курс
curl -s http://$DEV/api/course/export -o check.json && jq '.days|length' check.json
```

- Экспорт читается → повторный Import того же файла + Apply + Activate.
- Экспорт не читается → устройство должно быть в SUSPENDED без звонков; восстановление:
  Import исправного JSON → Apply → Activate. Старое поколение будет заменено штатно.

3. Если после Apply нужен откат к прежнему расписанию — используйте ранее сохранённый
   экспорт (`exported.json`), это единственный поддерживаемый путь. Автоматической
   миграции и «отката на предыдущее поколение» нет.

4. Полное стирание (`esptool erase_flash`) — **только** как последняя мера и только с
   согласия оператора: это удалит и NVS, и LittleFS.

---

## 15. Чек-лист PASS/FAIL

| № | Проверка | PASS |
|---|---|---|
| 1 | Validate 14-дневного курса | `ok=true`, дни/события верны, нет abort |
| 2 | Apply без активации | APPLIED, `day_index=-1`, тишина |
| 3 | Activate day 07 | `running=true`, `day_index=7`, звонки по дню 07 |
| 4 | Перезагрузка | день 07 восстановлен, `failure_count=0` |
| 5 | Черновик во время RUNNING | активный курс звенит, черновик не влияет |
| 6 | Правка одного дня | остальные дни не меняются; активный день — без Activate |
| 7 | Export → Import → Apply | события совпадают, `validate.ok=true` |
| 8 | Переход 13→00 | `day_index=0`, без suspend |
| 9 | 47 дней | импорт/активация 46/отказ 99, reboot сохраняет день |
| 10 | Нехватка памяти | контролируемая ошибка, состояние не меняется |

Отдельно фиксируйте в протоколе: `free_heap`/`largest`/`stack hw` для каждого из
Validate/Import/Apply и все строки `[COURSE]`.

---

## 16. Что этот документ не покрывает

- Реальную деградацию flash и поведение при тысячах циклов записи.
- Работу звукового тракта (MP3/I2S) — проверяется отдельным документом.
- Точность RTC (DS3231) и переходы DST.
- Многопользовательскую нагрузку (несколько одновременных веб-сессий с записью).
