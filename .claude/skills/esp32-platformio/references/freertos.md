# FreeRTOS на ESP32

ESP32 (кроме C3, у которого одно ядро) — двухъядерный чип, Arduino `loop()` по умолчанию выполняется как FreeRTOS-задача на ядре 1 (APP_CPU), ядро 0 (PRO_CPU) обычно занято Wi-Fi/BLE стеком.

## Создание задачи

```cpp
void sensorTask(void *pvParameters) {
  for (;;) {
    // работа
    vTaskDelay(pdMS_TO_TICKS(100)); // ОБЯЗАТЕЛЬНО отдавать управление, иначе watchdog reset
  }
}

void setup() {
  xTaskCreatePinnedToCore(
    sensorTask,      // функция задачи
    "SensorTask",    // имя (для отладки)
    4096,            // размер стека в байтах — начинай с 2048-4096, увеличивай при переполнении
    NULL,            // параметры
    1,               // приоритет (0 = idle, выше = приоритетнее)
    NULL,            // хэндл задачи (для управления/удаления)
    1                // ядро (0 или 1); используй tskNO_AFFINITY для планировщика
  );
}
```

**Частая ошибка**: забыть `vTaskDelay` в бесконечном цикле задачи — это блокирует watchdog timer (WDT) и приводит к перезагрузке платы ("Task watchdog got triggered"). Всегда включай хотя бы небольшую задержку или используй блокирующий примитив (очередь, семафор).

## Очереди (Queue) — для обмена данными между задачами

```cpp
QueueHandle_t sensorQueue;

void setup() {
  sensorQueue = xQueueCreate(10, sizeof(float));
}

void producerTask(void *pv) {
  float value = readSensor();
  xQueueSend(sensorQueue, &value, portMAX_DELAY);
}

void consumerTask(void *pv) {
  float value;
  if (xQueueReceive(sensorQueue, &value, portMAX_DELAY) == pdTRUE) {
    // обработка
  }
}
```
Не передавай указатели на локальные (стековые) переменные через очередь — данные копируются по значению, но если это указатель на что-то временное, получатель прочитает мусор.

## Mutex — для защиты общих ресурсов

```cpp
SemaphoreHandle_t i2cMutex;

void setup() {
  i2cMutex = xSemaphoreCreateMutex();
}

void taskUsingI2C(void *pv) {
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // работа с I2C
    xSemaphoreGive(i2cMutex);
  }
}
```
Используй mutex, если несколько задач обращаются к одной шине (I2C/SPI) или к общей переменной — иначе будут гонки данных (race condition), особенно заметные при работе на разных ядрах.

## Приоритеты и стабильность

- Не ставь всем задачам одинаково высокий приоритет "на всякий случай" — задача с более высоким приоритетом может голодать (starve) задачи Wi-Fi-стека, что приведёт к разрывам соединения.
- Задачи, критичные по времени (например, генерация точных импульсов) — приоритет выше, но короткие по выполнению.
- При случайных зависаниях/перезагрузках — сначала подозревай: (1) переполнение стека задачи, (2) отсутствие `vTaskDelay`, (3) гонки данных без mutex.
