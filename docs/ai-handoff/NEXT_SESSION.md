# Следующая сессия: стендовая проверка кнопки + LED, затем курсов A–F

Актуально на **2026-11-27**. Ветка **`standalone-max-button`** (worktree
`/root/gong-standalone-max-button`), база — `standalone-max` `c5cc32e`,
функциональный коммит `e229f4a`. Push не выполнялся: ветка только локальная.

Это единственный актуальный handoff для данной ветки. История: этой сессии —
`docs/ai-handoff/SESSION_STATE_2026-11-27.md` (кнопка/LED), предыдущих —
`SESSION_STATE_2026-11-23.md` (этапы A–F курсов) и
`SESSION_STATE_2026-11-22.md`. Спецификация новой функции — `docs/BUTTON_LED.md`.

## Сначала прочитать

1. `AGENTS.md` — правила безопасности.
2. `docs/BUTTON_LED.md` — кнопка, LED, сброс пароля, тестовая матрица.
3. `docs/DYNAMIC_COURSE_HARDWARE_TEST.md` — процедура испытаний курсов.
4. `docs/ai-handoff/SESSION_STATE_2026-11-27.md` — что именно сделано и какие
   инварианты нельзя ломать.

## Состояние кода

| Что | Значение |
|---|---|
| Ветка | `standalone-max-button` (от `standalone-max` `c5cc32e`) |
| HEAD ветки | см. `git log -1` (реле: коммиты `feat(relay)` и `feat(web)` поверх `460a1b4`) |
| Функция кнопки/LED | `e229f4a`, дефекты review закрыты в `cc2c9a2` |
| Функция реле | `relayhandler` + интеграция + `/api/relay` + карточка Gong & Relay; спецификация — `docs/RELAY.md` |
| Сборка | `pio run -e esp32dev`: SUCCESS, RAM 21.0 %, Flash 87.7 % |
| Кнопка/LED/реле в прошивке | **выключены** (`BUTTON_PIN`/`STATUS_LED_PIN`/`RELAY_PIN` = -1) |
| Host-тесты | button 532, relay 120 (103+17), store 509, editor 739, scheduler 270, course 578, active_day 275, emergency 58 — 0 падений |
| ASan/UBSan | button/scheduler/emergency/course — чисто |
| Прошивка на устройстве | не менялась этой сессией |

`standalone-max` и `standalone` не тронуты. Ветка `standalone` остаётся только
источником исходной реализации кнопки/LED.

## Блокер №1 — плата и полярность реле (решается до любой прошивки)

В `24447ef` в `server/platformio.ini` строка `board = esp32dev` заменена на
`board = esp32-s3-devkitc-1`, а устройство по всем отчётам — ESP32-D0WD-V3
(классический ESP32). Номера GPIO32/13 существуют у обоих чипов, но прошивка,
собранная под S3, на этой плате не запустится; схема
(`footprint/schematic_netlist.md`) использует GPIO32 = BUTTON и GPIO13 = STATUS_LED.

Пока плата не подтверждена оператором:

- кнопка и LED выключены (`-1`), прошивка собирается и работает как раньше;
- `GONG_GPIO_VALID()` в `config.h` блокирует только несуществующие у целевого
  чипа номера (S3: 22–25; ESP32: 40–48), а не номера 32/13;
- включённый код проверен только компиляцией целевым тулчейном
  (`PLATFORMIO_BUILD_FLAGS="-DRELAY_PIN=27 -DBUTTON_PIN=32 -DSTATUS_LED_PIN=13"
  pio run -e esp32dev`, SUCCESS; binary для S3, на плату не прошивается);
- полярность модуля реле не подтверждена: поддержаны оба уровня
  (`RELAY_ACTIVE_LOW`), пока не выбрано — реле выключено (`RELAY_PIN = -1`).

Решение (возврат `board = esp32dev` или подтверждение S3) принимает оператор:
это изменение аппаратной конфигурации.

## Порядок стендовой работы

1. Снять блокер по плате.
2. Включить кнопку и LED в `build_flags`:
   `-DBUTTON_PIN=32 -DSTATUS_LED_PIN=13` (только после п.1).
3. Зафиксировать состояние устройства ДО прошивки: `/api/status`,
   `/api/course/status`, экспорт действующего курса, лог Serial.
4. Прошить **только firmware**: `cd server && pio run -e esp32dev -t upload`.
   Никаких `erase_flash`/`uploadfs` — NVS и LittleFS остаются.
5. Сценарии кнопки/LED — `docs/BUTTON_LED.md` §9: удержания 2.9/3.0/3.1 с, стоп
   во время курсового события, нажатие во время гонга, кнопка зажата при
   включении, удержание 10 с → сброс пароля с проверкой сохранности Wi-Fi, курса,
   RTC и громкости, индикация ошибки при отсутствии курса/времени/ФС.
