/**
 * The ORM as laws over a random op sequence: a table and a plain in-memory model of it are driven
 * through the same braid of inserts, updates, deletes and reads, and after every single operation the
 * two are made to agree. The model is a dictionary from key to row; if the database ever disagrees with
 * it — a row that should be gone still finds, an update that should have missed reports success, a
 * select that returns the wrong set — the law fails and prints the operation that broke them apart.
 *
 * The example cases — the exact drift a schema check reports, a text key refused when empty, the types
 * nya_orm_open will not take — live in test_db_orm.c. This is what a script of examples cannot reach: a
 * schema kept honest across a thousand operations nobody chose the order of, which is the one thing a
 * store has to promise. Every case is a fresh :memory: database, so nothing carries between them.
 **/

// A larger entropy budget than the default 1024 so a sequence of tens of operations — each drawing a
// whole row of fields — spends real bytes rather than the zeroes a draw past the end reads. Set before
// the engine is included, which is also what makes this file its own unity build.
#define NYA_PROPERTY_ENTROPY_MAX 4096

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Cases per law. Each case is a whole op sequence, so this is thousands of sequences of tens of ops. */
#define CASES 1500

/** Fixed, so the suite is the same run every time. "orm" and "props" in ASCII. */
#define SEED 0x6F726D70726F7073ULL

/** Operations per sequence, at most. Enough that a table fills, empties and refills inside one case. */
#define OPS_MAX 40

/** Rows the model holds at most. The op mix keeps a table well under this; a full model just skips an insert. */
#define ROWS_MAX 64

/** Characters a name holds, terminator included. Short, so a name is drawn in full rather than always truncated. */
#define ROW_NAME_MAX 24

/** The span explicit keys are drawn from, small enough that a re-used key — and so a rejected insert — turns up. */
#define KEY_SPAN 32

/* THE DESCRIBED TYPE */

typedef struct {
    s64  id;
    u32  score;
    f64  ratio;
    b8   flag;
    char name[ROW_NAME_MAX];
} Row;

static const NYA_TypeReflection ROW_NAME_ARRAY = {
    .name          = "char[]",
    .kind          = NYA_REFLECT_ARRAY,
    .size          = sizeof(((Row*)nullptr)->name),
    .alignment     = alignof(char),
    .element       = nya_reflect_of(char),
    .element_count = ROW_NAME_MAX,
};

static const NYA_ReflectField ROW_FIELDS[] = {
    { .name = "id", .type = nya_reflect_of(s64), .offset = nya_offsetof(Row, id), .is_key = true },
    { .name = "score", .type = nya_reflect_of(u32), .offset = nya_offsetof(Row, score) },
    { .name = "ratio", .type = nya_reflect_of(f64), .offset = nya_offsetof(Row, ratio) },
    { .name = "flag", .type = nya_reflect_of(b8), .offset = nya_offsetof(Row, flag) },
    { .name = "name", .type = &ROW_NAME_ARRAY, .offset = nya_offsetof(Row, name) },
};

static const NYA_TypeReflection ROW = {
    .name        = "Row",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(Row),
    .alignment   = alignof(Row),
    .fields      = ROW_FIELDS,
    .field_count = nya_carray_length(ROW_FIELDS),
};

/* THE MODEL */

/** The plain truth the database is checked against: the rows that ought to be in the table, by key. */
typedef struct {
    Row rows[ROWS_MAX];
    u32 count;
} Model;

static s32 model_index_of(const Model* model, s64 id) {
    for (u32 index = 0; index < model->count; index++) {
        if (model->rows[index].id == id) return (s32)index;
    }
    return -1;
}

/** Draws a row's non-key fields. The ratio is built to be finite so it compares equal after a round trip. */
static void draw_fields(NYA_Property* property, OUT Row* row) {
    row->score = (u32)nya_property_draw_u64(property);
    row->flag  = nya_property_draw_bool(property, 50);

    // A finite double, assembled from drawn integers rather than raw bits, so it is never a NaN that
    // would refuse to equal itself and turn a faithful round trip into a spurious failure.
    u64 whole    = nya_property_draw_below(property, 1000000);
    u64 fraction = nya_property_draw_below(property, 1000);
    row->ratio   = (f64)whole + (f64)fraction / 1000.0;

    u64 length = nya_property_draw_below(property, ROW_NAME_MAX);
    for (u64 index = 0; index < length; index++) row->name[index] = (char)('a' + nya_property_draw_below(property, 26));
    for (u64 index = length; index < ROW_NAME_MAX; index++) row->name[index] = '\0';
}

/** Whether two rows carry the same key and the same fields, which is what a faithful store round trips. */
static b8 rows_equal(const Row* a, const Row* b) {
    return a->id == b->id && a->score == b->score && a->ratio == b->ratio && a->flag == b->flag && memcmp(a->name, b->name, ROW_NAME_MAX) == 0;
}

/**
 * A key for an update, delete or find: half the time one that is live in the model, so the found path
 * is walked, and half the time one drawn from the whole span, which is a miss as often as not. An empty
 * model always draws from the span, since it has no live key to offer.
 * */
static s64 pick_key(NYA_Property* property, const Model* model) {
    if (model->count > 0 && nya_property_draw_bool(property, 55)) return model->rows[nya_property_draw_below(property, model->count)].id;

    return (s64)(1 + nya_property_draw_below(property, KEY_SPAN));
}

/* THE LAW */

/**
 * The table agrees with the model after every operation. Insert either lets the database assign the key
 * or supplies one that may already be taken; update, delete and find name a key that is there as often
 * as one that is not, so the not-found paths are walked too. After each op the whole table is read back
 * and matched against the model, row for row.
 * */
