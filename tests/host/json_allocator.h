#pragma once
#include "Arduino.h"
#include <ArduinoJson.h>

// Keep ArduinoJson's real BasicJsonDocument/parser. Only its external pool
// allocator is injectable; production sources and installed headers are intact.
namespace host {
struct JsonAllocator {
    void* allocate(size_t n) {
        if (forbidAllocation()) std::abort();
        return failJsonPool()?nullptr:std::malloc(n);
    }
    void deallocate(void* p) { std::free(p); }
    void* reallocate(void* p,size_t n) {
        if (forbidAllocation()) std::abort();
        return failJsonPool()?nullptr:std::realloc(p,n);
    }
};
}
using HostDynamicJsonDocument = ArduinoJson::BasicJsonDocument<host::JsonAllocator>;
#define DynamicJsonDocument HostDynamicJsonDocument
