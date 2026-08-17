#include "gnyame/gnyame.h"

/*
 * The sky, and the sun that lights the world, from one number.
 *
 * The point of this file is that there is exactly one phase and everything is derived from it: the
 * gradient behind the 2D world, the disc arcing across it, how many stars show through, and the
 * direction and colour of the 3D scene's directional light. A demo where the sky reddens at dusk while
 * the shadows keep pointing north is worse than one with no cycle at all, and that is what two
 * independent sources of time produce.
 *
 * ## Why the sky is 2D
 *
 * It is drawn by the background layer, which sits behind both scenes — so a 2D backdrop serves the 3D
 * scene too, and a cube-mapped skybox would serve only one of them. For a flat cartoon look that is not
 * a compromise: the reference art for this style is bands of flat colour, which is what the gradient
 * already is.
 *
 * The honest limit: it does not rotate with a 3D camera. Orbiting the cube turns the world and leaves the
 * sky where it was. A real skybox is the fix and would be a 3D pass of its own.
 */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One keyframe of the day.
 *
 * A table rather than a formula. A formula that produced believable dawn colours would be longer than
 * this and impossible to art-direct; four keys and a lerp is how the look gets tuned by eye.
 * */
typedef struct {
    /** Where in the day this key sits, in [0, 1]. Must ascend. */
    f32 at;

    NYA_Color top;
    NYA_Color bottom;

    /** The disc, and the light it casts. Sun by day, moon by night. */
    NYA_Color disc;
    NYA_Color light;

    /** How lit the shadowed side of everything is, and how strong the sun is. See NYA_Render3DLight. */
    f32 ambient;
    f32 intensity;

    /** How visible the stars are, in [0, 1]. */
    f32 stars;
} GNY_SkyKey;

/*
 * Midnight, dawn, noon, dusk, and midnight again.
 *
 * The last key repeats the first so the interpolation can run to 1.0 without wrapping — a wrap would be
 * one branch and one chance to get the direction wrong, and a duplicated row costs nothing.
 */
