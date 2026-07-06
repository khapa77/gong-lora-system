# Отладка на PlatformIO

## Базовые команды

```bash
pio run                    # сборка
pio run -t upload          # прошивка
pio device monitor         # серийный монитор
pio run -t upload -t monitor  # прошить и сразу открыть монитор
pio device list            # список подключённых портов
pio boards espressif32      # список доступных плат этого семейства (проверить актуальное имя board)
```

## Плата не прошивается ("Failed to connect", timeout)

Порядок проверки:
1. **Драйвер USB-UART не установлен** — проверь, виден ли порт в `pio device list` / диспетчере устройств. Частые чипы: CP2102, CH340, FTDI — драйвер зависит от конкретного модуля.
2. **Не тот `upload_port`** — если портов несколько, укажи явно в `platformio.ini`:
   ```ini
   upload_port = /dev/ttyUSB0   ; или COM5 на Windows
   ```
3. **Не зажат BOOT при прошивке** (актуально для некоторых китайских клонов без авто-ресета) — держать BOOT, нажать EN/RESET, отпустить EN, начать прошивку, отпустить BOOT.
4. **Неверная скорость** — попробуй понизить `upload_speed = 115200` вместо дефолтных 921600, если модуль/кабель некачественный.
5. **Плохой USB-кабель** — очень частая причина, особенно "только для зарядки" кабели без линий данных.

## Плата перезагружается в цикле ("Brownout detector was triggered")

Это сообщение про просадку напряжения питания, не программная ошибка. Проверяй:
- Питание от USB-порта компьютера/слабого блока — часто не тянет пиковый ток при Wi-Fi TX (см. `references/hardware.md`).
- Длинные/тонкие провода питания при работе от внешнего источника — падение напряжения на проводе.
- Можно временно отключить детектор для диагностики (НЕ для продакшна):
  ```cpp
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  ```
  Предупреждай, что это маскирует симптом, а не чинит причину.

## "Guru Meditation Error" / краш с бэктрейсом

Пример вывода:
```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited). Exception was unhandled.
...
Backtrace: 0x400d1234:0x3ffb1e40 0x400d5678:0x3ffb1e60 ...
```
- **LoadProhibited/StoreProhibited** — обычно разыменование нулевого или "битого" указателя.
- **IntegerDivideByZero** — деление на ноль.
- Декодируй backtrace в читаемые имена функций:
  ```bash
  xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/esp32dev/firmware.elf <адреса из backtrace>
  ```
  Либо включи `monitor_filters = esp32_exception_decoder` в `platformio.ini` — PlatformIO автоматически расшифрует бэктрейс в мониторе:
  ```ini
  monitor_filters = esp32_exception_decoder
  ```

## Task watchdog got triggered

Задача FreeRTOS не отдаёт управление достаточно долго. Проверь:
- Отсутствие `vTaskDelay`/`delay` в длинных циклах.
- Блокирующие вызовы без таймаута (например, ожидание сети без ограничения по времени).
- См. `references/freertos.md` для деталей многозадачности.

## Логирование для диагностики

```cpp
#define CORE_DEBUG_LEVEL 5  // задать до подключения Arduino.h, либо через build_flags
```
В `platformio.ini`:
```ini
build_flags = -DCORE_DEBUG_LEVEL=5
```
Уровни: 0=none, 1=error, 2=warn, 3=info, 4=debug, 5=verbose.
