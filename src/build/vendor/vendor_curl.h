/**
 * @file vendor_curl.h
 *
 * libcurl. cmake, static.
 *
 * TLS comes from the platform rather than a vendored copy: schannel on Windows, OpenSSL on Linux.
 * Both are present on any normal system, and neither drags a certificate bundle into the build,
 * since each uses the system trust store.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"

// clang-format off

#define CURL_SOURCE               "./vendor/curl"
#define CURL_BUILD_LINUX_X86_64   "./vendor/curl/build-linux-x86_64"
#define CURL_BUILD_WINDOWS_X86_64 "./vendor/curl/build-windows-x86_64"

#define CURL_A_LINUX_X86_64   CURL_BUILD_LINUX_X86_64 "/lib/libcurl.a"
#define CURL_A_WINDOWS_X86_64 CURL_BUILD_WINDOWS_X86_64 "/lib/libcurl.a"

#define CURL_CMAKE_COMMON           \
    NYA_CMAKE_STATIC,               \
    "-DBUILD_STATIC_LIBS=ON",       \
    "-DBUILD_CURL_EXE=OFF",         \
    "-DBUILD_TESTING=OFF",          \
    "-DCURL_DISABLE_INSTALL=ON",    \
    "-DHTTP_ONLY=ON",               \
    "-DCURL_USE_LIBPSL=OFF",        \
    "-DCURL_USE_LIBSSH2=OFF",       \
    "-DCURL_ZLIB=OFF",              \
    "-DCURL_BROTLI=OFF",            \
    "-DCURL_ZSTD=OFF"

// clang-format on

NYA_VendorRule vendor_curl_linux_x86_64 = {
    .name = "curl (linux-x86_64)",

    .includes = { "-I./vendor/curl/include/", },

    // A statically linked libcurl still needs OpenSSL's symbols; linking those dynamically is fine
    // because OpenSSL is present on any Linux system that can already browse the web.
    .linker_flags = { CURL_A_LINUX_X86_64, "-lssl", "-lcrypto", },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_curl_linux_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = CURL_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "-S", CURL_SOURCE, "-B", CURL_BUILD_LINUX_X86_64, CURL_CMAKE_COMMON, "-DCURL_USE_OPENSSL=ON", },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_curl_linux_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = CURL_A_LINUX_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", CURL_BUILD_LINUX_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};

NYA_VendorRule vendor_curl_windows_x86_64 = {
    .name = "curl (windows-x86_64)",

    .includes = { "-I./vendor/curl/include/", },

    // schannel is the Windows TLS stack, so there is no third party crypto to ship at all.
    .linker_flags = { CURL_A_WINDOWS_X86_64, "-lws2_32", "-lcrypt32", "-lbcrypt", "-lwldap32", "-lnormaliz", },

    .parts = {
        &(NYA_BuildRule){
            .name        = "vendor_curl_windows_x86_64_configure",
            .policy      = NYA_BUILD_ONCE,
            .output_file = CURL_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = {
                    "-S", CURL_SOURCE,
                    "-B", CURL_BUILD_WINDOWS_X86_64,
                    CURL_CMAKE_COMMON,
                    NYA_CMAKE_WINDOWS_TOOLCHAIN,
                    "-DCURL_USE_SCHANNEL=ON",
                    "-DCURL_USE_OPENSSL=OFF",
                },
            },

            .pre_build_hooks = { &hook_invalidate_stale_cmake_cache, },
        },
        &(NYA_BuildRule){
            .name        = "vendor_curl_windows_x86_64_compile",
            .policy      = NYA_BUILD_ONCE,
            .output_file = CURL_A_WINDOWS_X86_64,

            .command = {
                .program   = "cmake",
                .arguments = { "--build", CURL_BUILD_WINDOWS_X86_64, "--config", "Release", "--", "-j", NPROCS, },
            },
        },
    },
};
