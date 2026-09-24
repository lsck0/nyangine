/**
 * @file sbom.c
 *
 * `./build sbom`: a software bill of materials for the vendored dependencies, a licence-allowlist gate
 * over it, and a hook for a CVE scan.
 *
 * The tree of record is `.gitmodules` and the commit each submodule is pinned to in HEAD, so the SBOM is
 * generated from the same facts the build itself links against and cannot drift from a hand-kept list.
 * For every vendored dependency it captures the name, the pinned commit (the version a checkout resolves
 * to), the upstream source URL, and the licence — detected by reading the LICENSE/COPYING file in
 * `vendor/<name>` and matching it to an SPDX identifier.
 *
 * Two outputs, one walk: a machine-readable CycloneDX 1.5 JSON document a scanner consumes, and a
 * human-readable table beside it. Both are generated, so both are gitignored; only this command, the
 * allowlist it reads (licence_allowlist.h) and the summary printed to the terminal are the artifacts a
 * reviewer sees.
 *
 * The gate: a detected licence that is not on NYA_LICENCE_ALLOWLIST — or one that could not be determined
 * — fails the command, so a new copyleft or unknown dependency is caught the first time the SBOM is
 * regenerated rather than in an audit later.
 *
 * The CVE step is a hook, not a vendored scanner: it runs osv-scanner against the CycloneDX document when
 * osv-scanner is on PATH and the scan is opted into (it reaches the network, so it is off by default and
 * turned on in CI with NYA_SBOM_CVE_SCAN=1). Absent or not opted in, it prints how CI runs it and skips,
 * rather than failing a build that has no vulnerability database to consult.
 * */
#include "build/build.h"

#include "build/vendor/licence_allowlist.h"

/* CONSTANTS */

/** Where the generated SBOM goes. Gitignored: it is derived from the submodules on every run. */
#define SBOM_OUTPUT_DIRECTORY "./sbom"
#define SBOM_CDX_FILE         SBOM_OUTPUT_DIRECTORY "/nyangine.cdx.json"
#define SBOM_SUMMARY_FILE     SBOM_OUTPUT_DIRECTORY "/nyangine.sbom.txt"

/** The submodule manifest the SBOM is generated from. */
#define SBOM_GITMODULES_FILE "./.gitmodules"

/** The CVE scanner and the environment variable that opts its network-reaching scan in. See the file comment. */
#define SBOM_OSV_SCANNER_PROGRAM "osv-scanner"
#define SBOM_CVE_SCAN_ENV        "NYA_SBOM_CVE_SCAN"

/** More vendored submodules than there will ever be. Exceeding it is an assertion, not a silent truncation. */
#define SBOM_MAX_DEPENDENCIES 64

/** What the licence could not be resolved to. Never on the allowlist, so it fails the gate. */
#define SBOM_LICENCE_UNKNOWN "UNKNOWN"

/* TYPES */

/** One vendored dependency, fully resolved: what the SBOM row and the gate both read. */
typedef struct SbomDependency {
    NYA_ConstCString name;         // the submodule directory name under vendor/, e.g. "curl".
    NYA_ConstCString path;         // its path from the repo root, e.g. "vendor/curl".
    NYA_ConstCString url;          // the upstream source URL from .gitmodules.
    NYA_ConstCString commit;       // the commit HEAD pins the submodule to.
    NYA_ConstCString purl;         // a package URL a scanner keys on, or "" when the URL is not github.
    NYA_ConstCString licence;      // the detected SPDX identifier or expression, or SBOM_LICENCE_UNKNOWN.
    NYA_ConstCString licence_file; // which file the licence was read from, for the summary.
} SbomDependency;

/* PRIVATE API DECLARATION */

/** Parses .gitmodules into `deps`, returning how many submodules it found. */
NYA_INTERNAL u32 _sbom_read_submodules(NYA_Arena* arena, SbomDependency* deps);

/** The commit HEAD pins `path` to, read from the committed tree so the working copy need not be checked out. */
NYA_INTERNAL NYA_ConstCString _sbom_pinned_commit(NYA_Arena* arena, NYA_ConstCString path);

