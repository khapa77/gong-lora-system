# Bug Report: LoRa init failure — wrong unit conversion

**Branch:** time-to-time  
**Commit that introduced bug:** c357874 ("New idea")  
**Fix commit:** b6c5faa

---

## Symptom

Web UI показывает **"No clients seen yet"** — подключённые клиенты не появляются на вкладке Clients.

---

## Root Cause

В коммите c357874 значения в `config.h` были изменены с Hz-единиц на MHz/kHz:

```c
// ДО (Hz)
#define LORA_FREQ  433E6    // Гц
#define LORA_BW    125E3    // Гц

// ПОСЛЕ (MHz/kHz)
#define LORA_FREQ  433.0    // МГц
#define LORA_BW    125.0    // кГц
```

Но код в `lora_setup()` **обеих** прошивок по-прежнему делил эти значения:

```cpp
float freqMHz = (float)(LORA_FREQ / 1e6);  // 433.0 / 1e6 = 0.000433 MHz ← невалидно
float bwKHz   = (float)(LORA_BW  / 1e3);   // 125.0 / 1e3 = 0.125 kHz   ← невалидно
```

RadioLib (`SX1278::begin()`) принимает частоту в МГц и полосу в кГц. Оба параметра оказались вне допустимого диапазона:

| Параметр | Ожидается RadioLib | Передавалось | Результат |
|---|---|---|---|
| Частота | 410–525 MHz | 0.000433 MHz | `RADIOLIB_ERR_INVALID_FREQUENCY` |
| Полоса | 7.8–500 kHz | 0.125 kHz | `RADIOLIB_ERR_INVALID_BANDWIDTH` |

`lora_setup()` печатал `[LORA] Init FAILED` в Serial и выходил, не вызывая `radio.startReceive()`. LoRa полностью не работал на сервере и клиенте.

---

## Fix

Убрано ненужное деление — значения конфига уже в правильных единицах для RadioLib:

```cpp
// server/src/lorahandler.cpp  и  client/src/lorahandler.cpp
float freqMHz = (float)LORA_FREQ;   // MHz — передаётся напрямую
float bwKHz   = (float)LORA_BW;     // kHz — передаётся напрямую
```

Также исправлен комментарий в `config.h`: `// Bandwidth Hz` → `// Bandwidth kHz`.

---

## Affected files

- `server/src/lorahandler.cpp`
- `client/src/lorahandler.cpp`
- `server/src/config.h` (комментарий)
- `client/src/config.h` (комментарий)

---

## After fix

После перепрошивки сервера и клиента (`pio run -t upload`):
1. Serial выводит `[LORA] Server ready @ 433 MHz  SF=7 BW=125k`
2. Клиент получает HEARTBEAT каждые 30 с, отвечает ACK
3. Сервер регистрирует клиента → он появляется в web UI
