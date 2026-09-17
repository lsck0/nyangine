/**
 * @file constants.h
 *
 * Every tuning number the game uses, grouped by what reads it. Change values here, not at call sites.
 * */
#pragma once

#include "nyangine/nyangine.h"

// Both the HUD and the menus name a font, and the handles come from the generated asset index.
#include "generated/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* The ground is a polyline at a fixed spacing: wider is cheaper and more angular, which the crates notice. */

#define GNY_TERRAIN_HALF_WIDTH  2400.0F
#define GNY_TERRAIN_POINT_STEP  28.0F
#define GNY_TERRAIN_POINT_COUNT ((u32)((GNY_TERRAIN_HALF_WIDTH * 2.0F) / GNY_TERRAIN_POINT_STEP) + 1)

/** World y the terrain varies around, and by how much. Positive y is down; see physics2d.h. */
#define GNY_TERRAIN_BASE_Y    260.0F
#define GNY_TERRAIN_AMPLITUDE 110.0F

/** Below this a crate has left the world and is despawned rather than simulated forever. */
#define GNY_WORLD_KILL_Y 1600.0F

/** Sizes a spawned crate is picked between, in world units. */
#define GNY_BOX_MIN_SIZE 18.0F
#define GNY_BOX_MAX_SIZE 44.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CAMERA
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the camera starts. The terrain sits below this, leaving room above to drop things into. */
#define GNY_CAMERA_START_X    0.0F
#define GNY_CAMERA_START_Y    60.0F
#define GNY_CAMERA_START_ZOOM 1.0F

/**
 * How far a sound gets from the camera before it fades, in world units. About a third of the view, so an edge
 * impact sounds further than a central one.
 * */
#define GNY_CAMERA_EAR_DISTANCE 400.0F

/**
 * Fraction of the remaining gap closed per tick. Exponential easing settles without overshoot and without
 * knowing the target's speed; about 0.1 keeps up without looking welded on.
 * */
#define GNY_CAMERA_FOLLOW_EASING 0.12F

/** World units per second. Unlike the camera's pan speed, which scales with zoom. */
#define GNY_PLAYER_MOVE_SPEED 420.0F

/* Secondary views: a non-primary camera renders into its own texture, composited into a viewport. */

/** Size of the inset the demo opens when a crate is followed, in window pixels. */
#define GNY_CAMERA_VIEW_WIDTH  320.0F
#define GNY_CAMERA_VIEW_HEIGHT 200.0F

/** How far the inset sits from the bottom right corner. */
#define GNY_CAMERA_VIEW_MARGIN 16.0F

/** Zoom the inset uses. Tighter than the main view, since it is watching one thing. */
#define GNY_CAMERA_VIEW_ZOOM 1.6F

/** Opaque, unlike the primary target: the inset is a panel over the finished frame, not part of it. */
#define GNY_CAMERA_VIEW_CLEAR ((NYA_Color){ 0.06F, 0.07F, 0.10F, 1.0F })

#define GNY_CAMERA_VIEW_BORDER       ((NYA_Color){ 0.35F, 0.62F, 0.42F, 0.85F })
#define GNY_CAMERA_VIEW_BORDER_WIDTH 2.0F

/** How far the camera moves per second under the direction keys, and how fast the wheel zooms. */
#define GNY_CAMERA_PAN_SPEED 420.0F
#define GNY_CAMERA_ZOOM_STEP 1.12F
#define GNY_CAMERA_ZOOM_MIN  0.25F
#define GNY_CAMERA_ZOOM_MAX  4.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * POST PROCESSING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The pass that greys out the 2D world while the pause menu is open. */
#define GNY_PIPELINE_GRAYSCALE "gny_grayscale_pipeline"

/** The handle the colour grading pipeline is registered under. See gny_post_passes. */
#define GNY_PIPELINE_GRADE "gny_grade_pipeline"

/** Passes a scene's chain can hold: the grade from gny_post_passes, and the pause grey. */
#define GNY_POST_PASSES_MAX 2

/**
 * Luminance a pixel needs before it glows in the 2D world. Half is about a lit crate; the terrain fill (~0.15)
 * stays under it, and a sleeping crate's tint drops below it.
 * */
#define GNY_BLOOM_2D_THRESHOLD 0.50F

/** How hard the glow is added back. Below one, so a glowing crate keeps its edges and colour. */
#define GNY_BLOOM_2D_INTENSITY 0.70F

/** Pixels between the bloom's taps. See NYA_PostBloom.spread. */
#define GNY_BLOOM_2D_SPREAD 3.0F

/*
 * The 3D scene's own bloom numbers: a daylight landscape where the 2D threshold would make almost every pixel
 * bright.
 */

/**
 * Just below the brightest lit surfaces. mesh3d_tonemap puts lit surfaces under ~0.86 and emission above ~0.95,
 * so the rim, highlights and horizon catch a little glow while a saturated crate (luma ~0.6) stays clear.
 * */
#define GNY_BLOOM_3D_THRESHOLD 0.78F

/** Around one, so what clears the threshold is visible. */
#define GNY_BLOOM_3D_INTENSITY 1.10F

/** Tighter than the 2D world's, so a lamp gets a halo rather than a haze. */
#define GNY_BLOOM_3D_SPREAD 3.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * AUDIO
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Frames per second without focus. See NYA_AppOptions.unfocused_frame_rate_limit. Thirty stays smooth in a
 * corner of the screen at a quarter of the GPU cost.
 * */
#define GNY_UNFOCUSED_FRAME_RATE 30

/**
 * The Steam app id the Steam builds relaunch through and connect as. 480 is Valve's public Spacewar test app; a real
 * game puts its own here and in packaging/steam.
 * */
#define GNY_STEAM_APP_ID 480

/** Level the background track sits at, under the master and music gains. */
#define GNY_MUSIC_GAIN 0.45F

/**
 * Where the music slider starts. A setting, unlike GNY_MUSIC_GAIN (the track's mix level): written by
 * gny_actions_init, edited in the pause menu, persisted.
 * */
#define GNY_MUSIC_VOLUME_DEFAULT 0.7F

/** How much one press of left or right on a volume row moves it. */
#define GNY_VOLUME_STEP 0.05F

