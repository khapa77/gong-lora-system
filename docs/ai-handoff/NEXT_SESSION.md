# Следующая сессия: Dynamic Course Editor

Актуально на 2026-11-22. Предыдущая архитектура в `docs/ai-handoff/SESSION_STATE.md` устарела.

## Сначала прочитать

1. `docs/DYNAMIC_COURSE_SPEC.md` — окончательная спецификация.
2. `docs/DYNAMIC_COURSE_PLAN.md` — поэтапный план.
3. `docs/ai-handoff/SESSION_STATE_2026-11-22.md` — текущее состояние, Git, решения, дефекты.

## Где мы остановились

- Спецификация и план сохранены в `docs/`.
- Исходники прошивки не изменялись.
- Реализация не начата.
- Заказчик разрешил разработку отдельной командой, начиная с этапа 1 — модели курса и host-тестов.

## Первый этап

Создать:

- `server/src/course_model.h`
- `server/src/course_model.cpp`
- `tests/test_course_model.cpp`
- `tests/course_cases.h`
- `tests/run_course_tests.py`

Требования к этапу:

- Модель без Arduino, пригодная для host-тестов.
- Курс 1..47 дней, вставка/удаление/очистка/копирование, перенумерация.
- Формула дня по гражданским датам, modulo.
- JSON v2: templates + days.
- Импорт v1 в обеих формах, экспорт v2.
- День 13 — копия дня 12, затем независимая правка.
- Ошибки не меняют исходный курс.
- Сохранить существующий редактор событий: маршруты `GET /api/day?n=N`, `POST/PUT/DELETE /api/day/entry`, `GET /api/days`, поля событий и поведение «изменение видно сразу, звук не прерывается».
- Обычная правка события не требует Activate и не создаёт новое поколение. Структурные изменения — только через черновик и Apply.
- При реализации хранения добавить атомарную замену файла дня через `dNN.tmp` и `dNN.bak`; текущий `saveDayArray()` пишет напрямую в `"w"` (`schedule.cpp:773`).

После этапа:

- `python3 -B tests/run_course_tests.py --build-dir /tmp/gong-course-tests`
- PlatformIO build, если заказчик разрешит.
- Diff и отчёт.
- Не коммитить без разрешения.

## Запреты

- Не менять `config.h`, GPIO, audio, LoRa, WiFi.
- Не прошивать ESP32.
- Не стирать NVS/LittleFS.
- Не удалять старые `dayNN.conf` и `gong.conf`.
- Не делать commit/push/reset без разрешения.
- Не запускать субагентов без разрешения.
- Не менять исходники вне согласованного этапа.

## Полезные команды

- Git: `git --no-optional-locks status --short`, `git --no-optional-locks log -1 --oneline`.
- Host-тесты: `python3 -B tests/run_course_tests.py --build-dir /tmp/gong-course-tests`.
- PlatformIO: `/usr/local/bin/pio run -e esp32dev` (env называется esp32dev, плата esp32-s3-devkitc-1).
