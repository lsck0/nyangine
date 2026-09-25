#include "build/build.h"

/* The GLSL-ES cross compiler. Keyed off the header the way base_backtrace.h keys the symbolizer off backtrace.h: build.c puts SPIRV-Cross's include path and link flags on the rebuild command together, and only once the vendored .so exists, so when it does not, __has_include is false, the code below degrades to writing no GLSL, and the tool still links. See vendor_sdl_shadercross.h for the flags. */
#if !OS_WINDOWS && __has_include("spirv_cross_c.h")
#define NYA_BUILD_HAS_SPIRV_CROSS 1
#include "spirv_cross_c.h"
#else
#define NYA_BUILD_HAS_SPIRV_CROSS 0
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
 * Cross compiles the SPIR-V at `spirv` to GLSL ES 300, writing it to `glsl`. The point of stage 1 of the
 * web backend: a later GLES3/WebGL2 renderer loads these instead of the .spv Vulkan takes. Does nothing
 * when the tool was built without SPIRV-Cross (NYA_BUILD_HAS_SPIRV_CROSS). Returns whether a `.glsl` was
 * written: false means the SPIR-V uses a feature GLSL ES 300 lacks (the mesh3d shaders' textureGather), which
 * the caller answers by compiling and converting a NYA_WEB_SHADER variant instead. True with no SPIRV-Cross,
 * since there is then nothing to convert and nothing for the caller to retry.
 *
 * `warn_on_failure` logs the SPIRV-Cross reason when the conversion is refused. The caller sets it false for
 * the first, desktop-SPIR-V attempt, whose failure is expected for the mesh3d shaders and answered by the web
 * variant, so a handled textureGather does not warn; and true for the web variant, whose failure is real.
 * */
NYA_INTERNAL b8 _nya_asset_shader_compile_glsl_es(NYA_ConstCString spirv, NYA_ConstCString glsl, b8 warn_on_failure);

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
 * The suffix of the GLSL ES 300 variant, cross compiled from each `.spv` by _nya_asset_shader_compile_glsl_es.
 * Named `<shader>.<stage>.glsl` beside `<shader>.<stage>.spv`, e.g. `shape.frag.spv` -> `shape.frag.glsl`.
 *
 * Deliberately not one of _NYA_ASSET_SHADER_FORMATS: those are the backends shadercross writes and the
 * loader picks between, and every one of them is baked into the release blob. Nothing loads GLSL yet (the
 * GLES3 backend is a later stage), so it is produced beside the others but kept out of the index and the
 * blob (_nya_asset_collect skips it) until a backend actually reads it.
 * */
#define SHADER_GLSL_ES_SUFFIX ".glsl"

/**
 * The suffix of the intermediate "web" SPIR-V, compiled from a shader's HLSL with NYA_WEB_SHADER defined
 * (see _nya_asset_shader_compile_glsl_es's caller for the convention). Named `<shader>.<stage>.web.spv`.
 *
 * Only the four mesh3d lit fragment shaders emit it: their desktop `.spv` uses textureGather (the PCF shadow
 * tap in mesh3d_shading.hlsli), which SPIRV-Cross cannot lower to GLSL ES 300 ("textureGather requires ESSL
 * 310"). NYA_WEB_SHADER swaps that one tap for four explicit ESSL-300-safe samples, and this SPIR-V is the
 * one the GLSL-ES step converts for those shaders. Native/desktop still load the gather `.spv`. It is a build
 * intermediate, kept beside the outputs like the .glsl but neither indexed nor baked (_nya_asset_collect
 * skips it, and _nya_asset_shader_prune ties it to the same source as the rest).
 * */
#define SHADER_WEB_SPIRV_SUFFIX ".web.spv"

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
                        "-d", _NYA_ASSET_SHADER_FORMATS[i][0],

                        // shadercross compiles from a temporary file, so relative `#include`s need the source root.
                        "-I", SHADER_SOURCE_DIRECTORY,
                    },
                },
            };
            NYA_EXPECT(nya_build(&rule));
        }

        // Compute shaders are desktop only: WebGL2/GLES3, the GLSL ES 300 backend's target, has no compute stage at all, so there is nothing to cross-compile and no web variant to try. Skip the whole GLSL step for a `.comp` shader, which leaves `./build build shaders` green with no spurious `.comp.glsl` or `.comp.web.spv` beside it. The engine's compute path is gated the same way behind !OS_WASM; see render_compute_particles.c and the renderer-web wall.
        if (nya_string_ends_with(shader, ".comp")) continue;

        // Beside the .spv, its GLSL ES 300 form for a later GLES3/WebGL2 backend. Compiled by the build tool in-process (shadercross has no GLSL target), and only when the .spv is newer than it: a changed shared include rebuilt the .spv above, so comparing against it also catches that.
        NYA_CString spirv = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%.*s.spv", (int)shader->length, shader->items));
        NYA_CString glsl  = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%.*s" SHADER_GLSL_ES_SUFFIX, (int)shader->length, shader->items));
        if (nya_filesystem_exists(spirv) && _nya_asset_shader_outdated(NYA_BUILD_IF_OUTDATED, spirv, glsl) && !_nya_asset_shader_compile_glsl_es(spirv, glsl, false)) {
            /* The desktop SPIR-V uses a feature GLSL ES 300 lacks — the four mesh3d lit fragment shaders reach mesh3d_shading.hlsli's textureGather shadow tap, which SPIRV-Cross refuses at version 300. Compile a web variant of the HLSL with NYA_WEB_SHADER defined, which takes the ESSL-300-safe four-tap path, and convert that in place of the gather one. The desktop .spv built above is untouched, so native keeps loading the gather build. The convention is by result, not a hardcoded list: any shader whose plain .spv will not lower to GLSL ES 300 gets a web variant, so a future ESSL-310 feature is handled the same way with no edit here. */
            NYA_CString web_spirv = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%.*s" SHADER_WEB_SPIRV_SUFFIX, (int)shader->length, shader->items));

            NYA_BuildRule web_rule = {
                .name        = nya_string_to_cstring(nya_arena_global, nya_string_sprintf(nya_arena_global, "%s -> %s", source, web_spirv)),
                // Include-aware like the desktop compiles above: a changed shared .hlsli rebuilds this too.
                .policy      = _nya_asset_shader_policy(newest_include, web_spirv),
                .input_file  = source,
                .output_file = web_spirv,
                .command = {
                    .program     = SHADERCROSS_BINARY,
                    .environment = { SHADERCROSS_LIBRARY_PATH },
                    .arguments = {
                        source,
                        "-o", web_spirv,
                        "-s", "hlsl",
                        "-d", "spirv",
                        "-I", SHADER_SOURCE_DIRECTORY,

                        // The one difference from the desktop compile: the shader's web path is taken.
                        "-DNYA_WEB_SHADER",
                    },
                },
            };
            NYA_EXPECT(nya_build(&web_rule));

            // Best effort like the desktop attempt above: a web variant that still will not convert is reported (warn_on_failure) and leaves no .glsl, rather than failing the whole step.
            (void)_nya_asset_shader_compile_glsl_es(web_spirv, glsl, true);
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

        u32 group = _nya_asset_blob_group(file);

        if (group != header_group) {
            if (header_group != 0) nya_string_extend(header_string, "#endif\n");
            if (group != 0) nya_string_extend_sprintf(header_string, "#if %s\n", _NYA_ASSET_SHADER_FORMATS[group - 1][2]);
            header_group = group;
        }

        NYA_String* blob_name = _nya_asset_blob_name(group);
        nya_string_extend_sprintf(header_string, "  { \"%.*s\", " NYA_FMT_STRING " + " FMTu64 ", " FMTu64 ", " FMTu64 ", 0x%016" PRIX64 "ULL },\n",
                                  NYA_FMT_STRING_ARG(file), NYA_FMT_STRING_ARG(blob_name), cursors[group], content->length, stored_size,
                                  nya_integrity_hash(stored, stored_size));

        // A byte costs at most the indent plus "0xAB" plus a separator, so the room for a whole file is known before writing any of it and the buffer grows once rather than per byte.
        NYA_String* blob_string = blob_strings[group];
        nya_array_reserve(blob_string, blob_string->length + stored_size * (NYA_ASSET_BLOB_INDENT + 6) + 1);

        for (u64 byte_index = 0; byte_index < stored_size; byte_index++) {
            const u8* c = &stored[byte_index];
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

        cursors[group] += stored_size;
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

        // only what the shader build writes: the per-backend formats shadercross emits and the GLSL ES variant produced beside them. Anything else in the directory is not this rule's to judge.
        NYA_ConstCString suffix = nullptr;
        for (u32 i = 0; i < nya_carray_length(_NYA_ASSET_SHADER_FORMATS); i++) {
            if (nya_string_ends_with(file, _NYA_ASSET_SHADER_FORMATS[i][1])) suffix = _NYA_ASSET_SHADER_FORMATS[i][1];
        }
        if (nya_string_ends_with(file, SHADER_GLSL_ES_SUFFIX)) suffix = SHADER_GLSL_ES_SUFFIX;
        // Last, so it wins over the .spv the format loop matched on: .web.spv is one intermediate, tied to the same source as the rest, and deleted with them when that source is gone.
        if (nya_string_ends_with(file, SHADER_WEB_SPIRV_SUFFIX)) suffix = SHADER_WEB_SPIRV_SUFFIX;
        if (suffix == nullptr) continue;

        nya_string_strip_prefix(file, SHADER_COMPILED_DIRECTORY "/");
        nya_string_strip_suffix(file, suffix);

        NYA_String* source = nya_string_sprintf(nya_arena_global, SHADER_SOURCE_DIRECTORY "/%.*s.hlsl", (int)file->length, file->items);
        if (nya_filesystem_exists(nya_string_to_cstring(nya_arena_global, source))) continue;

        NYA_EXPECT(nya_filesystem_delete(path));
        nya_log_info("Deleted %s: its source %.*s is gone.", path, (int)source->length, source->items);
    }
}

