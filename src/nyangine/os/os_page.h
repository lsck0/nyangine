/**
 * @file os_page.h
 *
 * Virtual memory straight from the operating system: address space, and pages behind it.
 *
 * ```c
 * NYA_Entity* table = nya_os_page_reserve(NYA_ENTITY_MAX * sizeof(NYA_Entity));
 * nya_assert(nya_os_page_commit(table, nya_os_page_size()));   // backs the first page
 * defer (void)nya_os_page_release(table, NYA_ENTITY_MAX * sizeof(NYA_Entity));
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

// FUNCTIONS AND MACROS

// VIRTUAL MEMORY

/** Bytes per page, which every commit is rounded to. */
NYA_API u64 nya_os_page_size(void) __attr_no_discard;

/**
 * Address space for `size` bytes with nothing behind it, so a table can keep stable pointers without paying for its
 * capacity. Null when the address space is exhausted.
 * */
NYA_API void* nya_os_page_reserve(u64 size) __attr_no_discard;

/**
 * Makes a reserved range readable and writable, rounded out to whole pages. Committed pages read as zero and take no
 * physical memory until written.
 * */
NYA_API b8 nya_os_page_commit(void* address, u64 size) __attr_no_discard;

/**
 * Returns a whole reservation, committed or not. `size` is what was reserved. False when the
 * operating system refused, which means the address or the size did not name a reservation: nothing
 * below base asserts, so the caller is the one that decides that is a bug.
 * */
NYA_API b8 nya_os_page_release(void* address, u64 size) __attr_no_discard;

// RESIDENCE

/**
 * Bytes of a mapped range that are in physical memory right now, in whole pages. The range need not be page aligned.
 * Costs a system call and a pass over its pages, so it is for reports, not per frame.
 * */
NYA_API u64 nya_os_page_resident_bytes(const void* address, u64 size) __attr_no_discard;

/** The whole process's resident set: the working set on Windows. */
NYA_API u64 nya_os_process_resident_bytes(void) __attr_no_discard;
