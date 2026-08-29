#include "nyangine/base/base_basic.h"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_INTEGRITY_SENTINEL_SIZE 8
#define _NYA_INTEGRITY_HASH_SIZE     8
#define _NYA_INTEGRITY_BLOCK_SIZE    (_NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE + _NYA_INTEGRITY_SENTINEL_SIZE)

typedef struct {
    u8 sentinel_begin[_NYA_INTEGRITY_SENTINEL_SIZE];
    u8 hash[_NYA_INTEGRITY_HASH_SIZE];
    u8 sentinel_end[_NYA_INTEGRITY_SENTINEL_SIZE];
} NYA_IntegrityBlock;

/*
 * Must survive -O3 -flto.
 *
 * The only reads of this are memcmps against a copy of the executable's own bytes, which the
 * optimizer is happy to constant fold — at which point the object has no remaining use and is
 * dropped, and the sentinel this whole scheme searches for is not in the binary. volatile alone did
 * not save it; `used` and `retain` are what pin the storage.
 */
NYA_INTERNAL volatile NYA_IntegrityBlock _NYA_INTEGRITY_BLOCK __attr_used __attr_retain = {
    .sentinel_begin = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE },
    .hash           = { 0 },
    .sentinel_end   = { 0xBA, 0xBE, 0xCA, 0xFE, 0xDE, 0xAD, 0xBE, 0xEF },
};

NYA_INTERNAL b8  _nya_integrity_find_sentinel(const u8* data, u64 len, OUT u64* out_offset);
NYA_INTERNAL u64 _nya_integrity_compute_mac(u8* data, u64 len, u64 hash_offset);
NYA_INTERNAL b8  _nya_integrity_code_region(OUT const u8** out_start, OUT u64* out_size);

/*
 * The MAC key.
 *
 * Split across two constants and combined at use rather than written as one literal, so that
 * grepping the binary for an obvious 16 byte blob does not immediately find it. That is
 * inconvenience, not secrecy: the key ships inside the executable and anyone who reverses the check
 * can read it. See the note in the header about what this does and does not buy.
 * */
#define _NYA_INTEGRITY_KEY_LOW  (0x9E3779B97F4A7C15ULL ^ 0x517CC1B727220A95ULL)
#define _NYA_INTEGRITY_KEY_HIGH (0xBF58476D1CE4E5B9ULL ^ 0x94D049BB133111EBULL)

/** Baseline hash of the mapped code, taken by nya_integrity_baseline_capture. */
NYA_INTERNAL u64 _nya_integrity_code_baseline       = 0;
NYA_INTERNAL b8  _nya_integrity_code_baseline_taken = false;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_integrity_assert(void) {
    // Only shipping builds carry a patched hash. A development build is recompiled constantly and a
    // test build is never stamped, so checking either would fail every time.
    if (!NYA_SHIPPING_BUILD) return;

    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    // Asks the filesystem layer rather than reimplementing /proc/self/exe and GetModuleFileNameA
    // here, so there is one definition of "where am I" to get right.
    NYA_String* executable_path = nullptr;
    NYA_Error   path_result     = nya_filesystem_executable_path(arena, &executable_path);

    b8 binary_valid = path_result.ok && nya_integrity_verify_file(nya_string_to_cstring(arena, executable_path));

    // Deliberately nya_assert_always: this check must never be weakened into something a build
    // configuration can remove, or a modified executable starts up silently. It is the same macro as
    // nya_assert — assertions cannot be compiled out — so the spelling is a marker for the next
    // editor rather than an enforced difference.
    nya_assert_always(binary_valid, "Executable integrity check failed. The executable is corrupted or was tampered with.");
}

b8 nya_integrity_verify_file(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    NYA_String* binary_content = nya_string_create(arena);
    if (!nya_file_read(path, binary_content).ok) return false;
    if (binary_content->length == 0) return false;

    u64 hash_offset = 0;
    if (!_nya_integrity_find_sentinel(binary_content->items, binary_content->length, &hash_offset)) return false;

    u64 stored_mac = 0;
    nya_memcpy(&stored_mac, &binary_content->items[hash_offset], sizeof(u64));

    return stored_mac == _nya_integrity_compute_mac(binary_content->items, binary_content->length, hash_offset);
}

