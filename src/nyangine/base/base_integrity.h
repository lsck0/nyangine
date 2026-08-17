/**
 * @file base_integrity.h
 *
 * Tamper detection for the executable. Two independent checks, because they catch different things.
 *
 * **On disk, at startup.** `nya_integrity_assert` hashes the executable file and compares it against
 * a value the build stamped into it. Catches a patched binary, a corrupted download, and anything
 * injected into the file itself.
 *
 * **In memory, while running.** `nya_integrity_verify_code` hashes the mapped executable pages and
 * compares them against a baseline taken at startup. The file check cannot see any of this: inline
 * hooks, injected trampolines and patched-out branches all happen after the file has been read, and
 * are how cheats actually work. Run it periodically; it is cheap enough to do on a timer.
 *
 * Both use SipHash with a key, not a plain checksum. A CRC can be recomputed by anyone, so it
 * detects accident and not intent.
 *
 * **What this is not.** The key is inside the binary that ships. Anyone willing to reverse it can
 * extract the key, recompute, and re-stamp. This raises the cost of tampering from "open a hex
 * editor" to "reverse engineer the check first", and that is the whole claim. It is not a defence
 * against a determined attacker, who owns the machine and always wins eventually. For anything that
 * genuinely must not be forged, the answer is a server that does not trust the client.
 * */
#pragma once

#include "nyangine/base/base.h"
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
 * Whether the file at `path` still matches the hash stamped into it. What nya_integrity_assert asks
 * about its own executable.
 *
 * Separate from the assert so a build can check an artifact it has just produced, and so the check
 * itself is testable without arranging for a tampered process to be running.
 * */
NYA_API b8 nya_integrity_verify_file(NYA_ConstCString path) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * IN MEMORY
 * ─────────────────────────────────────────────────────────
 */

/**
 * Records what the executable's code looks like right now.
 *
 * Call once, as early as possible, before anything has had a chance to hook. Every later
 * verification compares against this.
 * */
NYA_API void nya_integrity_baseline_capture(void);

/**
 * Re-hashes the mapped code and compares it against the baseline.
 *
 * Safe to call as often as you like, though it reads the whole code region, so a timer or a job is
 * a better home for it than the frame loop. Reports rather than acts: what to do about a modified
 * process is a game policy decision, not something a base module should make.
 * */
NYA_API NYA_IntegrityStatus nya_integrity_verify_code(void) __attr_no_discard;

/** Size of the code region being watched, or 0 if it could not be located. */
NYA_API u64 nya_integrity_code_size(void) __attr_no_discard;
