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
NYA_INTERNAL b8 _nya_asset_shader_outdated(NYA_BuildRulePolicy policy, NYA_ConstCString source, NYA_ConstCString target) __attr_no_discard;

/** Memo behind _nya_asset_enumerate. See the note there for why it is safe to share. */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _NYA_ASSET_FILES = nullptr;

/**
 * How the byte blob in assets.c is laid out.
 * */
#define NYA_ASSET_BLOB_BYTES_PER_LINE 24
#define NYA_ASSET_BLOB_INDENT         4

/**
 * Bytes an entry must save before it is stored compressed.
 *
 * A compressed entry costs an allocation and a decompression on every load; a verbatim one is a
 * pointer into `.rodata`.
 *
 * Absolute bytes, not a ratio. Over this tree's 1506 assets a ratio drops the wrong entries:
 * `pill.fbx` and `Cubie.fbx` shrink only 5% but save 19 KB and 18 KB, while the 101 entries in the
 * 10-20% band save 67 bytes each.
 *
 * 128 is the knee: 211 small entries stay verbatim at a cost of 17.5 KB of 715 KB. 256 would give up
 * 103 KB for another 465 entries.
 * */
#define NYA_ASSET_BLOB_MIN_COMPRESSION_SAVING_BYTES 128

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_asset_compile_shaders(void) {
    // when a shared `.hlsli` last changed, which no per shader rule can see.
    NYA_ConstCString include_roots[] = { SHADER_SOURCE_DIRECTORY, nullptr };
    u64              newest_include  = nya_pp_newest(include_roots, ".hlsli");

    // not the shared enumeration: this writes into ./assets/shader/compiled/, and a cached listing would
    // predate its own outputs.
    NYA_ArrayᐸNYA_Stringᐳ* shaders = _nya_asset_walk(SHADER_SOURCE_DIRECTORY);

    NYA_ConstCString formats[][2] = { { "dxil", ".dxil" }, { "msl", ".msl" }, { "spirv", ".spv" } };

    NYA_EXPECT(nya_filesystem_create_directory("./assets/shader/compiled/"));

    nya_array_foreach (shaders, shader) {
        if (!nya_string_ends_with(shader, ".hlsl")) continue;

        NYA_CString source = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_prefix(shader, SHADER_SOURCE_DIRECTORY "/");
        nya_string_strip_suffix(shader, ".hlsl");
        nya_string_extend_front(shader, "./assets/shader/compiled/");

        for (u32 i = 0; i < nya_carray_length(formats); i++) {
            NYA_CString target = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%.*s%s", (int)shader->length, shader->items, formats[i][1]));
            NYA_BuildRulePolicy policy = _nya_asset_shader_policy(newest_include, target);

            // a Windows host cannot build shadercross (DXC does not compile under MinGW), so it relies on
            // shaders compiled elsewhere and must say so when one is stale.
            if (!nya_filesystem_exists(SHADERCROSS_BINARY) && _nya_asset_shader_outdated(policy, source, target)) {
                nya_log_panic("%s needs compiling, but %s does not exist. Compile shaders on a Linux host and copy ./assets/shader/compiled/.",
                              source, SHADERCROSS_BINARY);
            }

            NYA_BuildRule rule = {
                .name        = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%s -> %s", source, target)),
                .policy      = policy,
                .input_file  = source,
                .output_file = target,
                .command = {
                    .program     = SHADERCROSS_BINARY,
                    .environment = { SHADERCROSS_LIBRARY_PATH },
                    .arguments = {
                        source,
                        "-o", target,
                        "-s", "hlsl",
                        "-d", formats[i][0],

                        // shadercross compiles from a temporary file, so relative `#include`s need the source root.
                        "-I", SHADER_SOURCE_DIRECTORY,
                    },
                },
            };
            NYA_EXPECT(nya_build(&rule));
        }
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
    NYA_String* header_string       = nya_string_create(arena);
    NYA_String* blob_string         = nya_string_create(arena);

    // the same list nya_asset_index built its handles from. A second walk could see a file appear or
    // vanish and emit a handle with no blob entry.
    NYA_ArrayᐸNYA_Stringᐳ* files = _nya_asset_enumerate();
    nya_string_extend(result, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(result, "#include \"nyangine/nyangine.h\"\n\n");
    NYA_String* header_count_string = nya_string_sprintf(arena, "static const u64 NYA_ASSET_BLOB_HEADER_COUNT = " FMTu64 ";\n", files->length);
    nya_string_extend(header_string, "static const NYA_AssetBlobHeader NYA_ASSET_BLOB_HEADER[] = {\n");
    nya_string_extend(blob_string, "static const u8 NYA_ASSET_BLOB[] = {\n");

    NYA_ConstCString HEX = "0123456789ABCDEF";

    u64 cursor           = 0;
    u64 emitted          = 0;
    u64 total_raw        = 0;
    u64 total_stored     = 0;
    u64 compressed_count = 0;

    nya_array_foreach (files, file) {
        NYA_String* content = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(file, content));

        /*
         * Compressed per entry, kept only when smaller.
         *
         * Per entry so a load expands one asset rather than the whole blob. Already compressed formats such
         * as PNG and OGG stay verbatim and keep the zero copy load path.
         */
        const u8* stored      = content->items;
        u64       stored_size = content->length;

        u64 bound = nya_compress_bound(content->length);

        if (bound > 0) {
            u8* compressed = nya_arena_alloc(arena, bound);
            nya_assert(compressed != nullptr, "out of memory compressing '%.*s'", NYA_FMT_STRING_ARG(file));

            u64 written = nya_compress(content->items, content->length, compressed, bound);

            // Only when it actually saves something. See the threshold's note for why the test is on
            // bytes saved rather than on the ratio.
            if (written > 0 && written + NYA_ASSET_BLOB_MIN_COMPRESSION_SAVING_BYTES <= content->length) {
                stored      = compressed;
                stored_size = written;
                compressed_count++;
            }
        }

        total_raw += content->length;
        total_stored += stored_size;

        nya_string_extend_sprintf(header_string, "  { \"%.*s\", " FMTu64 ", " FMTu64 ", " FMTu64 " },\n", NYA_FMT_STRING_ARG(file), cursor,
                                  content->length, stored_size);

        // A byte costs at most the indent plus "0xAB" plus a separator, so the room for a whole file
        // is known before writing any of it and the buffer grows once rather than per byte.
        nya_array_reserve(blob_string, blob_string->length + stored_size * (NYA_ASSET_BLOB_INDENT + 6) + 1);

        for (u64 byte_index = 0; byte_index < stored_size; byte_index++) {
            const u8* c = &stored[byte_index];
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

        cursor += stored_size;
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

    nya_log_info("Bundled " FMTu64 " assets: " FMTu64 " KB into " FMTu64 " KB (" FMTu64 " compressed, " FMTu64 " stored verbatim).",
                 files->length, total_raw / 1024, total_stored / 1024, compressed_count, files->length - compressed_count);

    /*
     * Deliberately not run through clang-format, which every other generated file here is.
     * */
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Every asset file under ./assets/, walked once per build tool invocation and memoised.
 * */
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _nya_asset_enumerate(void) {
    if (_NYA_ASSET_FILES != nullptr) return _NYA_ASSET_FILES;

    _NYA_ASSET_FILES = _nya_asset_walk("./assets");
    return _NYA_ASSET_FILES;
}

/**
 * Collects every regular file under `directory`, sorted.
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
 * */
NYA_INTERNAL NYA_BuildRulePolicy _nya_asset_shader_policy(u64 newest_include, NYA_ConstCString target) {
    nya_assert(target != nullptr);

    u64 target_modified_at = 0;
    if (!nya_filesystem_last_modified(target, &target_modified_at).ok) return NYA_BUILD_IF_OUTDATED;

    return newest_include > target_modified_at ? NYA_BUILD_ALWAYS : NYA_BUILD_IF_OUTDATED;
}

/** Whether nya_build would run the rule `policy`, `source` and `target` describe. */
NYA_INTERNAL b8 _nya_asset_shader_outdated(NYA_BuildRulePolicy policy, NYA_ConstCString source, NYA_ConstCString target) {
    nya_assert(policy == NYA_BUILD_ALWAYS || policy == NYA_BUILD_IF_OUTDATED);

    if (policy == NYA_BUILD_ALWAYS) return true;

    u64 source_modified_at = 0;
    u64 target_modified_at = 0;
    if (!nya_filesystem_last_modified(target, &target_modified_at).ok) return true;
    if (!nya_filesystem_last_modified(source, &source_modified_at).ok) return true;

    return source_modified_at > target_modified_at;
}
