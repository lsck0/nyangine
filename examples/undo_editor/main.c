/**
 * @file examples/undo_editor/main.c
 *
 * A headless "document editor" that shows three base/core primitives working together, with no
 * window, no renderer and no game loop:
 *
 *   - core_undo.h    — undo/redo as a ring of reflection snapshots over one struct.
 *   - base_newtype.h — parsed newtypes: text that only becomes a value once it has been validated.
 *   - core_taskgroup — a scoped nursery that fans work onto the job pool and joins it, first error out.
 *
 * ```
 * ./build run example undo_editor
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHY THE THREE BELONG IN ONE EXAMPLE
 * ─────────────────────────────────────────────────────────
 *
 * An editor is where they meet. The thing being edited is a struct, so its type reflects (core_undo
 * snapshots and restores it with no per-field code). The fields a person types are untrusted text, so
 * a newtype is the gate where "au thor" is refused and "ada-lovelace" becomes an NYA_Username the rest
 * of the program may trust without re-checking. And an editor validates in bulk — a paste of many
 * candidate slugs, say — so a task group fans that independent work out and reports the first bad one.
 *
 * The example is a straight line through those: build a document, edit it a few times recording each
 * commit, walk the history backwards and forwards proving the exact prior states come back, show the
 * newtype gate rejecting and accepting, then fan a batch of validations across the job pool twice —
 * once all-good, once with a ringer — to watch the group's error propagation.
 *
 * ─────────────────────────────────────────────────────────
 * REFLECTION WITHOUT THE GENERATOR
 * ─────────────────────────────────────────────────────────
 *
 * The `@reflect` pass scans only src/nyangine and src/gnyame, never examples — an example is a caller
 * of the engine, not part of it (see src/build/misc.h). So the document's NYA_TypeReflection is
 * written by hand below, exactly as examples/accounts_api/notes/note_model.h writes its Model's. Inside
 * the engine this whole block would be one `// @reflect` struct; here it is a table, and core_undo
 * neither knows nor cares which produced it.
 * */
#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/* A NEWTYPE OF OUR OWN base_newtype.h ships NYA_Email, NYA_Username and NYA_UserId; this shows the other half of the file — defining one. A Slug is the URL-safe stub of a title: lowercase letters, digits and interior hyphens. The predicate is an ordinary function, so it reads, tests and documents the rule on its own; the macro turns it into a distinct type with a parse, a reader and an equality that the compiler keeps from ever being confused with a bare string or a sibling newtype. */

/** A slug is 1..63 bytes of lowercase alnum and interior hyphens: no leading, trailing or doubled '-'. */
#define DOC_SLUG_CAPACITY 64

NYA_INTERNAL b8 doc_slug_is_valid(const u8* bytes, u32 length) {
    if (bytes[0] == '-' || bytes[length - 1] == '-') return false; // no leading or trailing hyphen.
    for (u32 i = 0; i < length; i++) {
        const u8 c        = bytes[i];
        const b8 is_lower = c >= 'a' && c <= 'z';
        const b8 is_digit = c >= '0' && c <= '9';
        if (!is_lower && !is_digit && c != '-') return false;   // lowercase, digit or hyphen only.
        if (c == '-' && bytes[i + 1] == '-') return false;      // no doubled hyphen.
    }
    return true;
}

