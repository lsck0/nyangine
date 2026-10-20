#include "nyangine-build/build.h"

/* The build tool is compiled without the crypto module by design (see nyangine.c: crypto is on the project's
 * include line, not the tool's), because it hashes nothing. At-rest asset obfuscation is the one exception, and
 * only when opted in: NYA_ASSET_ENCRYPT_BLOB pulls in exactly the three crypto TUs the bundler needs plus
 * monocypher, so a tool built without the flag stays crypto-free and its output byte-identical. A tool built with
 * it must also be given the monocypher include directory, e.g. -Ivendor/monocypher/src. See pp/asset.h. */
#ifdef NYA_ASSET_ENCRYPT_BLOB
/* lz4 (pulled in above) already defines MIN with the same meaning; drop it so monocypher's identical macro
 * does not warn under -Werror. */
#undef MIN
#include "monocypher.c"
#include "nyangine-core/crypto/crypto_secret.c"
#include "nyangine-core/crypto/crypto_hash.c"
#include "nyangine-core/crypto/crypto_aead.c"
#include "nyangine-core/core/core_asset_crypt.h"
#endif

/* PRIVATE API DECLARATION */

NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _nya_asset_walk(NYA_ConstCString directory) __attr_no_discard;
NYA_INTERNAL NYA_ArrayᐸNYA_Stringᐳ* _nya_asset_enumerate(void) __attr_no_discard;
NYA_INTERNAL b8                     _nya_asset_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32                    _nya_asset_path_compare(const NYA_String* a, const NYA_String* b);
NYA_INTERNAL u32                    _nya_asset_blob_group(const NYA_String* file) __attr_no_discard;
NYA_INTERNAL NYA_String*            _nya_asset_blob_name(u32 group) __attr_no_discard;
NYA_INTERNAL NYA_BuildRulePolicy    _nya_asset_shader_policy(u64 newest_include, NYA_ConstCString target) __attr_no_discard;
NYA_INTERNAL b8 _nya_asset_shader_outdated(NYA_BuildRulePolicy policy, NYA_ConstCString source, NYA_ConstCString target) __attr_no_discard;

/**
 * Deletes every compiled shader whose `.hlsl` is gone. Found as `mesh3d_outline.vert.*`, left over from the
 * inverted hull outline after screen space ink replaced it, and still baked into every release blob.
 * */
NYA_INTERNAL void _nya_asset_shader_prune(void);

/**
 * Where a shader output is written before it is renamed over `target`. Per process, so two builds sharing
 * ./assets/shader/compiled/ (worktrees link it) never write the same file, and a reader only ever sees a
 * complete one. Ends in `.tmp`, which _nya_asset_shader_prune never matches, so an in-flight one survives a
 * concurrent prune.
 * */
NYA_INTERNAL NYA_CString _nya_asset_shader_temporary(NYA_ConstCString target) __attr_no_discard;

/** Compiles `source` with shadercross to the `format` backend at `target`, through a temporary. */
NYA_INTERNAL void _nya_asset_shader_compile(NYA_CString source, NYA_CString target, NYA_ConstCString format, NYA_BuildRulePolicy policy);

/**
 * Every format shaders compile to: the name shadercross takes, the file suffix, and the targets that bake it into
 * the release blob, which are the ones with an SDL GPU backend that accepts it. Vulkan takes SPIR-V, Direct3D 12
 * DXIL, Metal MSL. The loader picks by what the device accepts (_nya_asset_pick_correct_compiled_shader), so a
 * Linux build carries SPIR-V only and a Windows build SPIR-V and DXIL.
 * */
NYA_INTERNAL const NYA_ConstCString _NYA_ASSET_SHADER_FORMATS[][3] = {
    { "dxil", ".dxil", "OS_WINDOWS" },
    { "msl", ".msl", "OS_MAC" },
    { "spirv", ".spv", "OS_LINUX || OS_WINDOWS" },
};

/**
 * What the shader build wrote before the GLSL ES step went with the web client. Nothing writes it any more,
 * so _nya_asset_shader_prune deletes it outright; left in place it would be baked into every blob.
 * */
#define SHADER_RETIRED_GLSL_ES_SUFFIX ".glsl"

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

/* PUBLIC API IMPLEMENTATION */

