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
        if (!started_ || nvs_set_str(handle_,key,value.c_str())!=ESP_OK) return 0;
        return nvs_commit(handle_)==ESP_OK?value.length():0;
    }
};