#if NYA_BUILD_HAS_SPIRV_CROSS
/**
 * Cross compiles `ir` to GLSL ES 300 and returns the source (owned by `context`), or nullptr on failure
 * with the reason left in the context's last error. `uniforms_as_plain` is the one knob the caller turns:
 * see the fallback in _nya_asset_shader_compile_glsl_es for why. Takes the IR by copy so the caller can try
 * again with a different setting.
 * */
NYA_INTERNAL const char* _nya_asset_shader_emit_glsl_es(spvc_context context, spvc_parsed_ir ir, b8 uniforms_as_plain) {
    spvc_compiler compiler = nullptr;
    if (spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_COPY, &compiler) != SPVC_SUCCESS) return nullptr;

    // The shaders come from HLSL, where a texture and a sampler are separate objects, and Vulkan SPIR-V keeps them that way. GLSL ES has no separate samplers, only combined `sampler2D`, so SPIRV-Cross has to fold each texture+sampler pair into one before it can emit anything; without this, compile() fails with "Cannot find mapping for combined sampler". The dummy sampler covers a texture read with no sampler of its own (a texelFetch/Load), which the post-process passes do.
    spvc_variable_id dummy_sampler = 0;
    if (spvc_compiler_build_dummy_sampler_for_combined_images(compiler, &dummy_sampler) != SPVC_SUCCESS) return nullptr;
    if (spvc_compiler_build_combined_image_samplers(compiler) != SPVC_SUCCESS) return nullptr;

    spvc_compiler_options options = nullptr;
    if (spvc_compiler_create_compiler_options(compiler, &options) != SPVC_SUCCESS) return nullptr;

    // GLSL ES 3.00, what WebGL2 and GLES3 accept. Default the float precision to highp so the output does not depend on an implementation's mediump range, which varies and is too narrow for the engine's math.
    (void)spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, 300);
    (void)spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, true);
    (void)spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES_DEFAULT_FLOAT_PRECISION_HIGHP, true);
    if (uniforms_as_plain) (void)spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_EMIT_UNIFORM_BUFFER_AS_PLAIN_UNIFORMS, true);

    if (spvc_compiler_install_compiler_options(compiler, options) != SPVC_SUCCESS) return nullptr;

    const char* source = nullptr;
    if (spvc_compiler_compile(compiler, &source) != SPVC_SUCCESS) return nullptr;
    return source;
}

