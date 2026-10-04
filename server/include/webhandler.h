#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>

void   web_setup();
void   web_loop();
void   web_resetAuth();   // стереть пароль админки (команда в Serial, см. AUTH_RESET_CMD)
