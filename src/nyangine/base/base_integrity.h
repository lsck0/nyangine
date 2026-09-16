/**
 * @file base_integrity.h
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_IntegrityStatus NYA_IntegrityStatus;

enum NYA_IntegrityStatus {
    NYA_INTEGRITY_OK,

    /** The executable's code no longer matches what it was at startup. Hooked, patched, or injected into. */
    NYA_INTEGRITY_CODE_MODIFIED,

    /** No baseline was taken, so there is nothing to compare against. Not a finding. */
    NYA_INTEGRITY_NO_BASELINE,

    /** The code region could not be located on this platform. Not a finding either. */
    NYA_INTEGRITY_UNAVAILABLE,

    NYA_INTEGRITY_STATUS_COUNT,
};

__attr_allow_unused static NYA_ConstCString NYA_INTEGRITY_STATUS_NAME_MAP[NYA_INTEGRITY_STATUS_COUNT] = {
    [NYA_INTEGRITY_OK]            = "OK",
    [NYA_INTEGRITY_CODE_MODIFIED] = "CODE_MODIFIED",
    [NYA_INTEGRITY_NO_BASELINE]   = "NO_BASELINE",
    [NYA_INTEGRITY_UNAVAILABLE]   = "UNAVAILABLE",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * ON DISK
 * ─────────────────────────────────────────────────────────
 */

/** Verifies the executable file against its stamped MAC. Panics on a mismatch. Shipping builds only. */
NYA_API void nya_integrity_assert(void);

/** Stamps the MAC into a freshly linked binary. Called by the build system after linking. */
NYA_API NYA_Error nya_integrity_patch(NYA_ConstCString binary_path, OUT u64* out_mac) __attr_no_discard;

/**
 * Whether the file at `path` still matches the hash stamped into it. Separate from
 * nya_integrity_assert so a build can check a fresh artifact and tests need no tampered process.
 * */
NYA_API b8 nya_integrity_verify_file(NYA_ConstCString path) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * IN MEMORY
 * ─────────────────────────────────────────────────────────
 */

/**
 * Records the executable's code as it is now. Call once, as early as possible, before anything can
 * hook. Later verifications compare against this.
 * */
NYA_API void nya_integrity_baseline_capture(void);

/**
 * Re-hashes the mapped code and compares it with the baseline. Reads the whole code region, so run it
 * from a timer or job rather than every frame. Only reports; what to do is game policy.
 * */
NYA_API NYA_IntegrityStatus nya_integrity_verify_code(void) __attr_no_discard;

/** Size of the code region being watched, or 0 if it could not be located. */
NYA_API u64 nya_integrity_code_size(void) __attr_no_discard;
