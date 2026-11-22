# Следующая сессия: bounded-memory JSON import и режим ожидания курса

Актуально на 2026-11-22 (третья сессия дня). Предыдущая архитектура в `docs/ai-handoff/SESSION_STATE.md` устарела.

## Сначала прочитать

1. `AGENTS.md` — обязательные правила безопасности.
2. `docs/DYNAMIC_COURSE_SPEC.md` — окончательная спецификация.
3. `docs/DYNAMIC_COURSE_PLAN.md` — поэтапный план.
4. `docs/ai-handoff/SESSION_STATE_2026-11-22.md` — состояние, Git, решения, дефекты.

## Где мы остановились

- HEAD: `af1bd82` (JSON-driven GUI), ветка `standalone-max`, запушена в origin.
- Текущая сессия: исправлен kernel panic при Validate/Import большого JSON (bounded-память),
  устранён скрытый запуск legacy-расписания на чистой установке, Activate защищён.
  Локальный коммит без push.
- Этап 6 не начат. `end` не тронут, в индекс не добавлялся.

## Что исправлено в этой сессии

### Причина

`courseTryBoot()` не публикует эталонный курс, если на устройстве есть legacy-файлы
(`gong.conf`, `dayNN.conf`) — это защита существующих установок. В результате после чистой
установки прошивки с legacy-расписанием `course_state` отсутствует, `course_mode=false`,
legacy-планировщик активирует Day 00, а Course Editor отвечал `no course has been published`,
потому что `course_editor::loadCalendar()` требовал существующий `course_state`.

### Решение

1. **Bootstrap-черновик без календаря.** Поколение-черновик до первого Apply хранится
   в отдельном NVS-ключе `crs_boot` (`CourseStore::loadBootstrapGeneration()` /
   `saveBootstrapGeneration()`). `course_state` не создаётся, ничего не исполняется.
2. **Bootstrap-aware редактор.** `course_editor` умеет работать без календаря:
   пресеты 1..47 (GUI: 3/10/14/20/30/45) и импорт создают bootstrap-поколение;
   чтение/правка дня, вставка, удаление, очистка, переименование и экспорт работают от него;
   `info()` сообщает `hasCourse=false` и параметры черновика.
3. **Первый Apply — атомарная публикация.** `applyDraft()` при отсутствии календаря проверяет
   поколение и записывает один `course_state` blob: `active=draft=N`, `flags=APPLIED`,
   `activation_date=0`, `day_index=0`. Указатель bootstrap очищается. До ручного Activate
   звонков нет.
4. **Первенство курса на загрузке.** `sched_setup()` теперь вызывает `courseTryBoot()`
   при наличии `course_state` **или** пустом legacy-состоянии. Раньше курс читался только при
   пустом legacy; из-за этого после публикации курса перезагрузка возвращала legacy-расписание.
   Для APPLIED-курса legacy-RAM (entries/activeDay/activeDate) очищается. Устройства без
   `course_state` сохраняют legacy-поведение без изменений.
5. **Защита от случайной legacy-активации:** `POST /api/course/activate` до первого Apply
   отвечает `apply the draft first` (иначе кнопка редактора активировала legacy-день).
6. **Автосидирование не затирает черновик:** `ensureInitialCourse()` отказывает, если
   существует bootstrap-указатель.
7. **GUI:** карточка Course Editor показывает «черновик не опубликован» с длиной и поколением,
   подсказывает Apply, а Activate до публикации не отправляет запрос.

### Инцидент KEY_TOO_LONG (реальная ESP32)

Ключ `course_bootstrap` содержит 16 символов, а NVS допускает 15 (`NVS_KEY_NAME_MAX_SIZE` = 16
с завершающим нулём). Первое создание курса на железе падало с
`nvs_set_u32 fail: course_bootstrap KEY_TOO_LONG`. Ключ заменён на `crs_boot` (8).
Гостевой мок `tests/host/nvs.h` теперь сам отвергает имена длиннее 15
(`ESP_ERR_NVS_KEY_TOO_LONG`, `ESP_ERR_NVS_INVALID_NAME` для namespace), поэтому такой дефект
не может снова пройти host-тесты. Проверены все NVS-ключи и namespace проекта — все ≤ 15.

