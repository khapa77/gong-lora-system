# Сессия 2026-11-27: ветка `standalone-max-button` (кнопка + LED)

Ветка: `standalone-max-button` (новая, отдельный worktree
`/root/gong-standalone-max-button`), база — `standalone-max` `c5cc32e`.
`standalone-max` и `standalone` не менялись.

## Задача

Вернуть на актуальную архитектуру физическую кнопку и светодиод состояния,
удалённые в `b050f42` (LED + реле) и `b9d3aaa` (кнопка), не откатывая
CourseStore, course_editor, active_day_state, emergency и не возвращая
`relayhandler`.

## Что сделано

| Компонент | Файлы | Суть |
|---|---|---|
| Кнопка | `server/src/buttonhandler.{h,cpp}` | неблокирующий автомат, дребезг 50 мс, опрос 10 мс из `controlTask` под `sys_lock` |
| Boot-gate сброса пароля | там же | тик-автомат без `delay()`, `Settled/Holding/ResetGrant/WaitRelease`, ожидание стабильного отпускания |
| Светодиод | `server/src/statusled.{h,cpp}` | heartbeat 60 мс/3 с, ошибка 5 Гц, ровная подсказка готовности, прямой `led_set` для режима сброса |
| Сброс авторизации | `server/src/authreset.{h,cpp}` | стирает только NVS `auth` + `/auth.conf` |
| Единый ручной запуск | `server/src/mp3handler.{h,cpp}` | `mp3_manualPlay()` + чистые хелперы; `handlePlay()` переведён на него |
| Проводка | `server/src/main.cpp` | хуки кнопки, предикат LED, gate в `setup()`, вызовы в `controlTask` |
| Конфигурация | `server/src/config.h` | блоки кнопки и LED, `AUTH_NVS_NAMESPACE`, guard `#error` для S3 |
| Тесты | `tests/test_button.cpp`, `tests/run_button_tests.py`, `tests/host/Arduino.h`, `tests/host/Preferences.h` | 273 проверки, fake GPIO/millis/NVS/LittleFS |

## Инварианты, которые нельзя ломать

1. `BUTTON_PIN` и `STATUS_LED_PIN` по умолчанию **-1**. Включение — только после
   подтверждения физической платы (см. ниже), вместе с `build_flags`.
2. Кнопка не вызывает `mp3_*`/`sched_*`/`web_*` напрямую — только через
   `ButtonHooks` из `main.cpp`. Иначе host-тесты перестанут компилироваться.
3. Ручной запуск идёт только через `mp3_manualPlay()`: `/api/play` и кнопка
   обязаны иметь одинаковые правила.
4. `web_resetAuth()` стирает только `auth` (+ `/auth.conf`). Никаких
   `nvs_flash_erase`, `LittleFS.format`, удаления курсов.
5. `led_loop()`/`button_loop()` остаются внутри `sys_lock()` в `controlTask`;
   порядок `sys_lock -> audioMtx/logMtx` не меняется.
6. Новый `.cpp` в `server/src` не линкуется в host-раннеры автоматически — для
   кнопки добавлен отдельный раннер, прежние не тронуты.

## Блокирующий аппаратный вопрос

`server/platformio.ini`: `board = esp32-s3-devkitc-1`, а физическая плата —
ESP32-D0WD-V3 (классический ESP32), на которой и снята схема
(`footprint/schematic_netlist.md`: GPIO32 = BUTTON, GPIO13 = STATUS_LED).
На S3 GPIO32 не существует. Пока плата не подтверждена:

- кнопка/LED выключены (`-1`), прошивка собирается как раньше;
- `#error` в `config.h` не даст собрать `-DBUTTON_PIN=32` под S3-таргет;
- включённый код проверен только компиляцией:
  `PLATFORMIO_BUILD_FLAGS="-DBUTTON_PIN=13 -DSTATUS_LED_PIN=14" pio run -e esp32dev`
  (проверка компиляции, НЕ конфигурация для прошивки).

## Проверки

- host: `run_button_tests.py` 273/0; регрессия active_day 275, course 578,
  store 509, editor 739, scheduler 255, emergency 58 — всё 0 падений.
- санитайзеры: button + scheduler + emergency + course — чисто.
- `pio run -e esp32dev`: SUCCESS, RAM 21.0 %, Flash 87.6 % (кнопка выключена).

Точные SHA и номера сборки — в итоговом отчёте сессии; см. также
`docs/BUTTON_LED.md` (полная спецификация) и `docs/ai-handoff/NEXT_SESSION.md`.