NYA_INTERNAL const GNY_SkyKey _GNY_SKY_KEYS[] = {
    {
     .at        = 0.00F,
     .top       = { 0.03F, 0.04F, 0.10F, 1.0F },
     .bottom    = { 0.10F, 0.09F, 0.20F, 1.0F },
     .disc      = { 0.92F, 0.94F, 1.00F, 1.0F },
     .light     = { 0.55F, 0.62F, 0.85F, 1.0F },
     .ambient   = 0.42F,
     .intensity = 0.45F,
     .stars     = 1.00F,
     },
    {
     .at        = 0.25F,
     .top       = { 0.30F, 0.40F, 0.68F, 1.0F },
     .bottom    = { 0.98F, 0.62F, 0.42F, 1.0F },
     .disc      = { 1.00F, 0.72F, 0.42F, 1.0F },
     .light     = { 1.00F, 0.76F, 0.58F, 1.0F },
     .ambient   = 0.52F,
     .intensity = 0.85F,
     .stars     = 0.15F,
     },
    {
     .at        = 0.50F,
     .top       = { 0.33F, 0.62F, 0.94F, 1.0F },
     .bottom    = { 0.72F, 0.88F, 0.99F, 1.0F },
     .disc      = { 1.00F, 0.97F, 0.85F, 1.0F },
     .light     = { 1.00F, 0.99F, 0.94F, 1.0F },
     .ambient   = 0.62F,
     .intensity = 1.00F,
     .stars     = 0.00F,
     },
    {
     .at        = 0.75F,
     .top       = { 0.26F, 0.26F, 0.52F, 1.0F },
     .bottom    = { 0.96F, 0.48F, 0.42F, 1.0F },
     .disc      = { 1.00F, 0.58F, 0.36F, 1.0F },
     .light     = { 1.00F, 0.66F, 0.50F, 1.0F },
     .ambient   = 0.50F,
     .intensity = 0.80F,
     .stars     = 0.20F,
     },
    {
     .at        = 1.00F,
     .top       = { 0.03F, 0.04F, 0.10F, 1.0F },
     .bottom    = { 0.10F, 0.09F, 0.20F, 1.0F },
     .disc      = { 0.92F, 0.94F, 1.00F, 1.0F },
     .light     = { 0.55F, 0.62F, 0.85F, 1.0F },
     .ambient   = 0.42F,
     .intensity = 0.45F,
     .stars     = 1.00F,
     },
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 gny_sky_phase(void) {
    GNY_World* world = gny_world();

    /*
     * Wall-clock uptime rather than the simulation tick.
     *
     * The sky keeps moving while the physics is paused, which is the behaviour that makes pausing to look
     * at something not also freeze the light. It also means the cycle is the same length regardless of
     * tickrate, which a tick count would not be.
     */
    f32 seconds = nya_app_uptime_s() + world->sky_offset_s;

    f32 phase = fmodf(seconds / GNY_DAY_LENGTH_S, 1.0F);

    // fmodf keeps the sign of its argument, and a negative offset is a legal way to start the demo at
    // dusk. Wrapping here means every consumer below can assume [0, 1).
    return phase < 0.0F ? phase + 1.0F : phase;
}

GNY_SkyState gny_sky_state(void) {
    f32 phase = gny_sky_phase();

    u32 count = (u32)(sizeof(_GNY_SKY_KEYS) / sizeof(_GNY_SKY_KEYS[0]));

    // The last key is at 1.0 and duplicates the first, so this always finds a segment.
    u32 next = count - 1;
    for (u32 i = 1; i < count; i++) {
        if (phase <= _GNY_SKY_KEYS[i].at) {
            next = i;
            break;
        }
    }

    const GNY_SkyKey* from = &_GNY_SKY_KEYS[next - 1];
    const GNY_SkyKey* to   = &_GNY_SKY_KEYS[next];

    f32 span = to->at - from->at;
    f32 t    = span > 0.0F ? (phase - from->at) / span : 0.0F;

    /*
     * Smoothed rather than linear.
     *
     * A linear blend between keys changes fastest exactly at the keys, which puts a visible crease in the
     * sky at dawn and dusk — the two moments anyone is looking at it. Smoothstep flattens the ends so the
     * transition reads as continuous.
     */
    f32 eased = t * t * (3.0F - (2.0F * t));

    GNY_SkyState state = {
        .phase     = phase,
        .top       = nya_color_mix(from->top, to->top, eased),
        .bottom    = nya_color_mix(from->bottom, to->bottom, eased),
        .disc      = nya_color_mix(from->disc, to->disc, eased),
        .light     = nya_color_mix(from->light, to->light, eased),
        .ambient   = nya_lerp(from->ambient, to->ambient, eased),
        .intensity = nya_lerp(from->intensity, to->intensity, eased),
        .stars     = nya_lerp(from->stars, to->stars, eased),
        .is_night  = phase < 0.25F || phase > 0.75F,
    };

    /*
     * The disc rises in the east at 0.25 and sets in the west at 0.75, and the moon takes the other half.
     *
     * Written as one arc over half a day so both bodies use the same path: `arc` runs 0 to 1 across
     * whichever of them is up, which is all the drawing and the light direction need.
     */
    f32 arc = state.is_night ? fmodf(phase + 0.25F, 1.0F) * 2.0F : (phase - 0.25F) * 2.0F;

    state.arc = nya_clamp(arc, 0.0F, 1.0F);

    /*
     * The light direction, from the same arc.
     *
     * `direction` is the way light *travels*, so a body low in the east sends light toward the west and
     * downward. The z component is a constant tilt rather than derived: the arc is drawn as a
     * two-dimensional path across a 2D sky, and a light with no z at all lands exactly along one axis of
     * the 3D scene and makes a cube read as two faces instead of three.
     */
    f32 angle = state.arc * (f32)M_PI;

    state.direction = nya_vector_normalize((f32x3){
        -cosf(angle),
        // Never zero, even at the horizon: a perfectly horizontal sun lights nothing on the ground plane
        // and makes the shadow volume degenerate.
        -nya_max(sinf(angle), GNY_SKY_MIN_ELEVATION),
        -0.45F,
    });

    return state;
}

void gny_sky_draw(NYA_Window* window) {
    nya_assert(window != nullptr);

    GNY_SkyState sky = gny_sky_state();

    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    /*
     * A stack of flat bands rather than a real gradient.
     *
     * The batch's vertices carry a colour, so a two triangle quad with different colours at top and
     * bottom would interpolate properly and cost one draw call — but nya_render2d_rect takes a single
     * colour, and reaching past it into the batch is exactly what a layer must not do. Forty eight bands
     * is under a millisecond, batches into one call anyway, and is invisible as banding at this contrast.
     * A `nya_render2d_rect_gradient` primitive would replace all of this.
     */
    f32 band_height = height / (f32)GNY_SKY_BANDS;

    for (u32 i = 0; i < GNY_SKY_BANDS; i++) {
        f32 t = (f32)i / (f32)(GNY_SKY_BANDS - 1);

        NYA_Color color = nya_color_mix(sky.top, sky.bottom, t);

        // A half pixel of overlap, so rounding at a band edge cannot leave a hairline of whatever was
        // cleared underneath showing through.
        nya_render2d_rect(window, 0.0F, (f32)i * band_height, width, band_height + 0.5F, color);
    }

    if (sky.stars > 0.01F) _gny_sky_stars_draw(window, sky);

    _gny_sky_disc_draw(window, sky);
    _gny_sky_clouds_draw(window, sky);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A deterministic value in roughly 0 to 1 from an index, a channel, and a seed.
 *
 * Not NYA_RNG, deliberately. That takes a hex string seed and a distribution struct, and a zeroed
 * distribution asks for uniform(0, 0) and answers zero every time — a trap worth avoiding on a path where
 * the failure looks like every star landing in the corner. What is wanted here is not a random *sequence*
 * but a pure function: star seven has to be in the same place on every frame, in every run, and in a
 * screenshot taken from another build. A hash of the index gives exactly that with no state to carry.
 *
 * The hash is the engine's nya_ihash2 rather than the multiplicative one that used to be written out here.
 * That version aborted the sanitized build on the first frame: `index * 0x9E3779B1U` wraps, and while
 * unsigned wraparound is perfectly defined C, -fsanitize=unsigned-integer-overflow flags it because it is
 * far more often an accident than an intention. nya_ihash2 is the same idea already written, already
 * tested, and already carrying the __attr_no_sanitize that says the wrapping inside it is deliberate.
 *
 * `channel` rather than the seed offsets the callers used to pass: nya_ihash2 mixes y with a stride of 57,
 * so the four values one star needs stay independent. Offsetting the seed by one instead makes star i's
 * y-channel the same number as star i+1's x-channel, and stars lined up along a diagonal is what that
 * looks like on screen.
 *
 * nya_ihash2 returns roughly -1 to 1, like the noise functions beside it, so this maps to 0..1.
 * */
NYA_INTERNAL f32 _gny_sky_random(u32 index, s32 channel, u32 seed) {
    return (nya_ihash2((s32)index, channel, seed) * 0.5F) + 0.5F;
}

void _gny_sky_stars_draw(NYA_Window* window, GNY_SkyState sky) {
    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    /*
     * Positions from a fixed seed, so the stars are in the same place every frame and every run.
     *
     * Generated rather than stored because sixty-four points is cheaper to recompute than to keep, and
     * because a fixed seed makes the sky reproducible in a screenshot — which matters more than it sounds
     * when comparing two builds.
     */
    for (u32 i = 0; i < GNY_SKY_STAR_COUNT; i++) {
        f32 x = _gny_sky_random(i, 0, GNY_SKY_STAR_SEED) * width;

        // Only the upper part of the sky. Stars behind the hills would be drawn over by them anyway, and
        // spending the count on the part that shows makes the same number look denser.
        f32 y = _gny_sky_random(i, 1, GNY_SKY_STAR_SEED) * height * GNY_SKY_STAR_BAND;

        f32 size  = nya_lerp(GNY_SKY_STAR_MIN, GNY_SKY_STAR_MAX, _gny_sky_random(i, 2, GNY_SKY_STAR_SEED));
        f32 twinkle_phase = _gny_sky_random(i, 3, GNY_SKY_STAR_SEED) * 2.0F * (f32)M_PI;

        /*
         * Twinkling as a brightness wobble, not a size change.
         *
         * A star that changes size pops between whole pixels and reads as flickering rather than
         * twinkling. Modulating alpha keeps it smooth at any size.
         */
        f32 twinkle = 0.65F + (0.35F * sinf((nya_app_uptime_s() * GNY_SKY_TWINKLE_SPEED) + twinkle_phase));

        NYA_Color color = GNY_SKY_STAR_COLOR;
        color.a *= sky.stars * twinkle;

        nya_render2d_rect(window, x, y, size, size, color);
    }
}

void _gny_sky_disc_draw(NYA_Window* window, GNY_SkyState sky) {
    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    /*
     * A half circle path across the sky, flattened so the body stays in the upper portion.
     *
     * The horizontal position is the arc directly and the vertical is a sine of it, which puts the body
     * at its highest at the middle of its half of the day. Nothing here is astronomical: it is the shape
     * a cartoon sun moves along.
     */
    f32 x = sky.arc * width;
    f32 y = height * (GNY_SKY_DISC_HORIZON - (sinf(sky.arc * (f32)M_PI) * GNY_SKY_DISC_RISE));

    f32 radius = height * GNY_SKY_DISC_RADIUS;

    /*
     * A halo of two flat rings under the disc, not a blur.
     *
     * Concentric translucent circles are the cartoon idiom — the glow has an edge, which is the whole
     * point of the style — and they cost two more circles rather than a second pass. The bloom pass will
     * find the disc as well, since it is the brightest thing on screen.
     */
    NYA_Color halo = sky.disc;

    // The moon glows far less than the sun. One number rather than a second set of constants, because the
    // difference is a matter of degree and a moon with a sun's corona is the single most obvious tell.
    f32 halo_scale = sky.is_night ? GNY_SKY_MOON_HALO : 1.0F;

    halo.a = GNY_SKY_HALO_ALPHA * halo_scale;
    nya_render2d_circle(window, (f32x2){ x, y }, radius * GNY_SKY_HALO_OUTER, halo);

    halo.a = GNY_SKY_HALO_ALPHA * 1.6F * halo_scale;
    nya_render2d_circle(window, (f32x2){ x, y }, radius * GNY_SKY_HALO_INNER, halo);

    nya_render2d_circle(window, (f32x2){ x, y }, radius, sky.disc);

    /*
     * Craters, not a crescent.
     *
     * A crescent was the first attempt and it cannot be done this way. The trick is to punch a bite out of
     * the disc with a second circle in the background colour — but the two halo rings are already down
     * underneath, so "the background colour" is sky *plus* halo, and an opaque sky-coloured bite both fails
     * to match and cuts a visible hole in the glow. Reproducing what is behind it would mean reproducing
     * the halo, at which point drawing a mask is the cheaper answer.
     *
     * Flat darker circles inside the disc need no knowledge of what is behind them at all, because they
     * never cross its edge. They also happen to be the more cartoon-idiomatic moon.
     *
     * A real crescent wants either a stencil, or a small texture with an alpha channel, or a shader — all
     * of which are a bigger change than the shape is worth here.
     */
    if (sky.is_night) {
        NYA_Color crater = sky.disc;

        crater.r *= GNY_SKY_CRATER_SHADE;
        crater.g *= GNY_SKY_CRATER_SHADE;
        crater.b *= GNY_SKY_CRATER_SHADE;

        /*
         * Fixed offsets, in units of the radius, chosen so none of them reaches the edge.
         *
         * Hard-coded rather than hashed: there are three of them and their arrangement is the whole look, so
         * a table that can be nudged by eye beats a seed that has to be searched.
         */
        static const f32x2 craters[] = {
            { -0.34F, -0.22F },
            {  0.28F,  0.10F },
            { -0.06F,  0.40F },
        };

        static const f32 crater_sizes[] = { 0.26F, 0.18F, 0.13F };

        for (u32 i = 0; i < sizeof(craters) / sizeof(craters[0]); i++) {
            nya_render2d_circle(
                window,
                (f32x2){ x + (craters[i].x * radius), y + (craters[i].y * radius) },
                radius * crater_sizes[i],
                crater
            );
        }
    }
}

void _gny_sky_clouds_draw(NYA_Window* window, GNY_SkyState sky) {
    f32 width  = (f32)window->screen_width;
    f32 height = (f32)window->screen_height;

    /*
     * Each cloud is a row of overlapping circles with a flat bottom.
     *
     * Which is what makes it read as a cartoon cloud rather than as fog: hard edges, no gradient, and a
     * straight underside. Three circles and a rectangle per cloud, so the whole sky costs a couple of
     * dozen primitives in one draw call.
     */
    for (u32 i = 0; i < GNY_SKY_CLOUD_COUNT; i++) {
        f32 base_x = _gny_sky_random(i, 0, GNY_SKY_CLOUD_SEED) * width;
        f32 y      = height * nya_lerp(GNY_SKY_CLOUD_TOP, GNY_SKY_CLOUD_BOTTOM, _gny_sky_random(i, 1, GNY_SKY_CLOUD_SEED));
        f32 scale  = nya_lerp(GNY_SKY_CLOUD_MIN, GNY_SKY_CLOUD_MAX, _gny_sky_random(i, 2, GNY_SKY_CLOUD_SEED)) * height;

        // Drifting, and wrapped by the width plus its own size so a cloud leaves the screen before it
        // reappears on the other side rather than jumping.
        f32 drift = nya_app_uptime_s() * GNY_SKY_CLOUD_SPEED * (0.6F + (0.8F * _gny_sky_random(i, 3, GNY_SKY_CLOUD_SEED)));
        f32 x     = fmodf(base_x + drift, width + (scale * 4.0F)) - (scale * 2.0F);

        /*
         * Tinted by the sky rather than white.
         *
         * A white cloud at dusk is the one thing that gives away a static backdrop, because everything
         * around it has gone orange. Mixing toward the horizon colour costs nothing and ties the clouds to
         * the cycle.
         */
        NYA_Color color = nya_color_mix(GNY_SKY_CLOUD_COLOR, sky.bottom, GNY_SKY_CLOUD_TINT);
        color.a         = GNY_SKY_CLOUD_ALPHA;

        nya_render2d_circle(window, (f32x2){ x - scale, y }, scale * 0.62F, color);
        nya_render2d_circle(window, (f32x2){ x, y - (scale * 0.28F) }, scale * 0.85F, color);
        nya_render2d_circle(window, (f32x2){ x + scale, y }, scale * 0.55F, color);

        // The flat underside, filling the gap the circles leave between their lowest points.
        nya_render2d_rect(window, x - (scale * 1.55F), y, scale * 3.1F, scale * 0.62F, color);
    }
}
