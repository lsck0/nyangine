/**
 * @file system_music.c
 *
 * The background track as a registered system: queued on the first tick, started once loaded. A system
 * rather than a layer hook, because music plays on every screen and belongs to no layer.
 * */
#include "gnyame/gnyame.h"
#include "genyarated/assets.h"

void gny_system_music_update(f32 delta_time_s) {
    nya_unused(delta_time_s);

    GNY_World* world = gny_world();
    if (world == nullptr || world->music_started) return;

    NYA_AssetStatus status = nya_asset_status(NYA_ASSET_MUSIC_BGM_OPUS);

    if (status == NYA_ASSET_STATUS_UNLOADED) {
        // streamed: predecoding a minute of stereo holds it uncompressed for the whole run.
        NYA_Error queued = nya_asset_load((NYA_AssetLoadParameters){
            .type     = NYA_ASSET_TYPE_SOUND,
            .handle   = NYA_ASSET_MUSIC_BGM_OPUS,
            .as_sound = { .predecode = false },
        });

        // not fatal: a machine without an audio device still runs the demo.
        if (!queued.ok) {
            nya_log_warn("%s", (NYA_ConstCString)queued.message);
            world->music_started = true;
        }
        return;
    }

    if (status == NYA_ASSET_STATUS_LOADING) return;

    // latched either way: a track that failed to decode will not decode next tick either.
    world->music_started = true;

    if (status != NYA_ASSET_STATUS_LOADED) {
        nya_log_warn("The background track '%s' could not be loaded; running without music.", NYA_ASSET_MUSIC_BGM_OPUS);
        return;
    }

    nya_audio_play_music_with(
        NYA_ASSET_MUSIC_BGM_OPUS,
        (NYA_MusicParams){
            // effective volume, so the master slider moves this too.
            .gain       = GNY_MUSIC_GAIN * nya_settings_volume_effective(NYA_VOLUME_CHANNEL_MUSIC),
            .loop       = true,
            .fade_in_ms = GNY_MUSIC_FADE_IN_MS,
        }
    );

    // started then paused rather than never started, so `m` has something to resume.
    if (GNY_MUSIC_START_MUTED) nya_audio_pause_music();
}
