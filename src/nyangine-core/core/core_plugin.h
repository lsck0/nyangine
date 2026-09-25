/**
 * @file core_plugin.h
 *
 * Lua plugins: a directory on disk becomes a manifest, a permission set and a VM of its own, and the
 * host never learns anything about any particular one. Adding a plugin is dropping a folder in
 * `plugins/`; it takes no edit anywhere in the engine or the game.
 *
 * ```
 * plugins/<name>/
 *   manifest.nya      who wrote it, what it needs, what it wants to touch
 *   main.lua          the entry point, run last
 *   src/           every .lua in it runs first, in name order, into the same VM
 *   assets/           the plugin's own files; see ASSETS below
 * ```
 *
 * Everything in the module:
 *   nya_plugin_load_all              discovers NYA_PLUGIN_DIRECTORY and loads every plugin under it
 *   nya_plugin_load                  loads one plugin directory, manifest first
 *   nya_plugin_unload                the partner, by name, idempotent
 *   nya_plugin_unload_all            the partner of load_all, in reverse load order
 *   nya_plugin_enable / _disable     whether its hooks run; the VM and its state are untouched
 *   nya_plugin_is_enabled            what those two last set
 *   nya_plugin_count / _at / _find   what is loaded
 *   nya_plugin_current               whose code is running right now, or null
 *   nya_plugin_call                  calls one of a plugin's Lua functions, errors attributed to it
 *   nya_plugin_stats                 its systems, frame time and held bytes, from the system registry
 *   nya_plugin_error                 reports a failure in the plugin's own voice, at its author
 *   nya_plugin_qualify               "<plugin>:<name>", the one place an identity scopes a name
 *   nya_plugin_permissions_granted   the mask this build compiled in, whoever asks
 *   nya_plugin_permission_name       one permission as the text a manifest spells it with
 *   nya_plugin_manifest_load         parses one manifest.nya and nothing else
 *
 * ```c
 * // the game, once, after its own systems are registered:
 * NYA_EXPECT(nya_plugin_load_all(), "while loading plugins");
 *
 * // and from then on the registry runs them. To read what one costs:
 * NYA_SystemOwnerStats stats = nya_plugin_stats("hello");
 * nya_log_info("hello: %.3f ms, %llu bytes", nya_time_ns_to_ms(stats.time_ns), stats.memory_bytes);
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * PERMISSIONS, AND WHAT THEY DO NOT GUARANTEE
 * ─────────────────────────────────────────────────────────
 *
 * A plugin is arbitrary code written by somebody else. Treat it as hostile input, because that is what
 * it is.
 *
 * The game decides once, at compile time, what any plugin may reach: NYA_PLUGIN_PERMISSION_PROFILE is
 * a `-D` on the build command line and nothing at runtime can widen it. A plugin states in its
 * manifest what it wants; a plugin that wants more than the build grants is refused at load with a
 * message naming every permission it asked for and did not get. There is no prompt, no override file
 * and no per-plugin setting, because every one of those is a way for the answer to end up being yes.
 *
 * The mechanism is which bindings exist. Each plugin gets a LuaJIT VM of its own, opened with
 * `restricted` (no `io`, `os`, `package`, `ffi`, `debug`) and with only the `nya.*` functions its
 * permissions allow; see nya_lua_open_engine_permitted. A denied call is not a call that refuses, it
 * is a name that was never put in the VM. So a script cannot reach it by holding it wrong, by finding
 * it on another table, or by asking twice.
 *
 * **What this does guarantee.** A plugin cannot name an engine function it was not granted, cannot
 * reach the C ABI through `ffi`, cannot open a file or spawn a process through `os`/`io`, cannot see
 * another plugin's globals or functions, and cannot register anything under a name another plugin
 * could answer to.
 *
 * **What this does not guarantee, and would need an OS to.** It is not a sandbox and this header will
 * not call it one:
 *
 * - **No resource bound.** `while true do end` hangs the frame; a table built in a loop grows LuaJIT's
 *   heap until the process is out of memory. There is no instruction budget and no heap ceiling. What
 *   exists is measurement, not enforcement: nya_plugin_stats says which plugin is spending the frame
 *   and holding the memory, and a human or the game decides.
 * - **No protection from a bug in a binding.** A granted binding is C, and a C function that
 *   mishandles its arguments is exploitable whatever the permission said. The bindings are generated
 *   from one template for exactly this reason, so there is one marshaller to audit rather than a
 *   hundred.
 * - **Granularity is the permission, not the object.** NYA_PLUGIN_PERMISSION_ENTITIES is every entity,
 *   not the ones a plugin created. A plugin that may spawn may also despawn the player.
 * - **Nothing is verified.** No signature, no checksum, no reproducible build. A plugin is whatever is
 *   in the folder, and a folder can be replaced by anything with write access to it.
 * - **The permission is checked once, at load.** Nothing re-checks per call, because there is nothing
 *   to check: the function is either in the VM or it is not.
 *
 * Anything stronger than this needs a separate process with a seccomp filter or a job object, which is
 * a real answer and a different design. It is not pretended here.
 *
 * ─────────────────────────────────────────────────────────
 * NAMESPACING
 * ─────────────────────────────────────────────────────────
 *
 * Two plugins both defining `spawn` is the normal case, not the exception, and it is answered
 * structurally rather than by asking authors to prefix things:
 *
 * 1. **A VM per plugin.** A plugin's globals, functions and tables are its own. Two plugins defining
 *    `spawn` never meet, and neither can read or overwrite the other's.
 * 2. **The identity scopes every registration.** Anything a plugin puts into an engine registry goes
 *    through nya_plugin_qualify and comes out `<plugin>:<name>`, so `hello:spawn` and `world:spawn`
 *    are two systems. The plugin does not choose to do this and cannot skip it: the host knows which
 *    plugin is calling (nya_plugin_current) and qualifies the name before the registry sees it.
 * 3. **The directory is the identity.** A manifest whose `name` is not its own folder's name is
 *    refused, and a second plugin claiming a loaded name is refused. So the name is unique by
 *    construction, and it is the name the user installed.
 *
 * ─────────────────────────────────────────────────────────
 * TRACING AND ERRORS
 * ─────────────────────────────────────────────────────────
 *
 * Each plugin is one entry in the system registry, owned by NYA_SYSTEM_OWNER_PLUGIN under its own
 * name, reporting its VM's heap through NYA_SystemEntry.memory_bytes. So the registry's per-owner
 * accounting answers "what is this plugin costing me" with no bookkeeping of its own, and the debug
 * overlay's owner table lists plugins beside the engine and the game.
 *
 * An error inside a plugin is reported as the plugin's, never as the engine's: the log line names the
 * plugin, its version and its author, and points at the repository from the manifest. A plugin that
 * raises NYA_PLUGIN_ERROR_MAX times is disabled rather than allowed to fill the log, and says so.
 *
 * ─────────────────────────────────────────────────────────
 * ASSETS
 * ─────────────────────────────────────────────────────────
 *
 * `plugins/<name>/assets/` is the plugin's own directory and is *not* indexed by the asset system: the
 * asset handles are generated at build time from the tree and a plugin arrives after that. A plugin
 * with NYA_PLUGIN_PERMISSION_FILESYSTEM reads and writes inside its own directory through `nya.file.*`
 * and nothing else; without it the directory is unreachable from Lua. Handing plugin assets to the
 * asset system needs a runtime asset root, which does not exist yet.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_system.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where nya_plugin_load_all looks. Relative to the working directory, like every other asset root. */
