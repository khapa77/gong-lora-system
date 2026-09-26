#pragma once
#include <Arduino.h>

// WiFi: собственная AP (всегда) + необязательное подключение к существующей
// сети (STA). Учётные данные STA — в WIFI_CONFIG_FILE, задаются из веб-UI.

void   wifi_setup();   // поднимает AP, mDNS и, если настроено, начинает подключение STA
void   wifi_loop();    // таймауты/повторные попытки STA — каждый проход loop()

// Сохранить сеть и сразу начать подключение. pass="" — открытая сеть.
// false — неверные параметры (пустой SSID, пароль 1..7 символов и т.п.).
bool   wifi_setCredentials(const String& ssid, const String& pass);
void   wifi_forget();  // удалить сохранённую сеть, остаться только в режиме AP

// Асинхронный скан: wifi_startScan() запускает, wifi_scanJSON() отдаёт
// {"running":true} пока идёт, затем список сетей.
bool   wifi_startScan();
String wifi_scanJSON();

String wifi_statusJSON();   // без пароля — он никогда не покидает устройство
String wifi_apIP();
