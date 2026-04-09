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

volatile NYA_IntegrityBlock _NYA_INTEGRITY_BLOCK = {
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
  if (NYA_MODE_CURRENT == NYA_MODE_DEBUG) return;

  b8 binary_valid = true;

  NYA_CLEANUP_WITH(nya_arena_destroy) NYA_Arena* arena          = nya_arena_create();
  NYA_String*                                  binary_content = nya_string_create(arena);

  NYA_Result file_result;

#if OS_LINUX
  file_result = nya_file_read("/proc/self/exe", binary_content);
#elif OS_WINDOWS
  char path[260] = { 0 };
  GetModuleFileNameA(nullptr, path, sizeof(path));
  file_result = nya_file_read(path, binary_content);
#else
  file_result = nya_err(NYA_ERROR_NOT_SUPPORTED, "Integrity check not supported on this platform.");
#endif

  if (file_result.error != NYA_ERROR_NONE || binary_content->length == 0) binary_valid = false;

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

  nya_assert(binary_valid, "Executable integrity check failed. The executable is corrupted or was tampered with.");
}

b8 nya_integrity_patch(NYA_ConstCString binary_path, OUT u64* out_crc) {
  nya_assert(binary_path != nullptr);

  NYA_CLEANUP_WITH(nya_arena_destroy) NYA_Arena* arena  = nya_arena_create();
  NYA_String*                                  binary = nya_string_create(arena);

  NYA_Result result = nya_file_read(binary_path, binary);
  if (result.error != NYA_ERROR_NONE) return false;

  u64 hash_offset = 0;
  b8  ok          = _nya_integrity_find_sentinel(binary->items, binary->length, &hash_offset);
  if (!ok) return false;

  u64 crc = _nya_integrity_compute_crc64(binary->items, binary->length, hash_offset);
  nya_memcpy(&binary->items[hash_offset], &crc, sizeof(u64));

  result = nya_file_write(binary_path, binary);
  if (result.error != NYA_ERROR_NONE) return false;

  *out_crc = crc;
  return true;
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