#ifndef NYA_PLUGIN_DIRECTORY
#define NYA_PLUGIN_DIRECTORY "./plugins"
#endif

/**
 * How many plugins may be loaded at once.
 *
 * Each one costs a LuaJIT state (about 60 KB before the plugin allocates anything), an arena and a
 * system registry entry, and the registry accounts for NYA_SYSTEM_OWNER_MAX owners of which the engine
 * and the game take two. Eight is more than a game ships with and leaves the owner table room. Refused
 * with an error past it, never grown.
 * */
#ifndef NYA_PLUGIN_MAX
#define NYA_PLUGIN_MAX 8
#endif

/**
 * Errors one plugin may raise before it is disabled.
 *
 * A plugin whose `on_tick` throws throws every tick, so the choice is between a log nobody can read
 * and stopping it. Eight is enough to see the same failure a few times with its context and short
 * enough that a broken plugin costs one line of log per run.
 * */
#ifndef NYA_PLUGIN_ERROR_MAX
#define NYA_PLUGIN_ERROR_MAX 8
#endif

/** Names, versions and prose in a manifest, terminator included. A longer value is truncated, not refused. */
#define NYA_PLUGIN_NAME_MAX        64
#define NYA_PLUGIN_VERSION_MAX     32
#define NYA_PLUGIN_AUTHOR_MAX      96
#define NYA_PLUGIN_LICENSE_MAX     48
#define NYA_PLUGIN_DESCRIPTION_MAX 192
#define NYA_PLUGIN_URL_MAX         192

