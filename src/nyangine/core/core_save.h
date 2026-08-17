/**
 * @file core_save.h
 *
 * Where a game writes: settings, progress, logs, anything that has to survive the process.
 *
 * One directory, per user, per application, decided once at startup and reached through
 * nya_save_path. Everything below it is a *relative* path, and that is the whole design:
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
 *
 * ## Why one root, and why relative paths
 *
 * Because that is exactly the shape Steam Cloud's Auto-Cloud wants, and Auto-Cloud is the version of
 * cloud saves a game can have without writing any Steam code at all: the store page names a root
 * directory and a set of globs, and Steam syncs whatever matches. What breaks it is a save file that
 * lives outside that root, or one that stores an absolute path *inside itself* — the second machine
 * has a different home directory, and the file loads and then points nowhere.
 *
 * So: nothing here takes an absolute path, and nothing written through here should contain one.
 * Everything a game persists goes under nya_save_root, and a Steam build points Auto-Cloud at
 * `%WinAppDataLocal%/<app>` and `$XDG_DATA_HOME/<app>` — the two nya_filesystem_user_data_directory
 * already resolves to. The API surface stays identical whether the build is on Steam or not, which
 * is what keeps "it works on Steam" from being a different code path to test.
 *
 * The other half, ISteamRemoteStorage, is a real file API with quotas and conflict resolution, and
 * is what a game needs once saves get big or a player can have two machines writing at once. It is
 * not here; steam.h is still a stub. Auto-Cloud is what this shape buys today, and the day
 * ISteamRemoteStorage lands it can be implemented behind these five functions rather than beside
 * them.
 *
 * ## Which format
 *
 * Whatever the extension says, because nya_serde_save_file decides that way: `.json` writes JSON,
 * anything else writes the native `nya` format. Reading sniffs the bytes instead of trusting the
 * name, so a file's format can change without anything having to be told.
 *
 * - **`.nya` for anything a human should be able to read or edit.** Typed, indented under
 *   NYA_SERDE_PRETTY, and — the part that matters — its checksum covers the object tree rather than
 *   the bytes, so reformatting a settings file by hand does not invalidate it. This is what settings
 *   use.
 * - **`.json` for anything another program reads.** Lossy about integer widths; see serde_json.h.
 * - **NYA_SERDE_OBFUSCATE for a save a player should not casually edit.** Obfuscation, not
 *   encryption — the key is in the binary and it stops a text editor, not a determined player.
 * - **SQLite for progress that is queried rather than loaded whole.** nya_save_database_open puts
 *   the database under the same root, so it syncs with everything else. A run history, an unlock
 *   table, a per-seed leaderboard: things where "load the entire file to answer one question" is the
 *   wrong shape. See plugins/sqlite/sql.h.
 *
 * ## Writes are atomic
 *
 * nya_save_write goes to a temporary file beside the target and renames over it. A rename within one
 * directory is atomic on every filesystem this runs on, so a crash — or a machine losing power —
 * during a save leaves either the old file or the new one, never a truncated one. Writing in place
 * is how a player loses forty hours to a power cut during an autosave.
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
 * The subdirectory of the user's data directory everything lives under.
 *
 * Override with -DNYA_SAVE_APPLICATION=\"my-game\" per game. It is what appears in
 * `~/.local/share/<this>` and `%APPDATA%/<this>`, so it is player-visible and worth naming properly:
 * this is the directory someone is told to delete when their config is broken, and the one a Steam
 * Auto-Cloud rule names.
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
     * Absolute path of the save directory, or null when there is none.
     *
     * Null is a normal state rather than a failure to handle once: a machine with no writable home
     * directory still runs the game, and every function here answers NOT_FOUND instead of writing
     * somewhere a player would never find.
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
 *
 * Failing here is not fatal and does not stop the application: a machine with no writable home
 * directory can still play, it just cannot save. Every function below then fails with NOT_FOUND
 * rather than writing somewhere unexpected, which is the failure mode to prefer — a game that
 * silently saves next to its executable is a game that loses saves on the next update.
 * */
