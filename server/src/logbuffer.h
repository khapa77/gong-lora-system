#pragma once
#include <Arduino.h>

void   logbuffer_init();
void   logPrintf(const char* fmt, ...)  __attribute__((format(printf, 1, 2)));
String logbuffer_toJSON(int maxLines = 40);