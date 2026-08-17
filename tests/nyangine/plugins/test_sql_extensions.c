/**
 * The extensions every connection gets for free: sqlite-vec and the sqlean bundle.
 *
 * Registration is what is really under test here. Both are statically linked archives whose entry
 * points are handed to sqlite3_auto_extension inside nya_sql_open, and the failure mode when any
 * link of that chain breaks — an archive not built, a vendor listed after libsqlite3, an entry point
 * never registered — is identical and quiet: SQLite reports "no such function" and the caller cannot
 * tell it from a typo. So each block below calls something that only exists if the extension is
 * there, and asserts on the value rather than only on the call succeeding.
 *
 * Everything runs against ":memory:". None of these functions touches the filesystem.
 *
 * The three extensions deliberately left out of the sqlean bundle are asserted absent at the bottom,
 * so that switching one on is a decision someone makes here rather than something that happens by
 * accident.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Opens a fresh in memory database. Each test gets its own, so none can pollute another. */
static NYA_Database* open_memory(NYA_Arena* arena) {
  NYA_Database* db = nullptr;
  NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
  return db;
}

/** Runs a single row, single column query and hands back the value. */
static NYA_Value scalar(NYA_Database* db, NYA_Arena* arena, NYA_ConstCString sql, NYA_ConstCString column) {
  NYA_SqlResult result = { 0 };
  NYA_EXPECT(nya_sql_query(db, arena, sql, nullptr, 0, &result), sql);

  nya_assert(result.rows->length == 1, "'%s' was expected to return exactly one row, got " FMTu64, sql, result.rows->length);

  // Cast because nya_object_get takes a mutable key while every caller here passes a literal. It
  // only reads it; the signature is the thing that is wrong, not this call.
  NYA_Value* value = nya_object_get(result.rows->items[0], (NYA_CString)column);
  nya_assert(value != nullptr, "'%s' returned no column named '%s'", sql, column);

  return *value;
}

