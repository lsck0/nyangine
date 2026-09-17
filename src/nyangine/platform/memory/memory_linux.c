#include <sys/mman.h>
#include <unistd.h>

#include "nyangine/nyangine.h"

// glibc only declares it for _DEFAULT_SOURCE, which base_basic.h's strict POSIX request turns off.
extern int mincore(void* address, size_t length, unsigned char* vector);

/** How many pages one mincore call inspects, so the vector fits on the stack. */
#define _NYA_MEMORY_MINCORE_BATCH 1024

u64 nya_memory_page_size(void) {
    return (u64)sysconf(_SC_PAGESIZE);
}

void* nya_memory_reserve(u64 size) {
    nya_assert(size > 0);

    // no access and no swap reservation: nothing is charged until nya_memory_commit.
    void* address = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    return address == MAP_FAILED ? nullptr : address;
}

b8 nya_memory_commit(void* address, u64 size) {
    nya_assert(address != nullptr);

    u64 page  = nya_memory_page_size();
    u64 start = (u64)(uintptr_t)address & ~(page - 1);
    u64 end   = ((u64)(uintptr_t)address + size + page - 1) & ~(page - 1);

    return mprotect((void*)(uintptr_t)start, end - start, PROT_READ | PROT_WRITE) == 0;
}

void nya_memory_release(void* address, u64 size) {
    if (address == nullptr) return;

    s32 result = munmap(address, size);
    nya_assert(result == 0, "munmap() failed for a reservation of " FMTu64 " bytes", size);
}

u64 nya_memory_resident_bytes(const void* address, u64 size) {
    if (address == nullptr || size == 0) return 0;

    u64 page  = nya_memory_page_size();
    u64 start = (u64)(uintptr_t)address & ~(page - 1);
    u64 end   = ((u64)(uintptr_t)address + size + page - 1) & ~(page - 1);

    u64 resident = 0;

    for (u64 at = start; at < end;) {
        u64 pages = nya_min((end - at) / page, (u64)_NYA_MEMORY_MINCORE_BATCH);

        unsigned char in_core[_NYA_MEMORY_MINCORE_BATCH];
        if (mincore((void*)(uintptr_t)at, pages * page, in_core) != 0) return resident;

        for (u64 i = 0; i < pages; i++) resident += (in_core[i] & 1U) * page;

        at += pages * page;
    }

    return resident;
}

u64 nya_memory_process_resident_bytes(void) {
    // statm's second field is the resident set in pages.
    FILE* statm = fopen("/proc/self/statm", "r");
    if (statm == nullptr) return 0;

    unsigned long long total_pages    = 0;
    unsigned long long resident_pages = 0;

    s32 read = fscanf(statm, "%llu %llu", &total_pages, &resident_pages);
    (void)fclose(statm);

    return read == 2 ? (u64)resident_pages * nya_memory_page_size() : 0;
}