/** Long enough that the track fades in, since it has no lead-in. */
#define GNY_MUSIC_FADE_IN_MS 1500

/** Whether the background track starts silent. It is started and then paused, so `m` has something to resume. */
#define GNY_MUSIC_START_MUTED true

/* Impact audio. Hits below the physics threshold never reach the game (see physics2d.h); these shape the rest. */

/**
 * Impacts given a voice per frame. A collapsing stack produces dozens of hits and there are sixteen voices, so
 * this is a budget. Per frame, so the loudest impacts win. See sim.h.
 * */
#define GNY_HIT_VOICES_PER_FRAME 6

/** The approach speed, as a multiple of the hit threshold, at which an impact is as loud as it gets. */
#define GNY_HIT_LOUDEST_AT 6.0F

#define GNY_HIT_GAIN_MIN 0.20F
#define GNY_HIT_GAIN_MAX 0.85F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD COLOURS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Ground fill, and the brighter line along its surface. The line is what the bloom pass catches. */
#define GNY_TERRAIN_FILL    ((NYA_Color){ 0.13F, 0.15F, 0.18F, 1.0F })
#define GNY_TERRAIN_SURFACE ((NYA_Color){ 0.35F, 0.62F, 0.42F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * BACKGROUND
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Bands the sky gradient is approximated with. More is smoother and costs one quad each. */
#define GNY_SKY_BANDS 48

/** Columns each parallax ridge is sampled at. The profile is analytic, so this is only smoothness. */
#define GNY_RIDGE_COLUMNS 96

/** Slow moving specks, to make motion visible in the empty part of the sky. */
#define GNY_MOTE_COUNT 64

/** Seeds for nya_ihash2: mote positions and crate sizes. Different only so the two do not correlate. */
#define GNY_MOTE_SEED 0x5EED
#define GNY_BOX_SEED  0xC4A7E

/* Day/night cycle. The gradient colours come from the keyframe table in system_sky.c; this is the cycle's shape. */

/** How long a full day takes, in seconds. Short enough to see the whole cycle without waiting. */
#define GNY_DAY_LENGTH_S 120.0F

/** Where the day starts. 0.32 is mid morning, so the first frame is lit. */
#define GNY_SKY_START_PHASE 0.32F

/**
 * Lowest the sun may sit, as the sine of its arc. A horizontal light lights nothing and degenerates the shadow
 * volume.
 * */
#define GNY_SKY_MIN_ELEVATION 0.12F

/** The disc: its size as a fraction of the window, and the path it travels. */
#define GNY_SKY_DISC_RADIUS  0.035F
#define GNY_SKY_DISC_HORIZON 0.62F
#define GNY_SKY_DISC_RISE    0.44F

/** Two flat rings under the disc, a glow with an edge. */
#define GNY_SKY_HALO_ALPHA 0.10F
#define GNY_SKY_HALO_INNER 1.9F
#define GNY_SKY_HALO_OUTER 3.2F

/** How much the moon's craters darken it, and how much less it glows than the sun. See _gny_sky_disc_draw. */
#define GNY_SKY_CRATER_SHADE 0.82F
#define GNY_SKY_MOON_HALO    0.35F

/** Stars. Confined to the upper band, because the hills cover the rest. */
#define GNY_SKY_STAR_COUNT   96
#define GNY_SKY_STAR_SEED    0x57A45
#define GNY_SKY_STAR_BAND    0.55F
#define GNY_SKY_STAR_MIN     1.0F
#define GNY_SKY_STAR_MAX     2.5F
#define GNY_SKY_TWINKLE_SPEED 1.7F
#define GNY_SKY_STAR_COLOR   ((NYA_Color){ 1.0F, 0.98F, 0.90F, 1.0F })

/** Clouds: flat blobs with a straight underside, drifting. */
#define GNY_SKY_CLOUD_COUNT  7
#define GNY_SKY_CLOUD_SEED   0xC10D
#define GNY_SKY_CLOUD_TOP    0.08F
#define GNY_SKY_CLOUD_BOTTOM 0.42F
#define GNY_SKY_CLOUD_MIN    0.022F
#define GNY_SKY_CLOUD_MAX    0.055F
#define GNY_SKY_CLOUD_SPEED  6.0F
/**
 * Opaque: a cloud is overlapping shapes, and translucent parts double-blend at the seams. Softness comes from
 * GNY_SKY_CLOUD_TINT instead.
 * */
#define GNY_SKY_CLOUD_ALPHA  1.0F
#define GNY_SKY_CLOUD_COLOR  ((NYA_Color){ 1.0F, 0.99F, 0.97F, 1.0F })

/** How far clouds tint toward the horizon colour. A white cloud at dusk looks like a static backdrop. */
#define GNY_SKY_CLOUD_TINT 0.55F

#define GNY_RIDGE_FAR  ((NYA_Color){ 0.17F, 0.16F, 0.28F, 1.0F })
#define GNY_RIDGE_NEAR ((NYA_Color){ 0.11F, 0.12F, 0.20F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HUD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define GNY_UI_FONT      NYA_ASSET_FONTS_ALDRICH_TTF
#define GNY_UI_FONT_SIZE 17.0F

/** How often the startup script's optional hook runs, in seconds. See gny_world_script_tick. */
#define GNY_LUA_TICK_INTERVAL_S 1.0F

/* The panels themselves take their look from `engine.ui` in the config, see NYA_UIStyle. */
#define GNY_UI_MARGIN  16.0F
#define GNY_UI_WARNING ((NYA_Color){ 0.95F, 0.42F, 0.32F, 1.0F })

/** The HUD's status panel, and the space kept free at the top right for the debug overlay. */
#define GNY_UI_PANEL_WIDTH   300.0F
#define GNY_UI_OVERLAY_WIDTH 340.0F

/** The overlay's trace page, with its padding. */
#define GNY_UI_TRACE_WIDTH 500.0F