/** A path this module builds: `plugins/<name>/src/<file>.lua` plus room for a deep working directory. */
#define NYA_PLUGIN_PATH_MAX 512

/**
 * Dependencies and conflicts one manifest may name. A plugin needing more than eight others is a
 * distribution problem rather than a loader problem; the extras are reported and ignored.
 * */
#define NYA_PLUGIN_DEPENDENCY_MAX 8

/** Files under `src/` one plugin may have. Past it the rest are reported and skipped. */
#define NYA_PLUGIN_SOURCE_MAX 32

/**
 * Directories discovery will look at, which is not NYA_PLUGIN_MAX: a refused plugin takes no slot, so
 * a folder holding twenty candidates of which six load is a normal folder. Four times the load limit,
 * and the array it bounds is on the stack for the length of one call.
 * */
#define NYA_PLUGIN_CANDIDATE_MAX (NYA_PLUGIN_MAX * 4)

/** A qualified name: `<plugin>:<name>`, terminator included. */
#define NYA_PLUGIN_QUALIFIED_MAX (NYA_PLUGIN_NAME_MAX + NYA_PLUGIN_NAME_MAX + 1)

/* The layout, in one place, so a rename is one edit. */
#define NYA_PLUGIN_MANIFEST_FILE     "manifest.nya"
#define NYA_PLUGIN_ENTRY_FILE        "main.lua"
#define NYA_PLUGIN_SOURCE_DIRECTORY  "src"
#define NYA_PLUGIN_ASSET_DIRECTORY   "assets"
#define NYA_PLUGIN_SOURCE_EXTENSION  ".lua"

/* The hooks a plugin may define, all optional. A plugin defining none is legal and does nothing. */
#define NYA_PLUGIN_HOOK_LOAD   "on_load"
#define NYA_PLUGIN_HOOK_UNLOAD "on_unload"
#define NYA_PLUGIN_HOOK_FRAME  "on_frame"
#define NYA_PLUGIN_HOOK_TICK   "on_tick"
#define NYA_PLUGIN_HOOK_RENDER "on_render"

/*
 * ─────────────────────────────────────────────────────────
 * THE COMPILE TIME GRANT
 * ─────────────────────────────────────────────────────────
 *
 * The game's one decision. Set it on the build command line (`-DNYA_PLUGIN_PERMISSION_PROFILE=2`) and
 * it is fixed for the binary: there is no runtime path that widens it, on purpose.
 */

/** Nothing but logging and the clock. What a build that wants plugins present and inert compiles. */
#define NYA_PLUGIN_PERMISSION_PROFILE_LOCKED 0

/** Draw and read, change nothing: UI and input state, no key bindings, no entities. */
#define NYA_PLUGIN_PERMISSION_PROFILE_UI 1

/** UI, key bindings, entities, audio and the engine's assets. No filesystem, no network. */
#define NYA_PLUGIN_PERMISSION_PROFILE_GAMEPLAY 2

/** Everything, the plugin's own directory and the network included. */
#define NYA_PLUGIN_PERMISSION_PROFILE_ALL 3

/**
 * Conservative by default: a build that says nothing gets plugins that can draw and read and change
 * nothing. A game that wants more says so, and says it once.
 * */
#ifndef NYA_PLUGIN_PERMISSION_PROFILE
#define NYA_PLUGIN_PERMISSION_PROFILE NYA_PLUGIN_PERMISSION_PROFILE_UI
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_PluginDependency NYA_PluginDependency;
typedef struct NYA_PluginManifest   NYA_PluginManifest;
typedef struct NYA_Plugin           NYA_Plugin;

/* Before the typedef: a bare `typedef enum X X;` declares X with no underlying type, and the fixed
 * one below would then be a redeclaration that disagrees. lua.h forward declares the same pair. */
enum NYA_PluginPermission : u64;
typedef enum NYA_PluginPermission NYA_PluginPermission;

