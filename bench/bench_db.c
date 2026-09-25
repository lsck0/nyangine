/**
 * The db module's hot paths: a bound insert, a point lookup by key, and a small range scan.
 *
 * A server pays these per request, so the measurement is throughput against an in-memory database —
 * no disk, so what is timed is the SQL layer and SQLite's own work rather than the filesystem. The
 * insert cycles a bounded set of ids through INSERT OR REPLACE, so the table stays a fixed size and
 * the number is steady-state cost rather than a table that grows without end under the batch.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#define ROWS 1000U

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_db");
    defer      nya_arena_destroy(arena);

    // A scratch arena the query benches reset each iteration, so the rows a query allocates do not pile up across the batch and turn the measurement into an allocation of ever-growing size.
    NYA_Arena* scratch = nya_arena_create(.name = "bench_db_scratch");
    defer      nya_arena_destroy(scratch);

    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, ":memory:", &db), "the in-memory database opens");
    defer nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE runs (id INTEGER PRIMARY KEY, score INTEGER NOT NULL)"), "the table is created");

    // Seed the fixed row set the point and range benches read.
    for (u32 i = 0; i < ROWS; i++) {
        NYA_SqlValue seed[] = { nya_sql_s64((s64)i), nya_sql_s64((s64)(i * 7 % 500)) };
        NYA_EXPECT(nya_sql_exec_bound(db, "INSERT OR REPLACE INTO runs (id, score) VALUES (?, ?)", seed, 2), "a seed row inserts");
    }

    nya_bench_begin("db over an in-memory SQLite (per statement)");

    u64 cycle = 0;

    // A bound insert, the write a request makes. OR REPLACE over a cycling id keeps the table at ROWS so the cost measured is one statement's, not a table growing under the batch.
    nya_bench("insert or replace, bound", 1, {
        u64          id  = cycle++ % ROWS;
        NYA_SqlValue row[] = { nya_sql_s64((s64)id), nya_sql_s64((s64)(id % 500)) };
        NYA_Error inserted = nya_sql_exec_bound(db, "INSERT OR REPLACE INTO runs (id, score) VALUES (?, ?)", row, 2);
        nya_bench_keep(inserted.ok);
    });

    // A point lookup by primary key, the read behind "load this one row".
    nya_bench("point query by key", 1, {
        nya_arena_free_all(scratch);
        NYA_SqlValue  key    = nya_sql_s64((s64)(cycle++ % ROWS));
        NYA_SqlResult result = { 0 };
        NYA_Error queried = nya_sql_query(db, scratch, "SELECT id, score FROM runs WHERE id = ?", &key, 1, &result);
        nya_bench_keep(queried.ok ? result.rows->length : 0);
    });

    // A small range scan, the read behind a filtered list.
    nya_bench("range query, score > ?", 1, {
        nya_arena_free_all(scratch);
        NYA_SqlValue  bound  = nya_sql_s64(400);
        NYA_SqlResult result = { 0 };
        NYA_Error queried = nya_sql_query(db, scratch, "SELECT id, score FROM runs WHERE score > ? ORDER BY score", &bound, 1, &result);
        nya_bench_keep(queried.ok ? result.rows->length : 0);
    });

    return nya_bench_end();
}
