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
 * Every optional dependency named explicitly, including the ones turned off: cmake auto-detects
 * what it finds on the host, so anything unstated makes the build depend on which -dev packages
 * happen to be installed. nghttp2 and libidn2 were once left off this list, got auto-enabled
 * wherever present, and then failed to link on any machine without them — worked locally, broke CI.
 *
 * Turning them off costs HTTP/2 and internationalised domain names, neither needed for the JSON
 * REST this is here for.
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
     * OpenSSL only, because CURL_CMAKE_COMMON turns off everything else that could pull a system
     * library in. TLS is the one exception the file's header comment already explains: it comes
     * from the platform rather than being vendored, and OpenSSL is present on any Linux system that
     * can already browse the web.
     *
     * Keep this in step with CURL_CMAKE_COMMON above. A dependency enabled there and unnamed here
     * is a wall of undefined symbols at the final link; named here and disabled there is a library
     * the linker cannot find. Both have happened.
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

    // CURL_STATICLIB is not optional on Windows. Without it curl.h declares every entry point
    // __declspec(dllimport), the compiler emits calls through an import thunk, and the link fails
    // against the static archive with "a relevant symbol is available but cannot be used because it
    // is not an import library" — which describes the symptom and not the cause.
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
