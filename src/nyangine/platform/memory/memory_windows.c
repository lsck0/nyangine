#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include "nyangine/nyangine.h"

/** How many pages one working set query inspects, so the buffer fits on the stack. */
#define _NYA_MEMORY_WORKING_SET_BATCH 256

u64 nya_memory_page_size(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);

    return (u64)info.dwPageSize;
}

void* nya_memory_reserve(u64 size) {
    nya_assert(size > 0);

    return VirtualAlloc(nullptr, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
}

b8 nya_memory_commit(void* address, u64 size) {
    nya_assert(address != nullptr);

    // VirtualAlloc rounds the range out to whole pages itself.
    return VirtualAlloc(address, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void nya_memory_release(void* address, u64 size) {
    nya_unused(size);

    if (address == nullptr) return;

    BOOL released = VirtualFree(address, 0, MEM_RELEASE);
    nya_assert(released, "VirtualFree() failed for a reservation of " FMTu64 " bytes", size);
}

u64 nya_memory_resident_bytes(const void* address, u64 size) {
    if (address == nullptr || size == 0) return 0;

    u64 page  = nya_memory_page_size();
    u64 start = (u64)(uintptr_t)address & ~(page - 1);
    u64 end   = ((u64)(uintptr_t)address + size + page - 1) & ~(page - 1);

    HANDLE process  = GetCurrentProcess();
    u64    resident = 0;

    for (u64 at = start; at < end;) {
        u64 pages = nya_min((end - at) / page, (u64)_NYA_MEMORY_WORKING_SET_BATCH);

        PSAPI_WORKING_SET_EX_INFORMATION entries[_NYA_MEMORY_WORKING_SET_BATCH];
        for (u64 i = 0; i < pages; i++) entries[i].VirtualAddress = (PVOID)(uintptr_t)(at + (i * page));

        // the K32 name lives in kernel32, so this needs no psapi import library.
        if (!K32QueryWorkingSetEx(process, entries, (DWORD)(pages * sizeof(entries[0])))) return resident;

        for (u64 i = 0; i < pages; i++) resident += entries[i].VirtualAttributes.Valid ? page : 0;

        at += pages * page;
    }

    return resident;
}

u64 nya_memory_process_resident_bytes(void) {
    PROCESS_MEMORY_COUNTERS counters = { .cb = sizeof(counters) };
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return 0;

    return (u64)counters.WorkingSetSize;
}
