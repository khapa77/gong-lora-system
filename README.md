# Gong LoRa System

Система звонков на ESP32 с LoRa-сетью. **Один сервер** управляет расписанием и рассылает команды, **любое количество клиентов** слушают и воспроизводят звук одновременно.

```
[ Server ESP32 ]  ── LoRa 433 MHz ──>  [ Client ESP32 ] x N
  WiFi + Web UI                          MP3 playback
  Schedule / NTP                         ACK to server
  REST API
```

---

## Архитектура

| | Server | Client |
|---|---|---|
| WiFi | ✅ STA (или AP fallback) | ❌ не нужен |
| Web UI | ✅ http://ESP32_IP/ | ❌ |
| LoRa | ✅ отправляет | ✅ слушает |
| MP3 | ✅ играет локально | ✅ играет по команде |
| NTP | ✅ синхронизация | ❌ |
| ACK | ❌ | ✅ отправляет серверу |

---

## LoRa протокол

Каждое сообщение: `[1 байт тип][JSON payload]`

| Тип | Hex | Направление | JSON пример |
|-----|-----|-------------|-------------|
| GONG | `0x01` | Server → Clients | `{"track":1,"vol":25,"ts":12345}` |
| HEARTBEAT | `0x02` | Server → Clients | `{"time":"08:00:15","clients":3}` |
| SCHEDULE | `0x03` | Server → Clients | `[{"id":1,"hour":8,"min":0,...}]` |
| ACK | `0x04` | Client → Server | `{"id":"client_001","rssi":-52}` |

---

## Железо

### Одинаково для сервера и клиентов:

| Компонент | Модель |
|-----------|--------|
| МК | ESP32 (любой, DevKit) |
| LoRa | SX1278 / XL1278-SMT 433 МГц |
| MP3 | DFPlayer Mini (MP3-TF-16P) |
| SD-карта | MicroSD с MP3-файлами (001.mp3, 002.mp3 …) |
| Динамик | 4–8 Ω |

### Распиновка — SERVER (ESP32 + LoRa + MAX98357A)

```
ESP32          LoRa SX1278          (VSPI, RadioLib)
GPIO5   ──── NSS / CS
GPIO14  ──── RST
GPIO2   ──── DIO0
GPIO18  ──── SCK
GPIO19  ──── MISO
GPIO23  ──── MOSI
3.3V    ──── VCC
GND     ──── GND

ESP32          MAX98357A            (I2S)
GPIO26  ──── BCLK  (Bit Clock)
GPIO25  ──── LRC   (Word Select)
GPIO33  ──── DIN   (Data In)
5V      ──── VDD
GND     ──── GND
```

### Распиновка — CLIENT (ESP32 + LoRa + DFPlayer Mini)

```
ESP32          LoRa SX1278          (VSPI, RadioLib)
GPIO5   ──── NSS / CS
GPIO14  ──── RST
GPIO2   ──── DIO0
GPIO18  ──── SCK
GPIO19  ──── MISO
GPIO23  ──── MOSI
3.3V    ──── VCC
GND     ──── GND

ESP32          DFPlayer Mini        (UART2)
GPIO16  ──── RX_module (DFP TX)
GPIO17  ──── TX_module (DFP RX)   ─── 1kΩ ─── DFP RX
GPIO4   ──── BUSY
5V      ──── VCC
GND     ──── GND

GPIO33  ──── LED + 220Ω ──── GND  (статус-LED, опционально)
```

### LoRa RF-параметры (сервер и клиент должны совпадать)

| Параметр | Значение |
|----------|----------|
| Частота | 433 МГц |
| Spreading Factor | SF7 |
| Bandwidth | 125 кГц |
| Coding Rate | 4/5 |
| Sync Word | `0xF3` |
| TX Power | 20 дБм |

---

## Прошивка

### Server

```bash
cd server
# 1. Настрой WiFi (edit config.h -> AP_SSID/AP_PASSWORD или через веб)
# 2. Собери и залей прошивку:
pio run -t upload
# 3. Залей веб-интерфейс в SPIFFS:
pio run -t uploadfs
```

