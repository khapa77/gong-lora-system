#pragma once
#include "Arduino.h"
#include "nvs.h"
// The commit boundaries mirror the installed Preferences.cpp (Arduino 2.0.16).
class Preferences {
    nvs_handle_t handle_=0;
    bool started_=false;
public:
    bool begin(const char* name,bool readOnly=false) {
        started_=nvs_open(name,readOnly?NVS_READONLY:NVS_READWRITE,&handle_)==ESP_OK;
        return started_;
    }
    void end() { if (started_) nvs_close(handle_); started_=false; }
    bool isKey(const char* key) const {
        return started_ && host::nvs().visible.count(host::nvs().full(handle_,key))!=0;
    }
    int32_t getInt(const char* key,int32_t fallback=0) const {
        int32_t value=fallback;
        if (started_) nvs_get_i32(handle_,key,&value);
        return value;
    }
    uint32_t getUInt(const char* key,uint32_t fallback=0) const {
        uint32_t value=fallback;
        if (started_) nvs_get_u32(handle_,key,&value);
        return value;
    }
    String getString(const char* key,const char* fallback="") const {
        size_t len=0;
        if (!started_ || nvs_get_str(handle_,key,nullptr,&len)!=ESP_OK) return String(fallback);
        std::vector<char> buffer(len);
        if (nvs_get_str(handle_,key,buffer.data(),&len)!=ESP_OK) return String(fallback);
        return String(buffer.data());
    }
    size_t putInt(const char* key,int32_t value) {
        if (!started_ || nvs_set_i32(handle_,key,value)!=ESP_OK) return 0;
        return nvs_commit(handle_)==ESP_OK?sizeof(value):0;
    }
    size_t putUInt(const char* key,uint32_t value) {
        if (!started_ || nvs_set_u32(handle_,key,value)!=ESP_OK) return 0;
        return nvs_commit(handle_)==ESP_OK?sizeof(value):0;
    }
    size_t putString(const char* key,const String& value) {
        if (!started_ || nvs_set_str(handle_,key,value.c_str())!=0) return 0;
        return nvs_commit(handle_)==0?value.length():0;
    }
    // Arduino Preferences stores bool as uint8; the host model has no U8 cell,
    // so it is represented as U32 with the same 0/1 semantics.
    size_t putBool(const char* key,bool value) {
        if (!started_ || nvs_set_u32(handle_,key,value?1u:0u)!=ESP_OK) return 0;
        return nvs_commit(handle_)==ESP_OK?1u:0u;
    }
    bool getBool(const char* key,bool fallback=false) const {
        uint32_t value=fallback?1u:0u;
        if (started_ && nvs_get_u32(handle_,key,&value)==ESP_OK) return value!=0;
        return fallback;
    }
    // Arduino stores uint8_t as a U8 cell; the host model keeps the same 0..255
    // range in a U32 cell so range bugs (e.g. mode > 2) are still visible.
    size_t putUChar(const char* key, uint8_t value) {
        if (!started_ || nvs_set_u32(handle_,key,value)!=ESP_OK) return 0;
        return nvs_commit(handle_)==ESP_OK?sizeof(value):0;
    }
    uint8_t getUChar(const char* key, uint8_t fallback=0) const {
        uint32_t value=fallback;
        if (started_ && nvs_get_u32(handle_,key,&value)==ESP_OK) return (uint8_t)value;
        return fallback;
    }
    bool remove(const char* key) {
        if (!started_ || !isKey(key)) return false;
        if (nvs_erase_key(handle_,key)!=ESP_OK) return false;
        return nvs_commit(handle_)==ESP_OK;
    }
    // Mirrors nvs_erase_all(): every key of THIS namespace only, committed as
    // one operation. A failed commit restores the previous visible state, just
    // like the real flash-boundary model in nvs.h.
    bool clear() {
        if (!started_) return false;
        const auto handle = host::nvs().handles.find(handle_);
        if (handle == host::nvs().handles.end()) return false;
        const std::string prefix = handle->second + "/";
        for (auto it = host::nvs().visible.begin(); it != host::nvs().visible.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) it = host::nvs().visible.erase(it);
            else ++it;
        }
        return nvs_commit(handle_)==ESP_OK;
    }
};