void nya_asset_compile_shaders(void) {
    // when a shared `.hlsli` last changed, which no per shader rule can see.
    NYA_ConstCString include_roots[] = { SHADER_SOURCE_DIRECTORY, nullptr };
    u64              newest_include  = nya_pp_newest(include_roots, ".hlsli");

    // not the shared enumeration: this writes into ./assets/shader/compiled/, and a cached listing would predate its own outputs.
    NYA_ArrayᐸNYA_Stringᐳ* shaders = _nya_asset_walk(SHADER_SOURCE_DIRECTORY);

    NYA_EXPECT(nya_filesystem_create_directory(SHADER_COMPILED_DIRECTORY));
    _nya_asset_shader_prune();

    nya_array_foreach (shaders, shader) {
        if (!nya_string_ends_with(shader, ".hlsl")) continue;

        NYA_CString source = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_prefix(shader, SHADER_SOURCE_DIRECTORY "/");
        nya_string_strip_suffix(shader, ".hlsl");
        nya_string_extend_front(shader, SHADER_COMPILED_DIRECTORY "/");

        for (u32 i = 0; i < nya_carray_length(_NYA_ASSET_SHADER_FORMATS); i++) {
            NYA_CString target = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%.*s%s", (int)shader->length, shader->items, _NYA_ASSET_SHADER_FORMATS[i][1]));
            NYA_BuildRulePolicy policy = _nya_asset_shader_policy(newest_include, target);

            // a Windows host cannot build shadercross (DXC does not compile under MinGW), so it relies on shaders compiled elsewhere and must say so when one is stale.
            if (!nya_filesystem_exists(SHADERCROSS_BINARY) && _nya_asset_shader_outdated(policy, source, target)) {
                nya_log_panic("%s needs compiling, but %s does not exist. Compile shaders on a Linux host and copy ./assets/shader/compiled/.",
                              source, SHADERCROSS_BINARY);
            }

            _nya_asset_shader_compile(source, target, _NYA_ASSET_SHADER_FORMATS[i][0], policy);
        }
    }
}

