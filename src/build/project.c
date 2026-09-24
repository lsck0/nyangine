/**
 * @file project.c
 *
 * `./build project <path>`: reads a `project.nya` manifest and resolves it to a build plan.
 *
 * This is the first slice of consuming nyangine as a vendored dependency. A program that pulls the
 * engine in as a submodule describes itself in one `project.nya` — its profile, the engine modules it
 * wants, its source and asset roots, and the binaries it ships — and the build derives from that the
 * compile flags, the vendor subset to build and link, and the unity sources to fold in. This slice
 * parses the manifest and prints the resolved plan; it validates every field and refuses an unknown
 * profile. It adds a command and touches nothing on the existing in-tree build path.
 *
 * The manifest is written in the engine's own `.nya` object format, parsed by `nya_deserialize`, so the
 * reader is a dozen `nya_object_get` calls rather than a hand-rolled parser. See project.nya at the repo
 * root for the worked example, and docs/layering-core-split.md / the components-and-profiles TODO item
 * for where this is going: profile and modules become the flag and vendor-subset selectors the build
 * uses, and the source/asset roots let the engine live under vendor/nyangine while a consumer's code
 * sits in its own tree.
 */

/** The named build profiles a manifest may ask for. Each maps to a set of compile flags and a vendor subset. */
typedef enum NYA_BuildProfile {
    NYA_BUILD_PROFILE_CLI,     // a host tool: no renderer, no networking.
    NYA_BUILD_PROFILE_TUI,     // a terminal program: no renderer, terminal surface.
    NYA_BUILD_PROFILE_SERVER,  // a headless server: http/net, no renderer or core (NYA_SERVER).
    NYA_BUILD_PROFILE_DESKTOP, // a windowed program: the full engine.
    NYA_BUILD_PROFILE_GAME,    // a game: the full engine, same as desktop today.
    NYA_BUILD_PROFILE_WEB,     // a wasm client: emscripten, NYA_WEB_PROFILE.
    NYA_BUILD_PROFILE_COUNT,
} NYA_BuildProfile;

NYA_INTERNAL const NYA_ConstCString _NYA_BUILD_PROFILE_NAME[NYA_BUILD_PROFILE_COUNT] = {
    [NYA_BUILD_PROFILE_CLI]     = "cli",
    [NYA_BUILD_PROFILE_TUI]     = "tui",
    [NYA_BUILD_PROFILE_SERVER]  = "server",
    [NYA_BUILD_PROFILE_DESKTOP] = "desktop",
    [NYA_BUILD_PROFILE_GAME]    = "game",
    [NYA_BUILD_PROFILE_WEB]     = "web",
};

/** The headline compile flags a profile selects, for the plan print. The full flag list lives in flags.h. */
NYA_INTERNAL const NYA_ConstCString _NYA_BUILD_PROFILE_FLAGS[NYA_BUILD_PROFILE_COUNT] = {
    [NYA_BUILD_PROFILE_CLI]     = "-DNYA_NO_SDL -DNYA_NO_NET",
    [NYA_BUILD_PROFILE_TUI]     = "-DNYA_NO_SDL -DNYA_TERMINAL",
    [NYA_BUILD_PROFILE_SERVER]  = "-DNYA_NO_SDL -DNYA_SERVER",
    [NYA_BUILD_PROFILE_DESKTOP] = "(full engine)",
    [NYA_BUILD_PROFILE_GAME]    = "(full engine)",
    [NYA_BUILD_PROFILE_WEB]     = "emcc -DNYA_WEB_PROFILE -DNYA_NO_SDL",
};

/** Which vendor subset a profile builds and links. The macros are in src/build/vendor/vendor.h. */
NYA_INTERNAL const NYA_ConstCString _NYA_BUILD_PROFILE_VENDORS[NYA_BUILD_PROFILE_COUNT] = {
    [NYA_BUILD_PROFILE_CLI]     = "none (base/os only)",
    [NYA_BUILD_PROFILE_TUI]     = "none (base/os only)",
    [NYA_BUILD_PROFILE_SERVER]  = "NYA_SERVER_VENDORS",
    [NYA_BUILD_PROFILE_DESKTOP] = "NYA_PROJECT_VENDORS",
    [NYA_BUILD_PROFILE_GAME]    = "NYA_PROJECT_VENDORS",
    [NYA_BUILD_PROFILE_WEB]     = "the wasm subset",
};

