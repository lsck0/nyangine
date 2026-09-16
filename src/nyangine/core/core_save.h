/**
 * @file core_save.h
 *
 * ```c
 * NYA_Object* progress = nya_object_create(arena);
 * nya_object_set(progress, "depth", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 41 });
 *
 * NYA_EXPECT(nya_save_write("saves/slot0.nya", progress, NYA_SERDE_PRETTY));
 *
 * NYA_Object* loaded = nullptr;
 * if (nya_save_read(arena, "saves/slot0.nya", &loaded).ok) { ... }
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/serde/serde_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The subdirectory of the user's data directory everything lives under. Player-visible — it's what
 * appears in `~/.local/share/<this>` and `%APPDATA%/<this>`, the directory someone is told to delete
 * when their config is broken, and the one a Steam Auto-Cloud rule names. Override with
 * -DNYA_SAVE_APPLICATION=\"my-game\" per game.
 * */
#ifndef NYA_SAVE_APPLICATION
#define NYA_SAVE_APPLICATION "nyangine"
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SaveSystem NYA_SaveSystem;

struct NYA_SaveSystem {
    /** Owns `root`. Its own arena because the root is read for the life of the process. */
    NYA_Arena* allocator;

    /**
     * Absolute path of the save directory, or null when there is none — a normal state, not a
     * failure: a machine with no writable home directory still runs the game, and every function
     * here answers NOT_FOUND instead of writing somewhere unexpected.
     * */
    NYA_CString root;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Resolves the save root and creates it. Called by nya_app_init before the settings system comes up.
 * */
NYA_API NYA_Error nya_system_save_init(void);
NYA_API void      nya_system_save_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * PATHS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The absolute path of the save root, or null when it could not be created. For showing a player
 * where their files are and for writing a Steam Auto-Cloud rule against — not for building paths by
 * hand; nya_save_path does that and gets the separator right on both platforms.
 * */
NYA_API NYA_ConstCString nya_save_root(void) __attr_no_discard;

/**
 * The absolute path of `relative` under the save root. Null when there is no root, and null when
 * `relative` tries to escape one — a leading separator or a `..` segment is refused rather than
 * normalised, since quietly resolving it is how a save system writes outside the directory Steam is
 * syncing.
 * */
NYA_API NYA_String* nya_save_path(NYA_Arena* arena, NYA_ConstCString relative) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * OBJECTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Writes an object to `relative`, atomically. Parent directories are created. Format comes from the
 * extension; see the file header. `flags` is passed to serde — NYA_SERDE_PRETTY for a human-editable
 * file, NYA_SERDE_OBFUSCATE for one that should resist a text editor.
 * */
NYA_API NYA_Error nya_save_write(NYA_ConstCString relative, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

/**
 * Reads an object from `relative`. Everything in the tree comes from `arena`. Returns
 * NYA_ERROR_NOT_FOUND when the file isn't there — the ordinary case on a first run, not a problem;
 * check for it and fall back to defaults rather than treating every error the same.
 * */
NYA_API NYA_Error nya_save_read(NYA_Arena* arena, NYA_ConstCString relative, NYA_SerdeFlags flags, OUT NYA_Object** out_object) __attr_no_discard;

NYA_API b8 nya_save_exists(NYA_ConstCString relative) __attr_no_discard;

/** Harmless on a file that is not there, which is what makes "delete this slot" safe to call twice. */
NYA_API NYA_Error nya_save_delete(NYA_ConstCString relative) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * DATABASES
 * ─────────────────────────────────────────────────────────
 */

#ifdef NYA_PLUGIN_SQLITE

/**
 * Opens a SQLite database at `relative`, creating it and its directory if needed. Same root as
 * everything else, so it syncs under the same Auto-Cloud rule as the settings file; everything but
 * *where* is plugins/sqlite/sql.h's.
 * */
NYA_API NYA_Error nya_save_database_open(NYA_Arena* arena, NYA_ConstCString relative, OUT NYA_Database** out_database) __attr_no_discard;

#endif // NYA_PLUGIN_SQLITE

/*
 * ─────────────────────────────────────────────────────────
 * VERSIONING
 * ─────────────────────────────────────────────────────────
 */

/**
 * The key every object written through here should carry. Save formats change; the files already on
 * disk don't, and a written-down version is the only cheap way to tell which shape a file is —
 * guessing from which keys are present gets less reliable with every release.
 *
 * ```c
 * nya_object_set(save, NYA_SAVE_VERSION_KEY, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 3 });
 * ```
 * */
#define NYA_SAVE_VERSION_KEY "save_version"

/**
 * The version an object says it is, or zero when it doesn't say — not an error: a file written
 * before versioning existed is version zero, and the game decides whether that's loadable.
 * */
NYA_API u32 nya_save_version(const NYA_Object* object) __attr_no_discard;
