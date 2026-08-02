/**
 * @file vendor_sqlean.h
 *
 * sqlean, a set of SQLite extensions.
 *
 * Deliberately has no build parts. sqlean's own Makefile only produces loadable shared extensions
 * (.so / .dll), which is the opposite of how the rest of the tree is linked. The supported way to
 * use these statically is the way SQLite intends: compile the `sqlite3-<name>.c` entry points into
 * the program and register them with sqlite3_auto_extension, rather than loading them at runtime.
 *
 * So this vendor contributes include paths only. When the engine starts using an extension, add
 * its `src/sqlite3-<name>.c` to the unity build next to the other sources.
 * */
#pragma once

#include "nyangine/nyangine.h"

#define SQLEAN_SOURCE "./vendor/sqlean/src"

NYA_VendorRule vendor_sqlean_linux_x86_64 = {
    .name = "sqlean (linux-x86_64)",

    .includes = { "-I" SQLEAN_SOURCE, },
};

NYA_VendorRule vendor_sqlean_windows_x86_64 = {
    .name = "sqlean (windows-x86_64)",

    .includes = { "-I" SQLEAN_SOURCE, },
};
