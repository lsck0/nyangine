#include "nyangine-std/base/base_basic.h"
#include "nyangine-core/nyangine.h"

#if OS_LINUX
#include <elf.h>
#include <pthread.h>
#endif

// PRIVATE API DECLARATION

#define _NYA_INTEGRITY_SENTINEL_SIZE 8
#define _NYA_INTEGRITY_HASH_SIZE     8
#define _NYA_INTEGRITY_BLOCK_SIZE    (_NYA_INTEGRITY_SENTINEL_SIZE + _NYA_INTEGRITY_HASH_SIZE + _NYA_INTEGRITY_SENTINEL_SIZE)

typedef struct {
    u8 sentinel_begin[_NYA_INTEGRITY_SENTINEL_SIZE];
    u8 hash[_NYA_INTEGRITY_HASH_SIZE];
    u8 sentinel_end[_NYA_INTEGRITY_SENTINEL_SIZE];
} NYA_IntegrityBlock;

// Must survive -O3 -flto.
NYA_INTERNAL volatile NYA_IntegrityBlock _NYA_INTEGRITY_BLOCK __attr_used __attr_retain = {
    .sentinel_begin = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE },
    .hash           = { 0 },
    .sentinel_end   = { 0xBA, 0xBE, 0xCA, 0xFE, 0xDE, 0xAD, 0xBE, 0xEF },
};

NYA_INTERNAL b8  _nya_integrity_find_sentinel(const u8* data, u64 len, OUT u64* out_offset);
NYA_INTERNAL u64 _nya_integrity_compute_mac(u8* data, u64 len, u64 hash_offset);
NYA_INTERNAL b8  _nya_integrity_code_region(OUT const u8** out_start, OUT u64* out_size);

/** Reads `path` and computes its MAC, along with the one stamped into it. False when either cannot be had. */
NYA_INTERNAL b8 _nya_integrity_file_mac(NYA_ConstCString path, OUT u64* out_stored, OUT u64* out_computed);

/** What the startup thread runs: the file check, then the code baseline. */
NYA_INTERNAL void _nya_integrity_startup(void);

// The MAC key.
#define _NYA_INTEGRITY_KEY_LOW  (0x9E3779B97F4A7C15ULL ^ 0x517CC1B727220A95ULL)
#define _NYA_INTEGRITY_KEY_HIGH (0xBF58476D1CE4E5B9ULL ^ 0x94D049BB133111EBULL)

NYA_IntegrityState _nya_integrity_state;
b8                 _nya_integrity_started = false;

/** Folds a chunk hash into a digest. Order dependent, so a pass that skips or repeats a chunk folds differently. */
NYA_INTERNAL u64 _nya_integrity_fold(u64 digest, u64 hash) {
    return ((digest << 1) | (digest >> 63)) ^ hash;
}

// PUBLIC API IMPLEMENTATION

u64 nya_integrity_hash(const void* data, u64 size) {
    // keyed, so a value cannot simply be recomputed after an edit the way a CRC can.
    return nya_siphash(data, size, _NYA_INTEGRITY_KEY_LOW, _NYA_INTEGRITY_KEY_HIGH);
}

void nya_integrity_fail(NYA_IntegrityStatus status, NYA_ConstCString detail) {
    nya_assert(status > NYA_INTEGRITY_OK && status < NYA_INTEGRITY_STATUS_COUNT);

    nya_log_error("Integrity check failed (%s): %s. Exiting.", NYA_INTEGRITY_STATUS_NAME_MAP[status], detail);
    nya_log_file_flush();
    (void)fflush(stdout);
    (void)fflush(stderr);

    // _Exit rather than exit: another thread may be mid frame, and atexit handlers running under it would race.
    _Exit(NYA_INTEGRITY_EXIT_CODE);
}

u64 _nya_integrity_stamped_mac(void) {
    u64 mac = 0;
    for (u32 i = 0; i < _NYA_INTEGRITY_HASH_SIZE; i++) mac |= (u64)_NYA_INTEGRITY_BLOCK.hash[i] << (i * 8);
    return mac;
}