NYA_Error nya_integrity_patch(NYA_ConstCString binary_path, OUT u64* out_mac) {
    nya_assert(binary_path != nullptr);
    nya_assert(out_mac != nullptr);

    NYA_Arena*  arena = nya_arena_create();
    defer       nya_arena_destroy(arena);
    NYA_String* binary = nya_string_create(arena);

    // Each failure below used to collapse into a bare false, which told the build system that
    // patching had failed but never which of these it was. A missing sentinel and an unwritable
    // file need different fixes.
    NYA_TRY(nya_file_read(binary_path, binary));

    u64 hash_offset = 0;
    if (!_nya_integrity_find_sentinel(binary->items, binary->length, &hash_offset)) {
        return nya_error(NYA_ERROR_NOT_FOUND, "no integrity sentinel in '%s'; was it built with base_integrity linked in?", binary_path);
    }

    u64 mac = _nya_integrity_compute_mac(binary->items, binary->length, hash_offset);
    nya_memcpy(&binary->items[hash_offset], &mac, sizeof(u64));

    NYA_TRY(nya_file_write(binary_path, binary));

    *out_mac = mac;
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * IN MEMORY
 * ─────────────────────────────────────────────────────────
 */

void nya_integrity_baseline_capture(void) {
    /*
     * Not under ASan, where this read is a false positive by construction.
     *
     * The region below spans .rodata as well as .text — deliberately, since read-only data is worth
     * covering and it sits before etext. ASan gives every instrumented global a poisoned redzone,
     * and those redzones are interleaved through .rodata, so hashing the region as one contiguous
     * range walks straight into them. It reported a global-buffer-overflow in nya_siphash and, with
     * -fno-sanitize-recover=all, took the process with it: every sanitized build aborted inside
     * nya_app_init, which is why no test could bring the app up.
     *
     * Skipping rather than exempting the read. The alternative is no_sanitize("address") on
     * nya_siphash, which would blind a hash function used all over the engine to genuine overruns
     * for the benefit of one caller. Nothing is lost here: this exists to notice a *shipped* binary
     * being patched at runtime, and a shipping build has no sanitizers — FLAGS_RELEASE compiles
     * none in. nya_integrity_assert already declines to run outside a shipping build for the same
     * class of reason.
     */
    if (ASAN_ENABLED) {
        nya_log_debug("Runtime integrity baseline skipped: ASan instrumentation makes the code region unhashable.");
        return;
    }

    const u8* start = nullptr;
    u64       size  = 0;

    if (!_nya_integrity_code_region(&start, &size)) {
        nya_log_error("Could not locate the executable's code region; runtime integrity checks are disabled.");
        return;
    }

    _nya_integrity_code_baseline       = nya_siphash(start, size, _NYA_INTEGRITY_KEY_LOW, _NYA_INTEGRITY_KEY_HIGH);
    _nya_integrity_code_baseline_taken = true;

    nya_log_debug("Integrity baseline captured over " FMTu64 " bytes of code.", size);
}

NYA_IntegrityStatus nya_integrity_verify_code(void) {
    if (!_nya_integrity_code_baseline_taken) return NYA_INTEGRITY_NO_BASELINE;

    const u8* start = nullptr;
    u64       size  = 0;
    if (!_nya_integrity_code_region(&start, &size)) return NYA_INTEGRITY_UNAVAILABLE;

    u64 current = nya_siphash(start, size, _NYA_INTEGRITY_KEY_LOW, _NYA_INTEGRITY_KEY_HIGH);

    return current == _nya_integrity_code_baseline ? NYA_INTEGRITY_OK : NYA_INTEGRITY_CODE_MODIFIED;
}

u64 nya_integrity_code_size(void) {
    const u8* start = nullptr;
    u64       size  = 0;

    return _nya_integrity_code_region(&start, &size) ? size : 0;
}

/**
 * Locates the executable's own code in memory.
 *
 * This is the mapped image, not the file: it already has relocations applied and any hook already
 * written into it, which is the entire point of checking it separately from the file on disk.
 * */
NYA_INTERNAL b8 _nya_integrity_code_region(OUT const u8** out_start, OUT u64* out_size) {
#if OS_WINDOWS
    // Walk the PE headers from the module base to find the section marked executable.
    HMODULE module = GetModuleHandleA(nullptr);
    if (module == nullptr) return false;

    const u8*               base       = (const u8*)module;
    const IMAGE_DOS_HEADER* dos_header = (const IMAGE_DOS_HEADER*)base;
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) return false;

    const IMAGE_NT_HEADERS* nt_headers = (const IMAGE_NT_HEADERS*)(base + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE) return false;

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt_headers);
    for (u16 i = 0; i < nt_headers->FileHeader.NumberOfSections; i++) {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

        *out_start = base + section[i].VirtualAddress;
        *out_size  = section[i].Misc.VirtualSize;
        return true;
    }

    return false;