### Упрощение GUI: JSON-driven Course (четвёртая сессия)

- В Web GUI осталась одна карточка **Courses**: загрузка JSON v1/v2 -> проверка через
  `POST /api/course/validate` (без побочных эффектов) -> подтверждённый `Apply`
  (`POST /api/course/import` + `POST /api/course/apply`) -> выбор дня и `Activate this day`;
  плюс экспорт действующего курса (`GET /api/course/export?source=active`).
- Удалены карточка Course Editor (пресеты, вставка/удаление/копирование, очистка, черновики,
  переименование) и legacy-карточка Course (пошаговый импорт через `/api/course/day` +
  `/api/course/commit`), которая в режиме курса отвечала `structural import is not available in
  course mode`. Кнопка «Activate this day» из Schedule убрана — активация только в Courses.
- Backend не переписывался: Course Model/Store/Scheduler и старые маршруты остались рабочими
  (их используют host-тесты и совместимость). Новое: `course::formatVersion()`,
  `course_editor::inspectCourse()`, `beginExport(kExportActive|kExportDraft)`,
  `sched_courseInspect()`, `sched_courseExportBegin(activeOnly)`.
- Длина курса = число дней в JSON (1..47), после последнего дня снова 00; отдельных правил
  для 09->00/13->00/19->00 нет.

### Bounded-memory JSON и режим ожидания (пятая сессия)

**Причина panic.** `handleCourseValidate` -> `sched_courseInspect` -> `inspectCourse` передавал
`String` как `const std::string&` (полная копия ~50 КБ) и вызывал `course::fromJson`, который строит
дерево всего документа: ~300 байт на узел, для операторского файла это сотни КБ. На ESP32 без PSRAM
`operator new` бросал `std::bad_alloc`, обработчика не было -> `std::terminate` -> `abort`.
Host-тесты этого не видели: обычная куча хоста не ограничена.

**Исправление.**
- Новый сканер `Scanner` в `course_editor.cpp`: один проход по буферу, запоминает только байтовые
  диапазоны дней и шаблонов (до 47 дней, 64 шаблона, имя <= 128 Б, имя шаблона <= 32 Б, глубина <= 32,
  день <= 8 КБ). Это замена `fromJson` для всего документа.
- Каждый день разбирается отдельным маленьким документом через существующий `course::fromJson`
  (шаблон подставляется в обёртку), затем `resolvedEvents`; пик памяти — один день.
  Шаблоны, на которые никто не ссылается, тоже проверяются (по одному).
- `course_editor::inspectCourse/importCourse` получили форму `(const char*, size_t)`, обёртки
  `std::string` только делегируют — копии всего документа больше нет. `importCourse` пишет дни
  по одному в новое поколение, старый курс не трогается.
- `inspectCourse`/`importCourse` ловят `std::bad_alloc` и возвращают обычную ошибку.
- `webhandler`: `courseMemoryOk()` (нужно `2*body + 80 КБ` свободной кучи и непрерывный блок
  `body + 8 КБ`), `try/catch` вокруг validate/import/apply, лог свободной кучи, largest block и
  stack high-water mark до/после; в ответ validate добавлены `heap_free`/`heap_largest`.
- Измерено на хосте: старый путь (дерево всего 43 КБ файла) не укладывается в 96 КБ и бросает
  `bad_alloc`; новый путь на том же файле — пик 84 КБ при худшем дне из 32 событий, типичный день
  5–10 событий заметно меньше.

**Чистая установка / legacy.** `sched_setup()` больше не активирует Day 00 сам: если курса нет и
legacy-привязки в NVS нет, устройство переходит в режим `waiting` (ничего не звонит) и ждёт JSON.
Устройства с существующей legacy-привязкой (`active`/`aday`) работают как раньше. Фактический режим
виден в API: `/api/status` и `/api/course/status` отдают `run_mode` (`course`/`legacy`/`waiting`),
`waiting_for_course`, `course_active`.

