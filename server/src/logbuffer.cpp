#include "logbuffer.h"
#include <stdarg.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define LOG_LINE_COUNT  64
#define LOG_LINE_LEN    120

static struct {
    char lines[LOG_LINE_COUNT][LOG_LINE_LEN];
    int  head;
    int  wrapped;   // how many total lines written (for display count)
} ring;

// logPrintf is called concurrently from loraTask (Core 0) and loopTask /
// audioFeederTask (Core 1). Without a lock, two writers can race on
// ring.head and clobber each other's slot (garbled log lines; no crash,
// indices stay bounded — but logs are exactly what you need intact when
// debugging). Serial output is serialised under the same lock so interleaved
// half-lines from two cores don't happen either.
static SemaphoreHandle_t logMtx = nullptr;

void logbuffer_init() {
    if (!logMtx) logMtx = xSemaphoreCreateMutex();
    ring.head    = 0;
    ring.wrapped = 0;
    memset(ring.lines, 0, sizeof(ring.lines));
}

// M-3: the ring buffer stores lines truncated to LOG_LINE_LEN (bounded RAM,
// fine for the on-screen debug log), but Serial used to get that SAME
// truncated copy — a ~150-char LoRa init error got cut off exactly where the
// useful part started. SERIAL_LINE_LEN is generous headroom for Serial only;
// the ring buffer's per-line budget is unchanged.
#define SERIAL_LINE_LEN 256

void logPrintf(const char* fmt, ...) {
    char tmp[SERIAL_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (len <= 0) return;
    if (len >= SERIAL_LINE_LEN) len = SERIAL_LINE_LEN - 1;
    tmp[len] = '\0';

    // Formatting is done into the local buffer above (no lock needed);
    // only the shared Serial + ring state is guarded.
    if (logMtx) xSemaphoreTake(logMtx, portMAX_DELAY);

    // Write the FULL line to serial — never truncated to the ring buffer's
    // shorter per-line budget.
    Serial.write((const uint8_t*)tmp, len);

    // Strip trailing newline, then truncate to what the ring buffer can hold.
    int slen = len;
    while (slen > 0 && (tmp[slen-1] == '\n' || tmp[slen-1] == '\r')) slen--;
    if (slen >= LOG_LINE_LEN) slen = LOG_LINE_LEN - 1;
    tmp[slen] = '\0';

    if (slen > 0) {
        memcpy(ring.lines[ring.head], tmp, slen + 1);
        ring.lines[ring.head][LOG_LINE_LEN - 1] = '\0';
    }

    ring.head = (ring.head + 1) % LOG_LINE_COUNT;
    ring.wrapped++;

    if (logMtx) xSemaphoreGive(logMtx);
}

String logbuffer_toJSON(int maxLines) {
    String res = "[";

    if (logMtx) xSemaphoreTake(logMtx, portMAX_DELAY);

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
            // char is SIGNED on Xtensa: UTF-8 continuation bytes (>= 0x80)
            // are negative and the old `c >= 0x20` test silently dropped
            // them, mangling every Cyrillic log line in /api/logs. Compare
            // as unsigned so multi-byte UTF-8 passes through intact.
            else if ((unsigned char)c >= 0x20) res += c;
        }
        res += '"';
    }

    res += ']';

    if (logMtx) xSemaphoreGive(logMtx);
    return res;
}
