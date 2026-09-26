#pragma once
#include <Arduino.h>

// Физическая кнопка на BUTTON_PIN (config.h), срабатывает по удержанию:
// в тишине BUTTON_PLAY_HOLD_MS — гонг, во время звука BUTTON_STOP_HOLD_MS — стоп.
// Опрос из controlTask (раз в 10 мс), без прерываний: дребезг контактов в
// ISR дал бы пачку ложных событий.

void button_setup();
void button_loop();

// true — кнопка была зажата с включения и продержалась holdMs (блокирует
// setup() на это время, только если кнопка действительно зажата).
bool button_resetHeldAtBoot(uint32_t holdMs);
