#include "build/asset/asset.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_asset_compile_shaders(void) {

    NYA_Command find_source_shaders_command = {
    .arena     = nya_arena_global,
    .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
    .program   = "find",
    .arguments = { "./assets/shader/source/", "-name", "*.hlsl", },
  };
    NYA_EXPECT(nya_command_run(&find_source_shaders_command));
    nya_assert(find_source_shaders_command.exit_code == 0, "Failed to find source shaders.");
    NYA_ArrayᐸNYA_Stringᐳ* shaders = nya_string_split_lines(nya_arena_global, find_source_shaders_command.stdout_content);

    nya_array_foreach (shaders, shader) {
        if (nya_string_is_empty(shader)) continue;

        NYA_CString source = nya_string_to_cstring(nya_arena_global, shader);
        nya_string_strip_prefix(shader, "./assets/shader/source/");
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

        NYA_Command create_dirs_command = {
        .program   = "mkdir",
        .arguments = {
            "-p",
            "./assets/shader/compiled/",
        },
    };
        NYA_EXPECT(nya_command_run(&create_dirs_command));
        nya_assert(create_dirs_command.exit_code == 0, "Failed to create shader output directories.");

        // compile to DXIL
        NYA_String* compile_to_dxil_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_dxil);
        NYA_BuildRule compile_to_dxil_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_dxil_name),
        .policy      = NYA_BUILD_IF_OUTDATED,
        .input_file  = source,
        .output_file = target_dxil,
        .command = {
            .program = "./vendor/sdl-shadercross/build/shadercross",
            .arguments = {
                source,
                "-o", target_dxil,
                "-s", "hlsl",
                "-d", "dxil",
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_dxil_rule));

        // compile to Metal
        NYA_String* compile_to_metal_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_metal);
        NYA_BuildRule compile_to_metal_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_metal_name),
        .policy      = NYA_BUILD_IF_OUTDATED,
        .input_file  = source,
        .output_file = target_metal,
        .command = {
            .program = "./vendor/sdl-shadercross/build/shadercross",
            .arguments = {
                source,
                "-o", target_metal,
                "-s", "hlsl",
                "-d", "msl",
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_metal_rule));

        // compile to SPIR-V
        NYA_String* compile_to_spirv_name = nya_string_sprintf(nya_arena_global, "%s -> %s", source, target_spirv);
        NYA_BuildRule compile_to_spirv_rule      = {
        .name        = nya_string_to_cstring(nya_arena_global, compile_to_spirv_name),
        .policy      = NYA_BUILD_IF_OUTDATED,
        .input_file  = source,
        .output_file = target_spirv,
        .command = {
            .program = "./vendor/sdl-shadercross/build/shadercross",
            .arguments = {
                source,
                "-o", target_spirv,
                "-s", "hlsl",
                "-d", "spirv",
            },
        },
    };
        NYA_EXPECT(nya_build(&compile_to_spirv_rule));
    }
}

void nya_asset_index(void) {

    NYA_ConstCString asset_directory = "./assets/";
    NYA_ConstCString output_file     = "./assets/assets.h";

    NYA_Arena*  arena  = nya_arena_global;
    NYA_String* result = nya_string_create(arena);

    NYA_Command find_assets_command = {
      .arena     = arena,
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .program   = "find",
      .arguments = {
          asset_directory,
          "-type", "f",
          "-not", "-name", "*.c",
          "-not", "-name", "*.h",
          "-not", "-name", ".keep",
      },
  };
    NYA_EXPECT(nya_command_run(&find_assets_command));
    NYA_ArrayᐸNYA_Stringᐳ* files = nya_string_split_lines(arena, find_assets_command.stdout_content);
    nya_string_extend(result, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(result, "#pragma once\n\n");

    nya_array_foreach (files, file) {
        if (nya_string_is_empty(file)) continue;

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

    NYA_ConstCString asset_directory = "./assets/";
    NYA_ConstCString output_file     = "./assets/assets.c";

    NYA_Arena*  arena               = nya_arena_global;
    NYA_String* result              = nya_string_create(arena);
    NYA_String* header_count_string = nya_string_create(arena);
    NYA_String* header_string       = nya_string_create(arena);
    NYA_String* blob_string         = nya_string_create(arena);

    NYA_Command find_assets_command = {
      .arena     = arena,
      .flags     = NYA_COMMAND_FLAG_OUTPUT_CAPTURE,
      .program   = "find",
      .arguments = {
          asset_directory,
          "-type", "f",
          "-not", "-name", "*.c",
          "-not", "-name", "*.h",
          "-not", "-name", ".keep",
      },
  };
    NYA_EXPECT(nya_command_run(&find_assets_command));
    NYA_ArrayᐸNYA_Stringᐳ* files = nya_string_split_lines(arena, find_assets_command.stdout_content);
    nya_string_extend(result, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(result, "#include \"nyangine/nyangine.h\"\n\n");
    header_count_string = nya_string_sprintf(arena, "static const u64 NYA_ASSET_BLOB_HEADER_COUNT = " FMTu64 ";\n", files->length);
    nya_string_extend(header_string, "static const NYA_AssetBlobHeader NYA_ASSET_BLOB_HEADER[] = {\n");
    nya_string_extend(blob_string, "static const u8 NYA_ASSET_BLOB[] = {\n");

    u64 cursor = 0;
    nya_array_foreach (files, file) {
        if (nya_string_is_empty(file)) continue;

        NYA_String* content = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(file, content));

        nya_string_extend_sprintf(header_string, "  { \"%.*s\", " FMTu64 ", " FMTu64 " },\n", NYA_FMT_STRING_ARG(file), cursor, content->length);

        nya_array_foreach (content, c) {
            NYA_String* new = nya_string_sprintf(arena, "0x%02X,\n", *c);
            nya_string_extend(blob_string, new);
        }

        cursor += content->length;
    }
    nya_string_extend(blob_string, "};\n\n");
    nya_string_extend(header_string, "};\n\n");

    nya_string_extend(result, header_count_string);
    nya_string_extend(result, header_string);
    nya_string_extend(result, blob_string);

    NYA_EXPECT(nya_file_write(output_file, result));

    NYA_Command format_command = {
    .program   = "clang-format",
    .arguments = { "-i", output_file, },
  };
    NYA_EXPECT(nya_command_run(&format_command));
}
