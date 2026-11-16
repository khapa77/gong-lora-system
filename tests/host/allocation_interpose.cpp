// macOS host-only allocator fence, loaded into test subprocesses explicitly.
// dyld leaves calls from this interposer to the original allocator uninterposed.
#include <cstdlib>
extern "C" bool gong_allocation_forbidden;
static void checkAllocation() { if (gong_allocation_forbidden) std::abort(); }
static void* checkedMalloc(size_t n) { checkAllocation(); return std::malloc(n); }
static void* checkedCalloc(size_t a,size_t b) { checkAllocation(); return std::calloc(a,b); }
static void* checkedRealloc(void* p,size_t n) { checkAllocation(); return std::realloc(p,n); }
#define INTERPOSE(replacement, original) \
    __attribute__((used)) static struct { const void* replacement; const void* original; } \
    interpose_##original __attribute__((section("__DATA,__interpose"))) = \
        { reinterpret_cast<const void*>(&replacement), reinterpret_cast<const void*>(&original) }
INTERPOSE(checkedMalloc, malloc);
INTERPOSE(checkedCalloc, calloc);
INTERPOSE(checkedRealloc, realloc);