/** pkg:github/<owner>/<repo>@<commit> derived from a github URL, or "" when it is not one. */
NYA_INTERNAL NYA_ConstCString _sbom_purl(NYA_Arena* arena, NYA_ConstCString url, NYA_ConstCString commit);

/** Detects the SPDX licence of the dependency at `path`, setting `*out_file` to what it read. */
NYA_INTERNAL NYA_ConstCString _sbom_detect_licence(NYA_Arena* arena, NYA_ConstCString path, NYA_ConstCString* out_file);

/** Whether a detected licence (identifier or SPDX OR/AND expression) is covered by the allowlist. */
NYA_INTERNAL b8 _sbom_licence_allowed(NYA_Arena* arena, NYA_ConstCString licence);

/** Renders the dependencies as a CycloneDX 1.5 JSON document. */
NYA_INTERNAL NYA_String* _sbom_render_cyclonedx(NYA_Arena* arena, const SbomDependency* deps, u32 count);

/** Runs the CVE scanner over the generated SBOM, or explains how CI does and skips. */
NYA_INTERNAL void _sbom_cve_scan(NYA_Arena* arena);

/* PUBLIC API IMPLEMENTATION */

void sbom_runner(NYA_ArgCommand* command) {
    nya_unused(command);

    NYA_Arena* arena = nya_arena_create(.name = "sbom_runner");
    defer nya_arena_destroy(arena);

    SbomDependency* deps  = nya_arena_alloc(arena, SBOM_MAX_DEPENDENCIES * sizeof(SbomDependency));
    u32             count = _sbom_read_submodules(arena, deps);

    // Resolve each dependency: the pinned commit from the tree, the package URL, and the licence from disk.
    for (u32 i = 0; i < count; i++) {
        deps[i].commit  = _sbom_pinned_commit(arena, deps[i].path);
        deps[i].purl    = _sbom_purl(arena, deps[i].url, deps[i].commit);
        deps[i].licence = _sbom_detect_licence(arena, deps[i].path, &deps[i].licence_file);
    }

    // The machine-readable SBOM.
    NYA_EXPECT(nya_filesystem_create_directory(SBOM_OUTPUT_DIRECTORY), "while creating " SBOM_OUTPUT_DIRECTORY);
    NYA_String* cdx = _sbom_render_cyclonedx(arena, deps, count);
    NYA_EXPECT(nya_file_write(SBOM_CDX_FILE, cdx), "while writing " SBOM_CDX_FILE);

    // The human-readable summary: the same rows as a table, written beside the JSON and printed here so a
    // reviewer sees the licence set without opening a file.
    NYA_String* summary = nya_string_create(arena);
    nya_string_extend(summary, "Software bill of materials for the vendored dependencies.\n\n");
    nya_string_extend_sprintf(summary, "%-18s %-14s %-34s %s\n", "DEPENDENCY", "COMMIT", "LICENCE", "SOURCE");
    for (u32 i = 0; i < count; i++) {
        // Short commit: the first twelve hex characters, enough to identify it in the upstream history.
        char short_commit[13] = { 0 };
        for (u32 c = 0; c < 12 && deps[i].commit[c] != '\0'; c++) short_commit[c] = deps[i].commit[c];

        nya_string_extend_sprintf(summary, "%-18s %-14s %-34s %s\n", deps[i].name, short_commit, deps[i].licence, deps[i].url);
    }
    NYA_EXPECT(nya_file_write(SBOM_SUMMARY_FILE, summary), "while writing " SBOM_SUMMARY_FILE);

    nya_log_info("Wrote " SBOM_CDX_FILE " and " SBOM_SUMMARY_FILE " for " FMTu32 " dependencies.", count);
    nya_string_print(summary);

    // The gate. Collected rather than failed on the first, so one run names every offending dependency.
    u32 rejected = 0;
    for (u32 i = 0; i < count; i++) {
        if (_sbom_licence_allowed(arena, deps[i].licence)) continue;

        nya_log_error("%s is under '%s', which is not on the licence allowlist (see src/build/vendor/licence_allowlist.h).", deps[i].name,
                      deps[i].licence);
        rejected++;
    }
    if (rejected > 0) {
        nya_log_panic("The licence allowlist rejected " FMTu32 " dependenc%s; see above.", rejected, rejected == 1 ? "y" : "ies");
    }
    nya_log_info("Licence allowlist: all " FMTu32 " dependencies are permitted.", count);

    // The CVE hook, last: the SBOM exists and the licences are clean, so a scan has something to read.
    _sbom_cve_scan(arena);
}