/** Caps kept small on purpose: a manifest lists a handful of each, and a fixed array needs no allocator here. */
#define NYA_BUILD_PROJECT_MAX_MODULES  16
#define NYA_BUILD_PROJECT_MAX_ROOTS    16
#define NYA_BUILD_PROJECT_MAX_BINARIES 16

/** One binary the manifest ships: a name, an entry source root, and its own profile. */
typedef struct NYA_BuildBinary {
    NYA_ConstCString name;
    NYA_ConstCString entry;
    NYA_BuildProfile profile;
} NYA_BuildBinary;

/** A resolved project.nya. */
typedef struct NYA_BuildProject {
    NYA_ConstCString name;
    NYA_BuildProfile profile;

    NYA_ConstCString modules[NYA_BUILD_PROJECT_MAX_MODULES];
    u32              module_count;
    NYA_ConstCString sources[NYA_BUILD_PROJECT_MAX_ROOTS];
    u32              source_count;
    NYA_ConstCString assets[NYA_BUILD_PROJECT_MAX_ROOTS];
    u32              asset_count;
    NYA_BuildBinary  binaries[NYA_BUILD_PROJECT_MAX_BINARIES];
    u32              binary_count;
} NYA_BuildProject;

/** The profile named by `text`, or NYA_BUILD_PROFILE_COUNT when it is not one this build knows. */
NYA_INTERNAL NYA_BuildProfile _nya_build_profile_from(NYA_ConstCString text) {
    if (text == nullptr) return NYA_BUILD_PROFILE_COUNT;
    for (u32 profile = 0; profile < NYA_BUILD_PROFILE_COUNT; profile++) {
        if (strcmp(text, _NYA_BUILD_PROFILE_NAME[profile]) == 0) return (NYA_BuildProfile)profile;
    }
    return NYA_BUILD_PROFILE_COUNT;
}

/** Copies the string values of a manifest array field into `out`, up to `capacity`. A missing field is empty, not an error. */
NYA_INTERNAL NYA_Error _nya_build_read_strings(const NYA_Object* object, NYA_CString key, OUT NYA_ConstCString* out, u32 capacity, OUT u32* out_count) {
    *out_count = 0;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr) return NYA_OK;
    if (value->type != NYA_TYPE_ARRAY) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: '%s' must be an array of strings", key);

    for (u64 index = 0; index < value->as_array.length; index++) {
        if (*out_count >= capacity) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: '%s' has more than %u entries", key, capacity);

        NYA_Value* element = nya_array_get(&value->as_array, index);
        if (element->type != NYA_TYPE_STRING) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: '%s' must hold strings", key);

        out[(*out_count)++] = element->as_string;
    }

    return NYA_OK;
}