/** Frames `k` captures, and where the Chrome trace goes. Two seconds at 60 Hz. */
#define GNY_TRACE_CAPTURE_FRAMES 120
#define GNY_TRACE_CAPTURE_PATH   "./logs/trace.json"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MENUS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/* Registered as "menu" and "menu_title" by gny_fonts_register, at the sizes engine.ui asks for at scale 1. */
#define GNY_MENU_FONT       NYA_ASSET_FONTS_ALDRICH_TTF
#define GNY_MENU_TITLE_SIZE 44.0F
#define GNY_MENU_ITEM_SIZE  22.0F

/* The panel is centred and as tall as its rows, scrolling past the window. Pixels at the UI's reference height. */
#define GNY_MENU_WIDTH 420.0F

/* The look panel beside the pause menu: its width, what the animate toggle turns on, the largest scale it offers,
 * and the sheet its skinned look is cut from. */
#define GNY_LOOK_WIDTH      300.0F
#define GNY_UI_TRANSITION_S 0.08F
#define GNY_UI_APPEAR_S     0.15F
#define GNY_UI_SCALE_MAX    2.0F
#define GNY_MENU_SHEET      NYA_ASSET_UI_SHEET_PNG

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * 3D DEMO
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Metres, not pixels. The 2D world converts at NYA_PHYSICS2D_PIXELS_PER_METER (thirty-two); the 3D
 * one has no pixel scale, so these are real sizes. A one metre cube is a crate.
 */

/** Full edge length of the cube, in metres. */
#define GNY_CUBE3D_SIZE 1.0F

/** How far above the ground it starts, so the first thing the scene shows is the solver working. */
#define GNY_CUBE3D_DROP_HEIGHT 4.0F

#define GNY_CUBE3D_GROUND_SIZE      16.0F
/** Where the orbit starts: yaw and pitch in radians, range in metres. Twenty metres puts the whole basin in shot. */
#define GNY_CUBE3D_ORBIT_YAW   0.7F
#define GNY_CUBE3D_ORBIT_PITCH 0.45F
#define GNY_CUBE3D_ORBIT_RANGE 20.0F

/** Radians of orbit per pixel of mouse motion. */
#define GNY_CUBE3D_ORBIT_SENSITIVITY 0.006F

#define GNY_CUBE3D_ZOOM_STEP 1.12F
#define GNY_CUBE3D_RANGE_MIN 2.5F
#define GNY_CUBE3D_RANGE_MAX 45.0F

/**
 * Angular impulse per pixel of drag. Small, since the cube's angular momentum is low (a metre cube at
 * 400 kg/m³); ten times this sends it tumbling off.
 * */
#define GNY_CUBE3D_SPIN_STRENGTH 0.02F

/** How far the picking ray reaches, in metres. Past the far edge of the ground. */
#define GNY_CUBE3D_PICK_RANGE 100.0F

/* A flat palette: saturated objects on a light ground. */
#define GNY_CUBE3D_COLOR        ((NYA_Color){ 0.95F, 0.52F, 0.24F, 1.0F })
#define GNY_CUBE3D_HELD_COLOR   ((NYA_Color){ 0.99F, 0.82F, 0.34F, 1.0F })
/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TERRAIN 3D
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The 3D scene's ground: a heightmap from fBm noise, drawn as flat triangles and collided against as
 * a heightfield.
 */

/**
 * Cells per side (the vertex grid is one larger). Thirty-two draws 2048 flat triangles, 6144 vertices of the 3D
 * batch's 16384, leaving room for cubes in the same flush. Sixty-four would split into two draw calls before any
 * cube was queued.
 * */
#define GNY_TERRAIN3D_RES 32

/** Metres across; the camera limits are fitted to it. */
#define GNY_TERRAIN3D_EXTENT GNY_CUBE3D_GROUND_SIZE

#define GNY_TERRAIN3D_CELL (GNY_TERRAIN3D_EXTENT / (f32)GNY_TERRAIN3D_RES)

/**
 * How far a terrain chunk has to be from the viewer before it drops a detail level, doubling per level. In
 * chunk widths: the size of a chunk against the viewing distance decides when detail is worth it, not the size
 * of the world.
 * */
#define GNY_TERRAIN3D_LOD_DISTANCE (GNY_TERRAIN3D_CELL * (f32)NYA_TERRAIN3D_CHUNK_CELLS * 4.0F)

/**
 * Metres from lowest to highest, roughly; fBm is not strictly bounded. Gentle enough that cubes settle
 * differently each time without sliding off.
 * */
#define GNY_TERRAIN3D_AMPLITUDE 2.5F

/**
 * Where the rim starts lifting, as a fraction from centre to edge. Noise inside, rim outside, so they never
 * fight.
 * */
#define GNY_TERRAIN3D_RIM_START 0.55F

/**
 * How high the rim stands, in units of GNY_TERRAIN3D_AMPLITUDE. Above one, so it clears the noise and keeps the
 * pile in without invisible walls.
 * */
#define GNY_TERRAIN3D_RIM_HEIGHT 1.15F

/** World distance per unit of noise input. Lower is broader hills. */
#define GNY_TERRAIN3D_FREQUENCY 0.09F

#define GNY_TERRAIN3D_OCTAVES    4
#define GNY_TERRAIN3D_LACUNARITY 2.0F
#define GNY_TERRAIN3D_GAIN       0.5F

/** Slope friction for cubes. High, so landings settle instead of sliding. */
#define GNY_TERRAIN3D_FRICTION 0.85F

/**
 * The bands the surface is coloured in, low to high. Chosen per triangle, so boundaries fall on triangle edges
 * and read as facets.
 * */
#define GNY_TERRAIN3D_COLOR_LOW  ((NYA_Color){ 0.42F, 0.56F, 0.38F, 1.0F })
#define GNY_TERRAIN3D_COLOR_MID  ((NYA_Color){ 0.55F, 0.67F, 0.42F, 1.0F })
#define GNY_TERRAIN3D_COLOR_HIGH ((NYA_Color){ 0.72F, 0.74F, 0.58F, 1.0F })
/* Pale dune. */
#define GNY_TERRAIN3D_COLOR_PEAK ((NYA_Color){ 0.86F, 0.83F, 0.71F, 1.0F })

/** Where each band starts, as a fraction of the height range. */
#define GNY_TERRAIN3D_BAND_MID  0.35F
#define GNY_TERRAIN3D_BAND_HIGH 0.62F
#define GNY_TERRAIN3D_BAND_PEAK 0.84F