#elif OS_LINUX
    // Linker provided bounds of the text segment. Cheaper and more predictable than walking
    // dl_iterate_phdr, and it covers exactly the code this executable was built with — a hook
    // installed in a shared library is a different question and not one this answers.
    extern char __executable_start[];
    extern char etext[];

    if ((const u8*)etext <= (const u8*)__executable_start) return false;

    *out_start = (const u8*)__executable_start;
    *out_size  = (u64)((const u8*)etext - (const u8*)__executable_start);
    return true;
#else
    nya_unused(out_start, out_size);
    return false;
#endif
}

NYA_INTERNAL b8 _nya_integrity_find_sentinel(const u8* data, u64 len, OUT u64* out_offset) {
    nya_assert(data != nullptr);
    nya_assert(out_offset != nullptr);

    if (len < _NYA_INTEGRITY_BLOCK_SIZE) return false;

    for (u64 i = 0; i <= len - _NYA_INTEGRITY_BLOCK_SIZE; i++) {
        if (nya_memcmp(&data[i], (void*)_NYA_INTEGRITY_BLOCK.sentinel_begin, _NYA_INTEGRITY_SENTINEL_SIZE) != 0) continue;

        u64 end_offset = i + _NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE;
        if (nya_memcmp(&data[end_offset], (void*)_NYA_INTEGRITY_BLOCK.sentinel_end, _NYA_INTEGRITY_SENTINEL_SIZE) != 0) continue;

        *out_offset = i + _NYA_INTEGRITY_SENTINEL_SIZE;
        return true;
    }

    return false;
}

/**
 * Narrows a PE down to the part of it that Authenticode signing cannot move.
 *
 * A signed executable is the same file with a certificate appended, the security data directory
 * pointed at it, and the header checksum recomputed. Hashing any of those would mean the stamp
 * written before signing never matches the file that ships, so the region hashed stops at the end
 * of the last section and the two mutable header fields are reported for zeroing.
 *
 * The end of the last section rather than the certificate's own offset, because signing pads the
 * file to an eight byte boundary first, and those pad bytes exist in the signed file and not in the
 * unsigned one.
 *
 * Returns false for anything that is not a PE, which is how ELF keeps being hashed whole.
 * */
