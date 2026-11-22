#pragma once
#include "Arduino.h"
#include <map>
#include <functional>
#include <cerrno>
namespace host {
struct FsFaults {
    bool failRead=false, failWrite=false, shortWrite=false;
    std::string path;
    std::string renameErrorPath, removeErrorPath;
    int statError=0;
    std::string statErrorPath, directoryPath;
    bool rootMissing=false;
    unsigned writes=0, removes=0, renames=0;
    std::function<void()> afterReadOpen;
    bool matches(const char* name) const { return path.empty() || path==name; }
};
inline FsFaults& fs() { static FsFaults value; return value; }
inline std::map<std::string,std::string>& files() {
    static std::map<std::string,std::string> value; return value;
}
}
class File {
    std::string* data_=nullptr;
    size_t pos_=0;
public:
    File()=default;
    explicit File(std::string* data) : data_(data) {}
    explicit operator bool() const { return data_!=nullptr; }
    size_t size() const { return data_?data_->size():0; }
    int read() {
        return data_ && pos_<data_->size()?static_cast<unsigned char>((*data_)[pos_++]):-1;
    }
    size_t readBytes(char* out,size_t n) {
        const size_t actual=data_?std::min(n,data_->size()-pos_):0;
        if (actual) { std::memcpy(out,data_->data()+pos_,actual); pos_+=actual; }
        return actual;
    }
    size_t write(uint8_t c) {
        if (!data_) return 0;
        data_->push_back(static_cast<char>(c)); return 1;
    }
    size_t write(const uint8_t* p,size_t n) {
        if (!data_) return 0;
        ++host::fs().writes;
        if (host::fs().shortWrite && n) --n;
        data_->append(reinterpret_cast<const char*>(p),n); return n;
    }
    size_t print(const String& value) {
        return write(reinterpret_cast<const uint8_t*>(value.data()),value.size());
    }
    String readString() {
        if (!data_) return String();
        String value(data_->substr(pos_)); pos_=data_->size(); return value;
    }
    void close() { data_=nullptr; }
};
class HostLittleFS {
public:
    bool exists(const char* path) const {
        if ((host::fs().failRead && host::fs().matches(path)) ||
            (host::fs().statError && (host::fs().statErrorPath.empty() || host::fs().statErrorPath==path))) return false;
        return host::files().count(path)!=0;
    }
    File open(const char* path,const char* mode) {
        auto& files=host::files();
        if (std::strcmp(mode,"w")==0) {
            if (host::fs().failWrite && host::fs().matches(path)) return File();
            files[path].clear(); return File(&files[path]);
        }
        if (host::fs().failRead && host::fs().matches(path)) return File();
        const auto it=files.find(path);
        if (it!=files.end() && host::fs().afterReadOpen) host::fs().afterReadOpen();
        return it==files.end()?File():File(&it->second);
    }
    bool remove(const char* path) {
        ++host::fs().removes;
        if (host::fs().removeErrorPath==path) return false;
        return host::files().erase(path)!=0;
    }
    bool rename(const char* from,const char* to) {
        ++host::fs().renames;
        if (host::fs().renameErrorPath==from) return false;
        auto& files=host::files(); const auto it=files.find(from);
        if (it==files.end()) return false;
        files[to]=std::move(it->second); files.erase(it); return true;
    }
    bool mkdir(const char* /*path*/) { return true; }
    bool rmdir(const char* /*path*/) { return true; }
};
static HostLittleFS LittleFS;