6. Сценарии курсов A–F — `docs/DYNAMIC_COURSE_HARDWARE_TEST.md`
   (validate → import → apply → activate → reboot → полночь → copy-on-write →
   export), с фиксацией `[WEB] ... heap` и `[COURSE] ...`.

## Текущее состояние устройства (из прошлой сессии, не проверялось заново)

- ESP32-D0WD-V3 без PSRAM, IP `192.168.1.197`.
- Загружен курс **Gong #2**: 14 дней, 136 календарных событий, поколение **3**,
  активирован **день 07**, активация пережила перезагрузку.
- Прошивка на устройстве — до шаблонной архитектуры (`dcb2603`);
  `templates.json` отсутствует, поколение 3 развёрнутое (`layout=expanded`).

## Точные команды проверки

```bash
cd /root/gong-standalone-max-button
python3 -B tests/run_button_tests.py            --build-dir /tmp/t-button
python3 -B tests/run_course_store_tests.py      --build-dir /tmp/t-store
python3 -B tests/run_course_editor_tests.py     --build-dir /tmp/t-editor
python3 -B tests/run_course_scheduler_tests.py  --build-dir /tmp/t-sched
python3 -B tests/run_course_tests.py            --build-dir /tmp/t-course
python3 -B tests/run_active_day_state_tests.py  --build-dir /tmp/t-active
python3 -B tests/run_emergency_tests.py         --build-dir /tmp/t-emerg
cd server && pio run -e esp32dev
```

ASan/UBSan: те же раннеры с `--sanitize` (для `active_day_state` —
`--sanitize undefined`: `address` не линкуется из-за дубля `operator new`).

## Запреты

- Не менять `board`, GPIO, MP3/I2S, RTC, WiFi без отдельного разрешения.
- Не прошивать автоматически; никаких `erase_flash`/`uploadfs`.
- Не стирать NVS/LittleFS, не удалять `dayNN.conf`/`gong.conf`.
- Не возвращать `relayhandler` и управление реле.
- Не выполнять push/force push, reset/clean без разрешения.
- Локальные коммиты — только `git -c core.hooksPath=/dev/null commit`
  (pre-commit делает `git add -A`, post-commit — `git push origin HEAD`).
- Не трогать посторонние untracked-файлы (`end`, `resume_dsdsdsds`).
- Не ослаблять тесты ради PASS.

## Открытые риски

1. Плата (см. блокер) — пока не снят, аппаратные результаты недействительны.
2. Кнопка/LED ни разу не проверялись на железе: дребезг, наводки, полярность LED2,
   поведение при подаче питания с зажатой кнопкой — NOT TESTED.
3. Пики heap при импорте курса (host: validate 8–10 КБ, import ≈60 КБ на
   устройстве) и фрагментация кучи после долгой работы WiFi.
4. Обрыв питания в момент записи `templates.json` (tmp/bak проверены только на
   host-моке).
5. Конкурентный кэш шаблонов: подтверждён статически и регрессиями, но не в
   реальном времени (Activate в полночь, импорт во время RUNNING).
6. Flash занят на 87.6 % — запас ~195 КБ на 4 МБ раздел; следующая крупная
   функция потребует пересмотра разделов.

## Состояние после второго ревью (c2788d5)

Замечания второго независимого ревью закрыты (см. `docs/RELAY.md` §9):
Apply атомарен, отложенный гонг фиксируется только после фактического старта,
кнопка не подменяет чужой запуск, ошибки NVS репортируются, аудио-состояние
кэшируется (≤20 Гц), POST `/api/relay` валидируется целиком, форма не
перезаписывает редактируемое поле. Host: 3110/0. Сборки: выключенная 88.0 %,
включённая (compile-check, `-DRELAY_PIN=27 -DBUTTON_PIN=32 -DSTATUS_LED_PIN=13`)
89.1 % Flash, 21.2 % RAM. Аппаратная конфигурация по-прежнему BLOCKED.

## Третий раунд (26726b5)

Ревью `a42208e..c2788d5` дало «не принимать»: 5 HIGH/MEDIUM. Исправлено в
`26726b5`: `courseApplyInProgress` (Apply не может опубликовать старое
поколение), `courseReloadCalendar()` возвращает результат, теги запросов для
подтверждения `last_fire`, `mp3_play()` сообщает о реальном старте, кэш аудио
обновляется в `startNow()` (hold=0 больше не гасит реле), транзакционная запись
NVS, сброс `warm` при росте `pre`, `sched_msToNext()` возвращает -1 при
отложенной смене дня, Save не стирает новую правку. Host: 3131/0; ASan/UBSan
чисто; сборки 88.1 % / 89.1 % Flash, 21.0 % / 21.2 % RAM.
