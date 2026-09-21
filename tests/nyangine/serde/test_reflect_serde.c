/**
 * The reflection-driven serde path: a described struct to a file and back, and the check that says
 * what a document got wrong before any of it is applied.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <stdlib.h>

/** Where the fixtures land. Deleted on the way in, so a previous run cannot pass this one. */
#define FIXTURE_DIRECTORY "./.cache/test_reflect_serde"

/** What nya_reflect_check said, so the test can assert on it rather than on log output. */
typedef struct {
    u32  count;
    char last_path[NYA_REFLECT_PATH_MAX];
    char last_found[128];
    char last_expected[256];
} Findings;

static void collect(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
    Findings* findings = user_data;

    findings->count++;
    (void)snprintf(findings->last_path, sizeof(findings->last_path), "%s", path);
    (void)snprintf(findings->last_found, sizeof(findings->last_found), "%s", found);
    (void)snprintf(findings->last_expected, sizeof(findings->last_expected), "%s", expected);
}

static b8 contains(NYA_ConstCString haystack, NYA_ConstCString needle) {
    return strstr(haystack, needle) != nullptr;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_reflect_serde");
    defer      nya_arena_destroy(arena);

    (void)nya_filesystem_delete_recursive(FIXTURE_DIRECTORY);
    NYA_EXPECT(nya_filesystem_create_directory(FIXTURE_DIRECTORY));

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a described struct through a file and back is the same struct
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_reflect_save_file and nya_reflect_load_file\n");
    {
        NYA_SettingsGraphics written = {
            .msaa_samples      = 8,
            .fxaa              = false,
            .ambient_occlusion = true,
            .bloom             = false,
            .depth_of_field    = true,
            .eye_adaptation    = false,
            .light_shafts      = true,
            .motion_blur       = true,
            .shadows           = NYA_GRAPHICS_QUALITY_HIGH,
            .fov               = 73.5F,
            .render_scale      = 0.75F,
        };

        NYA_EXPECT(nya_reflect_save_file(nya_reflect_of(NYA_SettingsGraphics), &written, FIXTURE_DIRECTORY "/graphics.nya",
                                         NYA_SERDE_PRETTY));

        // Deliberately not zeroed: a field the file does carry has to overwrite whatever was there,
        // and a round trip that only works from a blank slate is not a round trip.
        NYA_SettingsGraphics read = NYA_SETTINGS_GRAPHICS_DEFAULT;

        NYA_EXPECT(nya_reflect_load_file(nya_reflect_of(NYA_SettingsGraphics), &read, FIXTURE_DIRECTORY "/graphics.nya", NYA_SERDE_NONE));

        nya_check(nya_memcmp(&written, &read, sizeof(written)) == 0, "a described struct should survive a file unchanged");
        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the low-level pair underneath it says the same thing
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_reflect_to_object and nya_reflect_from_object\n");
    {
        NYA_SettingsGraphics written = NYA_SETTINGS_GRAPHICS_DEFAULT;
        written.shadows              = NYA_GRAPHICS_QUALITY_LOW;
        written.fov                  = 91.25F;

        NYA_Object* document = nya_reflect_to_object(arena, nya_reflect_of(NYA_SettingsGraphics), &written);
        nya_check(document != nullptr, "a struct should produce a document");

        // An enum goes out as its variant's name, so renumbering the enum cannot silently change what
        // every existing file means.
        NYA_Value* shadows = nya_object_get(document, "shadows");
        nya_check(shadows != nullptr && shadows->type == NYA_TYPE_STRING, "an enum should be written as a name");
        nya_check(shadows != nullptr && nya_string_equals(shadows->as_string, "NYA_GRAPHICS_QUALITY_LOW"), "and as the right one");

        NYA_SettingsGraphics read = { 0 };
        NYA_EXPECT(nya_reflect_from_object(nya_reflect_of(NYA_SettingsGraphics), &read, document));

        nya_check(nya_memcmp(&written, &read, sizeof(written)) == 0, "and the document should read back as the same struct");
        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a good document has nothing to report
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_reflect_check on a good document\n");
    {
        NYA_SettingsGraphics graphics = NYA_SETTINGS_GRAPHICS_DEFAULT;
        NYA_Object*          document = nya_reflect_to_object(arena, nya_reflect_of(NYA_SettingsGraphics), &graphics);

        Findings findings = { 0 };

        u32 problems = nya_reflect_check(nya_reflect_of(NYA_SettingsGraphics), document, collect, &findings);

        nya_check(problems == 0, "a document this build wrote should have nothing wrong with it, got " FMTu32, problems);
        nya_check(findings.count == 0, "and nothing should have been reported");
        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a key that names no field is named and does not stop the rest
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_reflect_check on an unknown key\n");
    {
        NYA_Object* document = nya_object_create(arena);
        nya_object_set(document, "fov", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = 100.0F });
        nya_object_set(document, "fov_but_spelled_wrong", (NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = 100.0F });

        Findings findings = { 0 };

        u32 problems = nya_reflect_check(nya_reflect_of(NYA_SettingsGraphics), document, collect, &findings);

        nya_check(problems == 1, "exactly the unknown key should be reported, got " FMTu32, problems);
        nya_check(nya_string_equals(findings.last_path, "fov_but_spelled_wrong"), "the report should name the key, got '%s'",
                  findings.last_path);
        nya_check(contains(findings.last_expected, "msaa_samples"), "and list the keys it should have been one of, got '%s'",
                  findings.last_expected);

        // And the good key still lands, which is the whole reason the check is separate from the write.
        NYA_SettingsGraphics graphics = { 0 };
        NYA_EXPECT(nya_reflect_from_object(nya_reflect_of(NYA_SettingsGraphics), &graphics, document));

        nya_check(graphics.fov > 99.0F && graphics.fov < 101.0F, "the key that was right should still have been read, got %f",
                  (f64)graphics.fov);
        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a value of the wrong kind says what was found and what was wanted
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_reflect_check on a wrong value\n");
    {
        NYA_Object* document = nya_object_create(arena);
        nya_object_set(document, "fov", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "quite wide" });

        Findings findings = { 0 };

        u32 problems = nya_reflect_check(nya_reflect_of(NYA_SettingsGraphics), document, collect, &findings);

        nya_check(problems == 1, "the bad value should be reported, got " FMTu32, problems);
        nya_check(nya_string_equals(findings.last_path, "fov"), "the report should name the key, got '%s'", findings.last_path);
        nya_check(contains(findings.last_found, "quite wide"), "and quote what it found, got '%s'", findings.last_found);
        nya_check(contains(findings.last_expected, "f32"), "and say what it wanted, got '%s'", findings.last_expected);
        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: an enum name nothing in this build has
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_reflect_check on an unknown enum variant\n");
    {
        NYA_Object* document = nya_object_create(arena);
        nya_object_set(document, "shadows", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "NYA_GRAPHICS_QUALITY_ULTRA" });

        Findings findings = { 0 };

        u32 problems = nya_reflect_check(nya_reflect_of(NYA_SettingsGraphics), document, collect, &findings);

        nya_check(problems == 1, "the unknown variant should be reported, got " FMTu32, problems);
        nya_check(contains(findings.last_expected, "NYA_GRAPHICS_QUALITY_HIGH"), "and the report should list what this build has, got '%s'",
                  findings.last_expected);

        // The write leaves the field alone rather than zeroing it, so an option a newer build wrote
        // reads as "keep what you had" and not as "turn it off".
        NYA_SettingsGraphics graphics = { .shadows = NYA_GRAPHICS_QUALITY_MEDIUM };
        NYA_EXPECT(nya_reflect_from_object(nya_reflect_of(NYA_SettingsGraphics), &graphics, document));

        nya_check(graphics.shadows == NYA_GRAPHICS_QUALITY_MEDIUM, "an unreadable enum should leave the field alone");
        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: text longer than the array that holds it
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: over-long text into a char array\n");
    {
        static char long_name[NYA_SCENE_NAME_MAX * 2];
        for (u64 i = 0; i < sizeof(long_name) - 1; i++) long_name[i] = 'z';
        long_name[sizeof(long_name) - 1] = '\0';

        NYA_Object* document = nya_object_create(arena);
        nya_object_set(document, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = long_name });

        Findings findings = { 0 };

        u32 problems = nya_reflect_check(nya_reflect_of(NYA_SceneEntity), document, collect, &findings);

        nya_check(problems == 1, "text that does not fit should be reported, got " FMTu32, problems);
        nya_check(contains(findings.last_expected, "at most"), "and say how much fits, got '%s'", findings.last_expected);

        // Reported, then truncated rather than refused: the array is the struct's own storage.
        NYA_SceneEntity record = { 0 };
        NYA_EXPECT(nya_reflect_from_object(nya_reflect_of(NYA_SceneEntity), &record, document));

        nya_check(strlen(record.name) == NYA_SCENE_NAME_MAX - 1, "and the write should cut it to fit, got " FMTu64,
                  (u64)strlen(record.name));
        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a set of flags travels as names, not as a number
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: bitflags round trip\n");
    {
        NYA_SceneEntity written = { .state = NYA_ENTITY_STATE_ACTIVE | NYA_ENTITY_STATE_STATIC };

        NYA_Object* document = nya_reflect_to_object(arena, nya_reflect_of(NYA_SceneEntity), &written);

        NYA_Value* state = nya_object_get(document, "state");
        nya_check(state != nullptr && state->type == NYA_TYPE_ARRAY, "a set of flags should be written as a list of names");
        nya_check(state != nullptr && state->as_array.length == 2, "one name per flag that is set");

        NYA_SceneEntity read = { 0 };
        NYA_EXPECT(nya_reflect_from_object(nya_reflect_of(NYA_SceneEntity), &read, document));

        nya_check(read.state == written.state, "and read back as the same flags");
        printf("  PASSED\n");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a file that is not there, and one that is not the format
    // ─────────────────────────────────────────────────────────────────────────────
    printf("TEST: nya_reflect_load_file on bad input\n");
    {
        NYA_SettingsGraphics graphics = NYA_SETTINGS_GRAPHICS_DEFAULT;

        NYA_Error missing = nya_reflect_load_file(nya_reflect_of(NYA_SettingsGraphics), &graphics, FIXTURE_DIRECTORY "/nothing.nya",
                                                  NYA_SERDE_NONE);
        nya_check(!missing.ok, "a file that is not there should be an error, not a crash");

        NYA_EXPECT(nya_file_write(FIXTURE_DIRECTORY "/garbage.nya", "this is not a document at all"));

        NYA_Error garbage = nya_reflect_load_file(nya_reflect_of(NYA_SettingsGraphics), &graphics, FIXTURE_DIRECTORY "/garbage.nya",
                                                  NYA_SERDE_NONE);
        nya_check(!garbage.ok, "a file that is not the format should be an error, not a crash");

        // Untouched: the file is read whole before the instance is reached.
        NYA_SettingsGraphics untouched = NYA_SETTINGS_GRAPHICS_DEFAULT;

        nya_check(nya_memcmp(&graphics, &untouched, sizeof(graphics)) == 0, "and a failed load should leave the struct alone");
        printf("  PASSED\n");
    }

    (void)nya_filesystem_delete_recursive(FIXTURE_DIRECTORY);

    printf(nya_check_failures() == 0 ? "PASSED: test_reflect_serde\n" : "FAILED: test_reflect_serde\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
