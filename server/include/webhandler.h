#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <SPIFFS.h>

void   web_setup();
void   web_loop();

bool   wifi_connect();    // returns true if STA connected
void   wifi_startAP();

extern bool apMode;
