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