void nya_asset_index(void) {
    NYA_ConstCString inputs[]  = { "./assets", "./src/nyangine-build/pp/asset.c", nullptr };
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
    NYA_ConstCString inputs[]  = { "./assets", "./src/nyangine-build/pp/asset.c", nullptr };
    NYA_ConstCString outputs[] = { NYA_ASSET_BUNDLE_OUTPUT, nullptr };
    if (nya_pp_is_current("bundle_assets", inputs, outputs)) return;

    NYA_ConstCString output_file = NYA_ASSET_BUNDLE_OUTPUT;

    NYA_Arena*  arena         = nya_arena_global;
    NYA_String* result        = nya_string_create(arena);
    NYA_String* header_string = nya_string_create(arena);

    /* One byte array per group: group 0 is what every target bakes, group i + 1 the shaders compiled to format i, behind that format's target condition. The headers stay in path order, each behind its group's condition. */
    u32         group_count                                                  = nya_carray_length(_NYA_ASSET_SHADER_FORMATS) + 1;
    NYA_String* blob_strings[nya_carray_length(_NYA_ASSET_SHADER_FORMATS) + 1];
    u64         cursors[nya_carray_length(_NYA_ASSET_SHADER_FORMATS) + 1]      = { 0 };
    u64         emitted[nya_carray_length(_NYA_ASSET_SHADER_FORMATS) + 1]      = { 0 };
    for (u32 group = 0; group < group_count; group++) blob_strings[group] = nya_string_create(arena);

    // the same list nya_asset_index built its handles from. A second walk could see a file appear or vanish and emit a handle with no blob entry.
    NYA_ArrayᐸNYA_Stringᐳ* files = _nya_asset_enumerate();
    nya_string_extend(result, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(result, "#include \"nyangine-core/nyangine.h\"\n\n");

    /* At-rest obfuscation, OFF unless the tool was built with NYA_ASSET_ENCRYPT_BLOB. When off, none of this is
     * compiled or emitted and the file is byte for byte what it always was — no crypto is even linked into the
     * tool. When on, the key is derived once from the passphrase and baked in beside a flag the loader reads; see
     * pp/asset.h for the honest threat model. */
#ifdef NYA_ASSET_ENCRYPT_BLOB
    const b8        encrypt  = true;
    NYA_CryptoKey32 blob_key = { 0 };
    {
        NYA_CryptoSha256Digest digest = { 0 };
        nya_crypto_sha256((const u8*)NYA_ASSET_ENCRYPT_KEY, sizeof(NYA_ASSET_ENCRYPT_KEY) - 1, &digest);
        nya_memcpy(blob_key.bytes, digest.bytes, sizeof(blob_key.bytes));

        nya_string_extend(result, "#define NYA_ASSET_BLOB_ENCRYPTED 1\n");
        nya_string_extend(result, "static const NYA_CryptoKey32 NYA_ASSET_BLOB_KEY = { .bytes = {\n    ");
        for (u32 i = 0; i < sizeof(blob_key.bytes); i++) nya_string_extend_sprintf(result, "0x%02X, ", blob_key.bytes[i]);
        nya_string_extend(result, "\n} };\n\n");
    }
#endif

    nya_string_extend(header_string, "static const NYA_AssetBlobHeader NYA_ASSET_BLOB_HEADER[] = {\n");

    NYA_ConstCString HEX = "0123456789ABCDEF";

    u32 header_group     = 0;
    u64 total_raw        = 0;
    u64 total_stored     = 0;
    u64 compressed_count = 0;

    nya_array_foreach (files, file) {
        NYA_String* content = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(file, content));

        /* Compressed per entry, kept only when smaller. Per entry so a load expands one asset rather than the whole blob. Already compressed formats such as PNG and OGG stay verbatim and keep the zero copy load path. */
        const u8* stored      = content->items;
        u64       stored_size = content->length;

        u64 bound = nya_compress_bound(content->length);

        if (bound > 0) {
            u8* compressed = nya_arena_alloc(arena, bound);
            nya_assert(compressed != nullptr, "out of memory compressing '%.*s'", NYA_FMT_STRING_ARG(file));

            u64 written = nya_compress(content->items, content->length, compressed, bound);

            // Only when it actually saves something. See the threshold's note for why the test is on bytes saved rather than on the ratio.
            if (written > 0 && written + NYA_ASSET_BLOB_MIN_COMPRESSION_SAVING_BYTES <= content->length) {
                stored      = compressed;
                stored_size = written;
                compressed_count++;
            }
        }

        total_raw += content->length;
        total_stored += stored_size;

        /* What actually lands in the array. Without encryption it is the stored bytes verbatim, so the output is
         * unchanged. With it, the entry becomes nonce(24) || ciphertext(stored_size) || tag(16): the nonce is a
         * keyed hash of the plaintext, so it is unique per distinct content and reproducible across builds without a
         * counter to keep; the path is the AEAD's associated data, so an entry moved to another path fails to open,
         * exactly as a @secret field is bound to its name. compressed_size stays the ciphertext length the loader
         * decrypts; the header's hash and the cursor cover the whole framed entry. */
        const u8* blob_bytes      = stored;
        u64       blob_bytes_size = stored_size;
#ifdef NYA_ASSET_ENCRYPT_BLOB
        if (encrypt) {
            u8* framed = nya_arena_alloc(arena, stored_size + NYA_ASSET_BLOB_FRAME_OVERHEAD);
            nya_assert(framed != nullptr, "out of memory encrypting '%.*s'", NYA_FMT_STRING_ARG(file));

            NYA_CString aad = nya_string_to_cstring(arena, file);
            blob_bytes      = framed;
            blob_bytes_size = nya_asset_blob_frame(&blob_key, stored, stored_size, aad, framed);
        }
#endif

        u32 group = _nya_asset_blob_group(file);

        if (group != header_group) {
            if (header_group != 0) nya_string_extend(header_string, "#endif\n");
            if (group != 0) nya_string_extend_sprintf(header_string, "#if %s\n", _NYA_ASSET_SHADER_FORMATS[group - 1][2]);
            header_group = group;
        }

        NYA_String* blob_name = _nya_asset_blob_name(group);
        nya_string_extend_sprintf(header_string, "  { \"%.*s\", " NYA_FMT_STRING " + " FMTu64 ", " FMTu64 ", " FMTu64 ", 0x%016" PRIX64 "ULL },\n",
                                  NYA_FMT_STRING_ARG(file), NYA_FMT_STRING_ARG(blob_name), cursors[group], content->length, stored_size,
                                  nya_integrity_hash(blob_bytes, blob_bytes_size));

        // A byte costs at most the indent plus "0xAB" plus a separator, so the room for a whole file is known before writing any of it and the buffer grows once rather than per byte.
        NYA_String* blob_string = blob_strings[group];
        nya_array_reserve(blob_string, blob_string->length + blob_bytes_size * (NYA_ASSET_BLOB_INDENT + 6) + 1);

        for (u64 byte_index = 0; byte_index < blob_bytes_size; byte_index++) {
            const u8* c = &blob_bytes[byte_index];
            u8* out = blob_string->items + blob_string->length;

            if (emitted[group] % NYA_ASSET_BLOB_BYTES_PER_LINE == 0) {
                for (u64 i = 0; i < NYA_ASSET_BLOB_INDENT; i++) *out++ = ' ';
                blob_string->length += NYA_ASSET_BLOB_INDENT;
            }

            out[0] = '0';
            out[1] = 'x';
            out[2] = (u8)HEX[*c >> 4];
            out[3] = (u8)HEX[*c & 0x0F];
            out[4] = ',';
            out[5] = (emitted[group] % NYA_ASSET_BLOB_BYTES_PER_LINE == NYA_ASSET_BLOB_BYTES_PER_LINE - 1) ? '\n' : ' ';

            blob_string->length += 6;
            emitted[group]++;
        }

        cursors[group] += blob_bytes_size;
    }

    if (header_group != 0) nya_string_extend(header_string, "#endif\n");
    nya_string_extend(header_string, "};\n\n");

    for (u32 group = 0; group < group_count; group++) {
        NYA_String* blob_string = blob_strings[group];

        // no bytes, no array: a group with nothing in it would be an empty initializer.
        if (emitted[group] == 0) continue;

        // A blob whose last line was full already ends in a newline. One that did not ends in the separator space written after its final byte, which becomes that newline rather than being left behind as trailing whitespace.
        if (emitted[group] % NYA_ASSET_BLOB_BYTES_PER_LINE != 0) blob_string->items[blob_string->length - 1] = '\n';

        NYA_String* blob_name = _nya_asset_blob_name(group);
        if (group != 0) nya_string_extend_sprintf(result, "#if %s\n", _NYA_ASSET_SHADER_FORMATS[group - 1][2]);
        nya_string_extend_sprintf(result, "static const u8 " NYA_FMT_STRING "[] = {\n", NYA_FMT_STRING_ARG(blob_name));
        nya_string_extend(result, blob_string);
        nya_string_extend(result, "};\n");
        if (group != 0) nya_string_extend(result, "#endif\n");
        nya_string_extend(result, "\n");
    }

    nya_string_extend(result, header_string);
    nya_string_extend(result, "static const u64 NYA_ASSET_BLOB_HEADER_COUNT = nya_carray_length(NYA_ASSET_BLOB_HEADER);\n");

    NYA_EXPECT(nya_file_write(output_file, result));

    nya_log_info("Bundled " FMTu64 " assets: " FMTu64 " KB into " FMTu64 " KB (" FMTu64 " compressed, " FMTu64 " stored verbatim).",
                 files->length, total_raw / 1024, total_stored / 1024, compressed_count, files->length - compressed_count);

    /*
     * Deliberately not run through clang-format, which every other generated file here is.
     * */
}

