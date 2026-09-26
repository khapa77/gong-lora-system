#pragma once

// Физическая кнопка на BUTTON_PIN (config.h), срабатывает по удержанию:
// в тишине BUTTON_PLAY_HOLD_MS — гонг, во время звука BUTTON_STOP_HOLD_MS — стоп.
// Опрос из loop(), без прерываний: loop() крутится раз в ~1 мс, а дребезг
// контактов в ISR дал бы пачку ложных событий.

void button_setup();
void button_loop();
