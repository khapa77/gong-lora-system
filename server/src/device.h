#pragma once
#include <Arduino.h>

// Номер и подпись устройства — чтобы несколько гонгов рядом не путались.
//
//   номер 1–99  → Wi-Fi "GongServer-<N>", http://gong<N>.local
//   номер не задан (0) → "GongServer-<4 hex MAC>", http://gong<4 hex>.local —
//                        два новых устройства не появятся под одним именем
//   подпись (≤ DEVICE_NAME_MAX байт, например "Зал") — только в веб-интерфейсе
//
// Хранятся в NVS "device", задаются в веб-интерфейсе (карточка Device).
// Смена номера меняет имя сети — устройство перезагружается само.

void          device_setup();   // до wifi_setup(): из NVS (или -DDEVICE_NUM)
void          device_loop();    // из loop(): отложенная перезагрузка после смены номера

uint8_t       device_num();     // 0 — не задан
const String& device_name();    // подпись, может быть пустой
const String& device_ssid();    // имя точки доступа
const String& device_host();    // имя mDNS без ".local"

// false — номер вне 0–99. Смена номера — перезагрузка через пару секунд
// (*reboot = true), чтобы ответ успел уйти в браузер.
bool          device_set(uint8_t num, const String& name, bool* reboot);
String        device_toJSON();
