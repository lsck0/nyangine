#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _nya_asset_walk(NYA_ConstCString directory) __attr_no_discard;
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _nya_asset_enumerate(void) __attr_no_discard;
NYA_INTERNAL b8                     _nya_asset_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32                    _nya_asset_path_compare(const NYA_String* a, const NYA_String* b);
NYA_INTERNAL NYA_BuildRulePolicy    _nya_asset_shader_policy(u64 newest_include, NYA_ConstCString target) __attr_no_discard;

/** Memo behind _nya_asset_enumerate. See the note there for why it is safe to share. */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _NYA_ASSET_FILES = nullptr;

/**
 * How the byte blob in assets.c is laid out.
 *
 * These are what clang-format used to produce, kept because the generator emits the final shape
 * itself now; see the note at the end of nya_asset_bundle for why it no longer runs.
 * */
#define NYA_ASSET_BLOB_BYTES_PER_LINE 24
#define NYA_ASSET_BLOB_INDENT         4

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_asset_compile_shaders(void) {

    /*
     * When a shared `.hlsli` last changed, which no per shader rule can see.
     *
     * A rule compares its one input file against its one output file, and shadercross resolves the
     * `#include` itself, so editing mesh3d_shading.hlsli left every shader that includes it
     * "up to date" against a stale object. The include set is not tracked per shader — that would
     * mean parsing the sources — so any change here recompiles everything. There is one `.hlsli`
     * and twenty five shaders, and a full recompile is a couple of seconds.
     */
    NYA_ConstCString include_roots[] = { SHADER_SOURCE_DIRECTORY, nullptr };
    u64              newest_include  = nya_pp_newest(include_roots, ".hlsli");

    // Deliberately not the shared enumeration: this function writes into ./assets/shader/compiled/,
    // so anything it cached would be a snapshot of the tree from before its own outputs existed.
    NYA_ArrayᐸNYA_Stringᐳ* shaders = _nya_asset_walk(SHADER_SOURCE_DIRECTORY);

    nya_array_foreach (shaders, shader) {
        if (!nya_string_ends_with(shader, ".hlsl")) continue;

        NYA_CString source = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_prefix(shader, SHADER_SOURCE_DIRECTORY "/");
        nya_string_strip_suffix(shader, ".hlsl");
        nya_string_extend_front(shader, "./assets/shader/compiled/");

        nya_string_extend(shader, ".dxil");
        NYA_CString target_dxil = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_suffix(shader, ".dxil");

        nya_string_extend(shader, ".msl");
        NYA_CString target_metal = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_suffix(shader, ".msl");

        nya_string_extend(shader, ".spv");
        NYA_CString target_spirv = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_suffix(shader, ".spv");

        NYA_EXPECT(nya_filesystem_create_directory("./assets/shader/compiled/"));

        // compile to DXIL
        NYA_String* compile_to_dxil_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_dxil);
        NYA_BuildRule compile_to_dxil_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_dxil_name),
        .policy      = _nya_asset_shader_policy(newest_include, target_dxil),
        .input_file  = source,
        .output_file = target_dxil,
        .command = {
            .program = SHADERCROSS_BINARY,
            .environment = { SHADERCROSS_LIBRARY_PATH },
            .arguments = {
                source,
                "-o", target_dxil,
                "-s", "hlsl",
                "-d", "dxil",

                // So a shader can `#include` a shared `.hlsli`. shadercross compiles from a temporary
                // whose name is not the source path, so a relative include resolves against nothing
                // without this — the error is "file not found" on a file sitting right beside the source.
                "-I", SHADER_SOURCE_DIRECTORY,
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_dxil_rule));

        // compile to Metal
        NYA_String* compile_to_metal_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_metal);
        NYA_BuildRule compile_to_metal_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_metal_name),
        .policy      = _nya_asset_shader_policy(newest_include, target_metal),
        .input_file  = source,
        .output_file = target_metal,
        .command = {
            .program = SHADERCROSS_BINARY,
            .environment = { SHADERCROSS_LIBRARY_PATH },
            .arguments = {
                source,
                "-o", target_metal,
                "-s", "hlsl",
                "-d", "msl",

                // So a shader can `#include` a shared `.hlsli`. shadercross compiles from a temporary
                // whose name is not the source path, so a relative include resolves against nothing
                // without this — the error is "file not found" on a file sitting right beside the source.
                "-I", SHADER_SOURCE_DIRECTORY,
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_metal_rule));

        // compile to SPIR-V
        NYA_String* compile_to_spirv_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_spirv);
        NYA_BuildRule compile_to_spirv_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_spirv_name),
        .policy      = _nya_asset_shader_policy(newest_include, target_spirv),
        .input_file  = source,
        .output_file = target_spirv,
        .command = {
            .program = SHADERCROSS_BINARY,
            .environment = { SHADERCROSS_LIBRARY_PATH },
            .arguments = {
                source,
                "-o", target_spirv,
                "-s", "hlsl",
                "-d", "spirv",

                // So a shader can `#include` a shared `.hlsli`. shadercross compiles from a temporary
                // whose name is not the source path, so a relative include resolves against nothing
                // without this — the error is "file not found" on a file sitting right beside the source.
                "-I", SHADER_SOURCE_DIRECTORY,
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_spirv_rule));
    }
}