/**
 * Per-triangle colour jitter around its band, so bands do not look like flat sheets. Hashed from the cell
 * index, so it does not shimmer.
 * */
#define GNY_TERRAIN3D_SHADE_JITTER 0.06F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FALLING CUBES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How many fall at once. Twenty-four is enough for a pile and few enough to see each one land. */
#define GNY_TERRAIN3D_CUBE_COUNT 24

/** Full edge length, metres, sampled between these per cube so the pile is not a stack of clones. */
#define GNY_TERRAIN3D_CUBE_MIN_SIZE 0.45F
#define GNY_TERRAIN3D_CUBE_MAX_SIZE 0.95F

/** Metres above the highest ground the drop starts from. */
#define GNY_TERRAIN3D_CUBE_DROP 6.0F

/** How far apart in height consecutive cubes start, so they arrive in a stream rather than a sheet. */
#define GNY_TERRAIN3D_CUBE_STAGGER 1.1F

/** Metres from the centre the drop is spread over. Well inside the terrain, so nothing lands off it. */
#define GNY_TERRAIN3D_CUBE_SPREAD (GNY_TERRAIN3D_EXTENT * 0.32F)

#define GNY_TERRAIN3D_CUBE_DENSITY     400.0F
#define GNY_TERRAIN3D_CUBE_FRICTION    0.6F
#define GNY_TERRAIN3D_CUBE_RESTITUTION 0.15F

/** Radians per second of initial tumble, either side, so no two land on the same face. */
#define GNY_TERRAIN3D_CUBE_SPIN 3.0F

/** Below this a cube has fallen off the world and is put back at the top. */
#define GNY_TERRAIN3D_CUBE_KILL_Y (-12.0F)

/*
 * Glass cubes: translucent solids, the case sorted transparency exists for. The cel shader's tight highlight and
 * strong rim are what read as glass.
 */

/** One in this many cubes is glass. Four leaves enough opaque ones for the glass to be seen against. */
#define GNY_CUBE3D_GLASS_EVERY 4

/**
 * Pale and nearly neutral: saturated glass reads as plastic, and a blue tint would merge with the water. A
 * quarter alpha shows the far wall through the near one without three stacked turning opaque.
 * */
#define GNY_CUBE3D_GLASS_COLOR ((NYA_Color){ 0.80F, 0.88F, 0.86F, 0.26F })

/**
 * A glass material: hard highlight, tight bands, strong rim, no edge darkening. `metallic` is highlight
 * strength, `roughness` band softness.
 * */
#define GNY_CUBE3D_GLASS_METALLIC    0.95F
#define GNY_CUBE3D_GLASS_ROUGHNESS   0.12F
#define GNY_CUBE3D_GLASS_REFLECTANCE 1.0F

/**
 * How far the glass bends what is behind it. See NYA_Render3DMaterial.refraction. Small: it is a screen-space
 * offset, and large values look torn.
 * */
#define GNY_CUBE3D_GLASS_REFRACTION 0.45F

/** Blur levels the glass cubes cycle through: clear, lightly frosted, heavily frosted. */
#define GNY_CUBE3D_GLASS_BLURS \
    { 0.0F, 0.35F, 0.85F }

/** The palette the pile is coloured from. Saturated, so they read against the muted ground. */
#define GNY_TERRAIN3D_CUBE_COLORS                                                                                                            \
    {                                                                                                                                        \
        { 0.95F, 0.52F, 0.24F, 1.0F }, { 0.36F, 0.68F, 1.00F, 1.0F }, { 0.96F, 0.56F, 0.52F, 1.0F }, { 0.99F, 0.82F, 0.34F, 1.0F },           \
        { 0.55F, 0.82F, 0.55F, 1.0F }, { 0.72F, 0.56F, 0.92F, 1.0F },                                                                         \
    }

/** How far a sound gets from the 3D camera before it fades, in metres. About half the terrain. */
#define GNY_CUBE3D_EAR_DISTANCE (GNY_TERRAIN3D_EXTENT * 0.5F)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SKY 3D
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The 3D scene's own sky, shaded from a view ray so it turns with the camera. The 2D world keeps the
 * backdrop in system_sky.c; both read the same GNY_SkyState, so the two agree about where the sun is.
 */

/**
 * Below the horizon: distant land in haze. A desaturated green-grey near the terrain's low band reads as more
 * landscape; a dark ground made the small lit island float in black.
 * */
#define GNY_SKY3D_GROUND ((NYA_Color){ 0.34F, 0.39F, 0.35F, 1.0F })

/** Angular radius of the disc, radians. Life-size (0.0087) reads as a dot. */
#define GNY_SKY3D_SUN_ANGLE 0.055F

#define GNY_SKY3D_SUN_INTENSITY  1.0F
#define GNY_SKY3D_MOON_INTENSITY 0.55F

/** Halo exponent. Low is a wide glow, high a tight ring. See NYA_Render3DSky.sun_halo. */
#define GNY_SKY3D_SUN_HALO  48.0F
#define GNY_SKY3D_MOON_HALO 320.0F

/** Above one, so the horizon colour holds further up the sky than a linear ramp would put it. */
#define GNY_SKY3D_HORIZON_SOFTNESS 1.4F

/**
 * Width of the fade into the ground half, in sine-of-elevation units: wide enough to read as haze rather than a
 * drawn horizon.
 * */
#define GNY_SKY3D_GROUND_BLEND 0.14F

/**
 * Fog density per metre, fogging the far rim to about a third and leaving the foreground clear. A real
 * landscape's density would be invisible across sixteen metres.
 * */
#define GNY_SKY3D_FOG_DENSITY 0.022F

/** Thins with altitude, so fog pools in the basin. */
#define GNY_SKY3D_FOG_HEIGHT_FALLOFF 0.10F

/** Tint toward the light. High enough to see at dawn, low enough that midday does not look filtered. */
#define GNY_SKY3D_FOG_SUN_AMOUNT 0.45F

/* Water, the scene's translucent geometry, there to exercise transparency ordering. */

