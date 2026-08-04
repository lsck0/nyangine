#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

void nya_system_settings_init(void) {
    nya_settings_reset();

    nya_info("Settings system initialized.");
}

void nya_system_settings_deinit(void) {
    // Nothing is owned: the volumes are floats and the bindings are a fixed array inside NYA_App.
    // Kept for symmetry with every other system, and so persisting settings later has somewhere
    // obvious to go.
    nya_info("Settings system deinitialized.");
}

/*
 * ─────────────────────────────────────────────────────────
 * SETTINGS FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_SettingsSystem* nya_settings(void) {
    return &nya_app_get()->settings_system;
}

f32 nya_settings_volume(NYA_VolumeChannel channel) {
    nya_assert(channel < NYA_VOLUME_CHANNEL_COUNT, "Unknown volume channel %d.", (int)channel);

    return nya_settings()->volumes[channel];
}

void nya_settings_volume_set(NYA_VolumeChannel channel, f32 volume) {
    nya_assert(channel < NYA_VOLUME_CHANNEL_COUNT, "Unknown volume channel %d.", (int)channel);

    nya_settings()->volumes[channel] = nya_clamp(volume, 0.0F, 1.0F);
}

f32 nya_settings_volume_effective(NYA_VolumeChannel channel) {
    nya_assert(channel < NYA_VOLUME_CHANNEL_COUNT, "Unknown volume channel %d.", (int)channel);

    if (channel == NYA_VOLUME_CHANNEL_MASTER) return nya_settings()->volumes[NYA_VOLUME_CHANNEL_MASTER];

    return nya_settings()->volumes[NYA_VOLUME_CHANNEL_MASTER] * nya_settings()->volumes[channel];
}

void nya_settings_reset(void) {
    NYA_SettingsSystem* settings = nya_settings();

    for (u32 channel = 0; channel < NYA_VOLUME_CHANNEL_COUNT; channel++) settings->volumes[channel] = 1.0F;

    // NYA_KEY_UNKNOWN is the unbound marker, and it is zero, so this clears the whole table.
    nya_memset(settings->bindings, 0, sizeof(settings->bindings));
}
