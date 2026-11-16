#pragma once
#include "allocation.h"
// Host-only Arduino surface; no production header is replaced on the target.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <string>
#include <utility>

class String : public std::string {
public:
    using std::string::string;
    String() = default;
    String(const char* s) : std::string(host::failStrings()?"":s) {}
    String(const std::string& s) : std::string(s) {}
    String(std::string&& s) noexcept : std::string(std::move(s)) {}
    String(const String&) = default;
    String(String&&) noexcept = default;
    String& operator=(const String& s) {
        if (host::failStrings() || host::failStringCopies()) clear();
        else std::string::operator=(s);
        return *this;
    }
    String& operator=(const char* s) {
        std::string::operator=(host::failStrings()?"":s); return *this;
    }
    String& operator=(String&&) noexcept = default;
    using std::string::operator=;
    explicit String(int n) : std::string(std::to_string(n)) {}
    explicit String(unsigned int n) : std::string(std::to_string(n)) {}
    bool reserve(size_t n) {
        if (host::failReserve()) return false;
        try { std::string::reserve(n); return true; }
        catch (const std::bad_alloc&) { return false; }
    }
    bool isEmpty() const { return empty(); }
    void remove(size_t n) { erase(n); }
    void trim() {
        const size_t first = find_first_not_of(" \r\n\t");
        if (first == npos) { clear(); return; }
        const size_t last = find_last_not_of(" \r\n\t");
        *this = substr(first, last - first + 1);
    }
    size_t write(uint8_t c) { push_back(static_cast<char>(c)); return 1; }
    size_t write(const uint8_t* p, size_t n) {
        append(reinterpret_cast<const char*>(p), n); return n;
    }
};
inline String operator+(const String& a, const String& b) {
    return String(static_cast<const std::string&>(a) + static_cast<const std::string&>(b));
}
inline String operator+(const char* a, const String& b) { return String(a) + b; }
inline String operator+(const String& a, const char* b) { return a + String(b); }
inline bool operator==(const String& a, const String& b) {
    return static_cast<const std::string&>(a) == static_cast<const std::string&>(b);
}
inline bool operator!=(const String& a, const String& b) { return !(a == b); }
inline bool operator==(const String& a, const char* b) { return a == String(b); }
inline bool operator==(const char* a, const String& b) { return String(a) == b; }
inline bool operator!=(const String& a, const char* b) { return !(a == b); }
template <class T> T constrain(T value, T lo, T hi) {
    return std::min(hi, std::max(lo, value));
}
namespace host {
inline time_t& clock() { static time_t value = 0; return value; }
inline unsigned long& ticks() { static unsigned long value = 100000; return value; }
}
inline time_t host_time(time_t* out) {
    if (out) *out = host::clock();
    return host::clock();
}
inline unsigned long millis() { return host::ticks(); }
