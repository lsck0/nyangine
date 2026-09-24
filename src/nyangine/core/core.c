#include "nyangine/core/core_app.c"
#include "nyangine/core/core_skeleton.c"
#include "nyangine/core/core_skeleton_inertial.c"
#include "nyangine/core/core_asset.c"
// After core_asset.c: uses its _NYA_ASSET_STAT_INTERVAL_NS, which is a translation-unit-local macro
// rather than something exported through a header.
#include "nyangine/core/core_config.c"
// the chain before the system that owns one per bus, and propagation after it, since it steers its voices.
#include "nyangine/core/core_audio_effects.c"
// the pure panner primitive before the system that composes it onto a voice.
#include "nyangine/core/core_audio_panner.c"
#include "nyangine/core/core_audio.c"
#include "nyangine/core/core_audio_propagation.c"
#include "nyangine/core/core_callback.c"
// After core_callback.c: its resolvers hand the http router a callback token back as a function.
#include "nyangine/core/core_http_reload.c"
#include "nyangine/core/core_control.c"
#include "nyangine/core/core_entity.c"
#include "nyangine/core/core_event.c"
#include "nyangine/core/core_i18n.c"
#include "nyangine/core/core_gamepad.c"
#include "nyangine/core/core_input.c"
#include "nyangine/core/core_job.c"
#include "nyangine/core/core_keys.c"
#include "nyangine/core/core_save.c"
// After core_save.c and core_world.c's header: a scene is a world written through the save root.
#include "nyangine/core/core_scene.c"
#include "nyangine/core/core_settings.c"
#include "nyangine/core/core_sim.c"
// After core_event.c, whose hook registry it registers a frame hook with, and after the plugins, which
// are included before core in nyangine.c and are what it is a facade over.
#include "nyangine/core/core_social.c"
#include "nyangine/core/core_terrain2d.c"
#include "nyangine/core/core_terrain3d.c"
#include "nyangine/core/core_skeleton_layer.c"
#include "nyangine/core/core_skeleton_blend.c"
#include "nyangine/core/core_system.c"
// After core_system.c: a plugin is one entry in that registry, and after the Lua plugin, which is
// included before core in nyangine.c and is what a plugin's code runs in.
#include "nyangine/core/core_plugin.c"
// After core_plugin.c: it is what nya_plugin_load calls to prove a plugin's signature before running it.
#include "nyangine/core/core_plugin_signature.c"
#include "nyangine/core/core_tilemap.c"
#include "nyangine/core/core_nav.c"
#include "nyangine/core/core_tween.c"
#include "nyangine/core/core_undo.c"
#include "nyangine/core/core_window.c"
#include "nyangine/core/core_world.c"