// ON DISK

b8 nya_integrity_verify_file(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    u64 stored   = 0;
    u64 computed = 0;
    return _nya_integrity_file_mac(path, &stored, &computed) && stored == computed;
}

NYA_Error nya_integrity_patch(NYA_ConstCString binary_path, OUT u64* out_mac) {
    nya_assert(binary_path != nullptr);
    nya_assert(out_mac != nullptr);

    NYA_Arena*  arena = nya_arena_create();
    defer       nya_arena_destroy(arena);
    NYA_String* binary = nya_string_create(arena);

    // Each failure reports which step failed; a missing sentinel and an unwritable file need different fixes.
    NYA_TRY(nya_file_read(binary_path, binary));

    u64 hash_offset = 0;
    if (!_nya_integrity_find_sentinel(binary->items, binary->length, &hash_offset)) {
        return nya_error(NYA_ERROR_NOT_FOUND, "no integrity sentinel in '%s'; was it built with base_integrity linked in?", binary_path);
    }

    u64 mac = _nya_integrity_compute_mac(binary->items, binary->length, hash_offset);
    nya_memcpy(&binary->items[hash_offset], &mac, sizeof(u64));

    // atomic, so an interrupted stamp leaves the unstamped binary rather than a truncated one.
    NYA_TRY(nya_file_write_atomic(binary_path, binary));

    *out_mac = mac;
    return NYA_OK;
}

// AT RUNTIME

#if OS_WINDOWS
NYA_INTERNAL DWORD WINAPI _nya_integrity_thread(LPVOID user_data) {
    nya_unused(user_data);
    _nya_integrity_startup();
    return 0;
}
#else
NYA_INTERNAL void* _nya_integrity_thread(void* user_data) {
    nya_unused(user_data);
    _nya_integrity_startup();
    return nullptr;
}
#endif

void nya_integrity_start(void) {
    if (!NYA_SHIPPING_BUILD) return;
    nya_assert(!_nya_integrity_started, "nya_integrity_start runs once.");

    _nya_integrity_started = true;

    // reading and hashing the whole executable takes about 10 ms, so it runs beside startup instead of in front of it.
#if OS_WINDOWS
    HANDLE thread = CreateThread(nullptr, 0, _nya_integrity_thread, nullptr, 0, nullptr);
    if (thread != nullptr) {
        (void)CloseHandle(thread);
        return;
    }
#else
    pthread_t thread;
    if (pthread_create(&thread, nullptr, _nya_integrity_thread, nullptr) == 0) {
        (void)pthread_detach(thread);
        return;
    }
#endif

    _nya_integrity_startup();
}

void nya_integrity_sweep(u64 now_ns) {
    if (!NYA_SHIPPING_BUILD || !_nya_integrity_started) return;

    NYA_IntegrityState* state = &_nya_integrity_state;
    if (!atomic_load(&state->baseline_ready)) return;

    if (state->sweep_next_ns == 0) state->sweep_next_ns = now_ns;

    for (u32 step = 0; step < NYA_INTEGRITY_SWEEP_CATCH_UP_MAX && now_ns >= state->sweep_next_ns; step++) {
        NYA_IntegrityStatus status = nya_integrity_sweep_step(state);
        if (status != NYA_INTEGRITY_OK) nya_integrity_fail(status, "the executable's code changed while it ran");

        state->sweep_next_ns += NYA_INTEGRITY_SWEEP_INTERVAL_NS;
    }

    // a stall longer than the catch up is dropped rather than repaid over the frames after it.
    if (now_ns >= state->sweep_next_ns) state->sweep_next_ns = now_ns + NYA_INTEGRITY_SWEEP_INTERVAL_NS;
}

