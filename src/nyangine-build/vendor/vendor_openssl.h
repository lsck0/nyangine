/**
 * @file vendor_openssl.h
 *
 * OpenSSL, which is the system's rather than vendored: a rule with no parts and nothing but a link
 * line.
 *
 * Every other entry under src/nyangine-build/vendor/ builds something. This one builds nothing, because the
 * whole point of taking OpenSSL from the machine is that the machine's own updates are what fix it —
 * see tls.h for the argument, which is the one pgp.h makes about gpg.
 *
 * It is listed anyway rather than being two strings appended to the link line, so the reason is
 * written where a reader looking for OpenSSL would look, and so `./build stats` counts it among what
 * this program depends on.
 *
 * Linux only. The Windows build reaches TLS through Schannel inside curl and has no libssl to link, so
 * `tls` compiles to its "no TLS library" half there; see FLAGS_MODULE_TLS_WINDOWS_X86_64 in flags.h.
 * */
#pragma once

#include "nyangine-build/vendor/vendor_common.h"

NYA_VendorRule vendor_openssl_linux_x86_64 = {
    .name = "openssl (linux-x86_64, the system's)",

    // No `.includes`: the headers are wherever the system keeps them, which is on the default search path of the compiler that is building against that system's libraries.
    .linker_flags = { "-lssl", "-lcrypto", },

    // No `.parts`, and no `.options_file`: there is nothing to build and nothing whose options could go stale. nya_vendor_build over this rule does nothing at all, which is correct.
};