static b8 law_table_tracks_the_model(NYA_Property* property) {
    NYA_Arena* arena = nya_arena_create(.name = "property_orm");
    defer      nya_arena_destroy(arena);

    NYA_Database* db = nullptr;
    if (!nya_sql_open(arena, ":memory:", &db).ok) {
        nya_property_note(property, "the in-memory database would not open");
        return false;
    }
    defer nya_sql_close(db);

    NYA_OrmTable* table = nullptr;
    if (!nya_orm_open(arena, db, &ROW, "rows", &table).ok) {
        nya_property_note(property, "the table would not open");
        return false;
    }
    defer nya_orm_close(table);

    if (!nya_orm_schema_create(table).ok) {
        nya_property_note(property, "the schema would not create");
        return false;
    }

    Model model = { 0 };

    u32 ops = 1 + (u32)nya_property_draw_below(property, OPS_MAX);
    for (u32 op = 0; op < ops; op++) {
        u32 choice = (u32)nya_property_draw_below(property, 100);

        if (choice < 40 && model.count < ROWS_MAX) {
            // Insert. Half the time with a zero key the database assigns, half with an explicit key that
            // may collide with one already stored, which the table must refuse rather than overwrite.
            Row row = { 0 };
            draw_fields(property, &row);

            b8 assigned = nya_property_draw_bool(property, 50);
            row.id      = assigned ? 0 : (s64)(1 + nya_property_draw_below(property, KEY_SPAN));

            b8 taken = !assigned && model_index_of(&model, row.id) >= 0;

            NYA_Error inserted = nya_orm_insert(table, &row);

            if (taken) {
                // An explicit key already in the table is a duplicate the store has to reject.
                if (inserted.ok) {
                    nya_property_note(property, "inserting a row on the taken key %lld was accepted", (long long)row.id);
                    return false;
                }
            } else {
                if (!inserted.ok) {
                    nya_property_note(property, "inserting a fresh row on key %lld was refused", (long long)row.id);
                    return false;
                }

                // An assigned key must come back a real, unused rowid; either way the model now holds it.
                if (model_index_of(&model, row.id) >= 0) {
                    nya_property_note(property, "the database assigned key %lld, which was already live", (long long)row.id);
                    return false;
                }
                model.rows[model.count++] = row;
            }
        } else if (choice < 60) {
            // Update every non-key column of a key that is there as often as one that is not.
            Row row = { 0 };
            draw_fields(property, &row);
            row.id = pick_key(property, &model);

            s32       index   = model_index_of(&model, row.id);
            NYA_Error updated = nya_orm_update(table, &row);

            if (index >= 0) {
                if (!updated.ok) {
                    nya_property_note(property, "updating the live key %lld was refused", (long long)row.id);
                    return false;
                }
                model.rows[index] = row;
            } else if (updated.kind != NYA_ERROR_NOT_FOUND) {
                nya_property_note(property, "updating the absent key %lld did not report NOT_FOUND", (long long)row.id);
                return false;
            }
        } else if (choice < 78) {
            // Delete a key, present or not; an absent one is a clean miss, never a silent success.
            s64       id      = pick_key(property, &model);
            s32       index   = model_index_of(&model, id);
            NYA_Error deleted = nya_orm_delete(table, nya_sql_s64(id));

            if (index >= 0) {
                if (!deleted.ok) {
                    nya_property_note(property, "deleting the live key %lld was refused", (long long)id);
                    return false;
                }
                model.rows[index] = model.rows[model.count - 1];
                model.count--;
            } else if (deleted.kind != NYA_ERROR_NOT_FOUND) {
                nya_property_note(property, "deleting the absent key %lld did not report NOT_FOUND", (long long)id);
                return false;
            }
        } else {
            // Find one key and match the row, or confirm the miss.
            s64  id    = pick_key(property, &model);
            s32  index = model_index_of(&model, id);
            Row  found = { 0 };

            NYA_Error read = nya_orm_find(table, arena, nya_sql_s64(id), &found);

            if (index >= 0) {
                if (!read.ok || !rows_equal(&found, &model.rows[index])) {
                    nya_property_note(property, "find on the live key %lld did not return its row", (long long)id);
                    return false;
                }
            } else if (read.kind != NYA_ERROR_NOT_FOUND) {
                nya_property_note(property, "find on the absent key %lld did not report NOT_FOUND", (long long)id);
                return false;
            }
        }

        // After every operation the whole table is the model: the same count, and every model row present
        // and unchanged. select over no clause is every row, which is the set the model claims to be.
        void* rows  = nullptr;
        u32   count = 0;
        if (!nya_orm_select(table, arena, nullptr, nullptr, 0, &rows, &count).ok) {
            nya_property_note(property, "selecting every row failed after op %u", op);
            return false;
        }

        if (count != model.count) {
            nya_property_note(property, "the table holds %u rows, the model holds %u, after op %u", count, model.count, op);
            return false;
        }

        for (u32 index = 0; index < model.count; index++) {
            s64 id       = model.rows[index].id;
            s32 in_table = -1;
            for (u32 seen = 0; seen < count; seen++) {
                if (((Row*)nya_orm_at(table, rows, seen))->id == id) {
                    in_table = (s32)seen;
                    break;
                }
            }

            if (in_table < 0) {
                nya_property_note(property, "the model's key %lld is missing from the table after op %u", (long long)id, op);
                return false;
            }

            if (!rows_equal(nya_orm_at(table, rows, (u32)in_table), &model.rows[index])) {
                nya_property_note(property, "the row for key %lld in the table differs from the model after op %u", (long long)id, op);
                return false;
            }
        }
    }

    return true;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    u32 failures = 0;

    failures += nya_property_check("the table tracks the model across a random op sequence", CASES, SEED, law_table_tracks_the_model);

    return failures == 0 ? 0 : 1;
}
