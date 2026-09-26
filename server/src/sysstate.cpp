#include "sysstate.h"
#include "logbuffer.h"
#include <Preferences.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t stateMtx = nullptr;
static bool        fsOk        = false;
static const char* resetReason = "unknown";
static uint32_t    crashCount  = 0;

void sys_lock()   { if (stateMtx) xSemaphoreTakeRecursive(stateMtx, portMAX_DELAY); }
void sys_unlock() { if (stateMtx) xSemaphoreGiveRecursive(stateMtx); }

static const char* reasonName(esp_reset_reason_t r, bool& crash) {
    crash = false;
    switch (r) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_EXT:       return "external reset";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT:  crash = true; return "brownout (power dip)";
        case ESP_RST_PANIC:     crash = true; return "crash (panic)";
        case ESP_RST_INT_WDT:   crash = true; return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  crash = true; return "task watchdog (hang)";
        case ESP_RST_WDT:       crash = true; return "watchdog";
        default:                return "unknown";
    }
}

void sys_setup() {
    stateMtx = xSemaphoreCreateRecursiveMutex();

    // Core dump стирается при каждой загрузке (main.cpp), логи живут в RAM —
    // без этого утром было не узнать, что ночью плата падала.
    bool crash;
    resetReason = reasonName(esp_reset_reason(), crash);
    Preferences p;
    p.begin("sys", false);
    crashCount = p.getUInt("crashes", 0);
    if (crash) p.putUInt("crashes", ++crashCount);
    p.end();
    logPrintf("[SYS] Reset reason: %s (crash count %u)\n", resetReason, (unsigned)crashCount);
}

void        sys_setFsOk(bool ok) { fsOk = ok; }
bool        sys_fsOk()           { return fsOk; }
const char* sys_resetReason()    { return resetReason; }
uint32_t    sys_crashCount()     { return crashCount; }