void nya_asset_index(void) {
    NYA_ConstCString inputs[]  = { "./assets", "./src/build/pp/asset.c", nullptr };
    NYA_ConstCString outputs[] = { NYA_ASSET_INDEX_OUTPUT, nullptr };
    if (nya_pp_is_current("index_assets", inputs, outputs)) return;

    NYA_ConstCString output_file = NYA_ASSET_INDEX_OUTPUT;

    NYA_Arena*  arena  = nya_arena_global;
    NYA_String* result = nya_string_create(arena);

    NYA_ArrayᐸNYA_Stringᐳ* files = _nya_asset_enumerate();
    nya_string_extend(result, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(result, "#pragma once\n\n");

    nya_array_foreach (files, file) {
        // ignore compiled shaders, since they are then picked by the asset system depending on the platform
        if (nya_string_contains(file, "/shader/compiled/")) continue;


        NYA_String* var_name = nya_string_clone(arena, file);

        // strip unnecessary infomations
        nya_string_replace(var_name, "shader/source", "shader");
        nya_string_strip_suffix(var_name, ".hlsl");

        // cleanup and convert to a valid C identifier
        nya_string_strip_prefix(var_name, "./assets/");
        nya_string_replace(var_name, "/", "_");
        nya_string_replace(var_name, ".", "_");
        nya_string_replace(var_name, "-", "_");
        nya_string_replace(var_name, " ", "_");
        nya_string_to_upper(var_name);

        nya_string_extend_sprintf(
            result,
            "#define NYA_ASSET_" NYA_FMT_STRING " \"" NYA_FMT_STRING "\"\n",
            NYA_FMT_STRING_ARG(var_name),
            NYA_FMT_STRING_ARG(file)
        );
    }

    NYA_EXPECT(nya_file_write(output_file, result));

    NYA_Command format_command = {
    .program   = "clang-format",
    .arguments = { "-i", output_file, },
  };
    NYA_EXPECT(nya_command_run(&format_command));
}

void nya_asset_bundle(void) {
    NYA_ConstCString inputs[]  = { "./assets", "./src/build/pp/asset.c", nullptr };
    NYA_ConstCString outputs[] = { NYA_ASSET_BUNDLE_OUTPUT, nullptr };
    if (nya_pp_is_current("bundle_assets", inputs, outputs)) return;

    NYA_ConstCString output_file = NYA_ASSET_BUNDLE_OUTPUT;

    NYA_Arena*  arena               = nya_arena_global;
    NYA_String* result              = nya_string_create(arena);
    NYA_String* header_count_string = nya_string_create(arena);
    NYA_String* header_string       = nya_string_create(arena);
    NYA_String* blob_string         = nya_string_create(arena);

    // The same list nya_asset_index built its handles from, not a second walk that could disagree
    // with it. A file appearing or vanishing between the two used to yield a handle in assets.h with
    // no matching entry in the blob, which only ever showed up as a failed load at runtime.
    NYA_ArrayᐸNYA_Stringᐳ* files = _nya_asset_enumerate();
    nya_string_extend(result, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(result, "#include \"nyangine/nyangine.h\"\n\n");
    header_count_string = nya_string_sprintf(arena, "static const u64 NYA_ASSET_BLOB_HEADER_COUNT = " FMTu64 ";\n", files->length);
    nya_string_extend(header_string, "static const NYA_AssetBlobHeader NYA_ASSET_BLOB_HEADER[] = {\n");
    nya_string_extend(blob_string, "static const u8 NYA_ASSET_BLOB[] = {\n");

    NYA_ConstCString HEX = "0123456789ABCDEF";

    u64 cursor  = 0;
    u64 emitted = 0;
    nya_array_foreach (files, file) {
        NYA_String* content = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(file, content));

        nya_string_extend_sprintf(header_string, "  { \"%.*s\", " FMTu64 ", " FMTu64 " },\n", NYA_FMT_STRING_ARG(file), cursor, content->length);

        // A byte costs at most the indent plus "0xAB" plus a separator, so the room for a whole file
        // is known before writing any of it and the buffer grows once rather than per byte.
        nya_array_reserve(blob_string, blob_string->length + content->length * (NYA_ASSET_BLOB_INDENT + 6) + 1);

        nya_array_foreach (content, c) {
            u8* out = blob_string->items + blob_string->length;

            if (emitted % NYA_ASSET_BLOB_BYTES_PER_LINE == 0) {
                for (u64 i = 0; i < NYA_ASSET_BLOB_INDENT; i++) *out++ = ' ';
                blob_string->length += NYA_ASSET_BLOB_INDENT;
            }

            out[0] = '0';
            out[1] = 'x';
            out[2] = (u8)HEX[*c >> 4];
            out[3] = (u8)HEX[*c & 0x0F];
            out[4] = ',';
            out[5] = (emitted % NYA_ASSET_BLOB_BYTES_PER_LINE == NYA_ASSET_BLOB_BYTES_PER_LINE - 1) ? '\n' : ' ';

            blob_string->length += 6;
            emitted++;
        }

        cursor += content->length;
    }

    // A blob whose last line was full already ends in a newline. One that did not ends in the
    // separator space written after its final byte, which becomes that newline rather than being
    // left behind as trailing whitespace.
    if (emitted % NYA_ASSET_BLOB_BYTES_PER_LINE != 0) blob_string->items[blob_string->length - 1] = '\n';

    nya_string_extend(blob_string, "};\n\n");
    nya_string_extend(header_string, "};\n\n");

    nya_string_extend(result, header_count_string);
    nya_string_extend(result, header_string);
    nya_string_extend(result, blob_string);

    NYA_EXPECT(nya_file_write(output_file, result));

    /*
     * Deliberately not run through clang-format, which every other generated file here is.
     *
     * This one is seven megabytes of hex, and formatting it took nine seconds — by a wide margin the
     * single most expensive step in a build, and spent entirely on the wrapping the loop above now
     * emits directly. The header table loses clang-format's column alignment as a result, which
     * costs nothing: the file says it is generated and nobody reads it.
     * */
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Every asset file under ./assets/, walked once per build tool invocation and memoised.
 *
 * Both the index and the bundle describe the same set of files — one as handles, one as bytes — so
 * they must agree, and the cheapest way to guarantee that is to only ever ask the filesystem once.
 *
 * The memo is populated on first call, which the build graph arranges to be after
 * nya_asset_compile_shaders has written ./assets/shader/compiled/. That ordering is load bearing:
 * bundle_assets depends on index_assets which depends on build_shaders. Calling this before the
 * shaders are compiled would cache a list that is missing them.
 * */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _nya_asset_enumerate(void) {
    if (_NYA_ASSET_FILES != nullptr) return _NYA_ASSET_FILES;

    _NYA_ASSET_FILES = _nya_asset_walk("./assets");
    return _NYA_ASSET_FILES;
}

/**
 * Collects every regular file under `directory`, sorted.
 *
 * This used to shell out to `find`, which is a problem on Windows off msys2: CreateProcessA resolves
 * `find` against PATH and Windows ships its own unrelated find.exe, a text search tool that would
 * take these arguments and produce nonsense rather than fail. Walking the directory ourselves has no
 * such ambiguity, and drops a subprocess per invocation.
 *
 * Sorted because the resulting order is baked into generated source: the walk returns whatever order
 * the filesystem feels like, which would make assets.c differ between machines for no reason.
 * */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _nya_asset_walk(NYA_ConstCString directory) {
    nya_assert(directory != nullptr);

    NYA_ArrayᐸNYA_Stringᐳ* files = nya_array_create(nya_arena_global, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(nya_arena_global, directory, _nya_asset_collect, files));

    nya_array_sort(files, _nya_asset_path_compare);
    return files;
}

NYA_INTERNAL b8 _nya_asset_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* files = (NYA_ArrayᐸNYA_Stringᐳ*)user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    NYA_String* file = nya_string_from(nya_arena_global, path);

    // nya_path_join normalises away a leading "./", but these paths are baked into generated source
    // as asset IDs that the runtime then looks up verbatim. Put it back, so the IDs stay exactly
    // what they were when this walked the tree with `find ./assets/`.
    if (!nya_string_starts_with(file, "./")) nya_string_extend_front(file, "./");

    /*
     * assets.c and assets.h are the generated output of this very walk, and .keep only exists to
     * keep an empty directory in git. None of the three is an asset.
     *
     * The .h rule also covers assets/shader/uniforms.h, which is the C side of the shaders' constant
     * buffers — a header the engine includes at compile time rather than bytes anything loads at
     * runtime. It lives under assets/ because it belongs beside the shaders it mirrors, and it is
     * correctly invisible to both the index and the bundle: there is no loader that could do
     * anything with a C header, and shipping one inside the blob would be pure waste.
     */
    if (nya_string_ends_with(file, ".c")) return true;
    if (nya_string_ends_with(file, ".h")) return true;
    if (nya_string_ends_with(file, ".keep")) return true;

    nya_array_push_back(files, *file);
    return true;
}

NYA_INTERNAL s32 _nya_asset_path_compare(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);
    if (difference != 0) return difference < 0 ? -1 : 1;

    if (a->length == b->length) return 0;
    return a->length < b->length ? -1 : 1;
}

/**
 * NYA_BUILD_ALWAYS when a shared include is newer than `target`, NYA_BUILD_IF_OUTDATED otherwise.
 *
 * Not a missing-file check: IF_OUTDATED already rebuilds an output that is not there, and so does a
 * target whose timestamp cannot be read, which is the same case.
 * */
NYA_INTERNAL NYA_BuildRulePolicy _nya_asset_shader_policy(u64 newest_include, NYA_ConstCString target) {
    nya_assert(target != nullptr);

    u64 target_modified_at = 0;
    if (!nya_filesystem_last_modified(target, &target_modified_at).ok) return NYA_BUILD_IF_OUTDATED;

    return newest_include > target_modified_at ? NYA_BUILD_ALWAYS : NYA_BUILD_IF_OUTDATED;
}
