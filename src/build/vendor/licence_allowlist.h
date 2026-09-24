/**
 * @file licence_allowlist.h
 *
 * The SPDX licence identifiers a vendored dependency is permitted to carry, read by `./build sbom`.
 *
 * The gate is the point of the list: `./build sbom` detects each dependency's licence by reading its
 * LICENSE/COPYING file, and fails the build if a detected licence is not one of these (or could not be
 * determined at all). A new dependency under a copyleft or unknown licence is caught the first time the
 * SBOM is regenerated, rather than discovered by a lawyer after it shipped.
 *
 * A dual-licensed dependency detects as an SPDX `OR` expression (e.g. "BSD-2-Clause OR GPL-2.0-or-later"
 * for lz4, whose `lib/` we take under the BSD side). Such an expression passes when *any* of its choices
 * is on this list, because an `OR` lets us pick the compatible one. An `AND` expression passes only when
 * *every* part is on the list.
 *
 * Every entry is a real SPDX identifier except the one LicenseRef, which names a proprietary licence with
 * no SPDX id; each carries the reason it is allowed.
 * */
#pragma once

#include "nyangine/nyangine.h"

// clang-format off

/**
 * Permitted licences, nullptr terminated. Add an entry only with the reason it is safe to link and ship,
 * on the same line, so the diff that widens the allowlist also records why.
 * */
NYA_INTERNAL const NYA_ConstCString NYA_LICENCE_ALLOWLIST[] = {
    "MIT",                              // permissive, attribution only.
    "BSD-2-Clause",                     // permissive, attribution only.
    "BSD-3-Clause",                     // permissive, attribution plus a no-endorsement clause.
    "Apache-2.0",                       // permissive, includes an explicit patent grant.
    "Zlib",                             // permissive; the SDL family.
    "curl",                             // curl's own MIT-style licence, its named SPDX id.
    "blessing",                         // the SQLite Blessing: a public-domain dedication with a warranty disclaimer.
    "CC0-1.0",                          // public-domain dedication; the fallback side of Monocypher's dual licence.
    "Unlicense",                        // public-domain dedication; the fallback side of ufbx's dual licence.
    "LicenseRef-Valve-SteamworksSDK",   // proprietary. Valve permits redistribution of redistributable_bin under the
                                        // Steamworks SDK Access Agreement; linked only in the steam-* builds, never the default.
    nullptr,
};

// clang-format on
