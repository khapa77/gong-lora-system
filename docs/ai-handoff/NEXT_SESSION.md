# Следующая сессия: аппаратное испытание шаблонных курсов (A–F)

Актуально на **2026-11-23**. HEAD / origin: `6d972f64ead591d936b16654a502f99b77596e68`,
ветка `standalone-max`, опубликована и синхронизирована.

Это единственный актуальный handoff. Историю по сессиям см. в
`docs/ai-handoff/SESSION_STATE_2026-11-22.md` (этапы 1–5, bootstrap, bounded-memory) и
`docs/ai-handoff/SESSION_STATE_2026-11-23.md` (этапы A–F, синхронизация кэша).

## Сначала прочитать

1. `AGENTS.md` — обязательные правила безопасности.
2. `docs/DYNAMIC_COURSE_TEMPLATE_PLAN.md` — архитектура хранения шаблонов, этапы A–F, §15.
3. `docs/DYNAMIC_COURSE_HARDWARE_TEST.md` — процедура испытаний на стенде.
4. `docs/ai-handoff/SESSION_STATE_2026-11-23.md` — детали решений, дефектов и измерений.

`docs/DYNAMIC_COURSE_SPEC.md` и `docs/DYNAMIC_COURSE_PLAN.md` описывают раннюю архитектуру
(до шаблонов); они не противоречат реализации, но в части хранения устарели — ориентир
шаблонного плана.

## Состояние кода

| Что | Значение |
|---|---|
| HEAD | `6d972f6` (epoch-проверка кэша шаблонов) |
| Ветка | `standalone-max`, синхронизирована с origin |
| Сборка | PlatformIO `esp32dev`: SUCCESS, RAM 21.0 %, Flash 87.6 % (1 377 857 Б) |
| Host-тесты | store 509, editor 739, scheduler 255, course 578, active_day 275, emergency 58 |
| ASan/UBSan | все наборы чисто (editor под sanitizer 725 из-за отключённого heap-бюджета) |
| Прошивка на ESP32 | **не обновлялась**: на устройстве прошивка до шаблонной архитектуры |

## Что готово (этапы A–F)

- **JSON v1 с `templates`/`days`** — штатный формат: уникальные шаблоны хранятся один раз,
  дни ссылаются на них (`"t3"`) либо несут собственные события. JSON v2 с полными списками
  событий импортируется по-прежнему и хранится развёрнуто (`layout=expanded`).
- **Этап A** (`6d48e95`): `templates.json` с CRC и tmp/bak, `CourseMeta::layout`, разрешение
  ссылок в `loadDay`, восстановление, валидация, удаление/клонирование шаблонных поколений.
- **Этап B** (`101912f`): компактная запись поколения (`writeCompactGeneration`,
  `applyCompactCourse`, `saveCompactDraft`), потоковые `putDayReference` /
  `finishCompactGeneration`; в тот момент намеренно не подключена к HTTP.
- **Этап C** (`7636996`): copy-on-write — правка ссылочного дня материализует только его,
  `templates.json` и соседние дни не меняются; правка активного дня без повторной активации.
- **Этап D** (`5b80d68`): компактный экспорт v1 (`templates` + ссылки), развёрнутый экспорт v2
  для старых поколений; экспорт не трогает кэш, календарь и LittleFS.
- **Этап E** (`b416318`): `importCourseAuto` подключён к `/api/course/import`, два слота
  кэша (active/draft), bounded-разбор событий, оптимизации памяти, выбор active-набора в
  boot/Apply/Activate/полночь/восстановлении.
- **Этап F** (`78feb67`): аудит, `docs/DYNAMIC_COURSE_HARDWARE_TEST.md`, фикс инвалидации
  кэша и пересчёт guard импорта.
- **Синхронизация** (`032dae4`, `6d972f6`): мьютекс кэша `CacheGuard` + ревизия поколений
  (`generationStamp`/`globalEpoch`), чтобы устаревший набор не публиковался.

## Ключевые инварианты (не ломать)

1. После Apply: `APPLIED`, `running=false`, `activation_date=0`, `day_index=-1`, звонков нет.
2. Activate — единственная точка привязки календаря; день выбирается валидным индексом.
3. Цикл: `day_index = (activated_day_index + elapsed_civil_days) % course_length`, 1–47 дней,
   переход последнего дня в 00 без специальных правил.
4. Кэш шаблонов строго помечен поколением; active-слот не вытесняется работой с черновиком.
5. Порядок блокировок: `sys_lock → cacheMutex`; обратного захвата нет, LittleFS под мьютексом
   не читается.
6. Материализация дня при правке — только этого дня (copy-on-write); шаблон неизменен.
7. Публикация атомарна: `templates.json` → дни → `meta.json` → NVS.
8. При любой ошибке (CRC, нет файла, нет шаблона, нет памяти) — SUSPENDED/EMERGENCY без
   неверных звонков и без частичной публикации.
