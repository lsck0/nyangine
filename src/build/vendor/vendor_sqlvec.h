/**
 * @file vendor_sqlvec.h
 *
 * sqlite-vec (asg017/sqlite-vec), vector search for SQLite, built into a static archive from one
 * object. Adds the `vec0` virtual table and `vec_*` functions: brute force k nearest neighbour over
 * float, int8 and binary vectors with L2, cosine and hamming distance. Vectors live in the same
 * database file as the rest of the game's data. Registered by plugins/sqlite/sql.c.
 *
 * The header is generated: `sqlite-vec.h.tmpl` has `${VERSION}` placeholders that
 * `make sqlite-vec.h` fills with envsubst, hence the `gettext` host dependency.
 *
 * `SQLITE_VEC_ENABLE_AVX` selects the vectorized distance kernels. Upstream detects AVX from
 * /proc/cpuinfo; the engine already builds with `-mavx -mavx2`, so it is always on.
 *
 * SQLITE_CORE and SQLITE_VEC_STATIC are on `cflags` because consumers of sqlite-vec.h need both.
 * Without SQLITE_CORE the header pulls in sqlite3ext.h, whose macros route every `sqlite3_*` call
 * through an unset pointer. SQLITE_VEC_STATIC drops `__declspec(dllexport)`.
 *
 * This archive calls into libsqlite3.a, so it must come before it in the link. See vendor.h.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"
// For SQLITE_BUILD_*: sqlite-vec compiles against sqlite3.h, which only exists once sqlite has been
// configured, so the path to it belongs to that vendor rather than being spelled out again here.
#include "build/vendor/vendor_sqlite.h"

#define SQLVEC_SOURCE_DIRECTORY "./vendor/sqlvec"
#define SQLVEC_SOURCE           SQLVEC_SOURCE_DIRECTORY "/sqlite-vec.c"
#define SQLVEC_HEADER           SQLVEC_SOURCE_DIRECTORY "/sqlite-vec.h"
#define SQLVEC_VERSION_FILE     SQLVEC_SOURCE_DIRECTORY "/VERSION"

#define SQLVEC_BUILD_LINUX_X86_64   SQLVEC_SOURCE_DIRECTORY "/build-linux-x86_64/"
#define SQLVEC_BUILD_WINDOWS_X86_64 SQLVEC_SOURCE_DIRECTORY "/build-windows-x86_64/"

#define SQLVEC_O_LINUX_X86_64   SQLVEC_BUILD_LINUX_X86_64 "sqlite-vec.o"
#define SQLVEC_O_WINDOWS_X86_64 SQLVEC_BUILD_WINDOWS_X86_64 "sqlite-vec.o"

#define SQLVEC_A_LINUX_X86_64   SQLVEC_BUILD_LINUX_X86_64 "libsqlvec.a"
#define SQLVEC_A_WINDOWS_X86_64 SQLVEC_BUILD_WINDOWS_X86_64 "libsqlvec.a"

// clang-format off

/** What both targets compile the one source with, target flags aside. */
#define SQLVEC_CFLAGS               \
    "-c", "-O3", "-std=c11",        \
    NYA_VENDOR_SECTIONS,            \
    "-mavx", "-mavx2",              \
    "-DSQLITE_VEC_ENABLE_AVX",      \
    "-DSQLITE_CORE",                \
    "-DSQLITE_VEC_STATIC",          \
    "-I" SQLVEC_SOURCE_DIRECTORY

// clang-format on

/*
 * The header does not depend on the target, so both rules carry the same part and the second finds
 * the header newer than VERSION and skips. NYA_VendorRule cannot share a part between vendors.
 */

