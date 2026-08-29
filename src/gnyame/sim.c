#include "gnyame/gnyame.h"
#include "generated/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Plays one impact, with its loudness taken from how hard it was. */
NYA_INTERNAL void _gny_sim_impact_play(const GNY_SimImpact* impact);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_sim_init(void) {
    NYA_EXPECT(nya_sim_observer_add(nya_callback(gny_sim_observe), nullptr), "while registering the simulation observer");
}

void gny_sim_observe(const NYA_ArrayᐸNYA_SimRecordᐳ* records, void* user_data) {
    nya_unused(user_data);

    GNY_World* world = gny_world();
    if (world == nullptr) return;

    /*
     * The loudest few impacts of the whole frame, kept as a small sorted array.
     *
     * Insertion into a six element list is a handful of comparisons and needs no allocation, which
     * matters because a collapsing stack can put dozens of records through here in one frame. Sorting
     * the whole record array to take the top six would be the obvious version and the wrong one.
     */
    GNY_SimImpact loudest[GNY_HIT_VOICES_PER_FRAME];
    u32           loudest_count = 0;

    u32 impacts = 0;
    u32 lost    = 0;

    nya_array_foreach (records, record) {
        switch ((GNY_SimRecordType)record->type) {
            case GNY_SIM_IMPACT: {
                const GNY_SimImpact* impact = record->data;

                impacts++;

                // Walk from the quiet end, shifting anything softer along, and drop off the bottom.
                u32 slot = loudest_count < GNY_HIT_VOICES_PER_FRAME ? loudest_count++ : GNY_HIT_VOICES_PER_FRAME;

                while (slot > 0 && loudest[slot - 1].approach_speed < impact->approach_speed) {
                    if (slot < GNY_HIT_VOICES_PER_FRAME) loudest[slot] = loudest[slot - 1];
                    slot--;
                }

                if (slot < GNY_HIT_VOICES_PER_FRAME) loudest[slot] = *impact;
            } break;

            case GNY_SIM_BOX_LOST: {
                lost++;
            } break;

            default: break;
        }
    }

    // Counters the HUD reads. Copied out of the records because those are cleared the moment this
    // returns — an observer that wanted to keep something had to say so.
    world->hits       += impacts;
    world->boxes_lost += lost;

    for (u32 i = 0; i < loudest_count; i++) _gny_sim_impact_play(&loudest[i]);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _gny_sim_impact_play(const GNY_SimImpact* impact) {
    /*
     * Loudness from how hard it was, normalised against the quietest impact that can appear at all.
     *
     * Linear rather than anything cleverer: the range between "just qualified" and "dropped from the
     * top of the screen" is about one to six, small enough that a curve would be inventing detail
     * nobody can hear.
     */
    f32 threshold = nya_physics2d_hit_threshold();
    f32 strength  = nya_clamp((impact->approach_speed / threshold - 1.0F) / (GNY_HIT_LOUDEST_AT - 1.0F), 0.0F, 1.0F);

    /*
     * Sparks scaled by the same `strength` the sound is.
     *
     * One number driving both is what makes a hard landing read as hard rather than as two unrelated
     * effects that happen to coincide — a loud thump with a polite puff is worse than either alone.
     */
    (void)nya_particles_emit(
        gny_world()->sparks,
        (NYA_ParticleBurst){
            .shape       = NYA_PARTICLE_SHAPE_CONE,
            .position    = { impact->point.x, impact->point.y, 0.0F },
            .count       = (u32)nya_lerp((f32)GNY_SPARK_MIN, (f32)GNY_SPARK_MAX, strength),
            // Upward off the surface, which on a y-down screen is negative y.
            .direction   = { 0.0F, -1.0F, 0.0F },
            .spread      = GNY_SPARK_SPREAD,
            .speed       = { 70.0F, nya_lerp(180.0F, 560.0F, strength) },
            .lifetime_s  = { 0.25F, 0.75F },
            .size        = GNY_SPARK_SIZE_START,
            .size_end    = GNY_SPARK_SIZE_END,
            .color_start = GNY_SPARK_COLOR,
            // Zero alpha, so they fade rather than vanishing — a particle that disappears at full
            // brightness reads as a glitch.
            .color_end = { 1.0F, 0.35F, 0.05F, 0.0F },
            .gravity   = { 0.0F, GNY_SPARK_GRAVITY, 0.0F },
            .damping   = 1.5F,
        }
    );

    nya_audio_play_sound_at(
        NYA_ASSET_SOUNDS_HIT_WAV,
        impact->point,
        (NYA_SoundParams){
            .gain              = nya_lerp(GNY_HIT_GAIN_MIN, GNY_HIT_GAIN_MAX, strength) * nya_settings_volume_effective(NYA_VOLUME_CHANNEL_SOUND),
            .gain_variation_db = 1.5F,

            // Harder impacts sit lower, which is what a heavier thing sounds like. Combined with the
            // random detune so repeated landings still differ from each other.
            .pitch                     = nya_lerp(1.12F, 0.88F, strength),
            .pitch_variation_semitones = 0.6F,

            // Still worth setting even though the observer has already picked the loudest few: those
            // few compete with whatever else is playing, and a hard landing should win.
            .priority = (s32)(strength * 100.0F),
        }
    );
}
