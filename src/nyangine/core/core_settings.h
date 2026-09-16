/**
 * @file core_settings.h
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
 * MASTER is not a channel of its own: it scales all the others; see nya_settings_volume_effective.
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

/** Resets every volume to 1.0 and every action to unbound, then loads whatever is on disk over that. Never fails. */
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
 * */
#define NYA_SETTINGS_FILE "settings.nya"

/**
 * The version written into the file, and what a loader checks before trusting its shape.
 * */
#define NYA_SETTINGS_VERSION 1

/**
 * Writes the settings to NYA_SETTINGS_FILE, atomically.
 * */
NYA_API NYA_Error nya_settings_save(void) __attr_no_discard;

/**
 * Reads NYA_SETTINGS_FILE over the current settings.
 * */
NYA_API NYA_Error nya_settings_load(void) __attr_no_discard;

/** The settings as an object tree, for writing or for showing. Everything comes from `arena`. */
NYA_API NYA_Object* nya_settings_to_object(NYA_Arena* arena) __attr_no_discard;

/**
 * Applies whatever an object tree has to say about the settings, ignoring the rest.
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
