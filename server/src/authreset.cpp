#include "authreset.h"
#include "config.h"
#include "logbuffer.h"

#include <LittleFS.h>
#include <Preferences.h>

bool authreset_eraseCredentials() {
    bool ok = false;

    Preferences prefs;
    if (prefs.begin(AUTH_NVS_NAMESPACE, false)) {
        ok = prefs.clear();
        prefs.end();
    }

    // Устаревший файл импорта: если оставить, следующий loadAuth() втянет
    // старый хеш обратно и сброс окажется мнимым.
    const bool hadFile = LittleFS.exists(AUTH_CONFIG_FILE);
    bool fileRemoved = true;
    if (hadFile && !LittleFS.remove(AUTH_CONFIG_FILE)) fileRemoved = false;

    logPrintf("[AUTH] Credentials erased: NVS %s=%s, %s %s\n",
              AUTH_NVS_NAMESPACE,
              ok ? "cleared" : "CLEAR FAILED",
              AUTH_CONFIG_FILE,
              hadFile ? (fileRemoved ? "removed" : "REMOVE FAILED") : "absent");

    // Неудалённый legacy-файл делает сброс мнимым: loadAuth() при пустом NVS
    // импортирует его обратно. Поэтому это ошибка, а не предупреждение.
    return ok && fileRemoved;
}