/* PRIVATE API IMPLEMENTATION */

u32 _sbom_read_submodules(NYA_Arena* arena, SbomDependency* deps) {
    nya_assert(deps != nullptr);

    NYA_String* contents = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(SBOM_GITMODULES_FILE, contents), "while reading " SBOM_GITMODULES_FILE);

    // One record per `[submodule "..."]` stanza. The first split element is whatever precedes the first
    // header, which no stanza, so it is skipped by the missing path/url check below.
    NYA_ArrayᐸNYA_Stringᐳ* blocks = nya_string_split(arena, contents, "[submodule ");

    u32 count = 0;
    nya_array_foreach (blocks, block) {
        NYA_ConstCString path = nullptr;
        NYA_ConstCString url  = nullptr;

        NYA_ArrayᐸNYA_Stringᐳ* lines = nya_string_split_lines(arena, block);
        nya_array_foreach (lines, line) {
            nya_string_trim_whitespace(line);
            if (nya_string_starts_with(line, "path = ")) {
                nya_string_strip_prefix(line, "path = ");
                path = nya_string_to_cstring(arena, line);
            } else if (nya_string_starts_with(line, "url = ")) {
                nya_string_strip_prefix(line, "url = ");
                url = nya_string_to_cstring(arena, line);
            }
        }

        if (path == nullptr || url == nullptr) continue; // the preamble, or a malformed stanza.

        // The name is the last path component: "vendor/curl" -> "curl".
        NYA_ConstCString name = path;
        for (u32 i = 0; path[i] != '\0'; i++) {
            if (path[i] == '/') name = &path[i + 1];
        }

        nya_assert(count < SBOM_MAX_DEPENDENCIES, "More than %d vendored submodules.", SBOM_MAX_DEPENDENCIES);
        deps[count] = (SbomDependency){ .name = name, .path = path, .url = url };
        count++;
    }

    nya_assert(count > 0, "No submodules found in " SBOM_GITMODULES_FILE ".");
    return count;
}

NYA_ConstCString _sbom_pinned_commit(NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(path != nullptr);

    // The committed tree, not the working copy: `git ls-tree HEAD <path>` prints "<mode> commit <sha>\t<path>"
    // for a submodule gitlink, so the pinned commit is read even when the submodule is not checked out.
    NYA_String* line = build_capture(arena, "git", (const NYA_ConstCString[]){ "ls-tree", "HEAD", path, nullptr });

    NYA_ArrayᐸNYA_Stringᐳ* words = nya_string_split_words(arena, line);
    if (words->length < 3) nya_log_panic("'%s' is not a submodule in HEAD: git ls-tree gave no gitlink.", path);

    // "<mode> commit <sha>": the third word is the commit.
    return nya_string_to_cstring(arena, &words->items[2]);
}

NYA_ConstCString _sbom_purl(NYA_Arena* arena, NYA_ConstCString url, NYA_ConstCString commit) {
    nya_assert(url != nullptr);

    // Only github is vendored here, and its purl namespace is the owner/repo the clone URL already spells.
    NYA_String* rest = nya_string_create(arena);
    nya_string_extend(rest, url);
    if (!nya_string_starts_with(rest, "https://github.com/")) return ""; // a non-github remote has no github purl.

    nya_string_strip_prefix(rest, "https://github.com/");
    nya_string_strip_suffix(rest, ".git");

    NYA_String* purl = nya_string_create(arena);
    nya_string_extend_sprintf(purl, "pkg:github/" NYA_FMT_STRING "@%s", NYA_FMT_STRING_ARG(rest), commit);
    return nya_string_to_cstring(arena, purl);
}

/** Reads a licence file into `out` if it exists under `path`, returning whether it did. */
NYA_INTERNAL b8 _sbom_read_licence_file(NYA_Arena* arena, NYA_ConstCString path, NYA_ConstCString file, NYA_String* out) {
    NYA_String* full = nya_string_create(arena);
    nya_string_extend_sprintf(full, "%s/%s", path, file);
    if (!nya_filesystem_exists(nya_string_to_cstring(arena, full))) return false;

    NYA_EXPECT(nya_file_read(nya_string_to_cstring(arena, full), out), "while reading a licence file");
    return true;
}

