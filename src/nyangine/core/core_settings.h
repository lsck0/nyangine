/**
 * @file core_settings.h
 *
 * Player facing settings: the things a game puts on an options screen.
 *
 * One global, reached with nya_settings(), holding what the player has chosen rather than what the
 * engine has computed — volumes and key bindings today. It owns no memory and allocates nothing, so
 * it comes up before every other system and cannot fail.
 *
 * Bindings live here rather than in the input system because they are configuration: the same kind
 * of thing as a volume slider, written by an options menu and read every frame. core_input owns the
 * *querying* (nya_input_action_pressed and friends); this owns the storage.
 * */
#pragma once

#include "nyangine/base/base.h"
#include "nyangine/core/core_input.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SettingsSystem NYA_SettingsSystem;

/**
 * The mixes a player expects to control separately.
 *
 * MASTER is not a channel of its own: it scales all the others, which is what
 * nya_settings_volume_effective exists to compute.
 * */
typedef enum {
    NYA_VOLUME_CHANNEL_MASTER,
    /** Short one-off effects. */
    NYA_VOLUME_CHANNEL_SOUND,
    /** Background music. */
    NYA_VOLUME_CHANNEL_MUSIC,
    NYA_VOLUME_CHANNEL_VOICE,
    NYA_VOLUME_CHANNEL_UI,

    NYA_VOLUME_CHANNEL_COUNT,
} NYA_VolumeChannel;

struct NYA_SettingsSystem {
    /** Per channel, always within [0, 1]. Set through nya_settings_volume_set, which clamps. */
    f32 volumes[NYA_VOLUME_CHANNEL_COUNT];

    /**
     * Key bindings, indexed by action.
     *
     * A fixed array rather than a map: it is a few kilobytes, it is indexed by a small integer, and
     * it is read every frame by every action query. NYA_INPUT_BINDINGS_PER_ACTION alternatives per
     * action, so a game can offer the usual primary and secondary binding.
     * */
    NYA_InputBinding bindings[NYA_INPUT_ACTION_MAX][NYA_INPUT_BINDINGS_PER_ACTION];
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

/** Every volume starts at 1.0 and every action starts unbound. */
/** Resets to defaults, then loads whatever is on disk over the top. Never fails. */
NYA_API void nya_system_settings_init(void);

/** Writes the settings back out. A failure is logged rather than raised; teardown cannot unwind. */
NYA_API void nya_system_settings_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * PERSISTENCE
 * ─────────────────────────────────────────────────────────
 */

/**
 * Where the settings file lives, relative to the save root. See core_save.h.
 *
 * `.nya` rather than `.json` on purpose: the native format is typed, indents under NYA_SERDE_PRETTY,
 * and checksums the object tree rather than the bytes — so a player who opens this in a text editor,
 * changes a number and saves it with different whitespace still has a valid file. A JSON settings
 * file would be equally readable and would lose the distinction between a volume of `1` and a volume
 * of `1.0`, which is the difference between a u32 and an f32 coming back.
 * */
#define NYA_SETTINGS_FILE "settings.nya"

/**
 * The version written into the file, and what a loader checks before trusting its shape.
 *
 * Raise it when the meaning of an existing key changes. Adding a key does not need a raise: a loader
 * that does not find one leaves the default in place, which is already the right answer for a file
 * written by an older build.
 * */
#define NYA_SETTINGS_VERSION 1

/**
 * Writes the settings to NYA_SETTINGS_FILE, atomically.
 *
 * Called on the way out, so a game does not normally call it. Call it directly after a settings
 * screen closes, if losing the change to a crash would be annoying — which for a rebound key it
 * generally is.
 * */
NYA_API NYA_Error nya_settings_save(void);

/**
 * Reads NYA_SETTINGS_FILE over the current settings.
 *
 * Additive rather than replacing: anything the file does not mention keeps whatever it had, which is
 * what makes a file written by an older build load cleanly instead of zeroing the settings that did
 * not exist yet. Call nya_settings_reset first for "discard everything and load".
 *
 * NYA_ERROR_NOT_FOUND on a first run, which is not a problem — the defaults are already in place.
 * */
NYA_API NYA_Error nya_settings_load(void);

/** The settings as an object tree, for writing or for showing. Everything comes from `arena`. */
NYA_API NYA_Object* nya_settings_to_object(NYA_Arena* arena) __attr_no_discard;

/**
 * Applies whatever an object tree has to say about the settings, ignoring the rest.
 *
 * Every field is optional and every value is validated: a volume outside [0, 1] is clamped, a key
 * name SDL does not recognise leaves that binding alone, and an action index past
 * NYA_INPUT_ACTION_MAX is skipped. A settings file is a file a player is invited to edit, so it is
 * untrusted input and is treated as such rather than asserted on.
 * */
NYA_API void nya_settings_from_object(const NYA_Object* object);

/*
 * ─────────────────────────────────────────────────────────
 * SETTINGS FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/** The settings themselves, for reading a whole block at once or writing one back after a load. */
NYA_API NYA_SettingsSystem* nya_settings(void) __attr_no_discard;

/** A channel's own level, ignoring master. In [0, 1]. */
NYA_API f32 nya_settings_volume(NYA_VolumeChannel channel) __attr_no_discard;

/** Clamped into [0, 1], so a slider that overshoots is not a bug the audio layer has to defend against. */
NYA_API void nya_settings_volume_set(NYA_VolumeChannel channel, f32 volume);

/**
 * The channel scaled by master, which is the number to hand to the mixer.
 *
 * Asking for MASTER gives master itself rather than master squared.
 * */
NYA_API f32 nya_settings_volume_effective(NYA_VolumeChannel channel) __attr_no_discard;

/** Puts every volume back to 1.0 and drops every binding. */
NYA_API void nya_settings_reset(void);