/**
 * What a plugin may touch. A manifest writes these as a list of names, so adding one changes which
 * names appear in a file rather than the meaning of a number already in one.
 *
 * The underlying type is fixed because lua.h forward declares this enum to take it by value without
 * depending on core; see the note there.
 *
 * @reflect
 * */
enum NYA_PluginPermission : u64 {
    /** Logging and the frame clock, which every plugin gets whether it asks or not. */
    NYA_PLUGIN_PERMISSION_NONE = 0,

    /** Draw widgets through the `nya_ui_*` calls. */
    NYA_PLUGIN_PERMISSION_UI = 1ULL << 0,

    /** Read what the player is pressing. Reading is not binding; see KEYBINDING. */
    NYA_PLUGIN_PERMISSION_INPUT = 1ULL << 1,

    /**
     * Bind and rebind keys. Separate from INPUT because it is the one that takes something away from
     * the player: a plugin that may bind can take a key the game or another plugin wanted.
     * */
    NYA_PLUGIN_PERMISSION_KEYBINDING = 1ULL << 2,

    /** Spawn, despawn and move entities. Every entity, not only the ones it made. */
    NYA_PLUGIN_PERMISSION_ENTITIES = 1ULL << 3,

    /** Play sounds and read or set volumes. */
    NYA_PLUGIN_PERMISSION_AUDIO = 1ULL << 4,

    /** Ask the asset system for what the game shipped. */
    NYA_PLUGIN_PERMISSION_ASSETS = 1ULL << 5,

    /** Read and write inside its own `plugins/<name>/` directory, and nowhere else. */
    NYA_PLUGIN_PERMISSION_FILESYSTEM = 1ULL << 6,

    /**
     * Reach the network. No binding carries this yet, so granting it grants nothing today; it is here
     * so a manifest asking for it is refused by a build that does not allow it, rather than accepted
     * and then surprising somebody later.
     * */
    NYA_PLUGIN_PERMISSION_NETWORK = 1ULL << 7,
};

/**
 * One entry of a manifest's `dependencies` or `conflicts` list.
 *
 * @reflect
 * */
struct NYA_PluginDependency {
    /** The other plugin's name, as its own directory spells it. An empty name ends the list. */
    char name[NYA_PLUGIN_NAME_MAX];

    /**
     * The lowest version of it that will do, as `major.minor.patch`. Empty means any version.
     * Meaningless in `conflicts`, where any version conflicts.
     * */
    char version[NYA_PLUGIN_VERSION_MAX];
};

/**
 * `manifest.nya`, parsed. Every field is read through the reflection tables by
 * nya_reflect_from_object, so this struct is the schema and there is no second copy of it to drift.
 *
 * ```
 * nya 2 0
 * {
 *     name: string "hello";
 *     version: string "0.1.0";
 *     engine_version: string "0.0.0";
 *     author: string "someone";
 *     license: string "MIT";
 *     description: string "says hello";
 *     repository: string "https://github.com/someone/hello";
 *     permissions: string[] ["NYA_PLUGIN_PERMISSION_UI"];
 *     dependencies: array[] [object { name: string "other"; version: string "1.0.0"; }];
 * }
 * ```
 *
 * @reflect
 * */
struct NYA_PluginManifest {
    /** Required, and must equal the directory the manifest was found in. See NAMESPACING. */
    char name[NYA_PLUGIN_NAME_MAX];

    /** Required. `major.minor.patch`; what a repository index and an update check compare. */
    char version[NYA_PLUGIN_VERSION_MAX];

    /**
     * The lowest engine version this plugin runs on. A plugin asking for a newer engine than the one
     * running is refused, naming both. Empty skips the check.
     * */
    char engine_version[NYA_PLUGIN_VERSION_MAX];

    /** Who to tell when it breaks. Printed in every error attributed to this plugin. */
    char author[NYA_PLUGIN_AUTHOR_MAX];

    /** An SPDX identifier by convention. Recorded and shown, never enforced. */
    char license[NYA_PLUGIN_LICENSE_MAX];

    char description[NYA_PLUGIN_DESCRIPTION_MAX];

    /** The git URL it updates itself from, and the second half of an error message. */
    char repository[NYA_PLUGIN_URL_MAX];

    /**
     * What it wants to touch. Refused at load if this asks for more than the build granted; see
     * PERMISSIONS above. Absent means NYA_PLUGIN_PERMISSION_NONE, which is a legal and useless plugin.
     * */
    NYA_PluginPermission permissions;