NYA_ConstCString _sbom_detect_licence(NYA_Arena* arena, NYA_ConstCString path, NYA_ConstCString* out_file) {
    nya_assert(path != nullptr);
    nya_assert(out_file != nullptr);

    // Dual Apache/MIT as two files side by side (sqlite-vec): recognised before reading, since neither
    // file alone spells the choice. This is the Rust-crate convention.
    NYA_String* scratch = nya_string_create(arena);
    if (_sbom_read_licence_file(arena, path, "LICENSE-APACHE", scratch)
        && _sbom_read_licence_file(arena, path, "LICENSE-MIT", nya_string_create(arena))) {
        *out_file = "LICENSE-APACHE + LICENSE-MIT";
        return "Apache-2.0 OR MIT";
    }

    // The file the licence text lives in, by the names upstreams actually use. Readme.txt last, for the
    // Steamworks SDK, which ships no LICENSE file at all.
    static const NYA_ConstCString candidates[] = {
        "LICENSE", "LICENSE.txt", "LICENSE.md", "LICENCE", "LICENCE.md",
        "COPYING", "COPYING.txt", "COPYRIGHT", "Readme.txt", nullptr,
    };

    NYA_String*      text = nya_string_create(arena);
    NYA_ConstCString file = nullptr;
    for (u32 i = 0; candidates[i] != nullptr; i++) {
        if (_sbom_read_licence_file(arena, path, candidates[i], text)) {
            file = candidates[i];
            break;
        }
    }

    if (file == nullptr) {
        *out_file = "(none found)";
        return SBOM_LICENCE_UNKNOWN;
    }
    *out_file = file;

    // Matched most-specific first: the dual-licence pointer files and the named licences that a generic
    // family test would otherwise swallow, then the families themselves. lz4's file names GPL-2.0 as one
    // side of a dual licence; catching its dual marker here keeps the generic GPL test from flagging it.
    if (nya_string_contains(text, "BSD 2-Clause") && nya_string_contains(text, "GPL-2.0")) return "BSD-2-Clause OR GPL-2.0-or-later";
    if (nya_string_contains(text, "2-clause BSD") && (nya_string_contains(text, "CC-0") || nya_string_contains(text, "CC0"))) {
        return "BSD-2-Clause OR CC0-1.0";
    }
    if (nya_string_contains(text, "ALTERNATIVE A - MIT") && nya_string_contains(text, "Public Domain")) return "MIT OR Unlicense";

    if (nya_string_contains(text, "COPYRIGHT AND PERMISSION NOTICE")) return "curl";
    if (nya_string_contains(text, "SQLite Is Public Domain")) return "blessing";
    if (nya_string_contains(text, "Valve Corporation") && nya_string_contains(text, "Steamworks SDK")) return "LicenseRef-Valve-SteamworksSDK";

    if (nya_string_contains(text, "Apache License") && nya_string_contains(text, "Version 2.0")) return "Apache-2.0";

    // The zlib licence: SDL and its satellites. Its two unmistakable sentences, so plain BSD or MIT text
    // is not mistaken for it.
    if (nya_string_contains(text, "This software is provided 'as-is'")
        && nya_string_contains(text, "Permission is granted to anyone to use this software for any purpose")) {
        return "Zlib";
    }

    if (nya_string_contains(text, "Redistribution and use in source and binary forms")) {
        // The third BSD clause is the no-endorsement one; its presence is what separates 3-clause from 2.
        return nya_string_contains(text, "endorse or promote") ? "BSD-3-Clause" : "BSD-2-Clause";
    }

    if (nya_string_contains(text, "Permission is hereby granted, free of charge")) return "MIT";

    // Copyleft, deliberately after the dual-licence pointers above: a bare GPL file is not on the
    // allowlist and fails the gate, which is the case this whole command exists to catch.
    if (nya_string_contains(text, "GNU GENERAL PUBLIC LICENSE")) return "GPL-2.0-or-later";
    if (nya_string_contains(text, "This is free and unencumbered software released into the public domain")) return "Unlicense";

    return SBOM_LICENCE_UNKNOWN;
}

