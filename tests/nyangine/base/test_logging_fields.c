/**
 * Structured logging: a record is a message plus typed key/value fields, and a sink decides how it reads.
 *
 * The same event goes out two ways at once here. A plain line sink stands in for a terminal and holds the
 * human rendering the engine composes itself — the message followed by `key=value` pairs. A record sink
 * stands in for a server's log file and renders each record as one JSON object through
 * nya_log_record_render_json. The tests assert that both carry the fields, that the JSON parses and the
 * fields are there to read, that a plain nya_log_* line still works and still reaches a record sink, and
 * that overflowing the field cap is refused rather than undefined.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────
 * THE SINKS
 * ─────────────────────────────────────────────────────────
 */

/** The human rendering, as a terminal would hold it. */
static char HUMAN[16 * 1024] = { 0 };
static u64  HUMAN_LENGTH     = 0;

static void human_sink(NYA_LogLevel level, NYA_ConstCString message, u32 length, void* user_data) {
    nya_unused(level, user_data);

    if (HUMAN_LENGTH + length + 2 >= sizeof(HUMAN)) return;

    nya_memcpy(HUMAN + HUMAN_LENGTH, message, length);
    HUMAN_LENGTH        += length;
    HUMAN[HUMAN_LENGTH++] = '\n';
    HUMAN[HUMAN_LENGTH]   = '\0';
}

/** The JSON rendering of the most recent record, plus what the record itself said about its fields. */
static char LAST_JSON[8 * 1024] = { 0 };
static u32  LAST_JSON_LENGTH    = 0;
static u32  LAST_FIELD_COUNT    = 0;
static b8   LAST_OVERFLOWED     = false;
static u32  JSON_SINK_HITS      = 0;

static void json_sink(const NYA_LogRecord* record, void* user_data) {
    nya_unused(user_data);

    LAST_JSON_LENGTH = nya_log_record_render_json(record, LAST_JSON, sizeof(LAST_JSON));
    LAST_FIELD_COUNT = record->field_count;
    LAST_OVERFLOWED  = record->fields_overflowed;
    JSON_SINK_HITS++;
}

static void reset(void) {
    HUMAN_LENGTH     = 0;
    HUMAN[0]         = '\0';
    LAST_JSON_LENGTH = 0;
    LAST_JSON[0]     = '\0';
    LAST_FIELD_COUNT = 0;
    LAST_OVERFLOWED  = false;
    JSON_SINK_HITS   = 0;

    nya_log_ring_clear();
}