    /** Plugins that must be loaded first. Load order follows them. */
    NYA_PluginDependency dependencies[NYA_PLUGIN_DEPENDENCY_MAX];

    /** Plugins this one refuses to run beside, whichever of the two arrives second. */
    NYA_PluginDependency conflicts[NYA_PLUGIN_DEPENDENCY_MAX];
};

/**
 * One loaded plugin. Read it, do not write it: the host owns every field, and the VM behind `_state`
 * is the one thing here that is not data.
 * */
struct NYA_Plugin {
    NYA_PluginManifest manifest;

    /** `plugins/<name>`, as it was found. What the filesystem bindings resolve against. */
    char directory[NYA_PLUGIN_PATH_MAX];

    /**
     * What it actually got, which is its request intersected with the build's grant. Equal to
     * `manifest.permissions` for every loaded plugin, because a plugin that wanted more never loads;
     * kept separate so the two are never confused at a call site.
     * */
    NYA_PluginPermission permissions;

    /** Whether its hooks run. See nya_plugin_enable. */
    b8 enabled;

    /** How many errors it has raised. At NYA_PLUGIN_ERROR_MAX it is disabled and said so. */
    u32 error_count;

    /** Which of NYA_PLUGIN_HOOK_* it defines, so a frame does not ask Lua for a function it has not got. */
    b8 has_frame;
    b8 has_tick;
    b8 has_render;

