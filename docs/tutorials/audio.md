# Playing sound

A sound is an asset. Load it once, then play it by its handle as often as you like; each play takes
one voice from a fixed pool and returns a handle to it, so a sound that is already playing can still
be changed.

`examples/pinball3d` plays a hit on every impact, louder the harder the ball lands.
`src/gnyame/systems/system_music.c` streams the background track.

## Loading

```c
NYA_Error sound = nya_asset_load((NYA_AssetLoadParameters){
    .type     = NYA_ASSET_TYPE_SOUND,
    .handle   = NYA_ASSET_SOUNDS_HIT_WAV,
    .as_sound = { .predecode = true },
});

if (!sound.ok) nya_log_warn("%s", (NYA_ConstCString)sound.message);
```

`predecode` is the one decision. A short effect played often wants it: decoding at the moment of an
impact is exactly when a hitch shows. A minute of music does not, because predecoded it sits in
memory uncompressed for the whole run, so music streams.

The load is queued, not done, and resolves at the end of the frame. `nya_asset_status` says when it
has landed. A failed load is a warning and not a crash: a machine with no audio device still runs
the game.

## Playing

```c
nya_audio_play_sound(NYA_ASSET_SOUNDS_HIT_WAV, 0.8F);
nya_audio_play_sound_varied(NYA_ASSET_SOUNDS_HIT_WAV, gain);

NYA_SoundVoice fire = nya_audio_play_sound_with(NYA_ASSET_SOUNDS_FIRE_WAV, (NYA_SoundParams){
    .gain = 0.6F, .pitch = 1.0F, .loop = true,
});

// later, as the fire burns down
nya_audio_voice_set_gain(fire, 0.6F * fuel_left);
```

`_varied` detunes and relevels each play by a small random amount. The same sample played ten times
a second is what makes a game sound like a machine gun of identical clicks, and a semitone either
way is enough to stop it.

A voice handle carries a generation, so a handle to a voice that has finished and been reused is
simply ignored. Every function takes `NYA_SOUND_VOICE_NONE` safely, which is what a failed play
returns. When all `NYA_AUDIO_VOICES` are busy, `NYA_SoundParams.priority` decides who wins.

## Music

```c
nya_audio_play_music_with(NYA_ASSET_MUSIC_BGM_OPUS, (NYA_MusicParams){
    .gain       = 0.6F * nya_settings_volume_effective(NYA_VOLUME_CHANNEL_MUSIC),
    .loop       = true,
    .fade_in_ms = 2000,
});
```

Music is its own track, outside the voice pool, with `nya_audio_pause_music`, `_resume_music`,
`_stop_music` and `nya_audio_crossfade_music` to swap tracks without a gap.

## Where a sound is

```c
nya_audio_listener_set((NYA_AudioListener){ .position = player_position, .reference_distance = 8.0F });
nya_audio_play_sound_at(NYA_ASSET_SOUNDS_HIT_WAV, crate_position, (NYA_SoundParams){ .gain = 0.9F });
```

The listener is where the player hears from, in world units, and a sound played at a position pans
and fades against it. A 2D world says whether its screen is a wall or the ground with
`NYA_AudioListener.plane`; a 3D scene uses `nya_audio_listener_3d_set` and `_play_sound_at_3d`,
usually with the camera as the ear.

## Volume

The player's volumes live in the settings, per channel, set through `nya_settings_volume_set` and
saved with them. `nya_settings_volume_effective` is a channel already scaled by master, which is
what a gain should be multiplied by. Under those sit the mixer's own `nya_audio_set_master_gain`,
`_sound_gain` and `_music_gain`.

Each bus also takes effects (`nya_audio_bus_effects_set`), and a single voice a low pass
(`nya_audio_voice_filter_set`), which between them are the whole of "the player is underwater".

## Next

- `src/nyangine-core/core/core_audio_propagation.h` for sound that travels around walls.
- [Physics](physics.md): the impact list is where most game sounds come from.
