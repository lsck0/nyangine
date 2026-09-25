/**
 * The renderer's feature switches: what a zeroed struct means, how the three states resolve, and that the struct
 * and the enum are still in step.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Far larger than the stack wants, and the switches live on it. */
static NYA_Window window;

s32 main(void) {
    /* A zeroed struct overrides nothing, which is what lets a config file name only what it changes. */
    {
        nya_render_features_set(&window, (NYA_RenderFeatures){ 0 });

        for (u32 feature = 0; feature < NYA_RENDER_FEATURE_COUNT; feature++) {
            nya_check(nya_render_feature_enabled(&window, (NYA_RenderFeature)feature), "'%s' should be on by default",
                      nya_render_feature_name((NYA_RenderFeature)feature));

            // and a pass that did not ask to run still does not.
            nya_check(!nya_render_feature_on(&window, (NYA_RenderFeature)feature, false), "'%s' should not run unasked",
                      nya_render_feature_name((NYA_RenderFeature)feature));

            nya_check(nya_render_feature_on(&window, (NYA_RenderFeature)feature, true), "'%s' should run when asked",
                      nya_render_feature_name((NYA_RenderFeature)feature));
        }
    }

    /* Off wins over what a feature's own options asked for, and on overrides them. */
    {
        nya_render_features_set(&window, (NYA_RenderFeatures){ .bloom = NYA_RENDER_TOGGLE_OFF, .ink = NYA_RENDER_TOGGLE_ON });

        nya_check(!nya_render_feature_enabled(&window, NYA_RENDER_FEATURE_BLOOM), "bloom is off");
        nya_check(!nya_render_feature_on(&window, NYA_RENDER_FEATURE_BLOOM, true), "and stays off however loudly it is asked for");

        nya_check(nya_render_feature_enabled(&window, NYA_RENDER_FEATURE_INK), "ink is on");
        nya_check(nya_render_feature_on(&window, NYA_RENDER_FEATURE_INK, false), "and runs even though its own options did not ask");

        // the neighbours are untouched: one field written must not move another switch.
        nya_check(nya_render_feature_enabled(&window, NYA_RENDER_FEATURE_LIGHT_SHAFTS), "light shafts are untouched");
        nya_check(nya_render_feature_on(&window, NYA_RENDER_FEATURE_AMBIENT_OCCLUSION, false) == false, "so is ambient occlusion");
    }

    /* The switches read back as they were set, and the disabled row names exactly the ones that are off. */
    {
        NYA_RenderFeatures features = {
            .frustum_culling = NYA_RENDER_TOGGLE_OFF,
            .shadows         = NYA_RENDER_TOGGLE_OFF,
            .grade           = NYA_RENDER_TOGGLE_OFF,
        };

        nya_render_features_set(&window, features);

        NYA_RenderFeatures read_back = nya_render_features(&window);

        nya_check(read_back.frustum_culling == NYA_RENDER_TOGGLE_OFF && read_back.shadows == NYA_RENDER_TOGGLE_OFF
                      && read_back.grade == NYA_RENDER_TOGGLE_OFF && read_back.bloom == NYA_RENDER_TOGGLE_DEFAULT,
                  "the switches should read back as they were set");

        char text[256];

        u32 off = nya_render_features_disabled_text(&window, text, sizeof(text));

        nya_check(off == 3, "three switches are off, got " FMTu32, off);
        nya_check(strstr(text, "frustum culling") != nullptr && strstr(text, "shadows") != nullptr && strstr(text, "grade") != nullptr,
                  "and all three should be named, got '%s'", text);
        nya_check(strstr(text, "bloom") == nullptr, "and nothing that is on, got '%s'", text);

        // a buffer too small truncates rather than overruns, and still counts what was off.
        char small[12];

        nya_check(nya_render_features_disabled_text(&window, small, sizeof(small)) == 3, "a short buffer still counts them");
        nya_check(strlen(small) < sizeof(small), "and writes a terminated string, got '%s'", small);
    }

    /* Nothing is off once they are put back, so a caller can restore the default with a zeroed struct. */
    {
        nya_render_features_set(&window, (NYA_RenderFeatures){ 0 });

        char text[64];

        nya_check(nya_render_features_disabled_text(&window, text, sizeof(text)) == 0, "nothing is off again");
        nya_check(text[0] == '\0', "and the row is empty, got '%s'", text);
    }

    /* Every feature has a name, and no two share one. The names are what an overlay row and a log line say. */
    {
        for (u32 feature = 0; feature < NYA_RENDER_FEATURE_COUNT; feature++) {
            NYA_ConstCString name = nya_render_feature_name((NYA_RenderFeature)feature);

            nya_check(name != nullptr && name[0] != '\0', "feature " FMTu32 " has no name", feature);

            for (u32 other = 0; other < feature; other++) {
                nya_check(!nya_string_equals(name, nya_render_feature_name((NYA_RenderFeature)other)), "'%s' is named twice", name);
            }
        }
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
