/**
 * The savegame and settings boundary: a file on disk read back as a document and then applied.
 *
 * A save file is the one untrusted input that arrives with the player's own hands on it. It goes
 * through nya_serde_load_file, which is where the version and the checksum are read, and then into
 * nya_settings_from_object, which turns a document into live state without the file having been
 * vouched for by anything.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define FUZZ_TARGET "savegame"

/** Where the input is written before it is read back. Under the build's own scratch, not the save root. */
#define FUZZ_SAVE_DIRECTORY "./.objects"
#define FUZZ_SAVE_PATH      FUZZ_SAVE_DIRECTORY "/nya_fuzz_savegame.nya"

static void fuzz_setup(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    nya_system_settings_init();

    // the directory the input is staged in, created once rather than per case.
    if (!nya_filesystem_exists(FUZZ_SAVE_DIRECTORY)) NYA_EXPECT(nya_filesystem_create_directory(FUZZ_SAVE_DIRECTORY));
}

#define FUZZ_SETUP fuzz_setup

static void fuzz_once(const u8* data, u64 size) {
    NYA_Arena* arena = nya_arena_create(.name = "fuzz_savegame");
    defer      nya_arena_destroy(arena);

    NYA_String* content = nya_string_create_with_capacity(arena, size + 1);
    for (u64 i = 0; i < size; i++) nya_string_push_back(content, data[i]);

    if (!nya_file_write(FUZZ_SAVE_PATH, content).ok) return;

    // every flag a save may have been written with, since the reader branches on each and the file
    // does not say which were used.
    for (u32 flags = 0; flags <= (NYA_SERDE_OBFUSCATE | NYA_SERDE_NO_CHECKSUM); flags++) {
        NYA_Object* object = nullptr;

        if (!nya_serde_load_file(arena, FUZZ_SAVE_PATH, (NYA_SerdeFlags)flags, &object).ok || object == nullptr) continue;

        // the version field a migration would branch on, read the way the loaders read it.
        (void)nya_save_version(object);

        // and what a settings file does next: applied to live state, with nothing between the bytes
        // on disk and the values the game runs with.
        nya_settings_from_object(object);
    }

    // restored, so one case's file cannot decide what the next one reads.
    nya_settings_reset();
}

#include "fuzz/fuzz.h"
