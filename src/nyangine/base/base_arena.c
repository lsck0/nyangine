#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void  _nya_arena_align_and_pad_size(NYA_Arena* arena, u64* size);
NYA_INTERNAL void  _nya_arena_region_destroy(NYA_Arena* arena, NYA_ArenaRegion* region);
NYA_INTERNAL void* _nya_arena_free_list_find(NYA_ArenaFreeList* free_list, u64 size) __attr_no_discard;
NYA_INTERNAL void  _nya_arena_free_list_add(NYA_ArenaRegion* region, void* ptr, u64 size);
NYA_INTERNAL void  _nya_arena_free_list_defragment(NYA_ArenaFreeList* free_list);
NYA_INTERNAL void  _nya_arena_free_list_destroy(NYA_ArenaFreeList* free_list);

NYA_INTERNAL NYA_ArenaActionCallback _nya_arena_action_callback = nullptr;

NYA_Arena* nya_arena_global = nullptr;
NYA_Arena* nya_arena_temp   = nullptr;

__attr_constructor NYA_INTERNAL void _nya_arena_init(void) {
    nya_arena_global = nya_arena_create(.name = "global_arena");
    nya_arena_temp   = nya_arena_create(.name = "temp_arena");
}

__attr_destructor NYA_INTERNAL void _nya_arena_shutdown(void) {
    nya_arena_destroy(nya_arena_global);
    nya_arena_destroy(nya_arena_temp);
    nya_arena_global = nullptr;
    nya_arena_temp   = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NON-DEBUG API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Arena* _nya_arena_nodebug_create_with_options(NYA_ArenaOptions options) {
    NYA_Arena* arena = nya_malloc(sizeof(NYA_Arena));
    *arena           = _nya_arena_nodebug_create_with_options_on_stack(options);
    return arena;
}

NYA_Arena _nya_arena_nodebug_create_with_options_on_stack(NYA_ArenaOptions options) {
    nya_assert(options.region_size >= nya_kibyte_to_byte(4), "Region size must be at least 4 KiB.");
    nya_assert(options.region_size % options.alignment == 0, "Region size must be divisible by alignment.");
    nya_assert(options.alignment >= 8, "Alignment must be at least 8 bytes.");
    nya_assert(options.alignment % 2 == 0, "Alignment must be a power of two.");
    nya_assert(ASAN_PADDING % options.alignment == 0, "ASAN padding must be divisible by alignment.");

    NYA_Arena arena = {
        .options = options,
        .head    = nullptr,
        .tail    = nullptr,
    };

    return arena;
}

void* _nya_arena_nodebug_alloc(NYA_Arena* arena, u64 size) __attr_malloc {
    nya_assert(arena != nullptr);

    if (size == 0) return nullptr;

    _nya_arena_align_and_pad_size(arena, &size);

    if (size > arena->options.region_size) goto skip_search;

    nya_dll_foreach (arena, region) {
        // check free list if the average free size is bigger than the requested size
        if (region->free_list != nullptr && region->free_list->average_free_size >= (f32)size) {
            void* ptr = _nya_arena_free_list_find(region->free_list, size);
            if (ptr != nullptr) {
                asan_unpoison_memory_region(ptr, size - ASAN_PADDING);
                asan_poison_memory_region((u8*)ptr + size - ASAN_PADDING, ASAN_PADDING);

                return ptr;
            }
        }

        // check if there is space left in the region
        if (region->capacity - region->used >= size) {
            u8* ptr = region->memory + region->used;

            // align pointer
            uintptr_t aligned_ptr = ((uintptr_t)ptr + (arena->options.alignment - 1)) & ~(arena->options.alignment - 1);
            u64       padding     = aligned_ptr - (uintptr_t)ptr;

            // check if we still have enough space after alignment padding
            if (region->capacity - region->used - padding >= size) {
                ptr           = (u8*)aligned_ptr;
                region->used += size + padding;

                asan_unpoison_memory_region(ptr, size - ASAN_PADDING);
                asan_poison_memory_region(ptr + size - ASAN_PADDING, ASAN_PADDING);

                return ptr;
            }
        }

        continue;
    }

skip_search:
    // we didnt find a region with enough space or didnt want to search
    u64              new_region_size   = nya_max(arena->options.region_size, size);
    NYA_ArenaRegion* new_region        = nya_malloc(sizeof(NYA_ArenaRegion));
    u8*              new_region_memory = nya_malloc(new_region_size);
    nya_assert(new_region != nullptr);
    nya_assert(new_region_memory != nullptr);

    // align the initial pointer
    uintptr_t aligned_memory  = ((uintptr_t)new_region_memory + (arena->options.alignment - 1)) & ~(arena->options.alignment - 1);
    u64       initial_padding = aligned_memory - (uintptr_t)new_region_memory;

    *new_region = (NYA_ArenaRegion){
        .used       = size + initial_padding,
        .capacity   = new_region_size,
        .memory     = new_region_memory,
        .gc_counter = 0,
        .free_list  = nullptr,
        .next       = nullptr,
        .prev       = nullptr,
    };
    nya_dll_node_push_back(arena, new_region);

    u8* ptr = (u8*)aligned_memory;

    asan_unpoison_memory_region(ptr, size - ASAN_PADDING);
    asan_poison_memory_region(ptr + size - ASAN_PADDING, new_region->capacity - size + ASAN_PADDING);

    return ptr;
}

void* _nya_arena_nodebug_realloc(NYA_Arena* arena, void* ptr, u64 old_size, u64 new_size) {
    nya_assert(arena != nullptr);

    // edge cases
    if (ptr == nullptr) return nullptr;
    if (new_size == old_size) return ptr;
    if (new_size == 0) {
        _nya_arena_nodebug_free(arena, ptr, old_size);
        return nullptr;
    }

    _nya_arena_align_and_pad_size(arena, &old_size);
    _nya_arena_align_and_pad_size(arena, &new_size);
    u8* old_ptr = (u8*)ptr;

    // realloc to smaller size = partial free
    if (new_size < old_size) {
        // use old memory as the asan padding
        asan_poison_memory_region(old_ptr + new_size - ASAN_PADDING, ASAN_PADDING);

        // only free the excess if it's larger than ASAN_PADDING
        if (old_size - new_size > ASAN_PADDING) { /**/
            _nya_arena_nodebug_free(arena, old_ptr + new_size, old_size - new_size - ASAN_PADDING);
        }

        return old_ptr;
    }

    // find the region
    nya_dll_foreach (arena, region) {
        if (!(region->memory <= old_ptr && old_ptr < region->memory + region->capacity)) continue;

        // no checking if there is a free slot after in the free list

        // if its the last allocation in the region, we can maybe just extend it
        if (old_ptr + old_size == region->memory + region->used && region->used + (new_size - old_size) <= region->capacity) {
            region->used += new_size - old_size;

            asan_unpoison_memory_region(old_ptr, new_size - ASAN_PADDING);

            return old_ptr;
        }

        // allocate new memory and copy, dont double dipp on the padding
        void* new_ptr = _nya_arena_nodebug_alloc(arena, new_size - ASAN_PADDING);
        nya_memmove(new_ptr, old_ptr, old_size - ASAN_PADDING);
        _nya_arena_nodebug_free(arena, old_ptr, old_size - ASAN_PADDING);

        return new_ptr;
    }

    nya_unreachable();
}

void _nya_arena_nodebug_free(NYA_Arena* arena, void* ptr, u64 size) {
    nya_assert(arena != nullptr);

    if (ptr == nullptr || size == 0) return;

    _nya_arena_align_and_pad_size(arena, &size);
    u8* casted_ptr = (u8*)ptr;

    asan_poison_memory_region(ptr, size);

    // find the region
    nya_dll_foreach (arena, region) {
        if (!(region->memory <= casted_ptr && casted_ptr < region->memory + region->capacity)) continue;

        // last allocation, just move the used pointer back
        if (casted_ptr + size == region->memory + region->used) {
            region->used -= size;
            return;
        }

        // add to free list otherwise
        _nya_arena_free_list_add(region, ptr, size);

        // maybe defragment
        if (arena->options.defragmentation_enabled && region->free_list->defragmentation_counter >= arena->options.defragmentation_threshold) {
            _nya_arena_free_list_defragment(region->free_list);
        }

        return;
    }

    nya_unreachable();
}

void _nya_arena_nodebug_free_all(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    for (NYA_ArenaRegion* region = arena->head; region != nullptr;) {
        NYA_ArenaRegion* next = region->next;

        // deallocate region if unused long enough
        if (arena->options.defragmentation_enabled && region->used == 0 && region->gc_counter++ >= arena->options.garbage_collection_threshold) {
            _nya_arena_region_destroy(arena, region);
            region = next;
            continue;
        }

        region->used = 0;

        asan_poison_memory_region(region->memory, region->capacity);

        if (region->free_list != nullptr) {
            _nya_arena_free_list_destroy(region->free_list);
            region->free_list = nullptr;
        }

        region = next;
    }
}

void _nya_arena_nodebug_garbage_collect(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    for (NYA_ArenaRegion* region = arena->head; region != nullptr;) {
        NYA_ArenaRegion* next = region->next;
        if (region->used > 0) {
            region = next;
            continue;
        }

        _nya_arena_region_destroy(arena, region);
        region = next;
    }
}

void _nya_arena_nodebug_destroy(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    _nya_arena_nodebug_destroy_on_stack(arena);

    nya_free(arena);
}

void _nya_arena_nodebug_destroy_on_stack(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    for (NYA_ArenaRegion* region = arena->head; region != nullptr;) {
        NYA_ArenaRegion* next = region->next;
        _nya_arena_region_destroy(arena, region);
        region = next;
    }

    arena->head = nullptr;
    arena->tail = nullptr;
}

void* _nya_arena_nodebug_copy(NYA_Arena* dst, void* ptr, u64 size) {
    nya_assert(dst != nullptr);
    if (ptr == nullptr || size == 0) return nullptr;

    void* new_ptr = nya_arena_alloc(dst, size);
    if (!new_ptr) return nullptr;

    nya_memmove(new_ptr, ptr, size);
    return new_ptr;
}

void* _nya_arena_nodebug_move(NYA_Arena* src, NYA_Arena* dst, void* ptr, u64 size) {
    nya_assert(src != nullptr);
    nya_assert(dst != nullptr);

    if (ptr == nullptr || size == 0) return nullptr;

    void* new_ptr = nya_arena_alloc(dst, size);
    if (!new_ptr) return nullptr;

    nya_memmove(new_ptr, ptr, size);
    nya_arena_free(src, ptr, size);
    return new_ptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DEBUG API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Arena* _nya_arena_debug_create_with_options(NYA_ArenaOptions options, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_ARENA_NEW,
        .arena_name    = options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return _nya_arena_nodebug_create_with_options(options);
}

NYA_Arena _nya_arena_debug_create_with_options_on_stack(NYA_ArenaOptions options, const char* function, const char* file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_ARENA_NEW,
        .arena_name    = options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return _nya_arena_nodebug_create_with_options_on_stack(options);
}

void* _nya_arena_debug_alloc(NYA_Arena* arena, u64 size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    void* ptr = _nya_arena_nodebug_alloc(arena, size);
    if (ptr == nullptr) return nullptr;

    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_ALLOC,
      .arena_name    = arena->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_alloc      = {
          .ptr  = ptr,
          .size = size,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return ptr;
}

void* _nya_arena_debug_realloc(NYA_Arena* arena, void* ptr, u64 old_size, u64 new_size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    void* new_ptr = _nya_arena_nodebug_realloc(arena, ptr, old_size, new_size);
    if (new_ptr == nullptr && new_size != 0) return nullptr;

    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_REALLOC,
      .arena_name    = arena->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_realloc    = {
          .old_ptr  = ptr,
          .old_size = old_size,
          .new_ptr  = new_ptr,
          .new_size = new_size,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return new_ptr;
}

void _nya_arena_debug_free(NYA_Arena* arena, void* ptr, u64 size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_FREE,
      .arena_name    = arena->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_free       = {
          .ptr  = ptr,
          .size = size,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_free(arena, ptr, size);
}

void _nya_arena_debug_free_all(NYA_Arena* arena, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_FREE_ALL,
        .arena_name    = arena->options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_free_all(arena);
}

void _nya_arena_debug_garbage_collect(NYA_Arena* arena, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_GARBAGE_COLLECT,
        .arena_name    = arena->options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_garbage_collect(arena);
}

void _nya_arena_debug_destroy(NYA_Arena* arena, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_ARENA_DESTROY,
        .arena_name    = arena->options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_destroy(arena);
}

void _nya_arena_debug_destroy_on_stack(NYA_Arena* arena, const char* function, const char* file, u32 line) {
    NYA_ArenaAction action = {
        .type          = NYA_ARENA_ACTION_ARENA_DESTROY,
        .arena_name    = arena->options.name,
        .file_name     = file,
        .line_number   = line,
        .function_name = function,
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    _nya_arena_nodebug_destroy_on_stack(arena);
}

void* _nya_arena_debug_copy(NYA_Arena* dst, void* ptr, u64 size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    void* copy_ptr = _nya_arena_nodebug_copy(dst, ptr, size);
    if (copy_ptr == nullptr) return nullptr;

    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_COPY,
      .arena_name    = dst->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_copy       = {
          .ptr      = ptr,
          .size     = size,
          .copy_ptr = copy_ptr,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return copy_ptr;
}

void* _nya_arena_debug_move(NYA_Arena* src, NYA_Arena* dst, void* ptr, u64 size, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    void* move_ptr = _nya_arena_nodebug_move(src, dst, ptr, size);
    if (move_ptr == nullptr) return nullptr;

    NYA_ArenaAction action = {
      .type          = NYA_ARENA_ACTION_MOVE,
      .arena_name    = dst->options.name,
      .file_name     = file,
      .line_number   = line,
      .function_name = function,
      .as_move       = {
          .ptr             = ptr,
          .size            = size,
          .move_arena_name = dst->options.name,
          .move_ptr        = move_ptr,
      },
    };
    if (_nya_arena_action_callback) _nya_arena_action_callback(action);

    return move_ptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_arena_actions_set_callback(NYA_ArenaActionCallback callback) {
    _nya_arena_action_callback = callback;
}

u64 nya_arena_memory_usage_bytes(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    u64 total_usage = 0;
    nya_dll_foreach (arena, region) total_usage += region->used;

    return total_usage;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_arena_align_and_pad_size(NYA_Arena* arena, u64* size) {
    nya_assert(arena != nullptr);
    nya_assert(size != nullptr);

    if (*size == 0) return;

    *size = ((*size + (arena->options.alignment - 1)) & ~(arena->options.alignment - 1)) + ASAN_PADDING;
}

NYA_INTERNAL void _nya_arena_region_destroy(NYA_Arena* arena, NYA_ArenaRegion* region) {
    nya_dll_node_unlink(arena, region);

    if (region->free_list != nullptr) _nya_arena_free_list_destroy(region->free_list);
    nya_free(region->memory);
    nya_free(region);
}

NYA_INTERNAL void* _nya_arena_free_list_find(NYA_ArenaFreeList* free_list, u64 size) {
    nya_assert(free_list != nullptr);

    nya_dll_foreach (free_list, node) {
        if (node->size < size) continue;

        // exact fit
        if (node->size == size) {
            void* ptr = node->ptr;

            nya_dll_node_unlink(free_list, node);
            nya_free(node);

            free_list->node_counter--;
            if (free_list->node_counter == 0) {
                free_list->average_free_size = 0.0F;
            } else {
                free_list->average_free_size =
                    (free_list->average_free_size * (f32)(free_list->node_counter + 1) - (f32)size) / (f32)free_list->node_counter;
            }

            return ptr;
        }

        // free node is bigger than we need
        void* ptr = node->ptr;

        node->ptr                     = (u8*)node->ptr + size;
        node->size                   -= size;
        free_list->average_free_size  = (free_list->average_free_size * (f32)free_list->node_counter - (f32)size) / (f32)(free_list->node_counter);

        return ptr;
    }

    return nullptr;
}

NYA_INTERNAL void _nya_arena_free_list_add(NYA_ArenaRegion* region, void* ptr, u64 size) {
    nya_assert(ptr != nullptr);

    if (region->free_list == nullptr) {
        region->free_list = nya_malloc(sizeof(NYA_ArenaFreeList));
        nya_assert(region->free_list != nullptr);

        *region->free_list = (NYA_ArenaFreeList){
            .node_counter            = 0,
            .average_free_size       = 0.0F,
            .defragmentation_counter = 0,
            .head                    = nullptr,
            .tail                    = nullptr,
        };
    }

    NYA_ArenaFreeListNode* new_node = nya_malloc(sizeof(NYA_ArenaFreeListNode));
    nya_assert(new_node != nullptr);

    *new_node = (NYA_ArenaFreeListNode){
        .ptr  = ptr,
        .size = size,
        .prev = nullptr,
        .next = nullptr,
    };

    if (region->free_list->head == nullptr) {
        region->free_list->head = new_node;
        region->free_list->tail = new_node;
    } else {
        nya_dll_foreach (region->free_list, free_node) {
            if (free_node->next && (u8*)free_node->next->ptr < (u8*)new_node->ptr) continue;

            // we are now in the situation free_node < new_node < free_node->next by address
            nya_dll_node_link(region->free_list, free_node, new_node, free_node->next);
            break;
        }
    }

    if (region->free_list->node_counter == 0) {
        region->free_list->node_counter      = 1;
        region->free_list->average_free_size = (f32)size;
    } else {
        region->free_list->average_free_size =
            ((region->free_list->average_free_size * (f32)region->free_list->node_counter) + (f32)size) / (f32)(region->free_list->node_counter + 1);
        region->free_list->node_counter++;
    }

    /*
     * Counts frees since the last defragmentation, which is what the threshold in
     * _nya_arena_nodebug_free is compared against.
     *
     * Nothing incremented this before, so the comparison was always 0 >= threshold and
     * _nya_arena_free_list_defragment never ran at all: adjacent free blocks were never merged, and
     * a region that had released a contiguous span could not satisfy an allocation the size of it.
     *
     * Saturating rather than wrapping because the counter is a u8 and defragmentation can be turned
     * off, in which case nothing ever resets it — and FLAGS_SANITIZE treats unsigned wraparound as
     * an error.
     * */
    if (region->free_list->defragmentation_counter < U8_MAX) region->free_list->defragmentation_counter++;
}

NYA_INTERNAL void _nya_arena_free_list_defragment(NYA_ArenaFreeList* free_list) {
    nya_assert(free_list != nullptr);

    for (NYA_ArenaFreeListNode* node = free_list->head; node != nullptr && node->next != nullptr;) {
        // check if current node is directly before the next node
        if ((u8*)node->ptr + node->size == (u8*)node->next->ptr) {
            NYA_ArenaFreeListNode* next      = node->next;
            u64                    next_size = next->size;

            // merge nodes
            node->size += next_size;
            nya_dll_node_unlink(free_list, next);
            nya_free(next);
            free_list->node_counter--;

            // continue with the same node
            continue;
        }

        node = node->next;
    }

    /*
     * Recomputed rather than adjusted as nodes merge.
     *
     * The incremental form subtracted the absorbed node's size from the running total, but merging
     * two free blocks does not release any bytes — it only means one node describes what two did.
     * Every merge therefore pushed the average further below the truth, and since
     * _nya_arena_nodebug_alloc only searches the free list when average_free_size is at least the
     * requested size, a region that had just coalesced a large contiguous span would refuse to
     * allocate out of it and grow instead. Sixteen freed blocks in a row was enough to see it.
     *
     * The walk is the same order as the merge loop above, so this costs nothing worth saving.
     * */
    u64 total = 0;
    u64 count = 0;
    for (NYA_ArenaFreeListNode* node = free_list->head; node != nullptr; node = node->next) {
        total += node->size;
        count++;
    }

    free_list->node_counter      = count;
    free_list->average_free_size = count == 0 ? 0.0F : (f32)total / (f32)count;

    free_list->defragmentation_counter = 0;
}

NYA_INTERNAL void _nya_arena_free_list_destroy(NYA_ArenaFreeList* free_list) {
    nya_assert(free_list != nullptr);

    for (NYA_ArenaFreeListNode* node = free_list->head; node != nullptr;) {
        NYA_ArenaFreeListNode* next = node->next;
        nya_free(node);
        node = next;
    }

    nya_free(free_list);
}