/* PRIVATE API IMPLEMENTATION */

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
void _nya_asset_shader_prune(void) {
    NYA_ArrayᐸNYA_Stringᐳ* compiled = _nya_asset_walk(SHADER_COMPILED_DIRECTORY);

    nya_array_foreach (compiled, file) {
        NYA_CString path = nya_string_to_cstring(nya_arena_global, file);

        if (nya_string_ends_with(file, SHADER_RETIRED_GLSL_ES_SUFFIX)) {
            NYA_EXPECT(nya_filesystem_delete(path));
            nya_log_info("Deleted %s: nothing reads GLSL ES shaders any more.", path);
            continue;
        }

        // only what the shader build writes: the per-backend formats shadercross emits. Anything else in the directory is not this rule's to judge.
        NYA_ConstCString suffix = nullptr;
        for (u32 i = 0; i < nya_carray_length(_NYA_ASSET_SHADER_FORMATS); i++) {
            if (nya_string_ends_with(file, _NYA_ASSET_SHADER_FORMATS[i][1])) suffix = _NYA_ASSET_SHADER_FORMATS[i][1];
        }
        if (suffix == nullptr) continue;

        nya_string_strip_prefix(file, SHADER_COMPILED_DIRECTORY "/");
        nya_string_strip_suffix(file, suffix);

        NYA_String* source = nya_string_sprintf(nya_arena_global, SHADER_SOURCE_DIRECTORY "/%.*s.hlsl", (int)file->length, file->items);
        if (nya_filesystem_exists(nya_string_to_cstring(nya_arena_global, source))) continue;

        NYA_EXPECT(nya_filesystem_delete(path));
        nya_log_info("Deleted %s: its source %.*s is gone.", path, (int)source->length, source->items);
    }
}

