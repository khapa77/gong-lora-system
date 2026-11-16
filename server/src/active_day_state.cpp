#include "active_day_state.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

namespace active_day {
namespace {
struct Handle {
    nvs_handle_t value = 0;
    bool opened = false;
    ~Handle() { if (opened) nvs_close(value); }
    esp_err_t open(nvs_open_mode_t mode) {
        const esp_err_t error = nvs_open("gong", mode, &value);
        opened = error == ESP_OK;
        return error;
    }
};

bool readSchema(nvs_handle_t handle, bool& present, esp_err_t& error) {
    uint32_t schema = 0;
    error = nvs_get_u32(handle, "active_schema", &schema);
    present = error == ESP_OK;
    if (error == ESP_ERR_NVS_NOT_FOUND) return true;
    if (error == ESP_OK && schema == 1) return true;
    if (error == ESP_OK) error = ESP_ERR_NVS_TYPE_MISMATCH;
    return false;
}
}  // namespace

bool validDate(uint32_t date) {
    if (!date) return true;
    const unsigned year = date / 10000;
    const unsigned month = date / 100 % 100;
    const unsigned day = date % 100;
    // Four-digit dates, with the scheduler's existing valid-year threshold.
    if (year < 2024 || year > 9999 || month < 1 || month > 12 || day < 1) return false;
    static const uint8_t lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned limit = lengths[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ++limit;
    return day <= limit;
}

bool parseDate(const char* text, uint32_t& date) {
    if (!text) return false;
    if (!text[0]) { date = 0; return true; }
    if (strlen(text) != 10 || text[4] != '-' || text[7] != '-') return false;
    uint32_t value = 0;
    for (unsigned i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<unsigned>(text[i] - '0');
    }
    if (!value || !validDate(value)) return false;
    date = value;
    return true;
}

void formatDate(uint32_t date, char (&out)[11]) {
    if (!date) { out[0] = '\0'; return; }
    snprintf(out, sizeof(out), "%04u-%02u-%02u", unsigned(date / 10000),
             unsigned(date / 100 % 100), unsigned(date % 100));
}

bool encode(const Record& record, uint8_t dayCount, uint64_t& raw) {
    if (record.day >= dayCount || !validDate(record.date)) return false;
    raw = (uint64_t(1) << 56) | (uint64_t(record.day) << 48) | record.date;
    return true;
}

bool decode(uint64_t raw, uint8_t dayCount, Record& record) {
    if ((raw >> 56) != 1 || ((raw >> 32) & 0xffff) != 0) return false;
    Record candidate;
    candidate.day = uint8_t(raw >> 48);
    candidate.date = uint32_t(raw);
    if (candidate.day >= dayCount || !validDate(candidate.date)) return false;
    record = candidate;
    return true;
}

bool Store::classify(int error) {
    status_ = (error == ESP_ERR_NVS_TYPE_MISMATCH || error == ESP_ERR_NVS_INVALID_LENGTH)
        ? Status::Invalid : Status::Unknown;
    phase_ = Phase::Idle;
    return false;
}

FilePresence probeFile(const char* path) {
    // main.cpp mounts LittleFS at Arduino 2.0.16's default /littlefs.
    // Check the mount root too: VFS ENOENT for an unmounted path is not absence.
    struct stat info;
    if (::stat("/littlefs/", &info) != 0 || !S_ISDIR(info.st_mode)) return FilePresence::Error;
    char full[40];
    const int length = snprintf(full, sizeof(full), "/littlefs%s", path);
    if (length < 0 || size_t(length) >= sizeof(full)) return FilePresence::Error;
    if (::stat(full, &info) == 0) return S_ISREG(info.st_mode) ? FilePresence::Present : FilePresence::Error;
    return errno == ENOENT ? FilePresence::ConfirmedAbsent : FilePresence::Error;
}

Source Store::readLegacyFile(bool filesystemAvailable, Record& record) {
    if (!filesystemAvailable) { unknown(); return Source::Error; }
    const FilePresence presence = probeFile("/activeday.conf");
    if (presence == FilePresence::Error) { unknown(); return Source::Error; }
    if (presence == FilePresence::ConfirmedAbsent) {
        status_ = Status::Empty;
        return Source::Empty;
    }
    File file = LittleFS.open("/activeday.conf", "r");
    if (!file) { unknown(); return Source::Error; }
    DynamicJsonDocument doc(192);
    if (!doc.capacity()) { file.close(); unknown(); return Source::Error; }
    const DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error || !doc.is<JsonObject>() || !doc["day"].is<int>()) {
        status_ = Status::Invalid;
        return Source::Error;
    }
    const int day = doc["day"].as<int>();
    const bool hasDate = doc.containsKey("date");
    const JsonString jsonDate = doc["date"].as<JsonString>();
    const char* date = hasDate ? jsonDate.c_str() : "";
    // JsonString retains the decoded length, including any embedded NUL.
    if (day < 0 || day >= dayCount_ || !date ||
        (hasDate && jsonDate.size() != strlen(date)) || !parseDate(date, record.date)) {
        status_ = Status::Invalid;
        return Source::Error;
    }
    record.day = uint8_t(day);
    status_ = Status::Loaded;
    return Source::Legacy;
}

Source Store::load(bool filesystemAvailable, Record& record) {
    if (status_ != Status::Boot) return Source::Error;
    Handle handle;
    esp_err_t error = handle.open(NVS_READONLY);
    if (error == ESP_ERR_NVS_NOT_FOUND) return readLegacyFile(filesystemAvailable, record);
    if (error != ESP_OK) { classify(error); return Source::Error; }
    if (!readSchema(handle.value, schemaPresent_, error)) {
        classify(error); return Source::Error;
    }
    uint64_t raw = 0;
    error = nvs_get_u64(handle.value, "active", &raw);
    if (error == ESP_OK) {
        if (!decode(raw, dayCount_, record)) { status_ = Status::Invalid; return Source::Error; }
        status_ = Status::Loaded;
        return Source::Active;
    }
    if (error != ESP_ERR_NVS_NOT_FOUND) { classify(error); return Source::Error; }
    if (schemaPresent_) { unknown(); return Source::Error; }

    int32_t day = -1;
    const esp_err_t dayError = nvs_get_i32(handle.value, "aday", &day);
    if (dayError != ESP_OK && dayError != ESP_ERR_NVS_NOT_FOUND) {
        classify(dayError); return Source::Error;
    }
    char date[11] = {};
    size_t length = sizeof(date);
    const esp_err_t dateError = nvs_get_str(handle.value, "adate", date, &length);
    if (dateError != ESP_OK && dateError != ESP_ERR_NVS_NOT_FOUND) {
        classify(dateError); return Source::Error;
    }
    if (dayError == ESP_ERR_NVS_NOT_FOUND) {
        if (dateError == ESP_OK) { status_ = Status::Invalid; return Source::Error; }
        return readLegacyFile(filesystemAvailable, record);
    }
    if (day < 0 || day >= dayCount_ || !parseDate(date, record.date)) {
        status_ = Status::Invalid;
        return Source::Error;
    }
    record.day = uint8_t(day);
    status_ = Status::Loaded;
    return Source::Legacy;
}

bool Store::readActive(Record& record, uint64_t& raw) {
    Handle handle;
    esp_err_t error = handle.open(NVS_READONLY);
    if (error != ESP_OK) return classify(error);
    if (!readSchema(handle.value, schemaPresent_, error)) return classify(error);
    error = nvs_get_u64(handle.value, "active", &raw);
    if (error != ESP_OK) return classify(error);
    if (!decode(raw, dayCount_, record)) { status_ = Status::Invalid; return false; }
    return true;
}

bool Store::persist(const Record& record, bool recovery) {
    if (status_ != Status::Ready && status_ != Status::Loaded && status_ != Status::Empty &&
        !(recovery && status_ == Status::Unknown)) return false;
    uint64_t expected = 0;
    if (!encode(record, dayCount_, expected)) return false;
    Handle handle;
    if (handle.open(NVS_READWRITE) != ESP_OK) { unknown(); return false; }
    if (!schemaPresent_) {
        phase_ = Phase::SchemaSet;
        if (nvs_set_u32(handle.value, "active_schema", 1) != ESP_OK) { unknown(); return false; }
        phase_ = Phase::SchemaCommitted;
        if (nvs_commit(handle.value) != ESP_OK) { unknown(); return false; }
        uint32_t schema = 0;
        if (nvs_get_u32(handle.value, "active_schema", &schema) != ESP_OK || schema != 1) {
            unknown(); return false;
        }
        schemaPresent_ = true;
    }
    phase_ = Phase::Prepared;
    if (nvs_set_u64(handle.value, "active", expected) != ESP_OK) { unknown(); return false; }
    phase_ = Phase::SetAcked;
    if (nvs_commit(handle.value) != ESP_OK) { unknown(); return false; }
    phase_ = Phase::CommitAcked;
    uint64_t actual = 0;
    if (nvs_get_u64(handle.value, "active", &actual) != ESP_OK || actual != expected) {
        unknown(); return false;
    }
    phase_ = Phase::Verified;
    return true;
}

}  // namespace active_day