/** Where the surface sits. Below the terrain's mid height, so it pools in the basin rather than flooding. */
#define GNY_CUBE3D_WATER_LEVEL (-0.55F)

/** Three panes, so the sort has more than one pair to get right. */
#define GNY_CUBE3D_WATER_LAYERS 3

/** How far apart they sit. Small: the point is that they overlap on screen, not that they are separate. */
#define GNY_CUBE3D_WATER_GAP 0.10F

/** Narrower than the terrain, so the basin's rim is visibly above the surface. */
#define GNY_CUBE3D_WATER_SIZE (GNY_TERRAIN3D_EXTENT * 0.62F)

/** Alpha well below one, into the sorted stream. Three panes at a fifth each blend to about half. */
#define GNY_CUBE3D_WATER_COLOR ((NYA_Color){ 0.26F, 0.58F, 0.76F, 0.20F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FIRE AND SMOKE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * A plume made of billboards, the volumetric effect a renderer without depth prepass or compute can
 * do. Fire adds and smoke blends, so they are two systems; see nya_render3d_billboard.
 */

/**
 * The soft radial sprite for the plume: white with a smooth alpha falloff, so the burst sets the colour and one
 * texture serves fire and smoke.
 * */
#define GNY_CUBE3D_PUFF_TEXTURE NYA_ASSET_TEXTURES_PUFF_PNG

/** Up on the rim, outside GNY_TERRAIN3D_CUBE_SPREAD, so no crate lands in the fire. */
#define GNY_CUBE3D_PLUME_X (-6.2F)
#define GNY_CUBE3D_PLUME_Z 6.2F

/** How often a puff is emitted, in seconds. */
#define GNY_CUBE3D_PLUME_INTERVAL_S 0.045F

#define GNY_CUBE3D_FIRE_POOL  256
#define GNY_CUBE3D_SMOKE_POOL 256

/** Per emission. Few, because they are emitted often. */
#define GNY_CUBE3D_FIRE_COUNT  2
#define GNY_CUBE3D_SMOKE_COUNT 2

/** Fire is short-lived and small; smoke outlives it and grows. */
#define GNY_CUBE3D_FIRE_LIFETIME  ((f32x2){ 0.35F, 0.7F })
#define GNY_CUBE3D_SMOKE_LIFETIME ((f32x2){ 1.4F, 2.6F })

/* Sized to read from the default orbit twenty metres out: a bonfire, not a candle. */
#define GNY_CUBE3D_FIRE_SIZE      ((f32x2){ 0.35F, 0.7F })
#define GNY_CUBE3D_FIRE_SIZE_END  ((f32x2){ 0.05F, 0.15F })
#define GNY_CUBE3D_SMOKE_SIZE     ((f32x2){ 0.7F, 1.2F })
#define GNY_CUBE3D_SMOKE_SIZE_END ((f32x2){ 2.2F, 3.4F })

#define GNY_CUBE3D_FIRE_SPEED  ((f32x2){ 1.4F, 3.0F })
#define GNY_CUBE3D_SMOKE_SPEED ((f32x2){ 0.7F, 1.6F })

/** Upward in a narrow cone; a wide spread reads as an explosion. */
#define GNY_CUBE3D_PLUME_SPREAD 0.35F

/** Negative gravity: both rise. Smoke rises slower, having cooled. */
#define GNY_CUBE3D_FIRE_GRAVITY  ((f32x3){ 0.0F, 2.2F, 0.0F })
#define GNY_CUBE3D_SMOKE_GRAVITY ((f32x3){ 0.0F, 0.9F, 0.0F })

/**
 * Fire colours just above one: additive overlaps stay saturated under the tonemap's shoulder and cross the bloom
 * threshold. Higher values turn every overlap white. Tune this, not the bloom, when fire looks like a searchlight.
 * */
#define GNY_CUBE3D_FIRE_COLOR_START ((NYA_Color){ 1.15F, 0.52F, 0.14F, 1.0F })
#define GNY_CUBE3D_FIRE_COLOR_END   ((NYA_Color){ 0.55F, 0.09F, 0.02F, 0.0F })

/** Smoke: alpha well below one, which routes it into the sorted transparent stream. */
#define GNY_CUBE3D_SMOKE_COLOR_START ((NYA_Color){ 0.26F, 0.24F, 0.24F, 0.55F })
#define GNY_CUBE3D_SMOKE_COLOR_END   ((NYA_Color){ 0.46F, 0.46F, 0.48F, 0.0F })

/*
 * Not done yet: a soft-particle fade. Billboards intersecting the ground show a hard line, and fixing it needs
 * scene depth as a texture, which the renderer does not produce.
 */

/* The fire's loop, traced through the terrain like the impacts. How it sounds is in engine.nya under audio. */

#define GNY_CUBE3D_FIRE_SOUND NYA_ASSET_SOUNDS_FIRE_WAV

#define GNY_CUBE3D_FIRE_GAIN 0.7F

/** A bonfire's width, metres: the rim hides it gradually, not all at once. */
#define GNY_CUBE3D_FIRE_RADIUS 0.8F

/** In when the scene opens, out when it closes. */
#define GNY_CUBE3D_FIRE_FADE_MS 600

/** Seeds the per-cube size, position and colour hash. Arbitrary; changing it reshuffles the pile. */
#define GNY_TERRAIN3D_CUBE_SEED 0xC0BE5

/** The terrain's hash seed, mixed with the world seed so R gives a different landscape. */
#define GNY_TERRAIN3D_SEED 0x7E44A1

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SPARKS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Live sparks across every impact. A ceiling like NYA_PHYSICS2D_MAX_HITS: past it they are dropped and counted. */
#define GNY_SPARK_POOL 2048

/** Sparks per impact, from the quietest that qualifies to the loudest. */
#define GNY_SPARK_MIN 12
#define GNY_SPARK_MAX 64

/** Half angle of the spray, in radians: about fifty degrees either side of up. */
#define GNY_SPARK_SPREAD 0.9F

/** Stronger than world gravity, so sparks arc tightly. */
#define GNY_SPARK_GRAVITY 900.0F