NYA_NEWTYPE_STRING(DocSlug, doc_slug, DOC_SLUG_CAPACITY, doc_slug_is_valid);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE DOCUMENT, AND ITS REFLECTION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The struct the editor edits. The validated newtype values (an author's username, the slug) are copied
 * in as their bytes once they parse, so the stored document is plain reflectable data — char arrays and
 * integers — while the gate that put them there stayed a newtype. That is the usual division: a newtype
 * guards the boundary; the record it protects is ordinary.
 */

#define DOC_TITLE_MAX 64

typedef struct EditorDocument {
    char title[DOC_TITLE_MAX];        // free-text headline, edited freely.
    char author[NYA_USERNAME_CAPACITY]; // the bytes of a validated NYA_Username.
    char slug[DOC_SLUG_CAPACITY];       // the bytes of a validated DocSlug.
    u64  author_id;                   // the value of a validated NYA_UserId.
    u32  revision;                    // bumped on every commit, so two states are never mistaken as one.
    s32  body_words;                  // a stand-in for edited content.
    b8   published;
} EditorDocument;

/* One reflection sub-table per C array field; a bare `char[N]` is an ARRAY of char written as text. */
#define DOC_CHAR_ARRAY(count)                                                                       \
    { .name = "char[]", .kind = NYA_REFLECT_ARRAY, .size = (count), .alignment = alignof(char),     \
      .element = nya_reflect_of(char), .element_count = (count) }

NYA_INTERNAL const NYA_TypeReflection DOC_TITLE_ARRAY  = DOC_CHAR_ARRAY(DOC_TITLE_MAX);
NYA_INTERNAL const NYA_TypeReflection DOC_AUTHOR_ARRAY = DOC_CHAR_ARRAY(NYA_USERNAME_CAPACITY);
NYA_INTERNAL const NYA_TypeReflection DOC_SLUG_ARRAY   = DOC_CHAR_ARRAY(DOC_SLUG_CAPACITY);

NYA_INTERNAL const NYA_ReflectField DOC_FIELDS[] = {
    { .name = "title",      .type = &DOC_TITLE_ARRAY,        .offset = nya_offsetof(EditorDocument, title) },
    { .name = "author",     .type = &DOC_AUTHOR_ARRAY,       .offset = nya_offsetof(EditorDocument, author) },
    { .name = "slug",       .type = &DOC_SLUG_ARRAY,         .offset = nya_offsetof(EditorDocument, slug) },
    { .name = "author_id",  .type = nya_reflect_of(u64),     .offset = nya_offsetof(EditorDocument, author_id) },
    { .name = "revision",   .type = nya_reflect_of(u32),     .offset = nya_offsetof(EditorDocument, revision) },
    { .name = "body_words", .type = nya_reflect_of(s32),     .offset = nya_offsetof(EditorDocument, body_words) },
    { .name = "published",  .type = nya_reflect_of(b8),      .offset = nya_offsetof(EditorDocument, published) },
};

/** The whole-struct reflection core_undo snapshots and restores through. */
NYA_INTERNAL const NYA_TypeReflection DOCUMENT_REFLECTION = {
    .name        = "EditorDocument",
    .kind        = NYA_REFLECT_STRUCT,
    .size        = sizeof(EditorDocument),
    .alignment   = alignof(EditorDocument),
    .fields      = DOC_FIELDS,
    .field_count = nya_carray_length(DOC_FIELDS),
};

/** A bounded copy of a C string into a fixed field, always NUL terminated. The editor's text setter. */
NYA_INTERNAL void field_set(char* field, u32 capacity, NYA_ConstCString value) {
    u32 i = 0;
    for (; value[i] != '\0' && i + 1 < capacity; i++) field[i] = value[i];
    field[i] = '\0';
}

/** Prints the document as one line, so each step's state is visible in the run's output. */
NYA_INTERNAL void document_print(NYA_ConstCString label, const EditorDocument* doc) {
    nya_log_info("%-18s rev %u | \"%s\" by %s <#%llu> | %d words | %s", label, doc->revision, doc->title,
                 doc->author, (unsigned long long)doc->author_id, doc->body_words,
                 doc->published ? "published" : "draft");
}

/**
 * Two documents are equal when every field matches. Field by field rather than a memcmp of the whole
 * struct, because reflection snapshots the *values* — a string is its bytes up to the NUL — and neither
 * the char-array tail past that NUL nor the struct's padding is part of the state that round trips. The
 * undo proof is about the state coming back, so it compares the state, not the bytes around it.
 */
NYA_INTERNAL b8 document_equals(const EditorDocument* a, const EditorDocument* b) {
    return nya_string_equals(a->title, b->title) && nya_string_equals(a->author, b->author) &&
           nya_string_equals(a->slug, b->slug) && a->author_id == b->author_id && a->revision == b->revision &&
           a->body_words == b->body_words && a->published == b->published;
}

/* PART 1 — EDITS RECORDED AS SNAPSHOTS, THEN UNDONE AND REDONE Each commit bumps the revision and records the whole document. Recording is the "the user pressed save on this edit" moment: the snapshot is taken there and the caller may keep mutating immediately. */

/** Bumps the revision, records the new state, and prints it. The one place an edit becomes a commit. */
NYA_INTERNAL void commit(NYA_History* history, EditorDocument* doc, NYA_ConstCString label) {
    doc->revision++;
    nya_history_record(history, doc);
    document_print(label, doc);
}

NYA_INTERNAL void part_undo_redo(void) {
    nya_log_info("── PART 1: undo/redo over reflection snapshots ──");

    NYA_Arena* arena = nya_arena_create(.name = "undo_editor");
    defer      nya_arena_destroy(arena);

    // A history over EditorDocument keeping up to 16 snapshots, all out of `arena`.
    NYA_History* history = nya_history_create(arena, &DOCUMENT_REFLECTION, 16);

    // The starting document, and its first snapshot: the state undo can always return to.
    EditorDocument doc = { .title = "Untitled", .author = "ada-lovelace", .slug = "untitled",
                           .author_id = 1815, .revision = 0, .body_words = 0, .published = false };
    commit(history, &doc, "created");

    // A run of edits, each its own commit. We keep a copy of the mid-history state to prove later that undo returns *exactly* it, byte for byte, not merely something plausible.
    field_set(doc.title, DOC_TITLE_MAX, "On Analytical Engines");
    doc.body_words = 120;
    commit(history, &doc, "titled + drafted");

    EditorDocument after_two_edits = doc; // the state we will walk back to and compare against.

    doc.body_words = 640;
    commit(history, &doc, "expanded body");

    field_set(doc.slug, DOC_SLUG_CAPACITY, "on-analytical-engines");
    doc.published = true;
    commit(history, &doc, "slugged + published");

    EditorDocument newest = doc; // and the state redo must restore us to.

    nya_log_info("history holds %u snapshots; can_undo=%s", nya_history_count(history),
                 nya_history_can_undo(history) ? "true" : "false");

    // Two undos land us back on `after_two_edits`. Each writes the earlier state over `doc`.
    b8 stepped = nya_history_undo(history, &doc);
    nya_assert(stepped, "the second-from-newest state must exist");
    document_print("undo", &doc);
    stepped = nya_history_undo(history, &doc);
    nya_assert(stepped, "the third-from-newest state must exist");
    document_print("undo", &doc);

    nya_assert(document_equals(&doc, &after_two_edits), "undo must restore the exact prior state");
    nya_log_info("undo restored the exact 'titled + drafted' state (rev %u).", doc.revision);

    // And two redos walk forward to the newest state again.
    stepped = nya_history_redo(history, &doc);
    nya_assert(stepped, "there is a state to redo to");
    document_print("redo", &doc);
    stepped = nya_history_redo(history, &doc);
    nya_assert(stepped, "there is a second state to redo to");
    document_print("redo", &doc);

    nya_assert(document_equals(&doc, &newest), "redo must restore the exact newest state");
    nya_assert(!nya_history_can_redo(history), "we are back at the newest state, nothing to redo");
    nya_log_info("redo returned to the exact 'slugged + published' state (rev %u).", doc.revision);

    // A commit here would truncate the redo tail — standard editor semantics — but we are at the tip, so there is nothing to drop. Prove the round trip held its bytes and move on.
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PART 2 — THE NEWTYPE GATE: BAD INPUT REFUSED, GOOD INPUT ADMITTED
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * `_from_string` is the parse: it returns an NYA_Error with the rule that was broken, or writes a value
 * the compiler will not let you confuse with a bare string. Nothing here repairs input; it is admitted
 * or it is refused.
 */

NYA_INTERNAL void part_newtype_gate(void) {
    nya_log_info("── PART 2: parsed newtypes reject bad input, admit good ──");

    // A username that is too short: the built-in NYA_Username refuses it, with a reason.
    NYA_Username user  = { 0 };
    NYA_Error    bad_u = nya_username_from_string("ab", &user);
    nya_assert(!bad_u.ok, "a two-character username is below the minimum and must be refused");
    nya_log_info("username 'ab' refused: %s", (NYA_ConstCString)bad_u.message);

    // A good one is admitted, and its bytes can be read back for storage in the document.
    NYA_Error good_u = nya_username_from_string("ada-lovelace", &user);
    nya_assert(good_u.ok, "'ada-lovelace' is a valid username");
    nya_log_info("username '%s' admitted.", nya_username_cstring(&user));

    // Our own DocSlug refuses spaces and uppercase; the parse names the failing type.
    DocSlug   slug  = { 0 };
    NYA_Error bad_s = doc_slug_from_string("On Analytical Engines", &slug);
    nya_assert(!bad_s.ok, "a slug with spaces and capitals is not a slug");
    nya_log_info("slug 'On Analytical Engines' refused: %s", (NYA_ConstCString)bad_s.message);

    NYA_Error good_s = doc_slug_from_string("on-analytical-engines", &slug);
    nya_assert(good_s.ok, "the normalized form is a valid slug");
    nya_log_info("slug '%s' admitted.", doc_slug_cstring(&slug));

    // A UserId is a u64 that is never zero; "0" and non-digits are refused, a real id parses.
    NYA_UserId id      = { 0 };
    NYA_Error  bad_id  = nya_user_id_from_string("0", &id);
    nya_assert(!bad_id.ok, "zero is the null id no row has");
    nya_log_info("user id '0' refused: %s", (NYA_ConstCString)bad_id.message);

    NYA_Error good_id = nya_user_id_from_string("1815", &id);
    nya_assert(good_id.ok, "1815 is a fine id");
    nya_log_info("user id %llu admitted.", (unsigned long long)nya_user_id_value(id));
}

/* PART 3 — A TASK-GROUP NURSERY VALIDATING A BATCH IN PARALLEL An editor validates in bulk: someone pastes a column of candidate slugs and each is independent work. A task group fans them onto the job pool and joins them in one call, carrying the first failure back. Each task's argument outlives the wait — it lives in the caller's array, and the wait blocks until every task is done — so there is no lifetime the group leaves dangling. */

/** What one validation task reads and writes. `error` is written only by the task that owns it. */
typedef struct SlugCheck {
    NYA_ConstCString input; // the candidate text.
    NYA_Error        error; // NYA_OK once validated, or why it was refused.
    DocSlug          slug;  // the parsed value, on success.
} SlugCheck;

/**
 * One task: parse a candidate slug. Returning the error both surfaces it through the group's wait and,
 * as the first failure, raises the group's cancellation flag for the siblings — which a longer task
 * would poll with nya_taskgroup_is_cancelled to bow out early.
 */
NYA_INTERNAL NYA_Error slug_check_task(void* arg, NYA_TaskGroup* group) {
    nya_unused(group);
    SlugCheck* check = (SlugCheck*)arg;
    check->error     = doc_slug_from_string(check->input, &check->slug);
    return check->error;
}

/** Fans `count` candidates onto the pool under one group and returns the group's first failure. */
NYA_INTERNAL NYA_Error validate_batch(NYA_Arena* arena, SlugCheck* checks, u32 count) {
    NYA_TaskGroup* group = nullptr;
    NYA_TRY(nya_taskgroup_begin(arena, count, &group));
    for (u32 i = 0; i < count; i++) NYA_TRY(nya_taskgroup_spawn(group, slug_check_task, &checks[i]));

    NYA_Error first_failure = NYA_OK;
    nya_taskgroup_end(group, &first_failure); // blocks until every task has finished.
    return first_failure;
}

NYA_INTERNAL void part_task_group(void) {
    nya_log_info("── PART 3: a task-group nursery validates a batch in parallel ──");

    NYA_Arena* arena = nya_arena_create(.name = "undo_editor_tasks");
    defer      nya_arena_destroy(arena);

    // Round one: every candidate is a valid slug, so the group joins with no error.
    SlugCheck good_batch[] = {
        { .input = "on-analytical-engines" }, { .input = "notes-1843" }, { .input = "bernoulli-numbers" },
        { .input = "the-difference-engine" }, { .input = "punch-cards" },
    };
    const u32 good_count = nya_carray_length(good_batch);

    NYA_Error clean = validate_batch(arena, good_batch, good_count);
    nya_assert(clean.ok, "every candidate in the good batch is a valid slug");
    nya_log_info("validated %u candidates in parallel, all accepted:", good_count);
    for (u32 i = 0; i < good_count; i++) nya_log_info("    ok  %s", doc_slug_cstring(&good_batch[i].slug));

    // Round two: one ringer with spaces. The group's wait hands back that first failure for the caller to NYA_TRY, and swallows the rest — one error is more use than a pile.
    SlugCheck mixed_batch[] = {
        { .input = "well-formed" }, { .input = "also-fine" },
        { .input = "Not A Slug" }, // the ringer: spaces and capitals.
        { .input = "still-ok" },
    };
    const u32 mixed_count = nya_carray_length(mixed_batch);

    NYA_Error failed = validate_batch(arena, mixed_batch, mixed_count);
    nya_assert(!failed.ok, "the batch with a malformed slug must report a failure");
    nya_log_info("batch with a bad candidate reported the first failure: %s", (NYA_ConstCString)failed.message);
    nya_log_info("(the group joined every task regardless; a validation error is expected input, not a crash.)");
}

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc);
    nya_unused(argv);

    // The task group in part 3 rides on the engine's job pool, which core_app brings up. A headless app stands up the non-visual systems (the job pool among them) and no window or renderer, which is exactly what a simulation like this wants: nya_app_init does the backtrace wiring too.
    NYA_Error up = nya_app_init(.headless = true, .app_id = "undo_editor");
    if (!up.ok) {
        (void)fprintf(stderr, "Error: could not bring up the engine: %s\n", (NYA_ConstCString)up.message);
        return EXIT_FAILURE;
    }

    part_undo_redo();
    part_newtype_gate();
    part_task_group();

    nya_log_info("undo_editor: all three primitives demonstrated, clean.");

    nya_app_deinit();
    return EXIT_SUCCESS;
}