/** Parses LAST_JSON and reports whether it is well formed; on failure the raw line is in `out` for the message. */
static b8 last_json_parses(NYA_Arena* arena, OUT NYA_Object** out_object) {
    return nya_deserialize(arena, (const u8*)LAST_JSON, LAST_JSON_LENGTH, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, out_object).ok;
}

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_logging_fields");
    defer      nya_arena_destroy(arena);

    NYA_LogLevel original_level = nya_log_level_get();
    nya_log_level_set(NYA_LOG_LEVEL_TRACE);

    nya_log_sink_add(human_sink, nullptr);
    nya_log_record_sink_add(json_sink, nullptr);

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a record with typed fields renders through a human sink and a JSON sink
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: typed fields render both ways\n");
    {
        reset();
        nya_log_info_fields(
            "user login",
            nya_log_str("user", "alice"),
            nya_log_int("attempts", 3),
            nya_log_float("elapsed_ms", 12.5),
            nya_log_bool("ok", true)
        );

        // the human line: the message, then key=value pairs after it.
        nya_check(strstr(HUMAN, "user login") != nullptr, "human line missing the message: %s", HUMAN);
        nya_check(strstr(HUMAN, "user=alice") != nullptr, "human line missing a string field: %s", HUMAN);
        nya_check(strstr(HUMAN, "attempts=3") != nullptr, "human line missing an int field: %s", HUMAN);
        nya_check(strstr(HUMAN, "elapsed_ms=12.5") != nullptr, "human line missing a float field: %s", HUMAN);
        nya_check(strstr(HUMAN, "ok=true") != nullptr, "human line missing a bool field: %s", HUMAN);

        // the JSON line: one object per record, and it parses.
        nya_check(JSON_SINK_HITS == 1, "the record sink saw the record once, saw %u", JSON_SINK_HITS);

        NYA_Object* object = nullptr;
        nya_check(last_json_parses(arena, &object), "the JSON line does not parse: %s", LAST_JSON);

        if (object != nullptr) {
            NYA_Value* message = nya_object_get(object, "message");
            nya_check(message != nullptr && message->type == NYA_TYPE_STRING && strcmp(message->as_string, "user login") == 0,
                      "message field missing or wrong in %s", LAST_JSON);

            NYA_Value* user = nya_object_get(object, "user");
            nya_check(user != nullptr && user->type == NYA_TYPE_STRING && strcmp(user->as_string, "alice") == 0,
                      "user field missing or wrong in %s", LAST_JSON);

            NYA_Value* level = nya_object_get(object, "level");
            nya_check(level != nullptr && level->type == NYA_TYPE_STRING && strcmp(level->as_string, "INFO") == 0,
                      "level field missing or wrong in %s", LAST_JSON);

            // the numeric and boolean fields are there and carry the value, whatever number type the parser
            // chose for them.
            nya_check(nya_object_get(object, "attempts") != nullptr, "attempts field missing from %s", LAST_JSON);
            nya_check(nya_object_get(object, "elapsed_ms") != nullptr, "elapsed_ms field missing from %s", LAST_JSON);
            nya_check(nya_object_get(object, "ok") != nullptr, "ok field missing from %s", LAST_JSON);
        }

        nya_check(strstr(LAST_JSON, "\"attempts\":3") != nullptr, "int field wrong in %s", LAST_JSON);
        nya_check(strstr(LAST_JSON, "\"elapsed_ms\":12.5") != nullptr, "float field wrong in %s", LAST_JSON);
        nya_check(strstr(LAST_JSON, "\"ok\":true") != nullptr, "bool field wrong in %s", LAST_JSON);

        nya_check(!LAST_OVERFLOWED, "four fields are under the cap");
        nya_check(LAST_FIELD_COUNT == 4, "the record carried four fields, carried %u", LAST_FIELD_COUNT);

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a plain nya_log_* line still works, and still reaches a record sink
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: plain messages are unchanged and additive\n");
    {
        reset();
        nya_log_info("plain message %d", 7);

        nya_check(strstr(HUMAN, "plain message 7") != nullptr, "a plain line is unchanged: %s", HUMAN);
        nya_check(JSON_SINK_HITS == 1, "a plain line reaches the record sink too, hits %u", JSON_SINK_HITS);
        nya_check(LAST_FIELD_COUNT == 0, "a plain line carries no fields, carried %u", LAST_FIELD_COUNT);

        NYA_Object* object = nullptr;
        nya_check(last_json_parses(arena, &object), "a plain line still renders parseable JSON: %s", LAST_JSON);

        if (object != nullptr) {
            NYA_Value* message = nya_object_get(object, "message");
            nya_check(message != nullptr && message->type == NYA_TYPE_STRING && strcmp(message->as_string, "plain message 7") == 0,
                      "the message is the whole formatted string in %s", LAST_JSON);
        }

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a tag rides along on the record
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: a tag reaches both renderings\n");
    {
        reset();
        nya_log_tag_set("req=abc123");
        nya_log_info_fields("tagged event", nya_log_int("n", 1));
        nya_log_tag_clear();

        nya_check(strstr(HUMAN, "[req=abc123]") != nullptr, "the human line carries the tag: %s", HUMAN);
        nya_check(strstr(LAST_JSON, "\"tag\":\"req=abc123\"") != nullptr, "the JSON carries the tag: %s", LAST_JSON);

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: exactly NYA_LOG_FIELD_MAX fields are carried whole
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: the field cap is a hard edge, honoured exactly\n");
    {
        NYA_LogField many[NYA_LOG_FIELD_MAX + 1];
        for (u32 i = 0; i < nya_carray_length(many); i++) many[i] = nya_log_int("n", (s64)i);

        reset();
        _nya_log_fields(NYA_LOG_LEVEL_INFO, "main", "test_logging_fields.c", 1, "exactly the cap", many, NYA_LOG_FIELD_MAX);
        nya_check(!LAST_OVERFLOWED, "the cap itself is not an overflow");
        nya_check(LAST_FIELD_COUNT == NYA_LOG_FIELD_MAX, "all %u fields are carried, carried %u", NYA_LOG_FIELD_MAX, LAST_FIELD_COUNT);

        // ─────────────────────────────────────────────────────────────────────────────
        // TEST: one field past the cap is refused whole, not truncated, and never UB
        // ─────────────────────────────────────────────────────────────────────────────
        reset();
        _nya_log_fields(NYA_LOG_LEVEL_INFO, "main", "test_logging_fields.c", 1, "one too many", many, nya_carray_length(many));
        nya_check(LAST_OVERFLOWED, "one past the cap is an overflow");
        nya_check(LAST_FIELD_COUNT == 0, "an overflow carries no fields at all, carried %u", LAST_FIELD_COUNT);
        nya_check(strstr(HUMAN, "fields dropped") != nullptr, "the human line says the fields were dropped: %s", HUMAN);

        NYA_Object* object = nullptr;
        nya_check(last_json_parses(arena, &object), "an overflow still renders parseable JSON: %s", LAST_JSON);
        if (object != nullptr) nya_check(nya_object_get(object, "n") == nullptr, "no field survived the overflow in %s", LAST_JSON);

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: nya_log_record_render_json is bounded and always closes the object
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: JSON rendering stays within its buffer\n");
    {
        NYA_LogField  fields = nya_log_str("key", "value");
        NYA_LogRecord record = {
            .level       = NYA_LOG_LEVEL_WARN,
            .function    = "render",
            .file        = "render.c",
            .line        = 42,
            .tag         = "",
            .message     = "bounded",
            .fields      = &fields,
            .field_count = 1,
        };

        // a full buffer: everything is there and it parses.
        char full[512];
        u32  full_length = nya_log_record_render_json(&record, full, sizeof(full));
        nya_check(full_length > 0 && full[full_length - 1] == '}', "a full render closes the object: %s", full);
        nya_check(strstr(full, "\"line\":42") != nullptr, "the line number is a JSON number: %s", full);
        nya_check(strstr(full, "\"key\":\"value\"") != nullptr, "the field is there: %s", full);

        NYA_Object* object = nullptr;
        nya_check(nya_deserialize(arena, (const u8*)full, full_length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &object).ok, "the full render parses: %s", full);
        if (object != nullptr) {
            NYA_Value* value = nya_object_get(object, "key");
            nya_check(value != nullptr && value->type == NYA_TYPE_STRING && strcmp(value->as_string, "value") == 0, "the field survives a round trip: %s", full);
        }

        // a buffer with room for the members but not all of them: still closed, still valid JSON.
        char tight[24];
        u32  tight_length = nya_log_record_render_json(&record, tight, sizeof(tight));
        nya_check(tight_length > 0 && tight[tight_length - 1] == '}', "a tight render still closes the object: '%s'", tight);
        nya_check(tight[tight_length] == '\0', "and is still terminated");

        NYA_Object* tight_object = nullptr;
        nya_check(nya_deserialize(arena, (const u8*)tight, tight_length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &tight_object).ok,
                  "even a cut render is valid JSON: '%s'", tight);

        // the smallest useful buffer: an empty object.
        char minimal[3];
        u32  minimal_length = nya_log_record_render_json(&record, minimal, sizeof(minimal));
        nya_check(minimal_length == 2 && strcmp(minimal, "{}") == 0, "three bytes hold exactly an empty object, got '%s'", minimal);

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: record sinks are removed by their pair
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: record sink registration is bookkept like the line sinks\n");
    {
        reset();
        nya_check(nya_log_record_sink_remove(json_sink, nullptr), "removing a registered record sink reports true");

        nya_log_info_fields("after removal", nya_log_int("n", 1));
        nya_check(JSON_SINK_HITS == 0, "a removed record sink sees nothing, saw %u", JSON_SINK_HITS);
        nya_check(strstr(HUMAN, "after removal") != nullptr, "the human sink survives the record sink's removal: %s", HUMAN);

        nya_check(!nya_log_record_sink_remove(json_sink, nullptr), "removing a record sink twice reports false");
        nya_check(!nya_log_record_sink_remove(json_sink, (void*)1), "a different user_data is a different record sink");

        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // CLEANUP
    // ─────────────────────────────────────────────────────────────────────────────
    nya_log_sink_clear();
    nya_log_level_set(original_level);

    return nya_check_failures() == 0 ? 0 : 1;
}
