/**
 * @file vendor_curl.h
 *
 * libcurl, cmake, static.
 *
 * TLS comes from the platform: schannel on Windows, OpenSSL on Linux. Both use the system trust store,
 * so no certificate bundle is vendored.
 * */
#pragma once

#include "nyangine/nyangine.h"

#include "build/hooks.h"
#include "build/toolchain.h"
#include "build/vendor/vendor_common.h"

// clang-format off

#define CURL_SOURCE               "./vendor/curl"
#define CURL_BUILD_LINUX_X86_64   "./vendor/curl/build-linux-x86_64"
#define CURL_BUILD_WINDOWS_X86_64 "./vendor/curl/build-windows-x86_64"

#define CURL_A_LINUX_X86_64   CURL_BUILD_LINUX_X86_64 "/lib/libcurl.a"
#define CURL_A_WINDOWS_X86_64 CURL_BUILD_WINDOWS_X86_64 "/lib/libcurl.a"

/*
 * Every optional dependency named explicitly, including disabled ones. cmake auto-detects what the host
 * has, so anything unstated makes the build depend on installed -dev packages and fails to link
 * elsewhere.
 *
 * Disabling them costs HTTP/2 and internationalised domain names, which JSON REST calls do not need.
 */
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
    "-DCURL_ZSTD=OFF",              \
    "-DUSE_NGHTTP2=OFF",            \
    "-DUSE_NGTCP2=OFF",             \
    "-DUSE_LIBIDN2=OFF",            \
    "-DCURL_USE_LIBUV=OFF",         \
    "-DCURL_USE_GSSAPI=OFF"

// clang-format on

NYA_VendorRule vendor_curl_linux_x86_64 = {
    .name = "curl (linux-x86_64)",

    .includes = { "-I./vendor/curl/include/", },

    /*
     * OpenSSL only; CURL_CMAKE_COMMON disables every other system library.
     *
     * Keep this in step with CURL_CMAKE_COMMON. Enabled there but not linked here means undefined symbols;
     * linked here but disabled there means a missing library.
     */
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

    // CURL_STATICLIB is required on Windows. Without it curl.h declares entry points dllimport and linking
    // the static archive fails with "a relevant symbol is available but cannot be used because it is not
    // an import library".
    .includes = { "-I./vendor/curl/include/", "-DCURL_STATICLIB", },

    // schannel is the Windows TLS stack, so there is no third party crypto to ship at all.
    .linker_flags = { CURL_A_WINDOWS_X86_64, "-lws2_32", "-lcrypt32", "-lbcrypt", "-lsecur32", "-lwldap32", "-lnormaliz", },

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