**Два MP3 общим объёмом ~2 МБ:** в проекте включена таблица разделов `partitions_large_spiffs.csv` (SPIFFS ~2.5 МБ, без OTA). Положи в `server/data/` файлы `0001.mp3` и `0002.mp3`, затем `pio run -t uploadfs`. Убедись, что размер папки `data/` не превышает ~2.4 МБ.

### Clients

Для каждого клиента:
1. Открой `client/include/config.h`
2. Измени `CLIENT_ID` на уникальное имя (например `"room_A"`, `"room_B"`)
3. Залей прошивку:

```bash
cd client
pio run -t upload
```

---

## Первый запуск

### Сервер:
1. При первом запуске (нет `wifi.conf` в SPIFFS) → поднимает точку доступа **`GongServer` / `vipassana`**
2. Подключись к AP, открой `http://192.168.4.1`
3. В разделе **WiFi Settings** введи свою сеть → **Save & Restart**
4. Сервер перезагрузится и подключится к твоей сети
5. IP-адрес виден в Serial monitor: `[WIFI] Connected! IP: 192.168.1.XX`

### Клиенты:
- Питание → автоматически слушают LoRa
- После получения GONG-команды: воспроизводят трек, отправляют ACK
- Клиенты появляются в веб-интерфейсе сервера в панели **LoRa Clients**

---

## Веб-интерфейс (сервер)

`http://<SERVER_IP>/`

- **Manual Control** — запустить звонок локально / по LoRa / везде
- **Schedule** — добавить/редактировать/удалить расписание. Будильник срабатывает только при **подключении к роутеру (WiFi STA)** и после синхронизации NTP (1–2 мин). В режиме точки доступа (AP) расписание не проверяется. В Serial выводится `[SCHED] Time HH:MM` раз в минуту — сверь с локальным временем; если не совпадает, задай `NTP_UTC_OFFSET` в `server/src/config.h` (например, 10800 для UTC+3).
- **LoRa Clients** — видит все клиенты, их RSSI и время последнего ответа
- **WiFi Settings** — смена сети без перепрошивки
- **Sync Schedule** — принудительно разослать расписание всем клиентам

---

## REST API

```
GET  /api/schedule          — список записей расписания
POST /api/schedule          — добавить  {hour, min, track, desc}
PUT  /api/schedule?id=N     — изменить  {hour, min, track, desc, en}
DELETE /api/schedule?id=N   — удалить

POST /api/play              — играть локально    {track, vol}
POST /api/play/lora         — LoRa broadcast     {track, vol}
POST /api/play/all          — локально + LoRa    {track, vol}
POST /api/sync              — разослать расписание по LoRa

GET  /api/clients           — список известных клиентов
GET  /api/status            — статус сервера

GET  /api/wifi/status       — статус WiFi
POST /api/wifi/save         — {ssid, password}
POST /api/wifi/reset        — сброс WiFi
```

---

## Структура проекта

```
gong-lora-system/
├── server/                  # Server ESP32
│   ├── platformio.ini
│   ├── include/
│   │   ├── config.h         ← пины, константы
│   │   ├── mp3handler.h
│   │   ├── schedule.h
│   │   ├── lorahandler.h
│   │   └── webhandler.h
│   ├── src/
│   │   ├── main.cpp
│   │   ├── mp3handler.cpp
│   │   ├── schedule.cpp
│   │   ├── lorahandler.cpp
│   │   └── webhandler.cpp
│   └── data/
│       ├── index.html       ← веб-интерфейс (SPIFFS)
│       └── gong.conf
└── client/                  # Client ESP32 (N штук)
    ├── platformio.ini
    ├── include/
    │   ├── config.h         ← CLIENT_ID здесь!
    │   ├── mp3handler.h
    │   └── lorahandler.h
    └── src/
        ├── main.cpp
        ├── mp3handler.cpp
        └── lorahandler.cpp
```

---

## Источники

Проект создан на основе:
- [khapa77/Gong_new](https://github.com/khapa77/Gong_new)
- [khapa77/ring](https://github.com/khapa77/ring)
- [khapa77/gong_dullabha](https://github.com/khapa77/gong_dullabha)
