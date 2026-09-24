/**
 * The db layer's bound-value path, fed whatever. A server binds request bytes into statements as
 * parameters all day, so the untrusted surface is a value bound to a `?` — never the SQL text, which
 * is always a literal the program wrote. This target binds the fuzz input as both a BLOB and a TEXT
 * parameter, on insert and on a parameterised select, and leans on the sanitizers to catch a read
 * past the input or a mishandled length. The statement text is fixed; the input is only ever data.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define FUZZ_TARGET "db_sql"

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_db_sql");
    defer      nya_arena_destroy(arena);

    NYA_Database* db = nullptr;
    if (!nya_sql_open(arena, ":memory:", &db).ok) return;
    defer nya_sql_close(db);

    if (!nya_sql_exec(db, "CREATE TABLE t (id INTEGER PRIMARY KEY, blob BLOB, text TEXT)").ok) return;

    // The input as a BLOB value: exactly `size` bytes, so a length botch reads off the end under ASan.
    NYA_SqlValue insert[] = { nya_sql_s64(1), nya_sql_blob(data, size), nya_sql_text("marker") };
    (void)nya_sql_exec_bound(db, "INSERT OR REPLACE INTO t (id, blob, text) VALUES (?, ?, ?)", insert, 3);

    // The input as a TEXT value: the db layer must treat it as bytes, not trust a terminator that a fuzzed buffer does not carry. A copy is made so the text is a valid C string for that path.
    NYA_CString as_text = nya_arena_alloc(arena, size + 1);
    if (as_text != nullptr) {
        if (size > 0) nya_memcpy(as_text, data, size);
        as_text[size] = '\0';

        NYA_SqlValue text_insert[] = { nya_sql_s64(2), nya_sql_blob(data, size), nya_sql_text(as_text) };
        (void)nya_sql_exec_bound(db, "INSERT OR REPLACE INTO t (id, blob, text) VALUES (?, ?, ?)", text_insert, 3);
    }

    // The input as a bound parameter on a read, the request-filter shape.
    NYA_SqlValue     bound  = nya_sql_blob(data, size);
    NYA_SqlResult    result = { 0 };
    (void)nya_sql_query(db, arena, "SELECT id, blob, text FROM t WHERE blob = ?", &bound, 1, &result);
}

#include "tests/fuzz/fuzz.h"
