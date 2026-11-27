# Аппаратная конфигурация standalone-max-button

Плата утверждена владельцем: классический ESP32 DevKit, 38 выводов (ESP32-D0WD-V3),
та же разводка, что у донора `standalone @ e1a588e`. `platformio.ini`:
`board = esp32dev`, flash 4 МБ, `partitions_large_spiffs.csv`, LittleFS.

| Функция | GPIO | Уровень / режим | Источник |
|---|---|---|---|
| Кнопка | 32 | `INPUT_PULLUP`, active LOW, дребезг 50 мс | config.h, schematic_netlist.md |
| LED состояния | 13 | OUTPUT, active HIGH | config.h, schematic_netlist.md |
| Реле внешнего усилителя | 27 | OUTPUT, active LOW по умолчанию (см. ниже) | config.h, J3 |
| I2S LRC | 25 | — | config.h |
| I2S BCLK | 26 | — | config.h |
| I2S DOUT | 33 | — | config.h |
| I2C SDA (DS3231) | 21 | — | config.h |
| I2C SCL (DS3231) | 22 | — | config.h |

## Полярность реле — не подтверждена

Противоречие в доноре: `config.h` и схема (R11) описывают active HIGH, а
`platformio.ini` донора содержал `-DRELAY_ACTIVE_LOW=1`. В ветке по умолчанию
взята конфигурация **реальной прошивки донора** (`RELAY_ACTIVE_LOW = 1`),
поддержаны оба уровня (`-DRELAY_ACTIVE_LOW=0`). До стендового измерения
полярность считается НЕПОДТВЕРЖДЁННОЙ.

## Проверки GPIO

`GONG_GPIO_VALID()` — существование вывода у чипа; для классического ESP32
дополнительно: `GONG_GPIO_OUTPUT_OK()` (не flash 6–11, не input-only 34–39),
`GONG_GPIO_INPUT_PULLUP_OK()` (то же + подтяжка), `GONG_GPIO_STRAPPING()`
(0/2/5/12/15 → `#error`). Конфликты реле с кнопкой, LED и I2S — `#error`.

## Память

Размер flash и наличие PSRAM определяются `board`/`board_upload.flash_size` и
таблицей разделов, а не количеством контактов платы. Текущая сборка:
см. отчёт `результат_4.md` (RAM/Flash/firmware.bin).

## Сборка

    cd server
    pio run -e esp32dev                  # прошивка образа (без загрузки на плату)
    pio run -e esp32dev -t buildfs       # LittleFS-образ (при необходимости)
    # прошивка — только вручную и только на стенде:
    # pio run -e esp32dev -t upload

Полярность реле переключается одной строкой без правки кода:

    build_flags = -DRELAY_ACTIVE_LOW=0   # active HIGH
    build_flags = -DRELAY_ACTIVE_LOW=1   # active LOW (по умолчанию, как в прошивке донора)

## План стендовых испытаний (кратко)

1. Питание: осциллограф на RELAY_OUT от подачи питания и при brownout — импульса
   быть не должно, уровень соответствует выбранной полярности.
2. Определить фактическую полярность модуля (active HIGH/LOW) и зафиксировать
   `RELAY_ACTIVE_LOW`.
3. Прогрев: гонг 12:15:00 при warm-up 3000/12000/20000 мс — фронт реле в
   12:14:57 / 12:14:48 / 12:14:40 (±1 с тика планировщика), звук в 12:15:00.
4. Полночь: событие 00:00:00, warm-up 12000 мс — реле в 23:59:48.
5. Hold: измерить выключение через 3000 мс после фактического конца трека.
6. Кнопка: <3 с игнор; 3–8 с гонг; ≥8 с реле; ≥1 с STOP; 10 с при загрузке —
   сброс пароля; 60 с — защита от залипания.
7. LED: ERROR 5 Гц (100/100 мс) при отключённой filesystem/снятом курсе;
   возврат heartbeat после устранения.
8. Watchdog и задержки `controlTask` при одновременных HTTP + декодировании + NVS.