    /**
     * The VM and the arena behind it. Opaque on purpose: it is the only part of a plugin that is not
     * plain data, and a caller reaching into it would be reaching into another plugin's world.
     * */
    void* _state;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PERMISSIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Everything this build lets a plugin have, derived from NYA_PLUGIN_PERMISSION_PROFILE at compile time.
 *
 * A function rather than a macro so a caller cannot accidentally test against a different profile than
 * the one the engine was compiled with.
 * */
NYA_API NYA_PluginPermission nya_plugin_permissions_granted(void) __attr_no_discard;

/**
 * One permission as a manifest spells it: "NYA_PLUGIN_PERMISSION_UI". Null for a value that is not a
 * single permission, since that is a programming mistake rather than a state.
 *
 * Reads the reflection table, so it cannot disagree with what a manifest is allowed to say.
 * */
NYA_API NYA_ConstCString nya_plugin_permission_name(NYA_PluginPermission permission) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE MANIFEST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Parses `<directory>/manifest.nya` into `out_manifest` and nothing else: no VM, no registration, no
 * side effect. What a repository browser or a `--list-plugins` wants.
 *
 * Every key the file holds that the schema does not, and every value of the wrong type, is reported by
 * name and the file is refused. A manifest is untrusted input and a typo in a permission name must not
 * read as a permission not asked for.
 * */
NYA_API NYA_Error nya_plugin_manifest_load(NYA_ConstCString directory, OUT NYA_PluginManifest* out_manifest) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LOADING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Loads every plugin directory under NYA_PLUGIN_DIRECTORY, dependencies before dependents.
 *
 * A directory with no manifest is not a plugin and is skipped in silence. A plugin that refuses to
 * load — a bad manifest, a permission the build does not grant, a missing dependency, a conflict, a
 * syntax error in its Lua — is reported and the rest carry on: one bad plugin is not a reason for the
 * game not to start. Returns an error only for something the host itself could not do, such as a
 * plugins directory that exists and cannot be read.
 *
 * Missing directory is success with nothing loaded, since not having plugins is the normal case.
 * */
NYA_API NYA_Error nya_plugin_load_all(void) __attr_no_discard;

/**
 * Loads the one plugin in `directory`: manifest, permission check, VM, `src/` (every `.lua` in it) in name order,
 * `main.lua`, `on_load`, and a system registry entry under the plugin's own name.
 *
 * Every failure unwinds completely — the VM is closed, the arena freed, the registry entry removed —
 * so a refused plugin leaves nothing of itself behind.
 * */
NYA_API NYA_Error nya_plugin_load(NYA_ConstCString directory) __attr_no_discard;

/**
 * Unloads the plugin called `name`: `on_unload`, then its systems, then its VM and its arena.
 *
 * Idempotent, and a no-op for a name that was never loaded. Anything the plugin registered under a
 * qualified name goes with it, which is the other half of why registrations are qualified.
 * */
NYA_API void nya_plugin_unload(NYA_ConstCString name);

/** Unloads everything, in reverse load order, so a dependency outlives its dependents. */
NYA_API void nya_plugin_unload_all(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENABLING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Whether the plugin's per-frame hooks run. Its VM, its globals and everything it allocated are
 * untouched either way, so a disabled plugin picks up exactly where it stopped.
 *
 * This is what a settings screen toggles, and what the host does to a plugin that keeps failing.
 * Neither asserts on a name that was never loaded: a stale entry in a settings file is an operating
 * error, not a programming one.
 * */
NYA_API void nya_plugin_enable(NYA_ConstCString name);
NYA_API void nya_plugin_disable(NYA_ConstCString name);

/** What those two last set. False for a name that is not loaded. */
NYA_API b8 nya_plugin_is_enabled(NYA_ConstCString name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How many plugins are loaded. */
NYA_API u32 nya_plugin_count(void) __attr_no_discard;

/** The plugin at `index`, in load order. Never null: `index` is asserted against the count. */
NYA_API const NYA_Plugin* nya_plugin_at(u32 index) __attr_no_discard;

/** The plugin called `name`, or null. */
NYA_API const NYA_Plugin* nya_plugin_find(NYA_ConstCString name) __attr_no_discard;

/**
 * Which plugin's code is running right now, or null when the answer is the engine or the game.
 *
 * Set around every entry into a VM and cleared on the way out, so a binding a script called can say
 * who called it without the script being able to claim to be somebody else. This is what qualifies a
 * registration and what attributes an error.
 * */
NYA_API NYA_ConstCString nya_plugin_current(void) __attr_no_discard;

/**
 * What `name`'s systems cost, straight out of the system registry's per-owner accounting. Zero
 * everywhere for a plugin that is not loaded, and zero time while accounting is off; see
 * nya_system_accounting_enable.
 * */
NYA_API NYA_SystemOwnerStats nya_plugin_stats(NYA_ConstCString name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CALLING AND FAILING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Calls the global Lua function `function` in `name`'s VM, with `arguments`, and writes whatever it
 * returns into `out_result` when both `arena` and `out_result` are given.
 *
 * An error the script raises is reported through nya_plugin_error before it is returned, so the caller
 * gets a value to act on and the log gets the sentence that names the plugin. A function the plugin
 * does not define is NYA_ERROR_NOT_FOUND and is not counted against it, since asking for an optional
 * hook is not a failure.
 * */
NYA_API NYA_Error nya_plugin_call(
    NYA_ConstCString name,
    NYA_ConstCString function,
    NYA_Arena*       arena,
    const NYA_Value* arguments,
    u32              argument_count,
    OUT NYA_Value*   out_result
) __attr_no_discard;

/**
 * Reports `error` as this plugin's, in a line that cannot be mistaken for the engine breaking:
 *
 * ```
 * [plugin] hello 0.1.0 by someone: main.lua:12: attempt to index a nil value
 * [plugin] report this to its author: https://github.com/someone/hello
 * ```
 *
 * Counts against the plugin, and disables it at NYA_PLUGIN_ERROR_MAX. A no-op for a name that is not
 * loaded, so a late report from a plugin that has already gone is not a second failure.
 * */
NYA_API void nya_plugin_error(NYA_ConstCString name, NYA_ConstCString what, NYA_ConstCString detail);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * NAMESPACING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes `<plugin>:<name>` into `out`, which is how every name a plugin hands to an engine registry is
 * spelled. Returns `out`, so it composes into a call:
 *
 * ```c
 * char qualified[NYA_PLUGIN_QUALIFIED_MAX];
 * nya_system_register((NYA_SystemEntry){ .name = nya_plugin_qualify("hello", "spawn", qualified, sizeof(qualified)), ... });
 * ```
 *
 * `plugin` null means the engine or the game is calling, and then `name` is copied through unqualified:
 * the engine's own registrations are not in anybody's namespace.
 * */
NYA_API NYA_ConstCString nya_plugin_qualify(NYA_ConstCString plugin, NYA_ConstCString name, OUT char* out, u64 capacity);

#ifdef NYA_TESTING
/**
 * Unloads everything and returns the host to its just-linked state, for a test that loads the same
 * plugin twice.
 * */
NYA_INTERNAL void _nya_plugin_reset_for_test(void);
#endif
