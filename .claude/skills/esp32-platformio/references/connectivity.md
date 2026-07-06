# Wi-Fi, BLE, MQTT, OTA

## Wi-Fi — клиент (STA)

```cpp
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORD");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.localIP());
}
```
- Не блокируй `loop()` бесконечным `while` без `vTaskDelay`/`delay` — уже блокирует watchdog в некоторых конфигурациях.
- Для продакшн-кода лучше не хардкодить SSID/пароль — предложи `Preferences`/NVS или WiFiManager-подобный подход для конфигурации через веб-портал, если пользователь не просил обратного явно.

## Wi-Fi — точка доступа (AP) + веб-сервер

```cpp
#include <WiFi.h>
#include <WebServer.h>

WebServer server(80);

void setup() {
  WiFi.softAP("ESP32-Setup", "12345678");
  server.on("/", []() {
    server.send(200, "text/html", "<h1>ESP32</h1>");
  });
  server.begin();
}

void loop() {
  server.handleClient();
}
```

## BLE

Arduino-обёртка (`BLEDevice.h`) проще, но менее гибкая, чем нативный ESP-IDF BLE-стек. Для простых задач (передача показаний датчика, простой сервис/характеристика) — Arduino-обёртки достаточно:

```cpp
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

void setup() {
  BLEDevice::init("ESP32-Device");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pService->start();
  pServer->getAdvertising()->start();
}
```
Учитывай: BLE и Wi-Fi используют общий радиомодуль — одновременная интенсивная работа обоих может создавать помехи и требует внимательной настройки coexistence (обычно работает "из коробки" средне-нагруженно, но не рассчитывай на пиковую производительность обоих одновременно).

## MQTT (через PubSubClient)

Библиотека в `platformio.ini`:
```ini
lib_deps = knolleary/PubSubClient@^2.8
```

```cpp
#include <WiFi.h>
#include <PubSubClient.h>

WiFiClient espClient;
PubSubClient client(espClient);

void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP32Client")) {
      client.subscribe("esp32/commands");
    } else {
      delay(2000);
    }
  }
}

void setup() {
  client.setServer("broker.address", 1883);
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();
}
```

## OTA-обновления

Простейший вариант через ArduinoOTA (для обновлений в локальной сети во время разработки):

```cpp
#include <ArduinoOTA.h>

void setup() {
  ArduinoOTA.begin();
}

void loop() {
  ArduinoOTA.handle();
}
```

Для продакшн OTA (обновление "по воздуху" удалённо, не только в локальной сети) — нужна схема с двумя partition-слотами (`partitions.csv`) и HTTP(S)-загрузкой прошивки; это отдельная, более объёмная тема — уточни у пользователя, нужен ли ему именно продакшн-сценарий, прежде чем предлагать полную реализацию.
