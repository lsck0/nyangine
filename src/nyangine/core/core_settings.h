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
NYA_API void nya_system_settings_init(void);
NYA_API void nya_system_settings_deinit(void);

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
