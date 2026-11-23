# Состояние сессии 2026-11-23: шаблонные курсы (этапы A–F)

Проект: `/root/gong-lora-system`, ветка `standalone-max`.
HEAD и origin: `6d972f64ead591d936b16654a502f99b77596e68` (опубликован, fast-forward).
Предыдущее состояние: `SESSION_STATE_2026-11-22.md`.

## 1. Что сделано

За сессию реализованы шесть этапов перехода от «каждый день хранит полный список событий»
к «уникальные шаблоны хранятся один раз, дни ссылаются на них».

| Этап | Коммит | Содержание |
|---|---|---|
| A | `6d48e95` | `templates.json` (CRC, tmp/bak), `CourseMeta::layout`, кэш, разрешение ссылок в `loadDay`, восстановление, валидация, удаление/клонирование |
| B | `101912f` | компактная запись поколения (пока не подключена к HTTP) |
| C | `7636996` | copy-on-write для дней, ссылающихся на шаблон |
| D | `5b80d68` | компактный экспорт v1, прежний v2 для старых поколений |
| E | `b416318` | интеграция в HTTP, два слота кэша, bounded-разбор, оптимизации памяти |
| F | `78feb67` | аудит, hardware-документ, фикс инвалидации кэша, пересчёт guard |
| синх. | `032dae4` | мьютекс кэша `CacheGuard` (loopTask ↔ controlTask) |
| синх. | `6d972f6` | ревизия поколений: устаревший набор не публикуется |

Базовые коммиты до этапов: `4eba3e1` (Course Editor), `328fa40`, `4490398` (bootstrap и
NVS-ключ), `af1bd82` (JSON-driven GUI), `dcb2603` (bounded-memory import, режимы
course/legacy/waiting).

## 2. Форматы

### JSON курса

- **v1 с шаблонами** (штатный): `{"format":"gong-course","version":1,"name":...,
  "templates":{"name":[event...]},"days":["name",["event",...]]}` — день-строка это ссылка на
  шаблон, день-массив — собственные события.
- **v2** (совместимость): `days` — объекты `{"template":"..."}` или `{"events":[...]}`;
  полные списки событий хранятся развёрнуто.
- Событие: `id, hour, min, track, loop, vol, en, desc` (+ те же правила валидации модели).
- Длина курса = число дней, 1–47; цикл `(activated_day_index + elapsed_civil_days) % length`.

### поколение на LittleFS

```
/course/g<N>/meta.json        {"format":"gong-course-store","version":1,"generation":N,
                              "course_length":L,"name":"...","layout":"tpl1"?}
/course/g<N>/templates.json   {"format":"gong-course-templates","version":1,
                              "templates":{"name":[event...]}}      ← только tpl1
/course/g<N>/dNN.json         "days":[{"template":"name"}]  либо
                              "days":[{"events":[...]}]            ← всегда допустимы оба
```

Все файлы пишутся как `CRC32 xxxxxxxx\n<json>` через tmp → bak → rename; `meta.json` —
последним. `layout` отсутствует у всех поколений, опубликованных до этапа A (они
развёрнутые), и равен `"tpl1"` у шаблонных. Неизвестный `layout` отвергается.

### NVS

`course_state` (calendar blob: active/draft generation, activation_date, day_index,
course_length, flags, failure_count), `last_fire`, `crs_boot` (bootstrap-черновик до первого
Apply). Ключи ≤ 15 символов.

## 3. CourseStore API (существенное)

- Публикация: `applyCourse` (развёрнуто), `writeCompactGeneration` / `applyCompactCourse` /
  `saveCompactDraft` (шаблонно), `applyDraft`, `createGeneration` + `saveMeta` (потоковая
  сборка), `cloneGeneration`, `deleteGeneration`, `recover`.
- Чтение/запись дня: `loadDay` (разрешает ссылку), `loadDayDescriptor` (без разрешения, для
  экспорта), `saveDayAtomic` (только материализованный день), `putDayReference`.
