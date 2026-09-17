/**
 * @file memory.h
 *
 * ```c
 * NYA_Entity* table = nya_memory_reserve(NYA_ENTITY_MAX * sizeof(NYA_Entity));
 * nya_assert(nya_memory_commit(table, nya_memory_page_size()));   // backs the first page
 * defer nya_memory_release(table, NYA_ENTITY_MAX * sizeof(NYA_Entity));
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * VIRTUAL MEMORY
 * ─────────────────────────────────────────────────────────
 */

/** Bytes per page, which every commit is rounded to. */
NYA_API u64 nya_memory_page_size(void) __attr_no_discard;

/**
 * Address space for `size` bytes with nothing behind it, so a table can keep stable pointers without paying for its
 * capacity. Null when the address space is exhausted.
 * */
NYA_API void* nya_memory_reserve(u64 size) __attr_no_discard;

/**
 * Makes a reserved range readable and writable, rounded out to whole pages. Committed pages read as zero and take no
 * physical memory until written.
 * */
NYA_API b8 nya_memory_commit(void* address, u64 size) __attr_no_discard;

/** Returns a whole reservation, committed or not. `size` is what was reserved. */
NYA_API void nya_memory_release(void* address, u64 size);

/*
 * ─────────────────────────────────────────────────────────
 * RESIDENCE
 * ─────────────────────────────────────────────────────────
 */

/**
 * Bytes of a mapped range that are in physical memory right now, in whole pages. The range need not be page aligned.
 * Costs a system call and a pass over its pages, so it is for reports, not per frame.
 * */
NYA_API u64 nya_memory_resident_bytes(const void* address, u64 size) __attr_no_discard;

/** The whole process's resident set: the working set on Windows. */
NYA_API u64 nya_memory_process_resident_bytes(void) __attr_no_discard;