void nya_integrity_capture(NYA_IntegrityState* state, const u8* code, u64 size) {
    nya_assert(state != nullptr);
    nya_assert(code != nullptr || size == 0);
    nya_assert(!atomic_load(&state->baseline_ready), "a baseline is captured once.");

    // wider chunks rather than more of them, so the table stays a fixed size whatever the executable.
    u64 chunk_bytes = NYA_INTEGRITY_CODE_CHUNK_BYTES;
    while (chunk_bytes * NYA_INTEGRITY_CODE_CHUNK_MAX < size) chunk_bytes *= 2;

    u32 chunk_count = (u32)((size + chunk_bytes - 1) / chunk_bytes);
    u64 digest      = 0;

    for (u32 i = 0; i < chunk_count; i++) {
        u64 offset = (u64)i * chunk_bytes;
        u64 length = nya_min(chunk_bytes, size - offset);

        state->chunk_hashes[i] = nya_integrity_hash(&code[offset], length);
        digest                 = _nya_integrity_fold(digest, state->chunk_hashes[i]);
    }

    state->code             = code;
    state->code_size        = size;
    state->chunk_bytes      = chunk_bytes;
    state->chunk_count      = chunk_count;
    state->baseline_digest  = digest;
    state->last_pass_digest = digest;

    // last: the sweep reads nothing above until this is set.
    atomic_store(&state->baseline_ready, true);

    nya_assert(state->chunk_count <= NYA_INTEGRITY_CODE_CHUNK_MAX);
}

NYA_IntegrityStatus nya_integrity_sweep_step(NYA_IntegrityState* state) {
    nya_assert(state != nullptr);
    nya_assert(atomic_load(&state->baseline_ready), "sweeping before the baseline was captured.");

    if (state->chunk_count == 0) {
        state->sweep_passes++;
        return NYA_INTEGRITY_OK;
    }

    u32 index  = state->sweep_cursor;
    u64 offset = (u64)index * state->chunk_bytes;
    u64 hash   = nya_integrity_hash(&state->code[offset], nya_min(state->chunk_bytes, state->code_size - offset));

    state->sweep_digest = _nya_integrity_fold(state->sweep_digest, hash);
    state->sweep_cursor++;

    if (state->sweep_cursor == state->chunk_count) {
        state->last_pass_digest = state->sweep_digest;
        state->sweep_digest     = 0;
        state->sweep_cursor     = 0;
        state->sweep_passes++;
    }

    return hash == state->chunk_hashes[index] ? NYA_INTEGRITY_OK : NYA_INTEGRITY_CODE_MODIFIED;
}

// PRIVATE API IMPLEMENTATION

void _nya_integrity_startup(void) {
    NYA_Arena* arena = nya_arena_create(.name = "integrity");
    defer      nya_arena_destroy(arena);

    // asks the filesystem layer rather than reimplementing /proc/self/exe and GetModuleFileNameA here.
    NYA_String* executable_path = nullptr;
    if (!nya_filesystem_executable_path(arena, &executable_path).ok) nya_integrity_fail(NYA_INTEGRITY_FILE_MODIFIED, "the executable could not be located");

    u64 stored   = 0;
    u64 computed = 0;
    if (!_nya_integrity_file_mac(nya_string_to_cstring(arena, executable_path), &stored, &computed)) {
        nya_integrity_fail(NYA_INTEGRITY_FILE_MODIFIED, "the executable could not be read");
    }

    // published whatever it is: the watchdog compares it with the stamp itself, so skipping the check below is not enough.
    atomic_store(&_nya_integrity_state.executable_mac, computed);
    if (stored != computed) nya_integrity_fail(NYA_INTEGRITY_FILE_MODIFIED, "the executable does not match its stamp");

    const u8* code = nullptr;
    u64       size = 0;

    if (!_nya_integrity_code_region(&code, &size)) nya_log_warn("Could not locate the executable's code; it is not swept.");

    u64 started_ns = nya_clock_get_monotonic_ns();
    nya_integrity_capture(&_nya_integrity_state, code, size);

    nya_log_debug("Integrity baseline over " FMTu64 " KB of code in " FMTu32 " chunks took %.2f ms.", size / 1024, _nya_integrity_state.chunk_count,
                  nya_time_ns_to_ms(nya_clock_get_monotonic_ns() - started_ns));
}