**GUI.** Кнопка Activate изначально `disabled`, включается только при опубликованном курсе и валидном
индексе; сервер повторно проверяет курс и диапазон (`apply the draft first` / `invalid day`).
В статусе показывается `run_mode`, в алертах — сообщение о ожидании JSON или legacy-режиме.

## Проверено фактическим запуском (эта сессия)

- `run_course_editor_tests.py` — 495 checks, 0 FAIL; с ASan/UBSan — 490, 0 FAIL (бюджет heap не измеряется под sanitizer).
- `run_active_day_state_tests.py` — 275 PASS, 0 FAIL; с UBSan — 275 PASS, 0 FAIL.
- `run_course_tests.py` — 578 PASS, 0 FAIL.
- `run_course_store_tests.py` — 130 PASS, 0 FAIL.
- `run_course_scheduler_tests.py` — 130 checks, 0 FAIL (ASan/UBSan 130, 0 FAIL).
- `run_emergency_tests.py` — 58 checks, 0 FAIL (ASan/UBSan 58, 0 FAIL).
- `pio run -e esp32dev` — SUCCESS, RAM 20.9%, Flash 86.1% (1354221 B). Прошивка не заливалась.
- `node --check` для извлечённого `<script>` из `server/web/index.html` — JS_OK.
- Мутационная проверка: при возврате старого условия в `sched_setup()` падает
  `T03/bootstrap_course_takes_over_and_survives_reboot` — тест первенства курса не пустой.

## Новые тесты

`tests/test_course_editor.cpp`:
- bootstrap без курса: пустое состояние, отказы до появления черновика, пресет 14,
  правка/вставка/удаление/переименование в bootstrap-черновике;
- bootstrap apply: атомарная публикация, APPLIED, очистка указателя, перезагрузка, повторный Apply;
- bootstrap import (v1) без курса и последующий Apply;
- bootstrap presets: 3, 10, 14, 20, 30, 45 — создание и публикация;
- bootstrap failures: отказ NVS при Apply (черновик цел, повтор проходит), отказ записи meta,
  повреждённый meta, «висячий» указатель на отсутствующее поколение;
- bootstrap discard и запрет автосидирования при наличии черновика;
- опубликованный курс не использует bootstrap-ключ.

`tests/active_day_review_cases.h`:
- `T03/bootstrap_course_takes_over_and_survives_reboot` — публикация на legacy-устройстве,
  отсутствие звонков до Activate, перезагрузка, Activate, звонок 19:45, legacy-файлы целы;
- `T03/unpublished_draft_keeps_legacy_running` — черновик не меняет legacy-поведение,
  переживает перезагрузку.

`tests/test_active_day_state.cpp`: `resetRam()` теперь сбрасывает и RAM-состояние курса
(корректная симуляция перезагрузки).

## Что осталось и требует решения заказчика

1. **`GET/PUT /api/course/template?name=...` (спец. 11) не реализованы.** Шаблоны живут только
   в импортируемом JSON; в поколении хранятся материализованные дни. Нужна переработка
   хранилища (этап 2) — без отдельного разрешения не делалась.
2. **Legacy NVS-ключи не очищаются.** После публикации курса они просто игнорируются
   (первенство курса). Старые `dayNN.conf`/`gong.conf` не удаляются — так требует спецификация.
3. **`--sanitize address` для `active_day_state` не линкуется** (дубль `operator new`, clang 10).
   Ограничение существовало до сессии; UBSan проходит.
4. **Реальный Web GUI и железо не проверялись** (нет стенда): LittleFS/NVS при обрыве,
   DS3231, I2S, LoRa, пик heap — NOT TESTED.
5. `docs/DYNAMIC_COURSE_SPEC.md` не синхронизирован с реализацией по `last_fire`, маршрутам
   и bootstrap-черновику.

## Точные команды проверки

