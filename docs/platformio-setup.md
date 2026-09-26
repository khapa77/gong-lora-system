# PlatformIO с нуля: сборка и прошивка Gong Server

Инструкция для ветки `standalone` (FW 6.0-solo). Для других веток отличается
только шаг `git checkout`.

## 1. Что понадобится

- Компьютер с Linux, macOS или Windows.
- **Python 3.8 или новее**. Проверить: `python3 --version` (Windows: `python --version`).
- **Git**. Проверить: `git --version`.
- USB-кабель **с линиями данных**. Кабели «только для зарядки» — самая частая
  причина, почему плата не видна.
- ESP32 DevKit.

## 2. Установка PlatformIO

Есть два пути: консоль (CLI) или VS Code. Можно поставить оба, они не мешают
друг другу.

### Вариант А — консольный `pio` (рекомендуется)

**Linux / macOS:**

```bash
curl -fsSL -o get-platformio.py https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
python3 get-platformio.py
```

Установщик кладёт PlatformIO в `~/.platformio/penv`. Чтобы команда `pio`
работала из любой папки:

```bash
mkdir -p ~/.local/bin
ln -s ~/.platformio/penv/bin/pio ~/.local/bin/pio
ln -s ~/.platformio/penv/bin/platformio ~/.local/bin/platformio
```

Если `~/.local/bin` нет в PATH, добавьте в `~/.bashrc` (или `~/.zshrc` на macOS)
строку `export PATH="$HOME/.local/bin:$PATH"` и откройте новый терминал.

**Windows (PowerShell):**

```powershell
python -m pip install --upgrade pip
python -m pip install platformio
```

Если после этого `pio` не находится, добавьте в PATH папку `Scripts` вашего
Python или запускайте через `python -m platformio`.

**Проверка:**

```bash
pio --version
```

### Вариант Б — VS Code

1. Установите VS Code.
2. Откройте Extensions (Ctrl+Shift+X), найдите **PlatformIO IDE** и установите.
3. Перезапустите VS Code. Первая загрузка идёт несколько минут: в фоне
   скачивается ядро PlatformIO.
4. Открывайте папку `server/`, а не корень репозитория: `platformio.ini` лежит
   именно там.
5. Кнопки внизу окна: ✓ собрать, → прошить, 🔌 монитор порта.

## 3. Драйвер USB-UART

Посмотрите, какая микросхема стоит на плате рядом с USB-разъёмом:

| Чип | Драйвер |
|---|---|
| **CP2102 / CP2104** | Silicon Labs «CP210x VCP» |
| **CH340 / CH9102** | WCH «CH34x» |

- **Linux:** драйверы уже в ядре, ничего ставить не нужно.
- **macOS:** CP210x обычно работает сразу, для CH340 на старых версиях нужен
  драйвер от WCH.
- **Windows:** если в «Диспетчере устройств» плата видна с жёлтым знаком,
  скачайте драйвер с сайта производителя чипа.

## 4. Права на порт (только Linux)

