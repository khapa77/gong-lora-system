# Следующая сессия: JSON-driven Course GUI (этап 5 закрыт)

Актуально на 2026-11-22 (третья сессия дня). Предыдущая архитектура в `docs/ai-handoff/SESSION_STATE.md` устарела.

## Сначала прочитать

1. `AGENTS.md` — обязательные правила безопасности.
2. `docs/DYNAMIC_COURSE_SPEC.md` — окончательная спецификация.
3. `docs/DYNAMIC_COURSE_PLAN.md` — поэтапный план.
4. `docs/ai-handoff/SESSION_STATE_2026-11-22.md` — состояние, Git, решения, дефекты.

## Где мы остановились

- HEAD: `4490398` (NVS-ключ bootstrap), ветка `standalone-max`, запушена в origin.
- Текущая сессия: интерфейс упрощён до одной карточки Courses (JSON — единственный источник
  структуры курса); локальный коммит без push.
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

## Проверено фактическим запуском (эта сессия)

- `run_course_editor_tests.py` — 456 checks, 0 FAIL; с ASan/UBSan — 456, 0 FAIL.
- `run_active_day_state_tests.py` — 272 PASS, 0 FAIL; с UBSan — 272 PASS, 0 FAIL.
- `run_course_tests.py` — 578 PASS, 0 FAIL.
- `run_course_store_tests.py` — 130 PASS, 0 FAIL.
- `run_course_scheduler_tests.py` — 117 checks, 0 FAIL (ASan/UBSan 117, 0 FAIL).
- `run_emergency_tests.py` — 58 checks, 0 FAIL (ASan/UBSan 58, 0 FAIL).
- `pio run -e esp32dev` — SUCCESS, RAM 20.9%, Flash 85.2% (1340773 B). Прошивка не заливалась.
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