/** Start and end size of a spark in world units. Ends above zero so it fades instead of popping. */
#define GNY_SPARK_SIZE_START ((f32x2){ 3.5F, 8.0F })
#define GNY_SPARK_SIZE_END   ((f32x2){ 0.5F, 2.0F })

#define GNY_SPARK_COLOR ((NYA_Color){ 1.0F, 0.85F, 0.45F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIGHTING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Light where no crate reaches, as a multiplier. Not zero, which renders as a black rectangle. */
#define GNY_AMBIENT_LIGHT ((NYA_Color){ 0.34F, 0.36F, 0.46F, 1.0F })

/** World units a crate's glow reaches. A few crate widths, so overlapping ones pool. */
#define GNY_BOX_LIGHT_RADIUS 190.0F

/** Above one, so the crate over-brightens and reads as the source. */
#define GNY_BOX_LIGHT_INTENSITY 1.35F

#define GNY_BOX_LIGHT_COLOR ((NYA_Color){ 1.0F, 0.82F, 0.55F, 1.0F })

/* 3D demo dust, in metres: chips of ground, centimetre scale. */

/** Live dust particles at once. Small: one cube makes one impact at a time. */
#define GNY_CUBE3D_DUST_POOL 512

/** Particles per landing, from the softest that registers to the hardest. */
#define GNY_CUBE3D_DUST_MIN 10
#define GNY_CUBE3D_DUST_MAX 48

/** Metres per second. Fast enough to leave the contact point before gravity takes them. */
#define GNY_CUBE3D_DUST_SPEED ((f32x2){ 1.2F, 4.5F })

/** Metres. A chip of ground, not a boulder. */
#define GNY_CUBE3D_DUST_SIZE ((f32x2){ 0.03F, 0.09F })

/** Negative: y is up in 3D. */
#define GNY_CUBE3D_DUST_GRAVITY (-9.81F)

#define GNY_CUBE3D_DUST_COLOR ((NYA_Color){ 0.72F, 0.68F, 0.60F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TILEMAP
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Where the demo map's tile (0, 0) sits. The map is 20x12 tiles of 32 units, centred on x, with its solid bottom
 * rows just above GNY_TERRAIN_BASE_Y.
 * */
#define GNY_TILEMAP_ORIGIN ((f32x2){ -320.0F, GNY_TERRAIN_BASE_Y - 384.0F - 30.0F })

/** The kind tilemap colliders are spawned as, so they can be found and cleared as a group. */
#define GNY_TILEMAP_COLLIDER_KIND GNY_ENTITY_TILEMAP

/*
 * ─────────────────────────────────────────────────────────
 * ONE-WAY LEDGES
 * ─────────────────────────────────────────────────────────
 *
 * Three platforms above the terrain, placed where the opening view already looks. See entity_ledge.c.
 */

/** Full width and height of a ledge, world units: wide enough for a crate, thin enough to read as a shelf. */
#define GNY_LEDGE_SIZE ((f32x2){ 220.0F, 18.0F })

/** How far above GNY_TERRAIN_BASE_Y the lowest ledge sits, and the step up to each one after it. */
#define GNY_LEDGE_BASE_LIFT 190.0F
#define GNY_LEDGE_STEP_LIFT 150.0F

/** Horizontal placement of the three, relative to the middle of the opening view. */
#define GNY_LEDGE_LEFT_X   (-520.0F)
#define GNY_LEDGE_MIDDLE_X 0.0F
#define GNY_LEDGE_RIGHT_X  520.0F

/** How far the moving one slides, and how long one leg of the patrol takes. */
#define GNY_LEDGE_PATROL_DISTANCE 420.0F
#define GNY_LEDGE_PATROL_SECONDS  3.5F

/** High, so a crate rides the moving platform instead of sliding off. */
#define GNY_LEDGE_FRICTION 0.95F

/**
 * How long a drop-through window stays open, seconds.
 * */
#define GNY_LEDGE_DROP_SECONDS 0.45F

#define GNY_LEDGE_COLOR         ((NYA_Color){ 0.36F, 0.40F, 0.52F, 1.0F })
#define GNY_LEDGE_COLOR_MOVING  ((NYA_Color){ 0.46F, 0.52F, 0.68F, 1.0F })
#define GNY_LEDGE_EDGE_COLOR    ((NYA_Color){ 0.82F, 0.88F, 1.0F, 1.0F })
#define GNY_LEDGE_EDGE_THICKNESS 2.5F

/** The marker parented to the moving ledge: how far under it, and its size against a tile. */
#define GNY_LEDGE_MARKER_LIFT  22.0F
#define GNY_LEDGE_MARKER_SCALE 0.5F

/** The marker's sprite sheet: the tileset's four cells, played back and forth. */
#define GNY_LEDGE_MARKER_SHEET  NYA_ASSET_MAPS_TILESET_PNG
#define GNY_LEDGE_MARKER_CELL   32
#define GNY_LEDGE_MARKER_FRAMES 4
#define GNY_LEDGE_MARKER_FPS    6.0F

/** Sparks puffed from the marker each time the animation reaches its last cell. */
#define GNY_LEDGE_MARKER_SPARKS 10

/*
 * ─────────────────────────────────────────────────────────
 * NETWORKING
 * ─────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * THE 3D SCENE'S MODEL
 * ─────────────────────────────────────────────────────────
 */

/** The FBX the 3D scene loads. Read with ufbx at runtime; see NYA_ASSET_TYPE_MESH. */
#define GNY_CUBE3D_MODEL NYA_ASSET_MODELS_CUBIE_FBX

/**
 * Drawn at its own size: ufbx converts to metres and both models span about -1 to 1. test_asset_mesh asserts
 * this, so a re-export at another scale fails a test instead of shrinking the scene.
 * */
#define GNY_CUBE3D_MODEL_SCALE 1.0F

/** How far to one side of the cube it stands, so the two are both visible rather than intersecting. */
#define GNY_CUBE3D_MODEL_OFFSET 2.5F

/** Lifted by one: the origin is the centre and the lowest vertex is a unit below. */
#define GNY_CUBE3D_MODEL_LIFT 1.0F

#define GNY_CUBE3D_MODEL_COLOR ((NYA_Color){ 0.42F, 0.63F, 0.88F, 1.0F })

