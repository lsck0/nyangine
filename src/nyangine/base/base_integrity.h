/**
 * @file base_integrity.h
 *
 * Runtime tamper detection for the executable. This already stops a lot of bad things happening and will be the most
 * we will go into the direction of "anti-cheat". The main reason for this is to not run corrupted executables and to
 * give at least some protection against people injecting malware after pirating.
 * */
#pragma once

#include "nyangine/base/base.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API void      nya_integrity_assert(void);
NYA_API NYA_Error nya_integrity_patch(NYA_ConstCString binary_path, OUT u64* out_crc) __attr_no_discard;
