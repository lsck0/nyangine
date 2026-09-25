#include "nyangine-core/core/core_undo.h"

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_memory.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/serde/serde_nya_binary.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One entry: a value encoded to bytes, or {null, 0} when the slot is empty. */
typedef struct _NYA_HistorySlot {
    u8* bytes;
    u64 size;
} _NYA_HistorySlot;

/*
 * A ring of `capacity` slots. `head` is the ring index of the oldest live snapshot and `count` how
 * many are live; the redo tail is inside that range, past `cursor`. A logical index i (0 is oldest)
 * maps to the ring slot (head + i) % capacity, so eviction is a pointer bump rather than a shuffle.
 *
 * `cursor` is the logical index of the current state, valid whenever `count > 0`. undo walks it
 * towards zero, redo towards count - 1, and record truncates everything past it before appending.
 */
struct NYA_History {
    NYA_Arena*                arena;
    const NYA_TypeReflection* type;

    _NYA_HistorySlot* slots;
    u32               capacity;

    u32 count;
    u32 head;
    u32 cursor;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Returns a slot's bytes to the arena and marks it empty. Safe on an already empty slot. */
NYA_INTERNAL void _nya_history_slot_release(NYA_History* history, u32 index) {
    _NYA_HistorySlot* slot = &history->slots[index];
    if (slot->bytes != nullptr) {
        nya_arena_free(history->arena, slot->bytes, slot->size);
        slot->bytes = nullptr;
        slot->size  = 0;
    }
}

/** Copies `size` bytes into `index`, releasing whatever the slot held first. */
NYA_INTERNAL void _nya_history_slot_store(NYA_History* history, u32 index, const u8* data, u64 size) {
    _nya_history_slot_release(history, index);

    _NYA_HistorySlot* slot = &history->slots[index];
    slot->bytes            = nya_arena_alloc(history->arena, size);
    slot->size             = size;
    nya_memcpy(slot->bytes, data, size);
}

/** Decodes the snapshot at logical index `logical` over `value`. Its own bytes always decode, so a
 *  failure here is a corrupt ring or a type mismatch and takes the program down rather than lying. */
NYA_INTERNAL void _nya_history_slot_restore(NYA_History* history, u32 logical, void* value) {
    u32                     index = (history->head + logical) % history->capacity;
    const _NYA_HistorySlot* slot  = &history->slots[index];
    nya_assert(slot->bytes != nullptr, "restoring an empty history slot");

    NYA_Arena scratch = nya_arena_create_on_stack(.name = "history_restore");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Object* object = nullptr;
    NYA_EXPECT(nya_serde_nya_binary_decode(&scratch, slot->bytes, slot->size, history->type, &object),
               "decoding a %s snapshot from the undo history", history->type->name);
    NYA_EXPECT(nya_reflect_from_object(history->type, value, object), "applying a restored %s", history->type->name);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_History* nya_history_create(NYA_Arena* arena, const NYA_TypeReflection* type, u32 capacity) {
    nya_assert(arena != nullptr);
    nya_assert(type != nullptr);
    nya_assert(capacity >= 1, "an undo history needs room for at least one state");

    if (capacity > NYA_HISTORY_CAPACITY_MAX) capacity = NYA_HISTORY_CAPACITY_MAX;

    NYA_History* history = nya_arena_alloc(arena, sizeof(NYA_History));
    *history             = (NYA_History){
                    .arena    = arena,
                    .type     = type,
                    .slots    = nya_arena_alloc(arena, sizeof(_NYA_HistorySlot) * capacity),
                    .capacity = capacity,
    };
    nya_memset(history->slots, 0, sizeof(_NYA_HistorySlot) * capacity);

    return history;
}

void nya_history_destroy(NYA_History* history) {
    nya_assert(history != nullptr);

    for (u32 i = 0; i < history->capacity; i++) _nya_history_slot_release(history, i);

    NYA_Arena* arena = history->arena;
    nya_arena_free(arena, history->slots, sizeof(_NYA_HistorySlot) * history->capacity);
    nya_arena_free(arena, history, sizeof(NYA_History));
}

void nya_history_record(NYA_History* history, const void* value) {
    nya_assert(history != nullptr);
    nya_assert(value != nullptr);

    // Encode the value to compact bytes through its reflection, in a scratch arena that dies here.
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "history_record");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_Object* object = nya_reflect_to_object(&scratch, history->type, value);
    nya_assert(object != nullptr, "a value of a reflected type failed to snapshot");

    NYA_String* bytes = nullptr;
    NYA_EXPECT(nya_serde_nya_binary_encode(&scratch, object, history->type, &bytes),
               "snapshotting a %s for the undo history", history->type->name);

    // Drop the redo tail: a record after an undo makes the walked-back-past versions unreachable.
    while (history->count > history->cursor + 1) {
        u32 last = (history->head + history->count - 1) % history->capacity;
        _nya_history_slot_release(history, last);
        history->count--;
    }

    // Evict the oldest when the ring is full, so the bound holds however long the editor runs.
    if (history->count == history->capacity) {
        _nya_history_slot_release(history, history->head);
        history->head = (history->head + 1) % history->capacity;
        history->count--;
    }

    // Append the snapshot and make it the current state.
    u32 slot = (history->head + history->count) % history->capacity;
    _nya_history_slot_store(history, slot, bytes->items, bytes->length);
    history->count++;
    history->cursor = history->count - 1;
}

b8 nya_history_undo(NYA_History* history, OUT void* value) {
    nya_assert(history != nullptr);
    nya_assert(value != nullptr);

    if (history->count == 0 || history->cursor == 0) return false;

    history->cursor--;
    _nya_history_slot_restore(history, history->cursor, value);

    return true;
}

b8 nya_history_redo(NYA_History* history, OUT void* value) {
    nya_assert(history != nullptr);
    nya_assert(value != nullptr);

    if (history->count == 0 || history->cursor + 1 >= history->count) return false;

    history->cursor++;
    _nya_history_slot_restore(history, history->cursor, value);

    return true;
}

u32 nya_history_count(const NYA_History* history) {
    nya_assert(history != nullptr);
    return history->count;
}

b8 nya_history_can_undo(const NYA_History* history) {
    nya_assert(history != nullptr);
    return history->count > 0 && history->cursor > 0;
}

b8 nya_history_can_redo(const NYA_History* history) {
    nya_assert(history != nullptr);
    return history->count > 0 && history->cursor + 1 < history->count;
}
