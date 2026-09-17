/**
 * @file vendor_steam.h
 *
 * The Steamworks SDK, which Valve ships prebuilt. Nothing to build: a Steam target links the library and carries it
 * beside the executable. The plugin declares the flat C functions it calls, so no header is included.
 * */
#pragma once

#include "nyangine/nyangine.h"

// clang-format off

#define STEAM_REDISTRIBUTABLE_LINUX_X86_64   "./vendor/steam/redistributable_bin/linux64"
#define STEAM_REDISTRIBUTABLE_WINDOWS_X86_64 "./vendor/steam/redistributable_bin/win64"

#define STEAM_LIBRARY_LINUX_X86_64   STEAM_REDISTRIBUTABLE_LINUX_X86_64 "/libsteam_api.so"
#define STEAM_LIBRARY_WINDOWS_X86_64 STEAM_REDISTRIBUTABLE_WINDOWS_X86_64 "/steam_api64.dll"

// clang-format on

NYA_VendorRule vendor_steam_linux_x86_64 = {
    .name = "steam (linux-x86_64)",

    .linker_flags = { "-L" STEAM_REDISTRIBUTABLE_LINUX_X86_64, "-lsteam_api", },
};

NYA_VendorRule vendor_steam_windows_x86_64 = {
    .name = "steam (windows-x86_64)",

    // steam_api64.lib is an MSVC import library, which lld reads in mingw mode too.
    .linker_flags = { "-L" STEAM_REDISTRIBUTABLE_WINDOWS_X86_64, "-lsteam_api64", },
};
