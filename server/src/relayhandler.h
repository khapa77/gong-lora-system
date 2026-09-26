#pragma once
#include <Arduino.h>

// Реле питания внешнего усилителя / трансляционной линии (RELAY_PIN, config.h).
//
// Все запуски звука (расписание, веб-кнопка) идут через relay_play(), а не
// напрямую в mp3_play(): в режиме AUTO реле сначала включается, и звук
// стартует только через preMs — иначе усилитель ещё не вышел на режим и
// начало удара гонга срезается. После окончания трека реле держится holdMs
// (паузы между повторами loop не должны щёлкать реле) и отключается.
//
// Режимы:
//   AUTO — реле включено только на время гонга (по умолчанию)
//   ON   — всегда включено (ручное управление из веб-интерфейса)
//   OFF  — всегда выключено; гонг звучит только в локальном динамике MAX98357A
//
// Вызывается только из loopTask (loop(), веб-обработчики, sched_check()) —
// блокировок не нужно.

enum class RelayMode : uint8_t { AUTO, ON, OFF };

void   relay_setup();    // до первого relay_play(); читает RELAY_CONFIG_FILE
void   relay_loop();     // каждый проход loop()

void   relay_play(uint8_t track, uint8_t vol, uint8_t loop);
void   relay_stop();     // остановить звук и отменить отложенный старт

bool   relay_setMode(RelayMode mode);
bool   relay_setTiming(uint32_t preMs, uint32_t holdMs);
bool   relay_parseMode(const String& s, RelayMode& out);

bool     relay_isOn();
uint32_t relay_preMs();
uint32_t relay_holdMs();
String   relay_toJSON();