NYA_CString _nya_asset_shader_temporary(NYA_ConstCString target) {
    nya_assert(target != nullptr);
    return nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%s.%u.tmp", target, nya_os_process_id()));
}

void _nya_asset_shader_compile(NYA_CString source, NYA_CString target, NYA_ConstCString format, NYA_BuildRulePolicy policy) {
    nya_assert(source != nullptr && target != nullptr && format != nullptr);

    NYA_CString   temporary = _nya_asset_shader_temporary(target);
    NYA_BuildRule rule      = {
             .name        = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%s -> %s", source, target)),
             .policy      = policy,
             .input_file  = source,
             .output_file = target,
             .command = {
                 .program     = SHADERCROSS_BINARY,
                 .environment = { SHADERCROSS_LIBRARY_PATH },
                 // shadercross compiles from a temporary file, so relative `#include`s need the source root.
                 .arguments = { source, "-o", temporary, "-s", "hlsl", "-d", format, "-I", SHADER_SOURCE_DIRECTORY },
        },
    };
    NYA_EXPECT(nya_build(&rule));

    // an up to date rule ran nothing and left no temporary; only a fresh compile is moved over the old output.
    if (nya_filesystem_exists(temporary)) NYA_EXPECT(nya_filesystem_move(temporary, target), "while moving %s into place", target);
}

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

    // nya_path_join normalises away a leading "./", but these paths are baked into generated source as asset IDs that the runtime then looks up verbatim. Put it back, so the IDs stay exactly what they were when this walked the tree with `find ./assets/`.
    if (!nya_string_starts_with(file, "./")) nya_string_extend_front(file, "./");

    /* assets.c and assets.h are the generated output of this very walk, and .keep only exists to keep an empty directory in git. None of the three is an asset. */
    if (nya_string_ends_with(file, ".c")) return true;
    if (nya_string_ends_with(file, ".h")) return true;
    if (nya_string_ends_with(file, ".keep")) return true;
    if (nya_string_starts_with(file, NYA_ASSET_UNUSED_DIRECTORY)) return true;

    nya_array_push_back(files, *file);
    return true;
}

/** Which blob group a file bakes into: 0 for every target, i + 1 for a shader compiled to format i. */
NYA_INTERNAL u32 _nya_asset_blob_group(const NYA_String* file) {
    nya_assert(file != nullptr);

    if (!nya_string_contains(file, "/shader/compiled/")) return 0;

    for (u32 i = 0; i < nya_carray_length(_NYA_ASSET_SHADER_FORMATS); i++) {
        if (nya_string_ends_with(file, _NYA_ASSET_SHADER_FORMATS[i][1])) return i + 1;
    }

    return 0;
}

/** NYA_ASSET_BLOB for group 0, NYA_ASSET_BLOB_<FORMAT> for the group of a shader format. */
NYA_INTERNAL NYA_String* _nya_asset_blob_name(u32 group) {
    nya_assert(group <= nya_carray_length(_NYA_ASSET_SHADER_FORMATS));

    NYA_String* name = nya_string_from(nya_arena_global, "NYA_ASSET_BLOB");
    if (group == 0) return name;

    nya_string_extend_sprintf(name, "_%s", _NYA_ASSET_SHADER_FORMATS[group - 1][0]);
    nya_string_to_upper(name);
    return name;
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
