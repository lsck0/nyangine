/**
 * @file core_undo.h
 *
 * A bounded undo/redo history over one reflected value.
 *
 * The editor edits a struct; the history keeps a ring of past versions of it so a mistake is one call
 * to walk back. It knows nothing about the struct beyond its reflection: it snapshots through
 * nya_reflect_to_object and restores through nya_reflect_from_object, so any `@reflect` type — ints,
 * strings, nested structs, vectors, enums — records and restores with no code written for it.
 *
 * ```c
 * NYA_History* history = nya_history_create(arena, nya_reflect_of(NYA_SceneEntity), 64);
 *
 * nya_history_record(history, &entity);   // the state after each edit the user commits
 * // ... the user edits, and commits again ...
 * nya_history_record(history, &entity);
 *
 * if (nya_history_undo(history, &entity)) { ... }   // entity is now the previous committed state
 * if (nya_history_redo(history, &entity)) { ... }   // and forward again
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHAT A SNAPSHOT IS, AND WHY FULL RATHER THAN A DELTA
 * ─────────────────────────────────────────────────────────
 *
 * Each entry is a full snapshot of the value, encoded to compact bytes with the native binary format
 * (serde_nya_binary.h): the value is walked to an NYA_Object and that object is encoded once. A delta
 * would store only the fields that changed between two versions, which is smaller when an edit touches
 * one field of a large struct — but it needs a field-level diff of two reflected values, it makes
 * `undo` a replay from the last full snapshot rather than a single decode, and it saves nothing on the
 * struct sizes an editor actually holds. So v1 keeps whole snapshots: one decode restores any entry,
 * the bytes are already compact and lossless (IEEE-754 written raw, integers by width), and the memory
 * is bounded by the ring regardless. The seam to add deltas later is entirely inside this file.
 *
 * ─────────────────────────────────────────────────────────
 * THE BOUND
 * ─────────────────────────────────────────────────────────
 *
 * The ring holds at most `capacity` entries, itself capped at NYA_HISTORY_CAPACITY_MAX. Recording past
 * a full ring drops the oldest entry, so an editor left running for an afternoon costs a fixed amount
 * of memory rather than a growing one. Every byte comes from the arena passed to nya_history_create;
 * an evicted or truncated snapshot's bytes are returned to it, so the working set stays bounded across
 * any number of records.
 *
 * A record after one or more undos truncates the redo tail — the versions the user walked back past
 * are no longer reachable — which is the standard undo semantics a text editor has.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The hard cap on how many snapshots a history can keep. A `capacity` above this is clamped to it, so
 * the ring is bounded whatever a caller asks for. Large enough for an editor's undo stack, small
 * enough that the slot table is a rounding error next to the snapshots themselves.
 * */
#define NYA_HISTORY_CAPACITY_MAX 1024

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Opaque: created from an arena, its layout is nobody's business but core_undo.c's. */
typedef struct NYA_History NYA_History;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A history over values of `type`, keeping at most `capacity` of them (clamped to
 * NYA_HISTORY_CAPACITY_MAX). Everything it ever allocates comes from `arena`, so destroying the arena
 * destroys the history; there is nothing else to free.
 * */
NYA_API NYA_History* nya_history_create(NYA_Arena* arena, const NYA_TypeReflection* type, u32 capacity) __attr_no_discard;

/**
 * Returns everything the history took from its arena — every snapshot, the slot table and the handle
 * itself. Only needed to reclaim the memory while the arena lives on; destroying the arena does the
 * same and more. The handle is dangling afterwards.
 * */
NYA_API void nya_history_destroy(NYA_History* history);

/**
 * Snapshots `value` and makes it the current state. Drops the redo tail first (anything a prior undo
 * walked back past), and evicts the oldest entry if the ring is full.
 *
 * `value` must point at a value of the type the history was created for. Its snapshot is taken here and
 * now, so the caller may go on mutating it immediately.
 * */
NYA_API void nya_history_record(NYA_History* history, const void* value);

/**
 * Steps back one entry and writes that earlier state over `value`. Returns false, leaving `value`
 * untouched, when the current state is already the oldest — there is nothing earlier to go to.
 * */
NYA_API b8 nya_history_undo(NYA_History* history, OUT void* value);

/**
 * Steps forward one entry and writes that later state over `value`. Returns false, leaving `value`
 * untouched, when the current state is already the newest.
 * */
NYA_API b8 nya_history_redo(NYA_History* history, OUT void* value);

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

/** How many snapshots the ring is holding right now, redo tail included. */
NYA_API u32 nya_history_count(const NYA_History* history) __attr_no_discard;

/** Whether nya_history_undo would produce a state, so a button can be greyed out without trying it. */
NYA_API b8 nya_history_can_undo(const NYA_History* history) __attr_no_discard;

/** Whether nya_history_redo would produce a state. */
NYA_API b8 nya_history_can_redo(const NYA_History* history) __attr_no_discard;