/** Reads and validates `path` into `out_project`. Refuses a missing name, a missing or unknown profile, and a malformed binary. */
NYA_INTERNAL NYA_Error nya_build_project_read(NYA_Arena* arena, NYA_ConstCString path, OUT NYA_BuildProject* out_project) {
    *out_project = (NYA_BuildProject){ 0 };

    NYA_String* contents = nya_string_create(arena);
    NYA_TRY(nya_file_read(path, contents));

    NYA_Object* manifest = nullptr;
    // NO_CHECKSUM: a project.nya is hand-edited, like engine.nya, so its header checksum is not verified.
    NYA_TRY(nya_deserialize(arena, (const u8*)contents->items, contents->length, NYA_SERDE_FORMAT_NYA, NYA_SERDE_NO_CHECKSUM, &manifest));

    NYA_Value* name = nya_object_get(manifest, "name");
    if (name == nullptr || name->type != NYA_TYPE_STRING) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: a string 'name' is required");
    out_project->name = name->as_string;

    NYA_Value* profile = nya_object_get(manifest, "profile");
    if (profile == nullptr || profile->type != NYA_TYPE_STRING) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: a string 'profile' is required");
    out_project->profile = _nya_build_profile_from(profile->as_string);
    if (out_project->profile == NYA_BUILD_PROFILE_COUNT) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: '%s' is not a profile (cli, tui, server, desktop, game, web)", profile->as_string);
    }

    NYA_TRY(_nya_build_read_strings(manifest, "modules", out_project->modules, NYA_BUILD_PROJECT_MAX_MODULES, &out_project->module_count));
    NYA_TRY(_nya_build_read_strings(manifest, "sources", out_project->sources, NYA_BUILD_PROJECT_MAX_ROOTS, &out_project->source_count));
    NYA_TRY(_nya_build_read_strings(manifest, "assets", out_project->assets, NYA_BUILD_PROJECT_MAX_ROOTS, &out_project->asset_count));

    NYA_Value* binaries = nya_object_get(manifest, "binaries");
    if (binaries != nullptr) {
        if (binaries->type != NYA_TYPE_ARRAY) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: 'binaries' must be an array");

        for (u64 index = 0; index < binaries->as_array.length; index++) {
            if (out_project->binary_count >= NYA_BUILD_PROJECT_MAX_BINARIES) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: more than %u binaries", NYA_BUILD_PROJECT_MAX_BINARIES);

            NYA_Value* entry = nya_array_get(&binaries->as_array, index);
            if (entry->type != NYA_TYPE_OBJECT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: each 'binaries' entry must be an object");

            NYA_Value* binary_name    = nya_object_get(&entry->as_object, "name");
            NYA_Value* binary_entry   = nya_object_get(&entry->as_object, "entry");
            NYA_Value* binary_profile = nya_object_get(&entry->as_object, "profile");
            if (binary_name == nullptr || binary_name->type != NYA_TYPE_STRING) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: a binary needs a string 'name'");
            if (binary_entry == nullptr || binary_entry->type != NYA_TYPE_STRING) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: binary '%s' needs a string 'entry'", binary_name->as_string);

            // A binary may name its own profile; without one it takes the project's.
            NYA_BuildProfile resolved = out_project->profile;
            if (binary_profile != nullptr) {
                if (binary_profile->type != NYA_TYPE_STRING) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: binary '%s' profile must be a string", binary_name->as_string);
                resolved = _nya_build_profile_from(binary_profile->as_string);
                if (resolved == NYA_BUILD_PROFILE_COUNT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "project.nya: binary '%s' names an unknown profile '%s'", binary_name->as_string, binary_profile->as_string);
            }

            out_project->binaries[out_project->binary_count++] = (NYA_BuildBinary){
                .name    = binary_name->as_string,
                .entry   = binary_entry->as_string,
                .profile = resolved,
            };
        }
    }

    return NYA_OK;
}

void project_runner(NYA_ArgCommand* command) {
    NYA_Arena* arena = nya_arena_create(.name = "project_runner");
    defer nya_arena_destroy(arena);

    NYA_ArgParameter* manifest_arg = command->parameters[0];
    if (manifest_arg->values_count > 1) {
        nya_log_error("Resolve one manifest at a time; %u were named.", manifest_arg->values_count);
        return;
    }
    NYA_ConstCString path = manifest_arg->values_count == 0 ? "project.nya" : manifest_arg->values[0].as_string;

    NYA_BuildProject project = { 0 };
    NYA_EXPECT(nya_build_project_read(arena, path, &project), "while resolving the manifest");

    NYA_ConstCString profile = _NYA_BUILD_PROFILE_NAME[project.profile];
    nya_log_info("project '%s' — profile '%s'", project.name, profile);
    nya_log_info("  flags:   %s", _NYA_BUILD_PROFILE_FLAGS[project.profile]);
    nya_log_info("  vendors: %s", _NYA_BUILD_PROFILE_VENDORS[project.profile]);

    for (u32 index = 0; index < project.module_count; index++) nya_log_info("  module:  %s", project.modules[index]);
    for (u32 index = 0; index < project.source_count; index++) nya_log_info("  source:  %s", project.sources[index]);
    for (u32 index = 0; index < project.asset_count; index++) nya_log_info("  assets:  %s", project.assets[index]);

    for (u32 index = 0; index < project.binary_count; index++) {
        NYA_BuildBinary* binary = &project.binaries[index];
        nya_log_info("  binary:  %s (entry %s, profile %s)", binary->name, binary->entry, _NYA_BUILD_PROFILE_NAME[binary->profile]);
    }

    nya_log_info("manifest resolves. Building it out of tree is the next slice; see project.c.");
}