```bash
sudo usermod -a -G dialout $USER        # на Arch: группа uucp
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

После этого **выйдите из системы и зайдите заново**, иначе новая группа не
применится.

## 5. Получить проект

```bash
git clone https://github.com/khapa77/gong-lora-system.git
cd gong-lora-system
git checkout standalone
cd server
```

## 6. Первая сборка

```bash
pio run
```

Первый запуск идёт 5–10 минут: PlatformIO сам скачивает платформу
`espressif32 @ 6.7.0`, компилятор и библиотеки (ArduinoJson, ESP32-audioI2S,
RTClib). Следующие сборки занимают меньше минуты.

Успешная сборка заканчивается строкой `[SUCCESS]`.

## 7. Подключить плату и найти порт

```bash
pio device list
```

Как обычно выглядит порт:

- Linux: `/dev/ttyUSB0` или `/dev/ttyACM0`;
- macOS: `/dev/cu.usbserial-XXXX` или `/dev/cu.SLAB_USBtoUART`;
- Windows: `COM3`, `COM5` и т.п.

Если подключена одна плата, PlatformIO найдёт её сам. Иначе добавляйте к
командам `--upload-port <порт>`.

## 8. Прошивка: два шага

Прошивка и файлы (веб-интерфейс, MP3, расписания) записываются **отдельно**.

```bash
pio run -t upload      # 1) прошивка
pio run -t uploadfs    # 2) файловая система LittleFS из папки data/
```

- `uploadfs` нужен при первой установке и каждый раз, когда меняется что-то в
  `data/`: веб-интерфейс, MP3 или `dayNN.conf`.
- ⚠️ `uploadfs` **перезаписывает всю файловую систему**. Сохранённые на
  устройстве пароль админки (`auth.conf`), WiFi (`wifi.conf`), настройки реле
  (`relay.conf`) и изменения расписания через веб будут стёрты.
- Скорость загрузки 115200, поэтому `uploadfs` с двумя MP3 (~2,5 МБ) идёт
  несколько минут. Это нормально.

## 9. Монитор порта (логи)

```bash
pio device monitor
```

Выход: **Ctrl+C**. В `platformio.ini` уже выставлено `monitor_rts = 0` и
`monitor_dtr = 0`, поэтому открытие монитора не перезагружает плату.

При старте в логе должно появиться примерно такое:

```
  Gong Server v6.0-solo (standalone: WiFi + relay)
[RELAY] GPIO27 (active HIGH), mode=auto pre=800ms hold=3000ms
[WIFI] AP 'GongServer' started — IP: 192.168.4.1
[WEB] HTTP server listening on port 80
```

Прошить и сразу открыть лог одной командой:

```bash
pio run -t upload -t monitor
```

## 10. Первый запуск устройства

1. С телефона подключитесь к WiFi **GongServer**, пароль `vipassana` (его стоит
   сменить, см. ниже).
2. Откройте **http://192.168.4.1**.
3. Карточка **Clock**: установите дату и время. Без этого расписание не работает.
4. **Security**: задайте пароль админки.
5. **WiFi**: при желании подключите устройство к роутеру (Scan → выбрать сеть →
   Connect).
6. **Gong & Relay**: нажмите Play и проверьте звук и щелчок реле.

## 11. Настройки сборки

Правятся в `server/platformio.ini`, в секции `build_flags` (раскомментируйте
нужную строку):

- **Пароль точки доступа** (минимум 8 символов):

  ```bash
  export GONG_AP_PASSWORD='мой-пароль'
  ```

  затем раскомментируйте строку `-DAP_PASSWORD=...` и пересоберите.
- **Реле, которое включается низким уровнем** (большинство китайских модулей):
  `-DRELAY_ACTIVE_LOW=1`.
- **Другой пин реле**: `-DRELAY_PIN=4`.

## 12. Если что-то не работает

| Симптом | Что делать |
|---|---|
| `Failed to connect to ESP32: Timed out… waiting for packet header` | Во время `Connecting.....` зажмите кнопку **BOOT** и отпустите, когда начнётся запись. Если так бывает постоянно, помогает конденсатор 10 мкФ между EN и GND. |
| `Could not open port … Permission denied` | Linux: не применились права (шаг 4), перелогиньтесь. |
| `… port is busy` / `Access is denied` | Порт занят монитором, Arduino IDE или другим терминалом. Закройте их. |
| `pio device list` пустой | Кабель без линий данных, нет драйвера (шаг 3) или попробуйте другой USB-порт. |
| Постоянные перезагрузки, `Brownout detector was triggered` | Не хватает питания: смените USB-порт или кабель, запитайте от нормального источника 5 В. |
| Вместо веб-интерфейса «Upload filesystem data…» | Не был выполнен `pio run -t uploadfs`. |
| Ошибки после смены ветки или версии | `pio run -t clean`, затем снова `pio run`. |

## 13. Шпаргалка

```bash
cd gong-lora-system/server
pio run                        # собрать
pio run -t upload              # прошить
pio run -t uploadfs            # залить data/ (стирает настройки на устройстве!)
pio device monitor             # логи
pio run -t clean               # очистить сборку
pio device list                # список портов
pio pkg update                 # обновить библиотеки (осторожно, версии могут поменяться)
```