NYA_VendorRule vendor_sqlvec_linux_x86_64 = {
    .name = "sqlvec (linux-x86_64)",

    .cflags       = { "-DSQLITE_CORE", "-DSQLITE_VEC_STATIC", },
    .includes     = { "-I" SQLVEC_SOURCE_DIRECTORY, },
    .linker_flags = { SQLVEC_A_LINUX_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name = "vendor_sqlvec_linux_x86_64_directory",
            // See the same rule in vendor_sqlean.h: no policy means NYA_BUILD_ALWAYS, and a metarule
            // is dispatched before the policy is read, so this ran on every ./build regardless.
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = SQLVEC_BUILD_LINUX_X86_64,

            .command         = { .working_directory = SQLVEC_BUILD_LINUX_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name = "vendor_sqlvec_header",
            // Keyed on VERSION because that is what the template reads: bumping the submodule to a
            // new release has to regenerate the header, and "does sqlite-vec.h exist" would say yes.
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_VERSION_FILE,
            .output_file = SQLVEC_HEADER,

            // upstream's own recipe, which calls envsubst, git and date. Substituting here would silently miss
            // whatever placeholders the template gains.
            .command = {
                .working_directory = SQLVEC_SOURCE_DIRECTORY,
                .program           = "make",
                .arguments         = { "sqlite-vec.h", },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_linux_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_SOURCE,
            .output_file = SQLVEC_O_LINUX_X86_64,

            .command = {
                .program   = CC,
                .arguments = {
                    SQLVEC_CFLAGS,
                    "-fPIC",
                    "-I" SQLITE_BUILD_LINUX_X86_64,
                    SQLVEC_SOURCE,
                    "-o", SQLVEC_O_LINUX_X86_64,
                },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_linux_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_O_LINUX_X86_64,
            .output_file = SQLVEC_A_LINUX_X86_64,

            .command = {
                .program   = "ar",
                .arguments = { "rcs", SQLVEC_A_LINUX_X86_64, SQLVEC_O_LINUX_X86_64, },
            },
        },
    },
};

NYA_VendorRule vendor_sqlvec_windows_x86_64 = {
    .name = "sqlvec (windows-x86_64)",

    .cflags       = { "-DSQLITE_CORE", "-DSQLITE_VEC_STATIC", },
    .includes     = { "-I" SQLVEC_SOURCE_DIRECTORY, },
    .linker_flags = { SQLVEC_A_WINDOWS_X86_64, },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_windows_x86_64_directory",
            .policy      = NYA_BUILD_ONCE,
            .is_metarule = true,
            .output_file = SQLVEC_BUILD_WINDOWS_X86_64,

            .command         = { .working_directory = SQLVEC_BUILD_WINDOWS_X86_64, },
            .pre_build_hooks = { &hook_create_build_directory, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_header_windows",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_VERSION_FILE,
            .output_file = SQLVEC_HEADER,

            .command = {
                .working_directory = SQLVEC_SOURCE_DIRECTORY,
                .program           = "make",
                .arguments         = { "sqlite-vec.h", },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_windows_x86_64_compile",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_SOURCE,
            .output_file = SQLVEC_O_WINDOWS_X86_64,

            // No -fPIC: position independent code is the default and meaningless for a PE.
            .command = {
                .program   = CC,
                .arguments = {
                    FLAGS_TARGET_WINDOWS_X86_64
                    SQLVEC_CFLAGS,
                    "-I" SQLITE_BUILD_WINDOWS_X86_64,
                    SQLVEC_SOURCE,
                    "-o", SQLVEC_O_WINDOWS_X86_64,
                },
            },
        },
        &(NYA_BuildRule){
            .name        = "vendor_sqlvec_windows_x86_64_archive",
            .policy      = NYA_BUILD_IF_OUTDATED,
            .input_file  = SQLVEC_O_WINDOWS_X86_64,
            .output_file = SQLVEC_A_WINDOWS_X86_64,

            .command = {
                .program   = NYA_WINDOWS_AR,
                .arguments = { "rcs", SQLVEC_A_WINDOWS_X86_64, SQLVEC_O_WINDOWS_X86_64, },
            },
        },
    },
};
