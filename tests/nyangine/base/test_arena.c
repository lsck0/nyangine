/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine-std/base/base_arena.h"
#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_arena");

  // test basic alloc
  void* ptr1 = nya_arena_alloc(arena, 64);
  nya_assert(ptr1 != nullptr);

  void* ptr2 = nya_arena_alloc(arena, 128);
  nya_assert(ptr2 != nullptr);
  nya_assert(ptr2 != ptr1);

  // test zero-size alloc returns nullptr
  void* ptr_zero = nya_arena_alloc(arena, 0);
  nya_assert(ptr_zero == nullptr);

  // test alloc writes don't corrupt
  u8* data = (u8*)nya_arena_alloc(arena, 256);
  nya_assert(data != nullptr);
  for (u32 i = 0; i < 256; ++i) { data[i] = (u8)i; }
  for (u32 i = 0; i < 256; ++i) { nya_assert(data[i] == (u8)i); }

  // test realloc grow
  u8* realloc_ptr = (u8*)nya_arena_alloc(arena, 32);
  nya_assert(realloc_ptr != nullptr);
  for (u32 i = 0; i < 32; ++i) { realloc_ptr[i] = (u8)(i + 100); }
  u8* realloc_ptr_grown = (u8*)nya_arena_realloc(arena, realloc_ptr, 32, 64);
  nya_assert(realloc_ptr_grown != nullptr);
  for (u32 i = 0; i < 32; ++i) { nya_assert(realloc_ptr_grown[i] == (u8)(i + 100)); }

  // test realloc shrink
  u8* shrink_ptr = (u8*)nya_arena_alloc(arena, 128);
  nya_assert(shrink_ptr != nullptr);
  for (u32 i = 0; i < 128; ++i) { shrink_ptr[i] = (u8)i; }
  u8* shrink_ptr_shrunken = (u8*)nya_arena_realloc(arena, shrink_ptr, 128, 32);
  nya_assert(shrink_ptr_shrunken == shrink_ptr);
  for (u32 i = 0; i < 32; ++i) { nya_assert(shrink_ptr_shrunken[i] == (u8)i); }

  // test realloc with same size returns same pointer
  void* same_ptr = nya_arena_alloc(arena, 64);
  nya_assert(same_ptr != nullptr);
  void* same_ptr_realloc = nya_arena_realloc(arena, same_ptr, 64, 64);
  nya_assert(same_ptr_realloc == same_ptr);

  // test realloc nullptr returns nullptr
  void* null_realloc = nya_arena_realloc(arena, nullptr, 0, 64);
  nya_assert(null_realloc == nullptr);

  // test realloc to zero size frees and returns nullptr
  void* realloc_to_zero = nya_arena_alloc(arena, 64);
  nya_assert(realloc_to_zero != nullptr);
  void* freed = nya_arena_realloc(arena, realloc_to_zero, 64, 0);
  nya_assert(freed == nullptr);

  // test free last allocation reclaims space
  u64   usage_before = nya_arena_memory_usage_bytes(arena);
  void* free_ptr     = nya_arena_alloc(arena, 512);
  nya_assert(free_ptr != nullptr);
  nya_assert(nya_arena_memory_usage_bytes(arena) > usage_before);
  nya_arena_free(arena, free_ptr, 512);

  // test free nullptr is safe
  nya_arena_free(arena, nullptr, 0);
  nya_arena_free(arena, nullptr, 64);

  // test free zero size is safe
  void* dummy = nya_arena_alloc(arena, 32);
  nya_arena_free(arena, dummy, 0);

  // test multiple allocs and frees (middle allocation goes to free list)
  void* a = nya_arena_alloc(arena, 64);
  void* b = nya_arena_alloc(arena, 64);
  void* c = nya_arena_alloc(arena, 64);
  nya_assert(a != nullptr && b != nullptr && c != nullptr);
  nya_arena_free(arena, b, 64);
  void* b_reused = nya_arena_alloc(arena, 64);
  nya_assert(b_reused != nullptr);

  nya_arena_destroy(arena);

  // TEST: custom arena options
  NYA_Arena* custom_arena = nya_arena_create(
          .name                         = "custom_arena",
          .alignment                    = 16,
          .region_size                  = nya_kibyte_to_byte(64),
          .defragmentation_enabled      = true,
          .defragmentation_threshold    = 4,
          .garbage_collection_enabled   = true,
          .garbage_collection_threshold = 2
  );

  void* custom_ptr = nya_arena_alloc(custom_arena, 32);
  nya_assert(custom_ptr != nullptr);
  nya_assert((uintptr_t)custom_ptr % 16 == 0); // verify alignment
  nya_arena_destroy(custom_arena);

  // TEST: alignment verification
  NYA_Arena* aligned_arena = nya_arena_create(.name = "aligned_arena", .alignment = 32);

  for (u32 i = 0; i < 10; ++i) {
    void* aligned_ptr = nya_arena_alloc(aligned_arena, 17 + i); // various sizes
    nya_assert(aligned_ptr != nullptr);
    nya_assert((uintptr_t)aligned_ptr % 32 == 0);
  }
  nya_arena_destroy(aligned_arena);

  // TEST: large allocation (bigger than region size)
  NYA_Arena* small_region_arena = nya_arena_create(.name = "small_region_arena", .region_size = nya_kibyte_to_byte(4));

  void* large_ptr = nya_arena_alloc(small_region_arena, nya_kibyte_to_byte(8)); // larger than region
  nya_assert(large_ptr != nullptr);

  u8* large_data = (u8*)large_ptr;
  for (u32 i = 0; i < nya_kibyte_to_byte(8); ++i) { large_data[i] = (u8)(i & 0xFF); }
  for (u32 i = 0; i < nya_kibyte_to_byte(8); ++i) { nya_assert(large_data[i] == (u8)(i & 0xFF)); }

  nya_arena_destroy(small_region_arena);

  // TEST: multiple regions (exhaust one region and allocate into another)
  NYA_Arena* multi_region_arena = nya_arena_create(.name = "multi_region_arena", .region_size = nya_kibyte_to_byte(4));

  void* ptrs[10];
  for (u32 i = 0; i < 10; ++i) {
    ptrs[i] = nya_arena_alloc(multi_region_arena, nya_kibyte_to_byte(1));
    nya_assert(ptrs[i] != nullptr);
    // verify each allocation is unique
    for (u32 j = 0; j < i; ++j) { nya_assert(ptrs[i] != ptrs[j]); }
  }
  nya_assert(nya_arena_memory_usage_bytes(multi_region_arena) > nya_kibyte_to_byte(4)); // must span regions
  nya_arena_destroy(multi_region_arena);

  // TEST: free_all resets usage but retains regions
  NYA_Arena* free_all_arena = nya_arena_create(.name = "free_all_arena");

  for (u32 i = 0; i < 100; ++i) {
    void* tmp = nya_arena_alloc(free_all_arena, 128);
    nya_assert(tmp != nullptr);
  }
  u64 usage_before_free_all = nya_arena_memory_usage_bytes(free_all_arena);
  nya_assert(usage_before_free_all > 0);

  nya_arena_free_all(free_all_arena);
  nya_assert(nya_arena_memory_usage_bytes(free_all_arena) == 0);

  // can allocate again after free_all
  void* after_free_all = nya_arena_alloc(free_all_arena, 64);
  nya_assert(after_free_all != nullptr);

  nya_arena_destroy(free_all_arena);

  // TEST: garbage_collect frees unused regions
  NYA_Arena* gc_arena = nya_arena_create(
          .name                         = "gc_arena",
          .region_size                  = nya_kibyte_to_byte(4),
          .garbage_collection_enabled   = false,
          .garbage_collection_threshold = 1
  );

  // allocate to create multiple regions
  for (u32 i = 0; i < 20; ++i) {
    void* tmp = nya_arena_alloc(gc_arena, nya_kibyte_to_byte(1));
    nya_assert(tmp != nullptr);
  }

  // free all and then garbage collect
  nya_arena_free_all(gc_arena);
  nya_arena_garbage_collect(gc_arena);

  // should still be usable
  void* after_gc = nya_arena_alloc(gc_arena, 64);
  nya_assert(after_gc != nullptr);

  nya_arena_destroy(gc_arena);

  // TEST: copy allocates in destination and preserves data
  NYA_Arena* src_arena = nya_arena_create(.name = "src_arena");
  NYA_Arena* dst_arena = nya_arena_create(.name = "dst_arena");

  u8* copy_src = (u8*)nya_arena_alloc(src_arena, 256);
  nya_assert(copy_src != nullptr);
  for (u32 i = 0; i < 256; ++i) { copy_src[i] = (u8)(255 - i); }

  u8* copy_dst = (u8*)nya_arena_copy(dst_arena, copy_src, 256);
  nya_assert(copy_dst != nullptr);
  nya_assert(copy_dst != copy_src);
  for (u32 i = 0; i < 256; ++i) { nya_assert(copy_dst[i] == (u8)(255 - i)); }

  // copy nullptr returns nullptr
  void* null_copy = nya_arena_copy(dst_arena, nullptr, 64);
  nya_assert(null_copy == nullptr);

  // copy zero size returns nullptr
  void* zero_copy = nya_arena_copy(dst_arena, copy_src, 0);
  nya_assert(zero_copy == nullptr);

  nya_arena_destroy(src_arena);
  nya_arena_destroy(dst_arena);

  // TEST: move transfers data and frees from source
  NYA_Arena* move_src = nya_arena_create(.name = "move_src");
  NYA_Arena* move_dst = nya_arena_create(.name = "move_dst");

  u8* move_data = (u8*)nya_arena_alloc(move_src, 128);
  nya_assert(move_data != nullptr);
  for (u32 i = 0; i < 128; ++i) { move_data[i] = (u8)i; }

  u64 src_usage_before_move = nya_arena_memory_usage_bytes(move_src);
  u8* moved_ptr             = (u8*)nya_arena_move(move_src, move_dst, move_data, 128);
  nya_assert(moved_ptr != nullptr);
  nya_assert(moved_ptr != move_data);
  for (u32 i = 0; i < 128; ++i) { nya_assert(moved_ptr[i] == (u8)i); }

  // source should have less usage after move (freed the moved block)
  nya_assert(nya_arena_memory_usage_bytes(move_src) < src_usage_before_move);

  // move nullptr returns nullptr
  void* null_move = nya_arena_move(move_src, move_dst, nullptr, 64);
  nya_assert(null_move == nullptr);

  // move zero size returns nullptr
  void* zero_move = nya_arena_move(move_src, move_dst, move_data, 0);
  nya_assert(zero_move == nullptr);

  nya_arena_destroy(move_src);
  nya_arena_destroy(move_dst);

  // TEST: free list exact-fit reuse
  NYA_Arena* exact_fit_arena = nya_arena_create(.name = "exact_fit_arena");

  void* ef_a = nya_arena_alloc(exact_fit_arena, 64);
  void* ef_b = nya_arena_alloc(exact_fit_arena, 64);
  void* ef_c = nya_arena_alloc(exact_fit_arena, 64);
  nya_assert(ef_a != nullptr && ef_b != nullptr && ef_c != nullptr);

  nya_arena_free(exact_fit_arena, ef_b, 64);
  void* ef_b_reused = nya_arena_alloc(exact_fit_arena, 64); // should reuse ef_b's slot
  nya_assert(ef_b_reused != nullptr);

  nya_arena_destroy(exact_fit_arena);

  // TEST: free list partial reuse (request smaller than free node)
  NYA_Arena* partial_fit_arena = nya_arena_create(.name = "partial_fit_arena");

  void* pf_a = nya_arena_alloc(partial_fit_arena, 256);
  void* pf_b = nya_arena_alloc(partial_fit_arena, 64);
  nya_assert(pf_a != nullptr && pf_b != nullptr);

  nya_arena_free(partial_fit_arena, pf_a, 256);

  // allocate smaller - should carve out from the freed 256-byte block
  void* pf_small1 = nya_arena_alloc(partial_fit_arena, 64);
  nya_assert(pf_small1 != nullptr);
  void* pf_small2 = nya_arena_alloc(partial_fit_arena, 64);
  nya_assert(pf_small2 != nullptr);

  nya_arena_destroy(partial_fit_arena);

  // TEST: defragmentation merges adjacent free blocks
  NYA_Arena* defrag_arena = nya_arena_create(.name = "defrag_arena", .defragmentation_enabled = true, .defragmentation_threshold = 2);

  void* d_a = nya_arena_alloc(defrag_arena, 64);
  void* d_b = nya_arena_alloc(defrag_arena, 64);
  void* d_c = nya_arena_alloc(defrag_arena, 64);
  void* d_d = nya_arena_alloc(defrag_arena, 64);
  nya_assert(d_a && d_b && d_c && d_d);

  // free b and c (adjacent blocks) - should trigger defragmentation after threshold
  nya_arena_free(defrag_arena, d_b, 64);
  nya_arena_free(defrag_arena, d_c, 64);

  // after defragmentation, allocating 128 should succeed from merged block
  void* merged = nya_arena_alloc(defrag_arena, 128);
  nya_assert(merged != nullptr);

  nya_arena_destroy(defrag_arena);

  // TEST: stress test with many allocations and frees
  NYA_Arena* stress_arena = nya_arena_create(.name = "stress_arena", .region_size = nya_kibyte_to_byte(16));

  void* stress_ptrs[100];
  for (u32 i = 0; i < 100; ++i) {
    stress_ptrs[i] = nya_arena_alloc(stress_arena, 64 + (i % 128));
    nya_assert(stress_ptrs[i] != nullptr);
  }

  // free every other allocation
  for (u32 i = 0; i < 100; i += 2) { nya_arena_free(stress_arena, stress_ptrs[i], 64 + (i % 128)); }

  // allocate again
  for (u32 i = 0; i < 100; i += 2) {
    stress_ptrs[i] = nya_arena_alloc(stress_arena, 64 + (i % 128));
    nya_assert(stress_ptrs[i] != nullptr);
  }

  nya_arena_destroy(stress_arena);

  // TEST: realloc in-place extension (last allocation in region)
  NYA_Arena* inplace_arena = nya_arena_create(.name = "inplace_arena");

  u8* inplace_ptr = (u8*)nya_arena_alloc(inplace_arena, 64);
  nya_assert(inplace_ptr != nullptr);
  for (u32 i = 0; i < 64; ++i) { inplace_ptr[i] = (u8)i; }

  // extend the last allocation (should extend in place)
  u8* inplace_extended = (u8*)nya_arena_realloc(inplace_arena, inplace_ptr, 64, 128);
  nya_assert(inplace_extended != nullptr);
  nya_assert(inplace_extended == inplace_ptr); // should be same pointer (in-place extension)

  // verify original data preserved
  for (u32 i = 0; i < 64; ++i) { nya_assert(inplace_extended[i] == (u8)i); }

  // write to extended area
  for (u32 i = 64; i < 128; ++i) { inplace_extended[i] = (u8)i; }

  nya_arena_destroy(inplace_arena);

  // TEST: realloc requiring relocation (not last allocation)
  NYA_Arena* relocate_arena = nya_arena_create(.name = "relocate_arena");

  u8* reloc_a = (u8*)nya_arena_alloc(relocate_arena, 64);
  u8* reloc_b = (u8*)nya_arena_alloc(relocate_arena, 64); // blocks reloc_a from extending
  nya_assert(reloc_a != nullptr && reloc_b != nullptr);

  for (u32 i = 0; i < 64; ++i) { reloc_a[i] = (u8)(i + 50); }

  // realloc reloc_a - cannot extend in place due to reloc_b, must relocate
  u8* reloc_a_grown = (u8*)nya_arena_realloc(relocate_arena, reloc_a, 64, 256);
  nya_assert(reloc_a_grown != nullptr);
  nya_assert(reloc_a_grown != reloc_a); // should be different pointer (relocated)

  // verify data preserved
  for (u32 i = 0; i < 64; ++i) { nya_assert(reloc_a_grown[i] == (u8)(i + 50)); }

  nya_arena_destroy(relocate_arena);

  // TEST: random stress test with alloc/realloc/free
  {
    NYA_RNG             rng       = nya_rng_create();
    NYA_Arena*          stress    = nya_arena_create(.name = "stress_arena", .region_size = nya_kibyte_to_byte(64));
    NYA_RNGDistribution size_dist = {
      .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
      .uniform = { .min = 16, .max = 4096 }
    };
    NYA_RNGDistribution op_dist = {
      .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
      .uniform = { .min = 0, .max = 99 }
    };
    NYA_RNGDistribution idx_dist = {
      .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
      .uniform = { .min = 0, .max = 255 }
    };

    void* ptrs[256]  = { 0 };
    u64   sizes[256] = { 0 };

    for (u32 iter = 0; iter < 1000000; ++iter) {
      u32 op  = nya_rng_sample_u32(&rng, op_dist);
      u32 idx = nya_rng_sample_u32(&rng, idx_dist);

      if (op < 50) {
        // alloc
        u64 size = nya_rng_sample_u64(&rng, size_dist);
        if (ptrs[idx]) nya_arena_free(stress, ptrs[idx], sizes[idx]);
        ptrs[idx]  = nya_arena_alloc(stress, size);
        sizes[idx] = size;
        nya_assert(ptrs[idx] != nullptr);
      } else if (op < 80) {
        // realloc
        if (ptrs[idx]) {
          u64   new_size = nya_rng_sample_u64(&rng, size_dist);
          void* new_ptr  = nya_arena_realloc(stress, ptrs[idx], sizes[idx], new_size);
          nya_assert(new_ptr != nullptr);
          ptrs[idx]  = new_ptr;
          sizes[idx] = new_size;
        }
      } else {
        // free
        if (ptrs[idx]) {
          nya_arena_free(stress, ptrs[idx], sizes[idx]);
          ptrs[idx]  = nullptr;
          sizes[idx] = 0;
        }
      }
    }

    nya_arena_destroy(stress);
  }


  return 0;
}