/** Whether one bare SPDX identifier (already trimmed) is on the allowlist. */
NYA_INTERNAL b8 _sbom_identifier_allowed(NYA_ConstCString identifier) {
    for (u32 i = 0; NYA_LICENCE_ALLOWLIST[i] != nullptr; i++) {
        if (nya_string_equals(identifier, NYA_LICENCE_ALLOWLIST[i])) return true;
    }
    return false;
}

b8 _sbom_licence_allowed(NYA_Arena* arena, NYA_ConstCString licence) {
    nya_assert(licence != nullptr);

    if (nya_string_equals(licence, SBOM_LICENCE_UNKNOWN)) return false;

    NYA_String* expression = nya_string_create(arena);
    nya_string_extend(expression, licence);

    // "A OR B": we may pick either side, so one permitted choice is enough.
    if (nya_string_contains(expression, " OR ")) {
        NYA_ArrayᐸNYA_Stringᐳ* choices = nya_string_split(arena, expression, " OR ");
        nya_array_foreach (choices, choice) {
            nya_string_trim_whitespace(choice);
            if (_sbom_identifier_allowed(nya_string_to_cstring(arena, choice))) return true;
        }
        return false;
    }

    // "A AND B": both obligations bind, so every part must be permitted.
    if (nya_string_contains(expression, " AND ")) {
        NYA_ArrayᐸNYA_Stringᐳ* parts = nya_string_split(arena, expression, " AND ");
        nya_array_foreach (parts, part) {
            nya_string_trim_whitespace(part);
            if (!_sbom_identifier_allowed(nya_string_to_cstring(arena, part))) return false;
        }
        return true;
    }

    return _sbom_identifier_allowed(licence);
}

/** Appends the CycloneDX `licenses` array for a detected licence: an expression, a LicenseRef name, or an SPDX id. */
NYA_INTERNAL void _sbom_render_licences(NYA_String* out, NYA_ConstCString licence) {
    if (nya_string_contains(licence, " OR ") || nya_string_contains(licence, " AND ")) {
        nya_string_extend_sprintf(out, "        \"licenses\": [{ \"expression\": \"%s\" }],\n", licence);
    } else if (nya_string_starts_with(licence, "LicenseRef") || nya_string_equals(licence, SBOM_LICENCE_UNKNOWN)) {
        // CycloneDX carries a non-SPDX licence by name, not by id.
        nya_string_extend_sprintf(out, "        \"licenses\": [{ \"license\": { \"name\": \"%s\" } }],\n", licence);
    } else {
        nya_string_extend_sprintf(out, "        \"licenses\": [{ \"license\": { \"id\": \"%s\" } }],\n", licence);
    }
}

NYA_String* _sbom_render_cyclonedx(NYA_Arena* arena, const SbomDependency* deps, u32 count) {
    nya_assert(deps != nullptr);

    // A real timestamp, from date rather than an engine time API, since this is a build tool and date is
    // already how the packaging scripts stamp things.
    NYA_String* timestamp = build_capture(arena, "date", (const NYA_ConstCString[]){ "-u", "+%Y-%m-%dT%H:%M:%SZ", nullptr });
    nya_string_trim_whitespace(timestamp);

    NYA_String* out = nya_string_create(arena);
    nya_string_extend(out, "{\n");
    nya_string_extend(out, "  \"bomFormat\": \"CycloneDX\",\n");
    nya_string_extend(out, "  \"specVersion\": \"1.5\",\n");
    nya_string_extend(out, "  \"version\": 1,\n");
    nya_string_extend(out, "  \"metadata\": {\n");
    nya_string_extend_sprintf(out, "    \"timestamp\": \"" NYA_FMT_STRING "\",\n", NYA_FMT_STRING_ARG(timestamp));
    nya_string_extend(out, "    \"tools\": [{ \"vendor\": \"nyangine\", \"name\": \"build sbom\", \"version\": \"" VERSION "\" }],\n");
    nya_string_extend(out, "    \"component\": { \"type\": \"application\", \"name\": \"nyangine\", \"version\": \"" VERSION "\" }\n");
    nya_string_extend(out, "  },\n");
    nya_string_extend(out, "  \"components\": [\n");

    for (u32 i = 0; i < count; i++) {
        nya_string_extend(out, "    {\n");
        nya_string_extend(out, "      \"type\": \"library\",\n");
        nya_string_extend_sprintf(out, "      \"bom-ref\": \"%s@%s\",\n", deps[i].name, deps[i].commit);
        nya_string_extend_sprintf(out, "      \"name\": \"%s\",\n", deps[i].name);
        nya_string_extend_sprintf(out, "      \"version\": \"%s\",\n", deps[i].commit);
        if (deps[i].purl[0] != '\0') nya_string_extend_sprintf(out, "      \"purl\": \"%s\",\n", deps[i].purl);
        _sbom_render_licences(out, deps[i].licence);
        nya_string_extend(out, "      \"externalReferences\": [\n");
        nya_string_extend_sprintf(out, "        { \"type\": \"vcs\", \"url\": \"%s\" }\n", deps[i].url);
        nya_string_extend(out, "      ]\n");
        nya_string_extend_sprintf(out, "    }%s\n", i + 1 < count ? "," : "");
    }

    nya_string_extend(out, "  ]\n");
    nya_string_extend(out, "}\n");
    return out;
}