/** True when `sql` prepares. Used to tell a registered function from an absent one. */
static b8 prepares(NYA_Database* db, NYA_Arena* arena, NYA_ConstCString sql) {
  NYA_SqlResult result = { 0 };
  NYA_Error     error  = nya_sql_query(db, arena, sql, nullptr, 0, &result);

  return error.ok;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_sql_extensions");
  defer      nya_arena_destroy(arena);

  nya_info("SQLite %s, sqlite-vec %s", nya_sql_version(), nya_sql_vec_version());

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: sqlite-vec is linked in, and reports the version the submodule is at
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    // Two sources for one fact: the compiled in macro, and what the registered SQL function says.
    // They disagree if the archive was built from a different checkout than the header, which is
    // exactly what a stale build directory or a cache restored across a submodule bump produces.
    NYA_Value version = scalar(db, arena, "SELECT vec_version() AS version", "version");
    nya_assert(version.type == NYA_TYPE_STRING);
    nya_assert(strcmp(version.as_string, nya_sql_vec_version()) == 0, "the linked archive says %s, the header says %s", version.as_string,
               nya_sql_vec_version());
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: vec_* scalar functions compute what they claim to
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    // (1,2,3) to (4,5,6) is a distance of sqrt(27), and picking a case with a known answer is the
    // point: a function that is registered but computing nonsense passes a "did it run" check.
    NYA_Value l2 = scalar(db, arena, "SELECT vec_distance_l2(vec_f32('[1,2,3]'), vec_f32('[4,5,6]')) AS d", "d");
    nya_assert(l2.type == NYA_TYPE_F64);
    nya_assert(fabs(l2.as_f64 - 5.196152) < 0.0001, "expected sqrt(27), got %f", l2.as_f64);

    // A vector against itself is zero away from itself, whichever metric is used.
    NYA_Value cosine = scalar(db, arena, "SELECT vec_distance_cosine(vec_f32('[1,0,0]'), vec_f32('[1,0,0]')) AS d", "d");
    nya_assert(fabs(cosine.as_f64) < 0.0001, "expected 0, got %f", cosine.as_f64);

    // Three f32 is twelve bytes, and vec_length counts elements rather than bytes.
    NYA_Value length = scalar(db, arena, "SELECT vec_length(vec_f32('[1,2,3]')) AS n", "n");
    nya_assert(length.as_s64 == 3, "expected 3 elements, got " FMTs64, length.as_s64);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a vec0 table answers a k nearest neighbour query in the right order
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_EXPECT(nya_sql_exec(db, "CREATE VIRTUAL TABLE items USING vec0(embedding float[3])"));

    // Bound as a blob of little endian f32, which is how vec0 stores a vector. Deliberately not
    // through vec_f32('[...]') here: the blob path is the one a real caller uses, since an embedding
    // arrives as floats rather than as text.
    const f32 vectors[3][3] = {
      { 1.0f, 0.0f, 0.0f },
      { 0.0f, 1.0f, 0.0f },
      { 0.9f, 0.1f, 0.0f },
    };

    for (u32 i = 0; i < 3; i++) {
      NYA_SqlValue row[] = { nya_sql_s64((s64)i + 1), nya_sql_blob((const u8*)vectors[i], sizeof(vectors[i])) };
      NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO items (rowid, embedding) VALUES (?, ?)", row, 2));
    }

    // Querying with (1,0,0) exactly: rowid 1 is that vector, rowid 3 is close to it, rowid 2 is
    // orthogonal. Asserting the *order* is what makes this a search test rather than a scan.
    const f32 query[3] = { 1.0f, 0.0f, 0.0f };

    NYA_SqlValue  search[] = { nya_sql_blob((const u8*)query, sizeof(query)) };
    NYA_SqlResult nearest  = { 0 };
    NYA_EXPECT(nya_sql_query(db, arena, "SELECT rowid, distance FROM items WHERE embedding MATCH ? AND k = 2", search, 1, &nearest));

    nya_assert(nearest.rows->length == 2, "k = 2 asked for two rows, got " FMTu64, nearest.rows->length);
    nya_assert(nya_object_get(nearest.rows->items[0], "rowid")->as_s64 == 1, "the exact match should come first");
    nya_assert(nya_object_get(nearest.rows->items[1], "rowid")->as_s64 == 3, "the near match should come second, not the orthogonal one");

    // The exact match is at distance zero, and the row carries the distance as an ordinary column —
    // which is the whole reason a result is an NYA_Object rather than a special vector type.
    NYA_Value* distance = nya_object_get(nearest.rows->items[0], "distance");
    nya_assert(distance->type == NYA_TYPE_F64);
    nya_assert(fabs(distance->as_f64) < 0.0001, "expected 0, got %f", distance->as_f64);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: sqlean's statistics, which SQLite has none of
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    // generate_series is itself from sqlean's stats extension, so this asserts two things at once.
    NYA_Value median = scalar(db, arena, "SELECT median(value) AS m FROM generate_series(1, 9)", "m");
    nya_assert(fabs(median.as_f64 - 5.0) < 0.0001, "the median of 1..9 is 5, got %f", median.as_f64);

    NYA_Value percentile = scalar(db, arena, "SELECT percentile_90(value) AS p FROM generate_series(1, 10)", "p");
    nya_assert(percentile.as_f64 > 8.0, "the 90th percentile of 1..10 should be near 10, got %f", percentile.as_f64);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: sqlean's text, math, fuzzy, time, unicode and uuid extensions
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_Value reversed = scalar(db, arena, "SELECT text_reverse('nyangine') AS t", "t");
    nya_assert(strcmp(reversed.as_string, "enignayn") == 0, "got %s", reversed.as_string);

    NYA_Value split = scalar(db, arena, "SELECT text_split('a,b,c', ',', 2) AS t", "t");
    nya_assert(strcmp(split.as_string, "b") == 0, "text_split is 1 indexed, got %s", split.as_string);

    // SQLite has no trigonometry of its own; this is sqlean's math extension.
    NYA_Value sine = scalar(db, arena, "SELECT sin(0) AS s", "s");
    nya_assert(fabs(sine.as_f64) < 0.0001, "got %f", sine.as_f64);

    // One substitution turns "kitten" into "sitten", and so on to "sitting": three edits.
    NYA_Value distance = scalar(db, arena, "SELECT levenshtein('kitten', 'sitting') AS d", "d");
    nya_assert(distance.as_s64 == 3, "expected 3 edits, got " FMTs64, distance.as_s64);

    NYA_Value upper = scalar(db, arena, "SELECT upper('straße') AS u", "u");
    nya_assert(strcmp(upper.as_string, "straße") != 0, "sqlean's unicode upper should not leave non-ASCII alone, got %s", upper.as_string);

    // A v4 uuid is 36 characters with hyphens, and two calls must not agree.
    NYA_Value uuid = scalar(db, arena, "SELECT uuid4() AS u", "u");
    nya_assert(strlen(uuid.as_string) == 36, "expected a 36 character uuid, got '%s'", uuid.as_string);

    NYA_Value distinct = scalar(db, arena, "SELECT uuid4() <> uuid4() AS d", "d");
    nya_assert(distinct.as_s64 == 1, "two uuid4 calls returned the same value");

    // time_now returns a sqlean instant, and time_fmt_iso renders it. Only the shape is asserted:
    // the value is the wall clock, which a test cannot pin down.
    NYA_Value now = scalar(db, arena, "SELECT time_fmt_iso(time_now()) AS t", "t");
    nya_assert(strlen(now.as_string) > 10, "expected an ISO timestamp, got '%s'", now.as_string);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: sqlean's define, which has to be registered last to work at all
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    // The body of a defined function calls another sqlean function on purpose. That is the ordering
    // constraint sqlean_extensions.c documents: define_init has to run after everything else, and
    // when it does not this is what fails, with "no such function: text_reverse".
    // Two arguments: a name and an expression. The parameters are the `:name` placeholders in the
    // body, bound positionally when the function is called.
    NYA_EXPECT(nya_sql_exec(db, "SELECT define('backwards', 'text_reverse(:t)')"));

    NYA_Value result = scalar(db, arena, "SELECT backwards('abc') AS r", "r");
    nya_assert(strcmp(result.as_string, "cba") == 0, "got %s", result.as_string);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: every connection gets them, not only the first
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // sqlite3_auto_extension applies to connections opened *after* registration, so a bug that
    // registered once against the first handle rather than globally would pass every test above and
    // fail here. Two handles open at once, both used.
    NYA_Database* first = open_memory(arena);
    defer         nya_sql_close(first);

    NYA_Database* second = open_memory(arena);
    defer         nya_sql_close(second);

    nya_assert(scalar(first, arena, "SELECT vec_length(vec_f32('[1,2]')) AS n", "n").as_s64 == 2);
    nya_assert(scalar(second, arena, "SELECT vec_length(vec_f32('[1,2]')) AS n", "n").as_s64 == 2);
    nya_assert(strcmp(scalar(second, arena, "SELECT text_reverse('ab') AS t", "t").as_string, "ba") == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the extensions that are deliberately not bundled are not there
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    // writefile comes from sqlean's fileio. Left out because a query that can write anywhere the
    // process can is not something to hand to save data or a mod by default. If this ever starts
    // passing, someone added fileio — which is a decision, not an accident, and should come with a
    // change to this assertion and to sqlean_extensions.c.
    nya_assert(!prepares(db, arena, "SELECT writefile('/tmp/nyangine-should-not-exist', 'x')"), "sqlean's fileio is not meant to be registered");

    // regexp needs PCRE2 in an archive of its own; crypto needs a header upstream downloads at build
    // time. Both are absent for build reasons rather than policy ones.
    nya_assert(!prepares(db, arena, "SELECT regexp_like('abc', 'a.c')"), "sqlean's regexp is not meant to be registered");
    nya_assert(!prepares(db, arena, "SELECT sha256('abc')"), "sqlean's crypto is not meant to be registered");

    // The connection still works afterwards: a failed prepare is not a poisoned handle.
    nya_assert(scalar(db, arena, "SELECT 1 AS one", "one").as_s64 == 1);
  }

  printf("PASSED: test_sql_extensions\n");
  return 0;
}
