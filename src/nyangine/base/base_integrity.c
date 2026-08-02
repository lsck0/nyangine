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
NYA_INTERNAL u64 _nya_integrity_compute_crc64(u8* data, u64 len, u64 hash_offset);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_integrity_assert(void) {
    // Only shipping builds carry a patched hash. A development build is recompiled constantly and a
    // test build is never stamped, so checking either would fail every time.
    if (!NYA_SHIPPING_BUILD) return;

    b8 binary_valid = true;

    NYA_Arena* arena = nya_arena_create();
    defer      nya_arena_destroy(arena);

    NYA_String* binary_content = nya_string_create(arena);
    NYA_Error   file_result;

    // Asks the filesystem layer rather than reimplementing /proc/self/exe and GetModuleFileNameA
    // here, so there is one definition of "where am I" to get right.
    NYA_String* executable_path = nullptr;
    file_result                 = nya_filesystem_executable_path(arena, &executable_path);
    if (file_result.kind == NYA_ERROR_NONE) file_result = nya_file_read(nya_string_to_cstring(arena, executable_path), binary_content);

    if (file_result.kind != NYA_ERROR_NONE || binary_content->length == 0) binary_valid = false;

    u64 hash_offset = 0;
    b8  ok          = _nya_integrity_find_sentinel(binary_content->items, binary_content->length, &hash_offset);
    if (!ok) {
        binary_valid = false;
    } else {
        u64 stored_hash = 0;
        nya_memcpy(&stored_hash, &binary_content->items[hash_offset], sizeof(u64));

        u64 computed_hash = _nya_integrity_compute_crc64(binary_content->items, binary_content->length, hash_offset);
        if (stored_hash != computed_hash) binary_valid = false;
    }

    // Deliberately nya_assert_always: with a plain nya_assert, building with -DNYA_NO_ASSERT would
    // compile the tamper check away and let a modified executable start up silently.
    nya_assert_always(binary_valid, "Executable integrity check failed. The executable is corrupted or was tampered with.");
}

NYA_Error nya_integrity_patch(NYA_ConstCString binary_path, OUT u64* out_crc) {
    nya_assert(binary_path != nullptr);
    nya_assert(out_crc != nullptr);

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

    u64 crc = _nya_integrity_compute_crc64(binary->items, binary->length, hash_offset);
    nya_memcpy(&binary->items[hash_offset], &crc, sizeof(u64));

    NYA_TRY(nya_file_write(binary_path, binary));

    *out_crc = crc;
    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

NYA_INTERNAL u64 _nya_integrity_compute_crc64(u8* data, u64 len, u64 hash_offset) {
    u8 saved[_NYA_INTEGRITY_HASH_SIZE];
    nya_memcpy(saved, &data[hash_offset], _NYA_INTEGRITY_HASH_SIZE);
    nya_memset(&data[hash_offset], 0, _NYA_INTEGRITY_HASH_SIZE);

    u64 crc = nya_crc64(data, len);

    nya_memcpy(&data[hash_offset], saved, _NYA_INTEGRITY_HASH_SIZE);
    return crc;
}