/*
 * The second model, beside the cube, so two separate meshes share one batch, light and draw call. A path that
 * reused one buffer would show two identical shapes.
 */
#define GNY_CUBE3D_PILL NYA_ASSET_MODELS_PILL_FBX

/** Its own scale; two FBX files need not share units. */
#define GNY_CUBE3D_PILL_SCALE 1.0F

/** Mirrored across the cube from GNY_CUBE3D_MODEL_OFFSET, so the three stand in a row. */
#define GNY_CUBE3D_PILL_OFFSET (-GNY_CUBE3D_MODEL_OFFSET)

/** Half the pill's height: pill.fbx spans about 1.73 either side of its origin on y once its node stretches it. */
#define GNY_CUBE3D_PILL_LIFT 1.75F

#define GNY_CUBE3D_PILL_COLOR ((NYA_Color){ 0.96F, 0.56F, 0.52F, 1.0F })

/*
 * The skinned model: a two bone bar that bends, posed from its clips every tick. No body, since its shape
 * changes with the pose.
 */
#define GNY_CUBE3D_BENDER NYA_ASSET_MODELS_BENDER_FBX

/** Ground position on xz, in front of the row, where the default orbit sees it side on. */
#define GNY_CUBE3D_BENDER_X (-2.0F)
#define GNY_CUBE3D_BENDER_Z 3.5F

#define GNY_CUBE3D_BENDER_SCALE 1.0F

/** bender.fbx spans a metre either side of its origin, so this stands it on the ground. */
#define GNY_CUBE3D_BENDER_LIFT 1.0F

#define GNY_CUBE3D_BENDER_COLOR ((NYA_Color){ 0.93F, 0.78F, 0.36F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────
 * THE 3D SCENE'S LIGHTS
 * ─────────────────────────────────────────────────────────
 */

/** Two lamps orbiting the scene. Moving, since a static coloured light looks like coloured ambient. */
#define GNY_CUBE3D_LAMP_COUNT  2
#define GNY_CUBE3D_LAMP_RADIUS 4.0F
#define GNY_CUBE3D_LAMP_HEIGHT 2.2F
#define GNY_CUBE3D_LAMP_SPEED  0.7F

/** Each lamp's reach, less than the ground, so the falloff shows. */
#define GNY_CUBE3D_LAMP_RANGE 7.0F

#define GNY_CUBE3D_LAMP_INTENSITY 2.4F

/** Warm and cool, told apart by colour. */
#define GNY_CUBE3D_LAMP_A_COLOR ((NYA_Color){ 1.00F, 0.62F, 0.28F, 1.0F })
#define GNY_CUBE3D_LAMP_B_COLOR ((NYA_Color){ 0.36F, 0.68F, 1.00F, 1.0F })

/** The little sphere drawn at each lamp, and how strongly it glows. See NYA_Render3DMaterial.emission. */
#define GNY_CUBE3D_LAMP_MARKER_RADIUS 0.16F
#define GNY_CUBE3D_LAMP_EMISSION      1.6F


/*
 * ─────────────────────────────────────────────────────────
 * THE 3D SCENE'S SHADOWS
 * ─────────────────────────────────────────────────────────
 */

/**
 * The ambient on tops takes this much of the sky's zenith colour, and on undersides this much of the warm light
 * bounced off the sand. See NYA_Render3DLight.sky.
 * */
#define GNY_CUBE3D_AMBIENT_SKY_MIX    0.2F
#define GNY_CUBE3D_AMBIENT_GROUND_MIX 0.35F
#define GNY_CUBE3D_AMBIENT_BOUNCE     ((NYA_Color){ 0.92F, 0.76F, 0.58F, 1.0F })

/** How dark a shadow goes. Well under one, since a full shadow reads as a hole. */
#define GNY_CUBE3D_SHADOW_STRENGTH 0.45F

/**
 * How far casters reach from the scene's centre, in world units: the terrain's half-diagonal plus room for cubes
 * above the rim. Too small drops edge casters; too large puts the near cascade back in empty air. See
 * NYA_Render3DShadowFit.near_distance.
 * */
#define GNY_CUBE3D_SHADOW_SUBJECT_REACH (GNY_TERRAIN3D_EXTENT * 0.85F)

/*
 * ─────────────────────────────────────────────────────────
 * THE 3D SCENE'S EFFECTS
 * ─────────────────────────────────────────────────────────
 *
 * Their look comes from NYA_CONFIG.engine.renderer. These are how the scene drives them.
 */

/** A 2x2 sheet in reading order. Scuffs and blob shadows both use its soft round third cell. */
#define GNY_CUBE3D_DECAL_TEXTURE NYA_ASSET_TEXTURES_DECALS_PNG
#define GNY_CUBE3D_DECAL_BLOB    2

/** Impact marks kept at once. The oldest is painted over. */
#define GNY_CUBE3D_MARK_COUNT 48

/** How hard a landing leaves a mark, in the landing strength the dust uses. Soft settling leaves none. */
#define GNY_CUBE3D_MARK_STRENGTH 0.3F

/** Seconds a mark lasts, shrinking away over its last third. */
#define GNY_CUBE3D_MARK_LIFETIME_S 14.0F

/** A landing's scuff at full strength; weaker landings scale the alpha down. */
#define GNY_CUBE3D_SCUFF_COLOR ((NYA_Color){ 0.22F, 0.18F, 0.16F, 0.30F })

/** The blob under each of the three props: how dark, how wide against the prop, and how high it still shows. */
#define GNY_CUBE3D_BLOB_COLOR ((NYA_Color){ 0.10F, 0.08F, 0.14F, 0.45F })
#define GNY_CUBE3D_BLOB_SCALE 1.5F
#define GNY_CUBE3D_BLOB_REACH 4.0F

/** Camera speed, metres per second, at which speed lines start and at which they reach the configured amount. */
#define GNY_CUBE3D_SPEED_LINES_START 6.0F
#define GNY_CUBE3D_SPEED_LINES_FULL  30.0F

/** What `6` switches the lines to when the config has them off. */
#define GNY_CUBE3D_SPEED_LINES_AMOUNT 0.8F