9. Старые развёрнутые поколения и `layout=expanded` работают без изменений.

## Следующая задача: аппаратное испытание `6d972f6`

Полная процедура — `docs/DYNAMIC_COURSE_HARDWARE_TEST.md`. Кратко:

1. На MacBook: `git pull` (ветка `standalone-max`, HEAD `6d972f6`).
2. **Проверить `server/platformio.ini` — сейчас это блокер.** В коммите `24447ef`
   (`board`) строка `board = esp32dev` заменена на `board = esp32-s3-devkitc-1`
   (старая закомментирована). Устройство по отчётам — ESP32-D0WD-V3 (классический ESP32),
   а сборка `env:esp32dev` сейчас целится в S3. Прошивка, собранная как есть, на D0WD-V3
   не запустится. **Решение принимает оператор**: вернуть `board = esp32dev` (правка
   `platformio.ini` — изменение аппаратной конфигурации, без разрешения не делается) или
   подтвердить, что стенд на самом деле S3. До этого пункта аппаратные результаты
   недействительны.
3. `cd server && pio run -e esp32dev` — собрать.
4. Сохранить текущий статус устройства: `curl /api/course/status`, `curl /api/status`,
   экспорт действующего курса, лог Serial.
5. Прошить **только firmware**: `pio run -e esp32dev -t upload`. Никаких `erase_flash` и
   `uploadfs` — файловая система и NVS должны остаться.
6. Проверить совместимость старого поколения 3 (см. п. 4 hardware-документа).
7. Загрузить компактный JSON v1 с шаблонами, пройти Validate → Import → Apply → Activate →
   Reboot → полночь → Copy-on-Write → Export.
8. Зафиксировать heap/largest/stack из логов `[WEB] ...` и все строки `[COURSE]`.

## Текущее состояние устройства (из прошлой сессии)

- ESP32-D0WD-V3 без PSRAM, IP `192.168.1.197`.
- Успешно загружен операторский курс **Gong #2**: 14 дней, 136 календарных событий,
  опубликованное поколение **3**, активирован **день 07**, активация пережила перезагрузку.
- Прошивка на устройстве — **до** шаблонной архитектуры (этап 5 + bounded-memory,
  `dcb2603`). Новая архитектура A–F на железе **не проверялась**.
- `templates.json` на устройстве отсутствует — поколение 3 развёрнутое, должно читаться
  новым кодом как `layout=expanded`.

## Точные команды проверки (host)

```bash
cd /root/gong-lora-system
python3 -B tests/run_course_store_tests.py     --build-dir /tmp/t-store
python3 -B tests/run_course_editor_tests.py    --build-dir /tmp/t-editor
python3 -B tests/run_course_scheduler_tests.py --build-dir /tmp/t-sched
python3 -B tests/run_course_tests.py           --build-dir /tmp/t-course
python3 -B tests/run_active_day_state_tests.py --build-dir /tmp/t-active
python3 -B tests/run_emergency_tests.py        --build-dir /tmp/t-emerg
```

ASan/UBSan: те же раннеры с `--sanitize` (для `active_day_state` — `--sanitize undefined`,
`--sanitize address` не линкуется: дубль `operator new` с runtime clang 10 — ограничение
существовало до шаблонной работы).

PlatformIO: `cd server && pio run -e esp32dev`. `server/web/gz/index.html.gz` пересобирается
pre-скриптом и в git не входит.

## Запреты

- Не менять `config.h`, GPIO, MP3/I2S, RTC, LoRa, WiFi без отдельного разрешения.
- Не прошивать ESP32 автоматически и не выполнять `erase_flash`/`uploadfs`.
- Не стирать NVS/LittleFS, не удалять старые `dayNN.conf`/`gong.conf`.
- Не выполнять push/force push, reset/clean без разрешения.
- Локальные коммиты — только `git -c core.hooksPath=/dev/null commit` (pre-commit-hook делает
  `git add -A`, post-commit — `git push`).
- Не трогать файл `end` и не добавлять его в коммит.
- Не запускать субагентов и не начинать новые этапы разработки без разрешения.
- Не ослаблять тесты ради PASS.

## Открытые риски (для аппаратной проверки)

1. Реальные пики heap при импорте 27-КБ JSON (host: validate 8–10 КБ, import ≈60 КБ на
   устройстве) и largest free block.
2. Фрагментированная куча после длительной работы WiFi.
3. Обрыв питания в момент записи `templates.json` (tmp/bak проверены только на host-моке).
4. Конкурентный доступ к кэшу подтверждён статически и покрыт регрессиями, но не проверен в
   реальном времени (Activate в момент полуночи, импорт во время RUNNING).
5. Усечение экспорта при ошибке чтения в середине потока: клиент получает частичный JSON
   (повторная валидация его отвергнет, но явного маркера ошибки в потоке нет).
6. Конкурентный `handleCourseExport` (R_STREAM) с импортом: экспорт не берёт `sys_lock`.