- Шаблоны: `writeTemplates`, `writeTemplatesParts` (из диапазонов входного буфера),
  `prepareTemplates`, `prepareActiveTemplates`, `lookupTemplate`, `templatesReady`,
  `invalidateTemplates`, `openTemplateExport` / `nextTemplateExportChunk`,
  `templateExportNames`.
- Валидация/диагностика: `loadMeta`, `validateGeneration`, `hasCalendar`, `loadCalendar`.

## 4. Кэш шаблонов: два слота

- `active` — поколение, которое исполняет планировщик; заполняется только через
  `prepareActiveTemplates()` (boot, Apply, Activate, восстановление, `courseLoadDay`).
- `draft` — всё остальное: проверка, просмотр, правка черновика (`prepareTemplates()`).
- Поиск набора — только по совпадению поколения. Ошибка загрузки оставляет слот пустым,
  `loadDay` возвращает ошибку, планировщик уходит в SUSPENDED (fail-closed).
- Резидентно ≈2 КБ на 42 события, ≈12 КБ на 256; пик разбора — один шаблон/день.

## 5. Синхронизация FreeRTOS

Реальные контексты:

| Контекст | Что делает | `sys_lock` |
|---|---|---|
| `controlTask` (`main.cpp:42-58`) | `sys_lock(); sched_check(); sys_unlock();` → публикация дня | да |
| loopTask, обычные маршруты | `/api/course/activate`, `/api/day/entry*`, `/api/course/draft?i=N` | да |
| loopTask, `R_NOLOCK` | import, apply, insert, clear, preset, rename, discard, draft PUT/DELETE | **нет** |
| loopTask, `R_STREAM` | export (кэш не трогает) | нет |
| audioTask | кэш не использует | — |

Механизм: листовой мьютекс `CacheGuard` (на устройстве `xSemaphoreCreateMutexStatic`).
Сборка набора (LittleFS + разбор) — вне блокировки, под ней только `swap` и три поля.
Порядок блокировок единственный: `sys_lock → cacheMutex`; `course_store` никогда не берёт
`sys_lock` → deadlock невозможен. Аудиопоток не блокируется.

Защита от устаревшей публикации (сценарий A: сборка → B: инвалидация → A: коммит):

- `g_generationStamp[generation]` — увеличивается в `invalidateTemplatesFor()` (после
  `writeTemplates`/`writeTemplatesParts`, при `deleteGeneration`);
- `g_globalEpoch` — увеличивается при полной очистке (`invalidateTemplates`, `recover`);
- `prepareTemplates`/`prepareActiveTemplates` фиксируют обе ревизии до сборки и перепроверяют
  под блокировкой; при изменении набор отбрасывается с ошибкой
  `template generation N changed while loading`.

## 6. Copy-on-write

`loadDay` разрешает ссылку в отдельный `Day` — это и есть копия. Правка (`sched_addToDay`,
`sched_editInDay`, `sched_delFromDay`, `courseSaveActiveDay`, `course_editor::saveDay/clearDay`)
меняет только её и пишет через `saveDayAtomic` как собственный список `events`.
`templates.json` и файлы соседних дней не перезаписываются (проверено побайтовым сравнением).
Набор шаблонов подгружается в точках редактирования: `courseEnsureTemplates()` (schedule) и
`editingGeneration`/`ensureEditableGeneration` (editor). Правка активного дня — без Activate.

## 7. Импорт, экспорт, Apply

- `course_editor::importCourseAuto()` — штатный импорт: документ со ссылками пишется
  компактно, без ссылок — развёрнуто (v2-совместимость). Подключён к `sched_courseImport`.
- Импорт потоковый: сканер запоминает диапазоны дней/шаблонов, каждый день и шаблон
  разбирается отдельно (`course::parseEventArrayBounded`), `templates.json` собирается из
  диапазонов входного буфера (`writeTemplatesParts`), запись/проверка CRC потоковые.
- Экспорт: `layout=tpl1` → v1 с `templates` и ссылками; `layout=expanded` → прежний v2.
  Экспорт не меняет кэш, календарь и LittleFS.
- Apply: `templates.json` → дни → `meta.json` → NVS; состояние APPLIED, `activation_date=0`,
  `day_index=-1`, звонков нет.