```bash
cd /root/gong-lora-system
python3 -B tests/run_course_editor_tests.py --build-dir /tmp/gong-course-editor-tests
python3 -B tests/run_course_editor_tests.py --build-dir /tmp/gong-course-editor-tests-asan --sanitize
python3 -B tests/run_course_tests.py --build-dir /tmp/gong-course-tests
python3 -B tests/run_course_store_tests.py --build-dir /tmp/gong-course-store-tests
python3 -B tests/run_course_scheduler_tests.py --build-dir /tmp/gong-course-scheduler-tests
python3 -B tests/run_emergency_tests.py --build-dir /tmp/gong-emergency-tests
python3 -B tests/run_active_day_state_tests.py --build-dir /tmp/gong-active-day-tests
```

ASan/UBSan: те же раннеры с `--sanitize` (для `active_day_state` — только `--sanitize undefined`).

PlatformIO: `cd server && pio run -e esp32dev`. `server/web/gz/index.html.gz` пересобирается
pre-скриптом `scripts/gzip_web.py`.

## Запреты

- Не менять `config.h`, GPIO, audio, LoRa, WiFi.
- Не прошивать ESP32.
- Не стирать NVS/LittleFS.
- Не удалять старые `dayNN.conf` и `gong.conf`.
- Не делать push/reset/clean без разрешения. Локальные коммиты — только `-c core.hooksPath=/dev/null`.
- Не запускать субагентов без разрешения.
- Не трогать `end` и не добавлять его в коммит.

## 7. Этап A шаблонов выполнен (2026-11-23)

- План: `docs/DYNAMIC_COURSE_TEMPLATE_PLAN.md` (§8 — что сделано).
- `course_store`: `templates.json` (CRC+tmp/bak), `CourseMeta::layout` ("tpl1"),
  `prepareTemplates/templatesReady/invalidateTemplates`, разрешение ссылок в `loadDay`,
  восстановление и валидация, удаление и клонирование шаблонных поколений.
- `course_model`: аддитивные `toJsonTemplates`, `fromJsonUnresolved`; `fromJson`/`validate`
  без изменений.
- Импорт/публикация/экспорт/планировщик не менялись. Этапы B–F не начаты.
- Тесты: store 220/220; course 578; editor 495; scheduler 130; active_day 275; emergency 58;
  ASan/UBSan чисто; PlatformIO RAM 21.0 %, Flash 86.8 % (1 364 757 Б).
- Память: prepareTemplates — пик 39 КБ / резидентно 4,8 КБ (4×12 событий);
  пик 101 КБ / 12,1 КБ (4×32 события). При этапе E нужен guard по largest free block
  и вызов prepare в точках смены поколения до публикации дня.

## 8. Этап B (компактная запись) выполнен

- `course_store`: `writeCompactGeneration`, `applyCompactCourse`, `saveCompactDraft`,
  `putDayReference`, `finishCompactGeneration`. Только используемые шаблоны; лимиты по
  уникальным событиям.
- `course_editor::importCourseCompact` — не подключён к HTTP: до этапа E штатный импорт
  и Apply продолжают писать развёрнутые поколения.
- Измерено: 14 дней / 40 уникальных событий / 140 вхождений — 15 130 Б → 5 779 Б (0,38×),
  пик импорта 42 КБ, резидентных данных нет.
- Тесты: store 368/368, editor 546, course 578, scheduler 130, active_day 275,
  emergency 58; ASan/UBSan чисто; PlatformIO RAM 21.0 %, Flash 86.8 % (1 364 957 Б).

## 9. Этап C (copy-on-write) выполнен

- Правка ссылочного дня уже была копией (`loadDay` разрешает шаблон), добавлена загрузка
  набора шаблонов в точках редактирования: `courseEnsureTemplates()` в `schedule.cpp`
  (add/edit/delete/day-json), `editingGeneration()`/`ensureEditableGeneration()` в
  `course_editor.cpp`.
- Boot/Apply/Activate/полночь/восстановление не тронуты (этап E).
- Тесты: store 458, editor 567, scheduler 154, course 578, active_day 275, emergency 58;
  ASan/UBSan чисто; PlatformIO RAM 21.0 %, Flash 86.8 % (1 365 185 Б).
- Память COW: store 15 КБ, editor saveDay (с клоном черновика) 41 КБ.
