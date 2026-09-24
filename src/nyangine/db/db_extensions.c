/**
 * @file db_extensions.c
 * */

// SQLITE_CORE tells sqlite3ext.h these are compiled in, not loaded; asserting it here fails a misconfigured build rule with this sentence instead of at link time.
#ifndef SQLITE_CORE
#error "db_extensions.c must be compiled with -DSQLITE_CORE, see vendor_sqlean.h"
#endif

#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include "define/extension.h"
#include "fuzzy/extension.h"
#include "math/extension.h"
#include "stats/extension.h"
#include "text/extension.h"
#include "time/extension.h"
#include "unicode/extension.h"
#include "uuid/extension.h"
#include "vsv/extension.h"

// ipaddr is POSIX sockets (arpa/inet.h, inet_pton); upstream leaves it out of its Windows bundle too.
#ifndef _WIN32
#include "ipaddr/extension.h"
#endif

// ───────────────────────────────────── VENDORED SOURCES ─────────────────────────────────────

// Resolved through -I./vendor/sqlean/src, which the vendor rule puts on the command line.

#include "define/eval.c"
#include "define/extension.c"
#include "define/manage.c"
#include "define/module.c"

#include "fuzzy/caver.c"
#include "fuzzy/common.c"
#include "fuzzy/damlev.c"
#include "fuzzy/editdist.c"
#include "fuzzy/extension.c"
#include "fuzzy/hamming.c"
#include "fuzzy/jarowin.c"
#include "fuzzy/leven.c"
#include "fuzzy/osadist.c"
#include "fuzzy/phonetic.c"
#include "fuzzy/rsoundex.c"
#include "fuzzy/soundex.c"
#include "fuzzy/translit.c"

#include "math/extension.c"

#include "stats/extension.c"
#include "stats/scalar.c"
#include "stats/series.c"

#include "text/bstring.c"
#include "text/extension.c"
#include "text/rstring.c"
#include "text/runes.c"
#include "text/utf8/case.c"
#include "text/utf8/rune.c"
#include "text/utf8/utf8.c"

#include "time/duration.c"
#include "time/extension.c"
#include "time/time.c"

#include "unicode/extension.c"

#include "vsv/extension.c"

#ifndef _WIN32
#include "ipaddr/extension.c"
#endif

// uuid/extension.c and time/time.c both define a static `timespec_now`; one translation unit here, so one is renamed.
#define timespec_now sqlean_uuid_timespec_now
#include "uuid/extension.c"
#undef timespec_now

// ───────────────────────────────────── ENTRY POINT ─────────────────────────────────────

int nya_sqlean_init(sqlite3* db, char** error_message, const sqlite3_api_routines* api);

/**
 * Registers every extension above on `db`.
 * */
// Every registration's return code is checked; the first failure stops the rest.
#define _NYA_SQLEAN_TRY(call)                                                                                                                        \
    do {                                                                                                                                             \
        int _nya_sqlean_result = (call);                                                                                                             \
        if (_nya_sqlean_result != SQLITE_OK) return _nya_sqlean_result;                                                                              \
    } while (0)

int nya_sqlean_init(sqlite3* db, char** error_message, const sqlite3_api_routines* api) {
    (void)error_message;
    SQLITE_EXTENSION_INIT2(api);

    _NYA_SQLEAN_TRY(fuzzy_init(db));
    _NYA_SQLEAN_TRY(math_init(db));
    _NYA_SQLEAN_TRY(stats_init(db));
    _NYA_SQLEAN_TRY(text_init(db));
    _NYA_SQLEAN_TRY(time_init(db));
    _NYA_SQLEAN_TRY(unicode_init(db));
    _NYA_SQLEAN_TRY(uuid_init(db));
    _NYA_SQLEAN_TRY(vsv_init(db));

#ifndef _WIN32
    _NYA_SQLEAN_TRY(ipaddr_init(db));
#endif

    // Last, and upstream is emphatic about why: `define` lets a user defined function be written in
    // SQL, and the body of one may call any sqlean function. Registering it before the others means
    // any such function fails with "no such function" instead of working.
    _NYA_SQLEAN_TRY(define_init(db));

    return SQLITE_OK;
}
