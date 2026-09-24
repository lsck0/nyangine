/**
 * Reserving, committing and measuring virtual memory.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
    u64 page = nya_os_page_size();
    nya_check(page >= 4096 && (page & (page - 1)) == 0, "a page should be a power of two of at least 4 KiB, got " FMTu64, page);

    // A reservation takes no physical memory until committed pages are written.
    {
        u64 size    = 64 * page;
        u8* address = nya_os_page_reserve(size);
        nya_check(address != nullptr, "reserving 64 pages should succeed");
        defer (void)nya_os_page_release(address, size);

        nya_check(nya_os_page_commit(address, 4 * page), "committing the first four pages should succeed");
        nya_check(nya_os_page_resident_bytes(address, size) == 0, "committed but unwritten pages should not be resident");

        // committed memory reads as zero. only the pages written next are read, since a read maps them.
        u64 zeroes = 0;
        for (u64 i = 0; i < 2 * page; i++) zeroes += address[i] == 0 ? 1 : 0;
        nya_check(zeroes == 2 * page, "committed pages should read as zero");

        nya_memset(address, 0xAB, 2 * page);
        nya_check(nya_os_page_resident_bytes(address, size) == 2 * page, "two written pages should be resident, got " FMTu64,
                  nya_os_page_resident_bytes(address, size));

        // an unaligned range still counts the pages it touches.
        nya_check(nya_os_page_resident_bytes(address + 1, 10) == page, "a range inside one page should count that page");

        // a later commit extends the usable range without disturbing what was written.
        nya_check(nya_os_page_commit(address + (4 * page) + 1, page), "a commit starting mid page should round out");
        address[5 * page - 1] = 7;
        nya_check(address[0] == 0xAB && address[5 * page - 1] == 7, "committing more should keep earlier contents");
    }

    // The process total is at least what this test has resident on its own.
    {
        u64 size    = 32 * page;
        u8* address = nya_os_page_reserve(size);
        nya_check(address != nullptr && nya_os_page_commit(address, size), "reserving and committing 32 pages should succeed");
        defer (void)nya_os_page_release(address, size);

        nya_memset(address, 1, size);

        u64 process = nya_os_process_resident_bytes();
        nya_check(process >= size, "the process should hold at least the " FMTu64 " bytes just written, got " FMTu64, size, process);
    }

    // Nothing and null measure as nothing.
    nya_check(nya_os_page_resident_bytes(nullptr, page) == 0, "null should measure as nothing");
    nya_check(nya_os_page_resident_bytes(&page, 0) == 0, "an empty range should measure as nothing");
    nya_check(nya_os_page_release(nullptr, page), "releasing nothing is not a failure");

    return nya_check_failures() == 0 ? 0 : 1;
}
