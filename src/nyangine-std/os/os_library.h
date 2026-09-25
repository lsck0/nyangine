/**
 * @file os_library.h
 *
 * Asking the loader whether a shared library is here, and nothing more.
 *
 * ```c
 * if (!nya_os_library_probe("libssl.so.3")) return; // this machine has no OpenSSL
 * ```
 *
 * One question: can this soname be loaded right now. It is the same resolution the dynamic linker
 * does, so a yes here is a library a `dlopen` elsewhere will find, and a no is a missing dependency
 * caught before the code that needs it runs. Nothing is kept open: the library is closed again at
 * once, because the point is to check, not to use.
 *
 * `dlopen` on Linux, `LoadLibrary` on Windows. This is not the hot-reload path, which resolves the
 * program's own symbols by name after a code reload; it is for a genuine external runtime dependency
 * a build takes from the machine rather than vendoring — see base_preflight.h, which turns a no into a
 * descriptive crash at startup.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// FUNCTIONS

/**
 * Whether the shared library `soname` can be loaded right now, closing it again at once.
 *
 * `soname` is what the loader is given: a bare soname like `libssl.so.3` is searched for on the
 * loader's own path, and a path with a separator in it is taken as it stands. False when it is on no
 * search path, when loading it failed, or when `soname` is null or empty — this layer is below the
 * assertion machinery, so a bad argument is refused rather than asserted.
 * */
NYA_API b8 nya_os_library_probe(const char* soname) __attr_no_discard;