NYA_API NYA_Error nya_system_save_init(void);
NYA_API void      nya_system_save_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * PATHS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The absolute path of the save root, or null when it could not be created.
 *
 * For showing a player where their files are, and for a Steam Auto-Cloud rule to be written against.
 * Not for building paths by hand — nya_save_path does that, and does the separator right on both
 * platforms.
 * */
NYA_API NYA_ConstCString nya_save_root(void) __attr_no_discard;

/**
 * The absolute path of `relative` under the save root.
 *
 * Null when there is no root, and null when `relative` tries to escape one — a leading separator or
 * a `..` segment is refused rather than normalised, because the only thing that produces one is a
 * bug or a filename that came from somewhere untrusted, and quietly resolving it is how a save
 * system writes outside the directory Steam is syncing.
 * */
NYA_API NYA_String* nya_save_path(NYA_Arena* arena, NYA_ConstCString relative) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * OBJECTS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Writes an object to `relative`, atomically. Parent directories are created.
 *
 * The format comes from the extension; see the file header. `flags` is passed through to serde, so
 * NYA_SERDE_PRETTY is what a human-editable file wants and NYA_SERDE_OBFUSCATE is what a save that
 * should resist a text editor wants.
 *
 * Atomic in the sense that matters: the bytes land in a temporary file in the same directory and are
 * renamed over the target, so a reader either sees the whole previous file or the whole new one. The
 * temporary is removed on failure.
 * */
NYA_API NYA_Error nya_save_write(NYA_ConstCString relative, const NYA_Object* object, NYA_SerdeFlags flags) __attr_no_discard;

/**
 * Reads an object from `relative`. Everything in the tree comes from `arena`.
 *
 * NYA_ERROR_NOT_FOUND when the file is not there, which is the ordinary answer on a first run rather
 * than a problem — check for it and fall back to defaults instead of treating every error the same.
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
 * Opens a SQLite database at `relative`, creating it and its directory if needed.
 *
 * The same root as everything else, so a database is synced by the same Auto-Cloud rule as the
 * settings file rather than needing its own. Everything else is plugins/sqlite/sql.h's: this only
 * decides *where*.
 *
 * For progress that is queried rather than loaded whole — a run history, an unlock table, a per-seed
 * leaderboard. An object tree is the better answer for anything read in one piece, because a save
 * that is always loaded entirely gains nothing from a query engine and costs a schema.
 *
 * **Not per frame.** The API is synchronous; treat opening one and reading from it as a load
 * boundary operation. And close it before the arena it came from dies.
 * */
NYA_API NYA_Error nya_save_database_open(NYA_Arena* arena, NYA_ConstCString relative, OUT NYA_Database** out_database) __attr_no_discard;

#endif // NYA_PLUGIN_SQLITE

/*
 * ─────────────────────────────────────────────────────────
 * VERSIONING
 * ─────────────────────────────────────────────────────────
 */

/**
 * The key every object written through here should carry, and the reason to carry it.
 *
 * A save format changes. When it does, the files already on players' disks do not, and the only
 * thing that makes them loadable is having written down which shape they are. Reading a version out
 * of a file is cheap; guessing it from which keys happen to be present is not, and gets less
 * possible with every release.
 *
 * ```c
 * nya_object_set(save, NYA_SAVE_VERSION_KEY, (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 3 });
 * ```
 *
 * There is deliberately no migration machinery here. What a version *means* is the game's, and a
 * framework for it would be a framework for exactly one game's history.
 * */
#define NYA_SAVE_VERSION_KEY "save_version"

/**
 * The version an object says it is, or zero when it does not say.
 *
 * Zero is the useful answer for a file written before versioning existed, which is why it is not an
 * error: a save with no version is version zero, and the game decides whether that is loadable.
 * */
NYA_API u32 nya_save_version(const NYA_Object* object) __attr_no_discard;