/** World units per second a networked player moves. See gny_net_apply_command. */
#define GNY_PLAYER_SPEED 220.0F

/** How far apart players spawn, so two joining at once do not start inside each other. */
#define GNY_PLAYER_SPAWN_SPACING 64.0F

/**
 * How much faster than the configured player speed the server lets anyone move before it cuts them short and counts
 * it against them. Room for the speed being raised in the config file while the game runs.
 * */
#define GNY_NET_SPEED_HEADROOM 2.0F

/** Positions cross the network in sixteenths of a pixel, finer than anything drawn. See NYA_NetServerConfig.position_bits. */
#define GNY_NET_POSITION_BITS 4

/** Where the server keeps the key players pin, and where a player keeps the one a server recognises them by, under the save root. */
#define GNY_NET_SERVER_IDENTITY "net/server_identity.nya"
#define GNY_NET_PLAYER_IDENTITY "net/player_identity.nya"

/** Playback rate of the 3D demo's skinned bar and the 2D ledge marker. See GNY_ConfigGame.animation_speed. */
#define GNY_ANIMATION_SPEED 1.0F

/**
 * Drawn size of a player per side. Only gny_net_player_on_render reads it; players have no body. Smaller than
 * GNY_PLAYER_SPAWN_SPACING, so simultaneous spawns are visibly apart.
 * */
#define GNY_PLAYER_SIZE 24.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ROBOTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * One task for both brains: thrust toward a point. The drones fly it on screen with a nav waypoint as the point,
 * and training flies it offline with made up points, so these numbers are shared and a change retrains both.
 */

/** Drones flying the evolved genome, and all of them with the one DQN drone. */
#define GNY_ROBOT_NEAT_DRONES 5
#define GNY_ROBOT_DRONES      (GNY_ROBOT_NEAT_DRONES + 1)

#define GNY_ROBOT_ACCELERATION 900.0F
#define GNY_ROBOT_MAX_SPEED    260.0F

/** Fraction of velocity lost per second, so a brain that stops thrusting drifts to a halt. */
#define GNY_ROBOT_DRAG 1.5F

/** Offsets and speeds are divided by these before a brain sees them, so every sense sits in [-1, 1]. */
#define GNY_ROBOT_SENSE_RANGE 160.0F

/** Close enough to count as reached, for the DQN's terminal reward. */
#define GNY_ROBOT_REACH 12.0F

/** The fixed step both training loops simulate at, the game's own tick. */
#define GNY_ROBOT_TRAIN_DT (1.0F / 60.0F)

/** Steps in one scripted NEAT trial episode and in one DQN episode before it gives up on a target. */
#define GNY_ROBOT_TRIAL_STEPS   90
#define GNY_ROBOT_EPISODE_STEPS 120

/** What a brain senses (offset and velocity), and the DQN's choices: coast, or thrust one of eight ways. */
#define GNY_ROBOT_SENSES      4
#define GNY_ROBOT_DQN_ACTIONS 9

/** Seconds between training jobs. Each job runs whatever generations and gradient steps the rates have earned. */
#define GNY_ROBOT_TRAIN_INTERVAL_S 0.25F

/** The most one job may catch up on, so a long pause does not turn into one long job. */
#define GNY_ROBOT_MAX_GENERATIONS_PER_JOB 2
#define GNY_ROBOT_MAX_DQN_STEPS_PER_JOB   128

/** Gradient steps between two scorings of the DQN on the trial. */
#define GNY_ROBOT_DQN_SCORE_EVERY 500

/** Past this from the player a drone is brought back above them. */
#define GNY_ROBOT_RECALL_DISTANCE 1400.0F
#define GNY_ROBOT_RECALL_HEIGHT   240.0F

/** Cells of the nav grid over the 2D world, from this height down to the lowest the terrain reaches. */
#define GNY_ROBOT_NAV_CELL    40.0F
#define GNY_ROBOT_NAV_TOP     (-760.0F)
#define GNY_ROBOT_NAV_COLUMNS ((u32)((GNY_TERRAIN_HALF_WIDTH * 2.0F) / GNY_ROBOT_NAV_CELL))
#define GNY_ROBOT_NAV_ROWS    ((u32)((GNY_TERRAIN_BASE_Y + GNY_TERRAIN_AMPLITUDE - GNY_ROBOT_NAV_TOP) / GNY_ROBOT_NAV_CELL))

/** Cells ahead along the flow a drone aims at, so it cuts corners the grid would make it turn. */
#define GNY_ROBOT_NAV_LOOKAHEAD 2

/** Within this of the player, drones stop following the flow and circle. */
#define GNY_ROBOT_ORBIT_RADIUS 70.0F
#define GNY_ROBOT_ORBIT_SPEED  0.9F

#define GNY_ROBOT_SIZE       14.0F
#define GNY_ROBOT_NEAT_COLOR ((NYA_Color){ 0.45F, 0.95F, 0.60F, 1.0F })
#define GNY_ROBOT_DQN_COLOR  ((NYA_Color){ 1.00F, 0.70F, 0.30F, 1.0F })

/** The DQN colour light enough to read on a dark panel. */
#define GNY_ROBOT_DQN_TEXT ((NYA_Color){ 0.95F, 0.66F, 0.32F, 1.0F })

/** Where the best genome and the run history live under the save root. */
#define GNY_ROBOT_SAVE_FILE     "robots.nya"
#define GNY_ROBOT_DATABASE_FILE "robots.db"
#define GNY_ROBOT_SAVE_VERSION  1

/** The training panel under the HUD's status panel, and the genome drawn under that. */
#define GNY_ROBOT_PANEL_WIDTH  420.0F
#define GNY_ROBOT_BRAIN_HEIGHT 160.0F
#define GNY_ROBOT_NODE_RADIUS  7.0F
#define GNY_ROBOT_BRAIN_FILL   ((NYA_Color){ 0.06F, 0.07F, 0.09F, 0.94F })

/** Defaults for GNY_ConfigRobots fields left zero. */
#define GNY_ROBOT_POPULATION             48
#define GNY_ROBOT_GENERATIONS_PER_SECOND 2.0F
#define GNY_ROBOT_DQN_STEPS_PER_SECOND   240.0F
