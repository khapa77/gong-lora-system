#pragma once
#include <cstdlib>
#include <new>
extern "C" bool gong_allocation_forbidden;
namespace host {
__attribute__((no_sanitize("address", "undefined")))
inline bool& forbidAllocation() { return gong_allocation_forbidden; }
inline bool& fenceTriggered() { static bool value=false; return value; }
inline int& failNewAfter() { static int value=-1; return value; }
inline bool& failStrings() { static bool value=false; return value; }
inline bool& failStringCopies() { static bool value=false; return value; }
inline bool& failJsonPool() { static bool value=false; return value; }
inline bool& failReserve() { static bool value=false; return value; }
}
