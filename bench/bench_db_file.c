/**
 * The db module against a real file, which is what a server runs on: bench_db.c measures the SQL
 * layer over `:memory:`, this measures what the journal, the sync policy and the page cache add.
 *
 * Four shapes: an insert per transaction (the write a request makes), inserts batched in one
 * transaction (an import), a point read by key over a table larger than SQLite's default page cache,
 * and one writer beside READERS reader threads, each on a connection of its own, which is the shape
 * db.h's one-connection-per-thread model gives a threaded server.
 *
 * The file sits in the working directory, so the numbers include whatever filesystem that is on.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include <stdatomic.h>
#include <stdio.h>

#define DB_PATH "./_bench_db_file.db"

/** Rows the read benches look up in; at ~140 bytes a row that is ~3 MiB, past SQLite's 2 MiB default cache. */
#define ROWS 20'000U

/** Inserts one batched transaction carries. */
#define BATCH 1'000U

/** Reader threads beside the one writer in the mixed case. */
#define READERS 8U

/** One sample of the mixed case: long enough to cover several WAL checkpoints. */
#define MIXED_WINDOW_MS 250U

/** The payload every row carries, so a row is the size of a small record rather than two integers. */
#define PAYLOAD "a row the size of a small record, a title and a line of text, the kind a note or a session keeps beside its key"

/** An odd multiplier walks every key in [0, ROWS) in a scattered order, so a read is not the neighbour of the last. */
#define KEY_STRIDE 7919U

static void remove_files(void) {
    (void)remove(DB_PATH);
    (void)remove(DB_PATH "-journal");
    (void)remove(DB_PATH "-wal");
    (void)remove(DB_PATH "-shm");
}

typedef struct {
    atomic_bool  stop;
    _Atomic(u64) reads;
    _Atomic(u64) writes;
    _Atomic(u64) failures;
} Mixed;

/** What one thread of the mixed case gets: the shared counters and a connection opened for it on the main thread. */
typedef struct {
    Mixed*        mixed;
    NYA_Database* db;
} MixedThread;

static NYA_Database* open_connection(NYA_Arena* arena) {
    NYA_Database* db = nullptr;
    NYA_EXPECT(nya_sql_open(arena, DB_PATH, &db), "the bench database opens");
    return db;
}

static void mixed_reader(void* data) {
    Mixed*        mixed   = ((MixedThread*)data)->mixed;
    NYA_Database* db      = ((MixedThread*)data)->db;
    NYA_Arena*    scratch = nya_arena_create(.name = "bench_db_file_reader_scratch");
    defer         nya_arena_destroy(scratch);

    u64 key = (u64)(uintptr_t)&key % ROWS;
    while (!atomic_load_explicit(&mixed->stop, memory_order_relaxed)) {
        nya_arena_free_all(scratch);
        key                  = (key + KEY_STRIDE) % ROWS;
        NYA_SqlValue  bound  = nya_sql_s64((s64)key);
        NYA_SqlResult result = { 0 };
        NYA_Error     read   = nya_sql_query(db, scratch, "SELECT id, score, body FROM runs WHERE id = ?", &bound, 1, &result);
        atomic_fetch_add_explicit(read.ok ? &mixed->reads : &mixed->failures, 1, memory_order_relaxed);
    }
}

static void mixed_writer(void* data) {
    Mixed*        mixed = ((MixedThread*)data)->mixed;
    NYA_Database* db    = ((MixedThread*)data)->db;

    u64 key = 0;
    while (!atomic_load_explicit(&mixed->stop, memory_order_relaxed)) {
        key                = (key + KEY_STRIDE) % ROWS;
        NYA_SqlValue row[] = { nya_sql_s64((s64)key), nya_sql_s64((s64)(key % 500)), nya_sql_text(PAYLOAD) };
        NYA_Error    write = nya_sql_exec_bound(db, "INSERT OR REPLACE INTO runs (id, score, body) VALUES (?, ?, ?)", row, 3);
        atomic_fetch_add_explicit(write.ok ? &mixed->writes : &mixed->failures, 1, memory_order_relaxed);
    }
}