## 8. Измерения памяти (host)

| Операция | До A–F | После |
|---|---|---|
| Validate 47 дней / 256 уник. событий (27 КБ) | 77–101 КБ | **8–10 КБ** |
| Разбор дня/шаблона на 32 события | 82 КБ | **5 КБ** |
| Import того же курса (с host-моком FS 35 КБ) | 197 КБ | **94 КБ** (≈60 КБ на устройстве) |
| Экспорт худшего случая | 73 КБ | 65 КБ |
| Резидентный набор шаблонов | — | ≈2 КБ / ≈12 КБ |

Объём LittleFS: 14 дней (4 шаблона × 10 событий, 140 вхождений) — развёрнуто 15 130 Б →
компактно 5 779 Б (0,38×); экспорт того же курса 5 884 Б → 1 546 Б.

Guard импорта в `webhandler`: `2×body + 24 КБ` свободно и `body + 8 КБ` непрерывным блоком,
иначе JSON-ошибка без abort. Предел тела запроса — 48 КБ (политика).

## 9. Тесты (на HEAD `6d972f6`)

| Набор | Обычный | ASan/UBSan |
|---|---|---|
| course_store | 509 | 509 |
| course_editor | 739 | 725 |
| course_scheduler | 255 | 255 |
| course | 578 | 578 |
| active_day_state | 275 | 275 (UBSan) |
| emergency | 58 | 58 |

PlatformIO `env:esp32dev`: SUCCESS, RAM 21.0 % (68 900 Б), Flash 87.6 % (1 377 857 Б).
`node --check` для извлечённого JS — OK.

## 10. Дефекты, найденные и исправленные в этой сессии

1. `writeTemplates`/`writeTemplatesParts` инвалидировали **оба** слота кэша → импорт
   черновика вытеснял набор работающего курса (риск ложного SUSPENDED).
2. Guard импорта требовал `2×body + 80 КБ` (расчёт по старому дереву) → отклонял корректные
   файлы на устройстве со 125 КБ свободных. Пересчитан на `2×body + 24 КБ`.
3. Кэш не был сериализован между loopTask (R_NOLOCK-маршруты) и controlTask → реальная гонка
   на `std::map`. Добавлен `CacheGuard`.
4. Окно «устаревшей публикации»: набор, прочитанный до перезаписи `templates.json`, мог быть
   опубликован после неё. Добавлена ревизия поколения (epoch).

Каждый фикс покрыт регрессией и проверен мутацией (отключение механизма валит тесты).

## 11. Что НЕ проверено и открытые риски

- **Аппаратных испытаний архитектуры A–F нет.** Устройство содержит прошивку до шаблонной
  работы; `templates.json` на нём отсутствует.
- **Блокер конфигурации сборки**: `server/platformio.ini` в коммите `24447ef` переключён на
  `board = esp32-s3-devkitc-1`, тогда как стенд — ESP32-D0WD-V3. Прошивка «как есть» на
  D0WD-V3 не запустится. Нужно решение оператора (вернуть `board = esp32dev` или подтвердить
  S3). `platformio.ini` без разрешения не менялся.
- Реальные пики heap/largest/stack при импорте и загрузке шаблонов.
- Обрыв питания при записи `templates.json` (tmp/bak проверены только на host-моке).
- Конкурентный доступ в реальном времени (Activate в момент полуночи, импорт во время RUNNING)
  — механизм покрыт регрессиями, но не наблюдением на железе.
- Усечение экспорта при ошибке чтения в середине потока (частичный JSON; валидация его
  отвергнет, но маркера ошибки в потоке нет).
- Автоматической миграции опубликованных курсов нет (по требованию): компактность появляется
  после повторного импорта и Apply.

## 12. Следующие шаги

1. Решить вопрос `board` в `platformio.ini` (блокер).
2. Пройти `docs/DYNAMIC_COURSE_HARDWARE_TEST.md` на `6d972f6`.
3. Зафиксировать аппаратные результаты в этом файле (новый раздел, не переписывая историю).
4. Только после этого — планировать дальнейшие этапы (их нет в согласованном плане).
