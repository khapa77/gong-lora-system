#pragma once
#include <cstddef>
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

// ── Heap budget model ─────────────────────────────────────────────────────
// A test that wants to reproduce "no PSRAM" on the device caps the bytes the
// C++ allocations inside the code under test may use. The per-test operator
// new/delete charge and release through here, so peak usage is real and an
// over-budget allocation throws std::bad_alloc exactly like the device.
struct HeapBudget {
    std::size_t limit;   // 0 = unlimited
    std::size_t used;
    std::size_t peak;
};

inline HeapBudget& heapBudget() {
    static HeapBudget value = {0, 0, 0};
    return value;
}

inline void heapBudgetReset(std::size_t limit) {
    heapBudget().limit = limit;
    heapBudget().used = 0;
    heapBudget().peak = 0;
}

// Returns false when the allocation must fail.
inline bool heapBudgetCharge(std::size_t bytes) {
    HeapBudget& h = heapBudget();
    if (h.limit != 0 && h.used + bytes > h.limit) return false;
    h.used += bytes;
    if (h.used > h.peak) h.peak = h.used;
    return true;
}

inline void heapBudgetRelease(std::size_t bytes) {
    HeapBudget& h = heapBudget();
    if (bytes > h.used) { h.used = 0; return; }
    h.used -= bytes;
}
}