/** Runs one writer and READERS readers, sampling each kind's throughput per window into ns per operation. */
static void bench_mixed(NYA_Arena* arena) {
    Mixed mixed = { 0 };

    // opened here, one after another, and handed over: what is measured is the steady state, not nine opens racing.
    MixedThread per_thread[READERS + 1] = { 0 };
    for (u32 i = 0; i < READERS + 1; i++) per_thread[i] = (MixedThread){ .mixed = &mixed, .db = open_connection(arena) };

    NYA_Thread* threads[READERS + 1] = { 0 };
    NYA_EXPECT(nya_thread_spawn(arena, mixed_writer, &per_thread[0], "bench_writer", &threads[0]));
    for (u32 i = 0; i < READERS; i++) NYA_EXPECT(nya_thread_spawn(arena, mixed_reader, &per_thread[i + 1], "bench_reader", &threads[i + 1]));

    // the first window is warmup: every connection opening and the first checkpoint land in it.
    nya_os_time_sleep_ms(MIXED_WINDOW_MS);

    f64 read_samples[NYA_BENCH_ROUNDS]  = { 0 };
    f64 write_samples[NYA_BENCH_ROUNDS] = { 0 };
    u64 reads_total                     = 0;
    u64 writes_total                    = 0;

    for (u32 round = 0; round < NYA_BENCH_ROUNDS; round++) {
        u64 reads_before  = atomic_load(&mixed.reads);
        u64 writes_before = atomic_load(&mixed.writes);
        u64 started_ns    = nya_clock_get_monotonic_ns();

        nya_os_time_sleep_ms(MIXED_WINDOW_MS);

        u64 elapsed_ns = nya_clock_get_monotonic_ns() - started_ns;
        u64 reads      = atomic_load(&mixed.reads) - reads_before;
        u64 writes     = atomic_load(&mixed.writes) - writes_before;
        reads_total   += reads;
        writes_total  += writes;

        // a window with no operation of a kind reports the whole window, so a starved writer shows as slow rather than dividing by zero.
        read_samples[round]  = (f64)elapsed_ns / (f64)(reads > 0 ? reads : 1);
        write_samples[round] = (f64)elapsed_ns / (f64)(writes > 0 ? writes : 1);
    }

    atomic_store(&mixed.stop, true);
    for (u32 i = 0; i < READERS + 1; i++) nya_thread_join(threads[i]);
    for (u32 i = 0; i < READERS + 1; i++) nya_sql_close(per_thread[i].db);

    nya_bench_report("mixed: point reads, 8 readers in total", read_samples, NYA_BENCH_ROUNDS, 1, 1);
    nya_bench_report("mixed: writes beside them", write_samples, NYA_BENCH_ROUNDS, 1, 1);
    printf(
        "    mixed totals over %u ms: " FMTu64 " reads, " FMTu64 " writes, " FMTu64 " failed\n", NYA_BENCH_ROUNDS * MIXED_WINDOW_MS, reads_total,
        writes_total, atomic_load(&mixed.failures)
    );
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "bench_db_file");
    defer      nya_arena_destroy(arena);

    NYA_Arena* scratch = nya_arena_create(.name = "bench_db_file_scratch");
    defer      nya_arena_destroy(scratch);

    remove_files();
    defer remove_files();

    u64           opened_ns = nya_clock_get_monotonic_ns();
    NYA_Database* db        = open_connection(arena);
    printf("open: " FMTu64 " us\n", (nya_clock_get_monotonic_ns() - opened_ns) / 1000);

    NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE runs (id INTEGER PRIMARY KEY, score INTEGER NOT NULL, body TEXT NOT NULL)"));

    NYA_EXPECT(nya_sql_transaction_begin(db));
    for (u32 i = 0; i < ROWS; i++) {
        NYA_SqlValue seed[] = { nya_sql_s64((s64)i), nya_sql_s64((s64)(i % 500)), nya_sql_text(PAYLOAD) };
        NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO runs (id, score, body) VALUES (?, ?, ?)", seed, 3));
    }
    NYA_EXPECT(nya_sql_transaction_commit(db));

    nya_bench_begin("db over a file (per statement)");

    u64 cycle = 0;

    nya_bench("insert, one transaction each", 1, {
        u64          id    = cycle++ % ROWS;
        NYA_SqlValue row[] = { nya_sql_s64((s64)id), nya_sql_s64((s64)(id % 500)), nya_sql_text(PAYLOAD) };
        nya_bench_keep(nya_sql_exec_bound(db, "INSERT OR REPLACE INTO runs (id, score, body) VALUES (?, ?, ?)", row, 3).ok);
    });

    nya_bench("insert, 1000 to a transaction", BATCH, {
        NYA_EXPECT(nya_sql_transaction_begin(db));
        for (u32 i = 0; i < BATCH; i++) {
            u64          id    = cycle++ % ROWS;
            NYA_SqlValue row[] = { nya_sql_s64((s64)id), nya_sql_s64((s64)(id % 500)), nya_sql_text(PAYLOAD) };
            nya_bench_keep(nya_sql_exec_bound(db, "INSERT OR REPLACE INTO runs (id, score, body) VALUES (?, ?, ?)", row, 3).ok);
        }
        NYA_EXPECT(nya_sql_transaction_commit(db));
    });

    nya_bench("point read by key", 1, {
        nya_arena_free_all(scratch);
        cycle                = (cycle + KEY_STRIDE) % ROWS;
        NYA_SqlValue  key    = nya_sql_s64((s64)cycle);
        NYA_SqlResult result = { 0 };
        NYA_Error     read   = nya_sql_query(db, scratch, "SELECT id, score, body FROM runs WHERE id = ?", &key, 1, &result);
        nya_bench_keep(read.ok ? result.rows->length : 0);
    });

    // closed before the threads open theirs, so the mixed case is the writer and readers alone.
    nya_sql_close(db);

    // what a job that opens its own connection pays per run, as blob_service's scrub does.
    nya_bench("open and close an existing file", 1, {
        nya_arena_free_all(scratch);
        NYA_Database* reopened = nullptr;
        NYA_EXPECT(nya_sql_open(scratch, DB_PATH, &reopened));
        nya_sql_close(reopened);
    });

    bench_mixed(arena);

    return nya_bench_end();
}
