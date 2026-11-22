#pragma once
// Logical NVS fault model, NOT an emulation of Espressif flash pages or CRCs.
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using esp_err_t = int;
using nvs_handle_t = unsigned int;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 0x1103;
constexpr esp_err_t ESP_ERR_NVS_NOT_ENOUGH_SPACE = 0x1105;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
namespace host {
enum class Type { I32, U32, U64, Text };
struct Cell {
    Type type = Type::I32;
    uint64_t number = 0;
    std::string text;
    static Cell integer(Type t, uint64_t n) { Cell c; c.type=t; c.number=n; return c; }
    static Cell string(const std::string& s) { Cell c; c.type=Type::Text; c.text=s; return c; }
};
struct PowerCut : std::exception {
    const char* what() const noexcept override { return "injected power cut"; }
};
enum class Action { None, ErrorBefore, ErrorDurable, CutBefore, CutAfter, ReadMismatch };
struct Fault {
    std::string operation, key;
    Action action = Action::None;
    bool armed = false;
    unsigned skip = 0;
};
struct NvsModel {
    std::map<std::string, Cell> visible, durable;
    std::map<nvs_handle_t, std::string> handles;
    nvs_handle_t nextHandle = 1;
    std::string lastKey;
    Fault fault;
    std::vector<std::string> trace;
    std::function<void()> beforeWrite;
    std::function<void(const std::string&,const std::string&)> beforeOperation;
    std::function<void(const char*)> afterU64Read;
    bool namespacePresent = false;
    unsigned int writes = 0, commits = 0, reads = 0;
    void seed(const std::string& key, const Cell& cell) {
        namespacePresent = true;
        visible["gong/"+key]=cell; durable["gong/"+key]=cell;
    }
    void inject(const std::string& operation, const std::string& key, Action action, unsigned skip=0) {
        fault.operation=operation; fault.key=key; fault.action=action; fault.armed=true;
        fault.skip=skip;
    }
    Action event(const std::string& operation, const std::string& key) {
        trace.push_back(operation+":"+key);
        if (beforeOperation) beforeOperation(operation,key);
        // Same logical boundary for legacy aday and the proposed active record.
        const bool matchingKey = fault.key==key ||
            (fault.key=="@state" && (key=="aday" || key=="active"));
        if (fault.armed && fault.operation==operation && matchingKey) {
            if (fault.skip) { --fault.skip; return Action::None; }
            fault.armed=false; return fault.action;
        }
        return Action::None;
    }
    void reboot() {
        visible=durable; handles.clear(); nextHandle=1; lastKey.clear();
        beforeWrite=nullptr; beforeOperation=nullptr; afterU64Read=nullptr; fault.armed=false;
    }
    std::string full(nvs_handle_t handle, const char* key) const {
        return handles.at(handle)+"/"+key;
    }
};
inline NvsModel& nvs() { static NvsModel model; return model; }
inline esp_err_t set(nvs_handle_t h, const char* key, const Cell& cell) {
    NvsModel& m=nvs(); ++m.writes;
    if (m.beforeWrite) m.beforeWrite();
    const Action a=m.event("set",key);
    if (a==Action::CutBefore) throw PowerCut();
    if (a==Action::ErrorBefore) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    m.lastKey=key; m.visible[m.full(h,key)]=cell;
    if (a==Action::ErrorDurable) {
        m.durable[m.full(h,key)]=cell; return ESP_FAIL;
    }
    if (a==Action::CutAfter) throw PowerCut();
    return ESP_OK;
}
inline esp_err_t get(nvs_handle_t h, const char* key, Type type, Cell& out) {
    NvsModel& m=nvs(); ++m.reads;
    const Action a=m.event("get",key);
    if (a==Action::CutBefore) throw PowerCut();
    if (a==Action::ErrorBefore) return ESP_FAIL;
    const auto it=m.visible.find(m.full(h,key));
    if (it==m.visible.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (it->second.type!=type) return ESP_ERR_NVS_TYPE_MISMATCH;
    out=it->second;
    if (a==Action::CutAfter) throw PowerCut();
    if (a==Action::ReadMismatch) out.number^=1;
    return ESP_OK;
}
}
inline esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* h) {
    host::NvsModel& m=host::nvs();
    if (m.event("open",name)==host::Action::ErrorBefore) return ESP_FAIL;
    if (mode==NVS_READONLY && !m.namespacePresent) return ESP_ERR_NVS_NOT_FOUND;
    if (mode==NVS_READWRITE) m.namespacePresent=true;
    *h=m.nextHandle++; m.handles[*h]=name; return ESP_OK;
}
inline void nvs_close(nvs_handle_t h) { host::nvs().handles.erase(h); }
inline esp_err_t nvs_commit(nvs_handle_t) {
    host::NvsModel& m=host::nvs(); ++m.commits;
    const host::Action a=m.event("commit",m.lastKey);
    if (a==host::Action::CutBefore) throw host::PowerCut();
    if (a==host::Action::ErrorBefore) { m.visible=m.durable; return ESP_FAIL; }
    m.durable=m.visible;
    if (a==host::Action::ErrorDurable) return ESP_FAIL;
    if (a==host::Action::CutAfter) throw host::PowerCut();
    return ESP_OK;
}
inline esp_err_t nvs_set_i32(nvs_handle_t h,const char* k,int32_t v) {
    return host::set(h,k,host::Cell::integer(host::Type::I32,static_cast<uint32_t>(v)));
}
inline esp_err_t nvs_set_u32(nvs_handle_t h,const char* k,uint32_t v) {
    return host::set(h,k,host::Cell::integer(host::Type::U32,v));
}
inline esp_err_t nvs_set_u64(nvs_handle_t h,const char* k,uint64_t v) {
    return host::set(h,k,host::Cell::integer(host::Type::U64,v));
}
inline esp_err_t nvs_set_str(nvs_handle_t h,const char* k,const char* v) {
    return host::set(h,k,host::Cell::string(v));
}
inline esp_err_t nvs_erase_key(nvs_handle_t h,const char* k) {
    host::NvsModel& m=host::nvs();
    const host::Action a=m.event("erase",k);
    if (a==host::Action::CutBefore) throw host::PowerCut();
    if (a==host::Action::ErrorBefore) return ESP_FAIL;
    m.visible.erase(m.full(h,k));
    if (a==host::Action::CutAfter) throw host::PowerCut();
    return ESP_OK;
}
inline esp_err_t nvs_get_i32(nvs_handle_t h,const char* k,int32_t* v) {
    host::Cell c; const esp_err_t e=host::get(h,k,host::Type::I32,c);
    if (e==ESP_OK) *v=static_cast<int32_t>(c.number);
    return e;
}
inline esp_err_t nvs_get_u32(nvs_handle_t h,const char* k,uint32_t* v) {
    host::Cell c; const esp_err_t e=host::get(h,k,host::Type::U32,c);
    if (e==ESP_OK) *v=static_cast<uint32_t>(c.number);
    return e;
}
inline esp_err_t nvs_get_u64(nvs_handle_t h,const char* k,uint64_t* v) {
    host::Cell c; const esp_err_t e=host::get(h,k,host::Type::U64,c);
    if (e==ESP_OK) *v=c.number;
    if (e==ESP_OK && host::nvs().afterU64Read) host::nvs().afterU64Read(k);
    return e;
}
inline esp_err_t nvs_get_str(nvs_handle_t h,const char* k,char* out,size_t* len) {
    host::Cell c; const esp_err_t e=host::get(h,k,host::Type::Text,c);
    if (e!=ESP_OK) return e;
    const size_t required=c.text.size()+1;
    if (!out) { *len=required; return ESP_OK; }
    if (*len<required) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(out,c.text.c_str(),required); *len=required; return ESP_OK;
}
