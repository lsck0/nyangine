/**
 * @file sqlean_extensions.c
 *
 * The sqlean extensions the engine registers, gathered into one statically linkable entry point.
 *
 * **Not part of the unity build.** plugins.c does not include this file, and must not: it is
 * compiled on its own by vendor_sqlean.h into libsqlean-<target>.a, with third party warning levels
 * rather than the engine's. Everything below is vendored code, and compiling it as part of the
 * engine translation unit would mean either its warnings in every build or the engine's warnings
 * turned down to accommodate it. sql.c only ever sees nya_sqlean_init, declared there and defined
 * here.
 *
 * sqlean ships one `sqlite3-<name>.c` per extension, each an entry point for a *loadable* .so or
 * .dll. None of them is compiled here. They cannot be: every one defines its own static
 * `sqlean_version` function, so any two in the same translation unit collide, and loading extensions
 * at runtime is not how anything else in this tree is linked. What they actually do is call the
 * `<name>_init(db)` that lives in the extension's own directory, so this file includes those sources
 * directly and calls the same init functions from one place — which is exactly what upstream's own
 * `sqlite3-sqlean.c` bundle does.
 *
 * ## What is registered, and what is not
 *
 * | Extension | Gives SQL                                                    |
 * | ---       | ---                                                          |
 * | define    | user defined functions, stored in the database               |
 * | fuzzy     | edit distance, soundex, transliteration                      |
 * | ipaddr    | IP address parsing and containment (not on Windows)          |
 * | math      | trigonometry, logarithms, rounding                           |
 * | stats     | median, percentile, stddev, and generate_series              |
 * | text      | split, replace, pad, reverse, and the rest of string handling |
 * | time      | a real duration and instant type                             |
 * | unicode   | case folding and normalization that knows about non-ASCII    |
 * | uuid      | uuid4 and uuid7 generation                                   |
 * | vsv       | reading CSV and friends as a virtual table                   |
 *
 * Three of the fourteen are deliberately absent:
 *
 * - **crypto** needs `src/crypto/xxhash.impl.h`, which is not in the repository — upstream's
 *   Makefile curls it from another project at build time. Downloading a source file during a build
 *   is not something this tree does, and base already has hashing and base64 anyway.
 * - **fileio** hands SQL `readfile()` and `writefile()`. A query that can write anywhere the process
 *   can is not something to switch on by default in a game, where queries may come from mods or save
 *   data. Nothing stops a game registering it itself.
 * - **regexp** carries a complete copy of PCRE2, which does not survive being compiled as one
 *   translation unit — its sources rely on per-file macro setup and fail on `PCRE2_EXP_DEFN`. It
 *   would need an archive of its own. SQLite's own LIKE and GLOB cover the common cases.
 * */

// SQLITE_CORE is what tells sqlite3ext.h these are compiled into the program rather than loaded, so
// SQLITE_EXTENSION_INIT1 becomes nothing and the API is called directly instead of through the
// dispatch pointer. The build rule passes it; asserting it here means a misconfigured rule fails
// with this sentence instead of at link time on a missing sqlite3_api symbol.
#ifndef SQLITE_CORE
#error "sqlean_extensions.c must be compiled with -DSQLITE_CORE, see vendor_sqlean.h"
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

// ipaddr is POSIX sockets: arpa/inet.h and inet_pton. Upstream leaves it out of its own Windows
// bundle for the same reason.
#ifndef _WIN32
#include "ipaddr/extension.h"
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * VENDORED SOURCES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Resolved through -I./vendor/sqlean/src, which the vendor rule puts on the command line, so these
 * read the same way sqlean's own headers include each other.
 */

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

/*
 * uuid/extension.c and time/time.c both define a static `timespec_now`, identical in body and
 * private to each file. Separate translation units upstream, so upstream never notices; one
 * translation unit here, so one of them has to be renamed.
 *
 * Renamed rather than split into a second object: the function is static and called only from within
 * its own file, so the macro cannot reach anything else, and the alternative costs an object file
 * and an archive member to work around two lines.
 */
#define timespec_now sqlean_uuid_timespec_now
#include "uuid/extension.c"
#undef timespec_now

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENTRY POINT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

int nya_sqlean_init(sqlite3* db, char** error_message, const sqlite3_api_routines* api);

/**
 * Registers every extension above on `db`.
 *
 * Shaped as a sqlite3_auto_extension entry point — the (db, errmsg, api) signature and the
 * SQLITE_OK return — so sql.c can hand it straight to SQLite without a wrapper. Under SQLITE_CORE
 * `api` is unused and SQLITE_EXTENSION_INIT2 expands to nothing, but the parameters stay because
 * that is the shape SQLite calls.
 * */
/*
 * Every registration's return code is checked, and the first failure stops the rest.
 *
 * All twelve were discarded and SQLITE_OK returned unconditionally, so a failed registration — an
 * out of memory, a name already taken — produced a database that reported itself as fully set up
 * and then failed with "no such function" at the point of use, arbitrarily far away. sqlite3_open
 * would have succeeded, and sql.c had nothing to notice.
 *
 * The registrations are not rolled back on failure. SQLite has no API to unregister a function, and
 * the connection is about to be closed by the caller that sees the error anyway.
 */
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
