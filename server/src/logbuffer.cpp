#include "logbuffer.h"
#include <stdarg.h>
#include <string.h>

#define LOG_LINE_COUNT  64
#define LOG_LINE_LEN    120

static struct {
    char lines[LOG_LINE_COUNT][LOG_LINE_LEN];
    int  head;
    int  wrapped;   // how many total lines written (for display count)
} ring;

void logbuffer_init() {
    ring.head    = 0;
    ring.wrapped = 0;
    memset(ring.lines, 0, sizeof(ring.lines));
}

void logPrintf(const char* fmt, ...) {
    char tmp[LOG_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (len <= 0) return;
    if (len >= LOG_LINE_LEN) len = LOG_LINE_LEN - 1;
    tmp[len] = '\0';

    // Write to serial
    Serial.write((const uint8_t*)tmp, len);

    // Strip trailing newline for storage
    int slen = len;
    while (slen > 0 && (tmp[slen-1] == '\n' || tmp[slen-1] == '\r')) slen--;
    tmp[slen] = '\0';

    if (slen > 0) {
        memcpy(ring.lines[ring.head], tmp, slen + 1);
        ring.lines[ring.head][LOG_LINE_LEN - 1] = '\0';
    }

    ring.head = (ring.head + 1) % LOG_LINE_COUNT;
    ring.wrapped++;
}

String logbuffer_toJSON(int maxLines) {
    String res = "[";
    int avail = ring.wrapped < LOG_LINE_COUNT ? ring.wrapped : LOG_LINE_COUNT;
    if (maxLines > avail) maxLines = avail;

    // Read from newest (head-1) backwards
    int idx = ring.head;
    for (int i = 0; i < maxLines; i++) {
        if (idx == 0) idx = LOG_LINE_COUNT - 1;
        else idx--;
        const char* s = ring.lines[idx];
        if (s[0] == '\0') continue;

        if (res.length() > 1) res += ',';
        res += '"';
        for (; *s; s++) {
            char c = *s;
            if      (c == '"')  res += "\\\"";
            else if (c == '\\') res += "\\\\";
            else if (c == '\n') res += "\\n";
            else if (c == '\r') res += "\\r";
            else if (c >= 0x20) res += c;
        }
        res += '"';
    }

    res += ']';
    return res;
}