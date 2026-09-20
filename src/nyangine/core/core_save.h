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
 * The subdirectory of the user's data directory saves and logs live under when the app names no app_id:
 * `~/.local/share/<this>` or `%APPDATA%/<this>`. Override with -DNYA_SAVE_APPLICATION=\"my-game\".
 * */
#ifndef NYA_SAVE_APPLICATION
#define NYA_SAVE_APPLICATION "nyangine"
#endif

/*
 * Two kinds of file live under this root and they want opposite treatment, so the pair of flag sets
 * is named here rather than spelled out at each call.
 *
 * Settings are the player's, and a player who opens the file and corrects a line must not be
 * punished for it: written laid out to be read, read without enforcing the checksum, and every entry
 * that cannot be used is named and skipped rather than taken as evidence the file is ruined.
 *
 * Save data is not the player's to edit. It is written obfuscated so a text editor shows nothing
 * useful, and read with the checksum enforced, so a file altered outside the game is refused with
 * NYA_ERROR_CORRUPT instead of loading a world that half makes sense. The checksum is an integrity
 * check and not a signature: it catches a torn write, a truncated cloud sync and a casual edit, and
 * it is not meant to stop someone determined to cheat in their own single player game.
 */

/** For a file the player is invited to edit. See nya_settings_save. */
#define NYA_SAVE_FLAGS_EDITABLE ((NYA_SerdeFlags)(NYA_SERDE_PRETTY | NYA_SERDE_NO_CHECKSUM))

/** For a file the game owns. See nya_scene_save. */
#define NYA_SAVE_FLAGS_DATA ((NYA_SerdeFlags)NYA_SERDE_OBFUSCATE)

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
     * Absolute path of the save directory, or null when there is none. Normal on a machine without a writable
     * home: every function here then answers NOT_FOUND instead of writing somewhere unexpected.
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
NYA_API NYA_Error nya_system_save_init(void) __attr_no_discard;

/**
 * The user data subdirectory this app writes to: NYA_AppOptions.app_id, else NYA_SAVE_APPLICATION. What
 * players are told to delete and what a Steam Auto-Cloud rule names.
 * */
NYA_API NYA_ConstCString nya_save_application(void) __attr_no_discard;
NYA_API void      nya_system_save_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * PATHS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The absolute save root, or null. For showing players where files are and for Auto-Cloud rules; build paths
 * with nya_save_path, which gets separators right.
 * */
NYA_API NYA_ConstCString nya_save_root(void) __attr_no_discard;

/**
 * The absolute path of `relative` under the save root. Null without a root, and null when `relative` tries to
 * escape it with a leading separator or `..`, which is refused rather than normalised.
 * */
NYA_API NYA_String* nya_save_path(NYA_Arena* arena, NYA_ConstCString relative) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * OBJECTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Writes an object to `relative` atomically, creating parent directories. The extension picks the format.
 * `flags` goes to serde: NYA_SERDE_PRETTY for hand editing, NYA_SERDE_OBFUSCATE to resist a text editor.
 * */
NYA_API NYA_Error nya_save_write(NYA_ConstCString relative, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

/**
 * Reads an object from `relative`, allocated from `arena`. NYA_ERROR_NOT_FOUND when the file is missing, the
 * normal first-run case; fall back to defaults for that one.
 * */
NYA_API NYA_Error nya_save_read(NYA_Arena* arena, NYA_ConstCString relative, NYA_SerdeFlags flags, OUT NYA_Object** out_object) __attr_no_discard;

NYA_API b8 nya_save_exists(NYA_ConstCString relative) __attr_no_discard;

/** Harmless on a missing file, so deleting a slot twice is safe. */
NYA_API NYA_Error nya_save_delete(NYA_ConstCString relative) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * DATABASES
 * ─────────────────────────────────────────────────────────
 */

#ifdef NYA_PLUGIN_SQLITE

/**
 * Opens a SQLite database at `relative`, creating it and its directory. Same root, so it syncs under the same
 * Auto-Cloud rule. The rest is plugins/sqlite/sql.h.
 * */
NYA_API NYA_Error nya_save_database_open(NYA_Arena* arena, NYA_ConstCString relative, OUT NYA_Database** out_database) __attr_no_discard;

#endif // NYA_PLUGIN_SQLITE

/*
 * ─────────────────────────────────────────────────────────
 * VERSIONING
 * ─────────────────────────────────────────────────────────
 */

/**
 * The key every saved object should carry. Formats change and old files do not, and a written version is the
 * only cheap way to tell them apart.
 *
 * ```c
 * nya_object_set(save, NYA_SAVE_VERSION_KEY, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 3 });
 * ```
 * */
#define NYA_SAVE_VERSION_KEY "save_version"

/**
 * The version an object declares, or zero when it declares none, which is how files from before versioning
 * read. The game decides whether zero is loadable.
 * */
NYA_API u32 nya_save_version(const NYA_Object* object) __attr_no_discard;