/**
 * Whether `program --version` runs and exits cleanly.
 *
 * Both halves matter: a program missing from PATH still spawns — the forked child fails execvp and
 * `_exit(127)`s, so nya_command_run returns ok with a 127 exit — so presence is the clean exit, not the
 * spawn. osv-scanner answers --version with 0, which is what this is for.
 * */
NYA_INTERNAL b8 _sbom_program_exists(NYA_ConstCString program) {
    NYA_Command probe = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SUPPRESS,
        .program   = program,
        .arguments = { "--version" },
    };
    NYA_Error ran = nya_command_run(&probe);
    return ran.ok && probe.exit_code == 0;
}

void _sbom_cve_scan(NYA_Arena* arena) {
    nya_assert(arena != nullptr);

    if (!_sbom_program_exists(SBOM_OSV_SCANNER_PROGRAM)) {
        nya_log_info("CVE scan skipped: '%s' is not installed. CI installs it and runs", SBOM_OSV_SCANNER_PROGRAM);
        nya_log_info("  %s scan --sbom=" SBOM_CDX_FILE, SBOM_OSV_SCANNER_PROGRAM);
        nya_log_info("against the OSV database; a finding fails that job. It is a hook, not vendored here.");
        return;
    }

    // Present but off by default: the scan reaches the OSV database over the network, so a developer's
    // build does not phone home unasked. CI sets NYA_SBOM_CVE_SCAN=1 to turn it on.
    NYA_ConstCString opt_in = getenv(SBOM_CVE_SCAN_ENV);
    if (opt_in == nullptr || opt_in[0] == '\0') {
        nya_log_info("'%s' is installed but the CVE scan is off (it reaches the network). Enable it with", SBOM_OSV_SCANNER_PROGRAM);
        nya_log_info("  " SBOM_CVE_SCAN_ENV "=1 ./build sbom");
        return;
    }

    nya_log_info("Scanning " SBOM_CDX_FILE " for known vulnerabilities with %s.", SBOM_OSV_SCANNER_PROGRAM);
    NYA_Command scan = {
        .flags     = NYA_COMMAND_FLAG_OUTPUT_SHOW,
        .program   = SBOM_OSV_SCANNER_PROGRAM,
        .arena     = arena,
        .arguments = { "scan", "--sbom=" SBOM_CDX_FILE },
    };

    NYA_Error ran = nya_command_run(&scan);
    if (!ran.ok) {
        // Could not reach the database (offline, rate limited): a skip with a notice, not a spurious
        // build failure. A real finding is a non-zero exit from a scanner that did run, handled below.
        nya_log_warn("CVE scan could not run to completion (%s); skipping rather than failing the build.", ran.message);
        return;
    }

    if (scan.exit_code != 0) nya_log_panic("%s reported known vulnerabilities; see above.", SBOM_OSV_SCANNER_PROGRAM);
    nya_log_info("CVE scan: no known vulnerabilities in the vendored dependencies.");
}