b8 _nya_integrity_file_mac(NYA_ConstCString path, OUT u64* out_stored, OUT u64* out_computed) {
    nya_assert(path != nullptr);
    nya_assert(out_stored != nullptr && out_computed != nullptr);

    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    NYA_String* binary_content = nya_string_create(arena);
    if (!nya_file_read(path, binary_content).ok) return false;
    if (binary_content->length == 0) return false;

    u64 hash_offset = 0;
    if (!_nya_integrity_find_sentinel(binary_content->items, binary_content->length, &hash_offset)) return false;

    nya_memcpy(out_stored, &binary_content->items[hash_offset], sizeof(u64));
    *out_computed = _nya_integrity_compute_mac(binary_content->items, binary_content->length, hash_offset);

    return true;
}

/**
 * Locates the executable's own code in memory.
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
    // The executable segment from the program headers at __executable_start: cheaper and more predictable than dl_iterate_phdr and exactly this executable's code; hooks in shared libraries are out of scope.
    extern char __executable_start[];

    const Elf64_Ehdr* header = (const Elf64_Ehdr*)__executable_start;
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0) return false;

    const Elf64_Phdr* segments = (const Elf64_Phdr*)((const u8*)header + header->e_phoff);

    // where the segment holding the headers was placed, so a position independent address resolves.
    const u8* base = nullptr;

    for (u16 i = 0; i < header->e_phnum; i++) {
        if (segments[i].p_type == PT_LOAD && segments[i].p_offset == 0) base = (const u8*)header - segments[i].p_vaddr;
    }

    if (base == nullptr) return false;

    for (u16 i = 0; i < header->e_phnum; i++) {
        if (segments[i].p_type != PT_LOAD || (segments[i].p_flags & PF_X) == 0) continue;

        *out_start = base + segments[i].p_vaddr;
        *out_size  = segments[i].p_memsz;
        return true;
    }

    return false;
#else
    nya_unused(out_start, out_size);
    return false;
#endif
}

NYA_INTERNAL b8 _nya_integrity_find_sentinel(const u8* data, u64 len, OUT u64* out_offset) {
    nya_assert(data != nullptr);
    nya_assert(out_offset != nullptr);

    if (len < _NYA_INTEGRITY_BLOCK_SIZE) return false;

    u8  first = _NYA_INTEGRITY_BLOCK.sentinel_begin[0];
    u64 last  = len - _NYA_INTEGRITY_BLOCK_SIZE;

    for (u64 i = 0; i <= last; i++) {
        // memchr to the next candidate first: a byte by byte memcmp over a 20 MB executable was most of startup.
        const u8* candidate = memchr(&data[i], first, last - i + 1);
        if (candidate == nullptr) return false;
        i = (u64)(candidate - data);

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
 * */
NYA_INTERNAL b8 _nya_integrity_pe_regions(const u8* data, u64 len, OUT u64* out_len, OUT u64* out_checksum_offset, OUT u64* out_security_offset) {
    if (len < 0x40 || data[0] != 'M' || data[1] != 'Z') return false;

    // Widened before anything is done with it: e_lfanew is whatever the file says, and every bound below would otherwise be computed in u32 and wrap on a corrupt or hostile value.
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

    // The data directory sits after a header whose size differs between PE32 and PE32+, the only thing the two formats disagree on here.
    u64 directory_offset = 0;
    if (magic == 0x10B) {
        directory_offset = optional_offset + 96;
    } else if (magic == 0x20B) {
        directory_offset = optional_offset + 112;
    } else {
        return false;
    }

    // Entry 4 is IMAGE_DIRECTORY_ENTRY_SECURITY, and each entry is eight bytes.
    u64 security_offset = directory_offset + (4ULL * 8ULL);
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

    // Zeroed rather than skipped, so the hashed bytes stay one contiguous run and a signed and an unsigned copy of the same executable produce the same value.
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