NYA_INTERNAL b8 _nya_integrity_pe_regions(const u8* data, u64 len, OUT u64* out_len, OUT u64* out_checksum_offset, OUT u64* out_security_offset) {
    if (len < 0x40 || data[0] != 'M' || data[1] != 'Z') return false;

    // Widened before anything is done with it: e_lfanew is whatever the file says it is, and every
    // bound below would otherwise be computed in u32 and wrap on a corrupt or hostile value.
    u32 pe_offset_field = 0;
    nya_memcpy(&pe_offset_field, &data[0x3C], sizeof(u32));

    u64 pe_offset = pe_offset_field;
    if (pe_offset + 24 > len) return false;
    if (data[pe_offset] != 'P' || data[pe_offset + 1] != 'E' || data[pe_offset + 2] != 0 || data[pe_offset + 3] != 0) return false;

    u16 section_count = 0;
    u16 optional_size = 0;
    nya_memcpy(&section_count, &data[pe_offset + 6], sizeof(u16));
    nya_memcpy(&optional_size, &data[pe_offset + 20], sizeof(u16));

    u64 optional_offset = pe_offset + 24;
    if (optional_offset + optional_size > len || optional_size < 2) return false;

    u16 magic = 0;
    nya_memcpy(&magic, &data[optional_offset], sizeof(u16));

    // The data directory sits after a header whose size differs between PE32 and PE32+, which is
    // the only thing the two formats disagree on here.
    u64 directory_offset = 0;
    if (magic == 0x10B) {
        directory_offset = optional_offset + 96;
    } else if (magic == 0x20B) {
        directory_offset = optional_offset + 112;
    } else {
        return false;
    }

    // Entry 4 is IMAGE_DIRECTORY_ENTRY_SECURITY, and each entry is eight bytes.
    u64 security_offset = directory_offset + (4 * 8);
    u64 checksum_offset = optional_offset + 64;
    if (security_offset + 8 > len || checksum_offset + 4 > len) return false;

    u64 sections_offset = optional_offset + optional_size;
    u64 end_of_sections = 0;
    for (u16 section = 0; section < section_count; section++) {
        u64 header = sections_offset + ((u64)section * 40);
        if (header + 40 > len) return false;

        u32 raw_size    = 0;
        u32 raw_pointer = 0;
        nya_memcpy(&raw_size, &data[header + 16], sizeof(u32));
        nya_memcpy(&raw_pointer, &data[header + 20], sizeof(u32));
        if (raw_pointer == 0 || raw_size == 0) continue; // uninitialised data, nothing in the file

        u64 section_end = (u64)raw_pointer + raw_size;
        if (section_end > end_of_sections) end_of_sections = section_end;
    }

    if (end_of_sections == 0 || end_of_sections > len) return false;

    *out_len             = end_of_sections;
    *out_checksum_offset = checksum_offset;
    *out_security_offset = security_offset;
    return true;
}

NYA_INTERNAL u64 _nya_integrity_compute_mac(u8* data, u64 len, u64 hash_offset) {
    u8 saved[_NYA_INTEGRITY_HASH_SIZE];
    nya_memcpy(saved, &data[hash_offset], _NYA_INTEGRITY_HASH_SIZE);
    nya_memset(&data[hash_offset], 0, _NYA_INTEGRITY_HASH_SIZE);

    // Zeroed rather than skipped, so the hashed bytes stay one contiguous run and a signed and an
    // unsigned copy of the same executable produce the same value.
    u8  saved_checksum[4] = { 0 };
    u8  saved_security[8] = { 0 };
    u64 hashed_len        = len;
    u64 checksum_offset   = 0;
    u64 security_offset   = 0;

    b8 is_pe = _nya_integrity_pe_regions(data, len, &hashed_len, &checksum_offset, &security_offset);
    if (is_pe) {
        nya_memcpy(saved_checksum, &data[checksum_offset], sizeof(saved_checksum));
        nya_memcpy(saved_security, &data[security_offset], sizeof(saved_security));
        nya_memset(&data[checksum_offset], 0, sizeof(saved_checksum));
        nya_memset(&data[security_offset], 0, sizeof(saved_security));
    }

    // Keyed, so the value cannot simply be recomputed after an edit the way a CRC can.
    u64 mac = nya_siphash(data, hashed_len, _NYA_INTEGRITY_KEY_LOW, _NYA_INTEGRITY_KEY_HIGH);

    if (is_pe) {
        nya_memcpy(&data[checksum_offset], saved_checksum, sizeof(saved_checksum));
        nya_memcpy(&data[security_offset], saved_security, sizeof(saved_security));
    }

    nya_memcpy(&data[hash_offset], saved, _NYA_INTEGRITY_HASH_SIZE);
    return mac;
}