b8 _nya_asset_shader_compile_glsl_es(NYA_ConstCString spirv, NYA_ConstCString glsl, b8 warn_on_failure) {
    nya_assert(spirv != nullptr);
    nya_assert(glsl != nullptr);

    // SPIR-V is a stream of 32-bit words; read the bytes the SPIR-V rule just wrote.
    NYA_String* spirv_bytes = nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read(spirv, spirv_bytes), "while reading %s to cross compile it to GLSL ES", spirv);
    nya_assert(spirv_bytes->length % sizeof(SpvId) == 0, "%s is " FMTu64 " bytes, not a whole number of SPIR-V words.", spirv, spirv_bytes->length);

    // An aligned copy: the parser takes a `const SpvId*`, and casting the byte buffer straight to one trips the alignment sanitizer the build tool runs under. The global arena aligns to 16.
    size_t word_count = spirv_bytes->length / sizeof(SpvId);
    SpvId* words      = nya_arena_alloc(nya_arena_global, spirv_bytes->length);
    nya_memcpy(words, spirv_bytes->items, spirv_bytes->length);

    // The context owns every allocation its children make, so one destroy at the end frees all of it, both compilers below and the returned GLSL string included.
    spvc_context context = nullptr;
    if (spvc_context_create(&context) != SPVC_SUCCESS) nya_log_panic("Could not create a SPIRV-Cross context for %s.", spirv);

    spvc_parsed_ir ir = nullptr;
    if (spvc_context_parse_spirv(context, words, word_count, &ir) != SPVC_SUCCESS)
        nya_log_panic("SPIRV-Cross could not parse %s: %s", spirv, spvc_context_get_last_error_string(context));

    // First as UBOs: a push constant or cbuffer block becomes a `uniform` block, the natural mapping. That fails for a block whose packing needs per-member byte offsets, because GLSL ES 300 has no offset qualifier on block members (no GL_ARB_enhanced_layouts). Fall back to plain uniforms, which carry no layout rule at all, and note it: the GLES3 backend uploads those with glUniform*, not a UBO binding.
    const char* source = _nya_asset_shader_emit_glsl_es(context, ir, false);
    if (source == nullptr) {
        source = _nya_asset_shader_emit_glsl_es(context, ir, true);
        if (source != nullptr) nya_log_info("%s: uniform block is not std140-expressible in GLSL ES 300, emitted as plain uniforms.", spirv);
    }

    // Non-fatal on purpose: stage 1 is meant to surface exactly which shaders a GLES3 backend cannot take as is, so a refusal is reported and the rest still build, rather than stopping the whole shader step. The caller reads the false to compile a NYA_WEB_SHADER variant and convert that in place of this one.
    if (source == nullptr) {
        if (warn_on_failure) nya_log_warn("%s did not convert to GLSL ES 300, no variant written: %s", spirv, spvc_context_get_last_error_string(context));
        spvc_context_destroy(context);
        return false;
    }

    NYA_EXPECT(nya_file_write(glsl, source), "while writing %s", glsl);
    spvc_context_destroy(context);
    return true;
}
#else
b8 _nya_asset_shader_compile_glsl_es(NYA_ConstCString spirv, NYA_ConstCString glsl, b8 warn_on_failure) {
    // Built without SPIRV-Cross (see the guard at the top of this file): the library links in on the next rebuild once the vendors exist, and until then there is nothing to cross compile with. True so the caller does not waste a shadercross run compiling a web variant it also could not convert.
    (void)spirv;
    (void)glsl;
    (void)warn_on_failure;
    return true;
}
#endif

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

    // The GLSL ES shader variants are produced beside the .spv for a later GLES3 backend, but nothing loads them yet, so like NYA_ASSET_UNUSED_DIRECTORY they are neither indexed nor baked into the blob.
    if (nya_string_ends_with(file, SHADER_GLSL_ES_SUFFIX)) return true;

    // The web SPIR-V is only the intermediate the GLSL-ES step reads for the four mesh3d shaders; the loader never sees it, so it is kept out of the index and blob too. It also ends in .spv, so were it not skipped here it would be baked as a spurious spirv asset.
    if (nya_string_ends_with(file, SHADER_WEB_SPIRV_SUFFIX)) return true;

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
