/**
 * @file constants.h
 *
 * Every tunable number and colour in gnyame, in one place, grouped by what it affects rather than by
 * which file used to hold it. Belongs here: anything a person would change to make the game feel or
 * look different. Not here: anything the code derives or depends on structurally — layer ids, for
 * instance, are string-literal pointers, not constants, and stay with the layers they name.
 *
 * A deliberate deviation from nyangine's own arrangement, where each tunable sits `#ifndef`-guarded
 * beside the mechanism it governs for a `-D` override — right for a library, where a constant is part
 * of the module's contract. A game is tuned as a whole, numbers adjusted against each other in one
 * sitting, so they live here instead; the section comments below are what keep a name three hundred
 * lines from its use legible.
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

/* The ground is a polyline sampled at a fixed spacing: wider is cheaper and more angular; the crates
 * notice. */

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
 * How far a sound gets from the camera before it fades, in world units. About a third of the view, so
 * an edge-of-screen impact reads as audibly further than a central one. See NYA_AudioListener.reference_distance.
 * */
#define GNY_CAMERA_EAR_DISTANCE 400.0F

/**
 * Fraction of the remaining gap closed per tick — exponential easing, not constant speed, so it starts
 * fast and settles without overshoot or needing to know the target's speed. ~0.1 reads as keeping up,
 * not welded on.
 * */
#define GNY_CAMERA_FOLLOW_EASING 0.12F

/**
 * World units per second. Separate from the camera's pan speed since the two are in different terms:
 * the camera's scales with zoom, this is a speed in the world.
 * */
#define GNY_PLAYER_MOVE_SPEED 420.0F

/*
 * ── Secondary views ──
 *
 * A non-primary camera renders into its own texture, composited into a viewport: a window onto
 * somewhere else in the world.
 */

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

/** The handle the bloom pipeline asset is registered under. */
#define GNY_PIPELINE_BLOOM "gny_bloom_pipeline"

/**
 * Luminance a pixel needs before it glows, in the 2D world. This and the two below were badly out
 * because the bloom pass ran with an unbound uniform buffer (see gny_bloom_pipeline_ensure), so its
 * cbuffer read as zeros and the pass was a silent pass-through — the world whited out the moment the
 * binding was fixed. Half is roughly where a lit crate sits; the terrain fill (~0.15) stays under it,
 * and a settled crate darkened by the sleep tint drops below threshold and stops glowing.
 * */
#define GNY_BLOOM_2D_THRESHOLD 0.50F

/**
 * How hard the glow is added back, below one so a glowing crate keeps its edges and colour instead of
 * becoming a white blob. Was 2.2, tuned for a threshold of 0.22 that no longer catches the scene.
 * */
#define GNY_BLOOM_2D_INTENSITY 0.70F

/**
 * How far apart the kernel's samples sit, in pixels. effect_bloom.frag.hlsl's five-by-five kernel steps
 * `±2` texels, so the true texel size gives a halo two pixels wide — correct but invisible at any real
 * window size. A multiple spreads the same 25 taps over a radius worth seeing; too large and the taps
 * separate into a visible boxy star. Three is a halo without visible taps.
 * */
#define GNY_BLOOM_2D_SPREAD 3.0F

/*
 * ── The 3D scene's own bloom ──
 *
 * Separate numbers: the 2D world is dim and darkens as crates settle (low threshold, high intensity);
 * the 3D scene is a lit daylight landscape where a 0.22 threshold makes almost every pixel "bright".
 * These were never caught either, for the same unbound-uniform reason as above — the pass ran with a
 * zeroed cbuffer, i.e. zero intensity, until the binding was fixed.
 */

/**
 * Just under where the brightest *lit* surfaces land, not above them: mesh3d_tonemap is identity to 0.6
 * and compresses above, so lit surfaces land under ~0.86 and emissive above ~0.95. The first value
 * (0.90) sat in the gap — arithmetically correct but nearly invisible, since the tonemap had already
 * solved the blowout problem. Now it sits inside the top of the lit range: the rim, sun highlights and
 * horizon catch a little glow while a saturated crate (luma ~0.6 in full sun) stays clear.
 * */
#define GNY_BLOOM_3D_THRESHOLD 0.78F

/** Around one, so what clears the threshold is visible. At 0.6 the little that got through was added
 *  back at under half strength, reading as switched off. */
#define GNY_BLOOM_3D_INTENSITY 1.10F

/** Tighter than the 2D world's, so a lamp gets a halo rather than a haze over the whole landscape. */
#define GNY_BLOOM_3D_SPREAD 3.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * AUDIO
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Level the background track sits at, under the master and music gains. */
#define GNY_MUSIC_GAIN 0.45F

/**
 * Where the music slider starts, before the player touches it. Separate from GNY_MUSIC_GAIN (the
 * track's own mix level): this is a *setting*, written by gny_actions_init, edited by the pause menu,
 * and persisted.
 * */
#define GNY_MUSIC_VOLUME_DEFAULT 0.7F

/** How much one press of left or right on a volume row moves it. */
#define GNY_VOLUME_STEP 0.05F

/** Long enough that the track arrives rather than starting, since it has no lead-in of its own. */
#define GNY_MUSIC_FADE_IN_MS 1500

/**
 * Whether the background track starts silent. Loaded and started, then paused immediately, rather than
 * not played at all — `m` resumes the track and there must be something to resume.
 * */
#define GNY_MUSIC_START_MUTED true

/*
 * ── Impact audio ──
 *
 * Everything below the physics hit threshold never reaches the game at all; see physics2d.h. These
 * decide what the ones that do sound like.
 */

/**
 * Impacts given a voice per frame. Sixteen voices total, and a collapsing stack can produce dozens of
 * hits in one frame, so this is a budget, not a limit on what the world may do — per frame, not per
 * tick, so the spender sees the whole frame and can pick the *loudest* impacts. See sim.h.
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

/** Seeds for nya_ihash2, giving the motes their positions and the crates their sizes. Arbitrary, and
 *  different from each other only so the two don't correlate. */
#define GNY_MOTE_SEED 0x5EED
#define GNY_BOX_SEED  0xC4A7E

/*
 * ── The day/night cycle ──
 *
 * GNY_SKY_TOP and GNY_SKY_BOTTOM are gone: the gradient's two colours now come from the keyframe table
 * in system_sky.c, since they change through the day. What's left here is the shape of the cycle.
 */

/** How long a full day takes, in seconds. Short enough to see the whole cycle without waiting. */
#define GNY_DAY_LENGTH_S 120.0F

/** Where the demo starts in the day. 0.32 is mid morning, so the first frame is lit and not black. */
#define GNY_SKY_START_PHASE 0.32F

/** Lowest the sun is allowed to sit, as a sine of its arc — a perfectly horizontal light lands along
 *  the ground plane, lighting nothing and degenerating the shadow volume. */
#define GNY_SKY_MIN_ELEVATION 0.12F

/** The disc: its size as a fraction of the window, and the path it travels. */
#define GNY_SKY_DISC_RADIUS  0.035F
#define GNY_SKY_DISC_HORIZON 0.62F
#define GNY_SKY_DISC_RISE    0.44F

/** Two flat rings under the disc. A cartoon glow has an edge, which is the point of the style. */
#define GNY_SKY_HALO_ALPHA 0.10F
#define GNY_SKY_HALO_INNER 1.9F
#define GNY_SKY_HALO_OUTER 3.2F

/** How much the moon's craters darken it, and how much less it glows than the sun. No crescent constant
 *  any more; see _gny_sky_disc_draw for why that occlusion trick can't work over a halo. */
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
 * Opaque, and it has to be: a cloud is three circles and a rectangle overlapping, and translucent parts
 * *double-blend* where they overlap, so every seam draws itself, plainly visible at 0.85. Softness
 * comes from GNY_SKY_CLOUD_TINT instead — mixing toward the horizon colour, from a paler colour rather
 * than a transparent one.
 * */
#define GNY_SKY_CLOUD_ALPHA  1.0F
#define GNY_SKY_CLOUD_COLOR  ((NYA_Color){ 1.0F, 0.99F, 0.97F, 1.0F })

/** How far the clouds are tinted toward the horizon colour: a white cloud at dusk gives away a static
 *  backdrop, and since clouds are opaque this alone keeps them from reading as cut-out paper. */
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

/** The larger face registered as "title". One face at two sizes is two atlases; see render_font.h. */
#define GNY_UI_TITLE_FONT_SIZE 28.0F

/** How often the startup script's optional hook runs, seconds. Not per frame — see gny_world_script_tick. */
#define GNY_LUA_TICK_INTERVAL_S 1.0F

#define GNY_UI_MARGIN  16.0F
#define GNY_UI_PADDING 12.0F

#define GNY_UI_PANEL   ((NYA_Color){ 0.04F, 0.05F, 0.07F, 0.78F })
#define GNY_UI_BORDER  ((NYA_Color){ 0.35F, 0.62F, 0.42F, 0.55F })
#define GNY_UI_TEXT    ((NYA_Color){ 0.90F, 0.92F, 0.95F, 1.0F })
#define GNY_UI_DIM     ((NYA_Color){ 0.58F, 0.62F, 0.70F, 1.0F })
#define GNY_UI_WARNING ((NYA_Color){ 1.0F, 0.72F, 0.30F, 1.0F })

/*
 * ── Frame trace ──
 *
 * The panel `t` opens: every perf span the previous frame recorded, indented by nesting depth.
 */

/** Widest the trace panel gets. Span names are short and the numbers are right aligned inside it. */
#define GNY_TRACE_WIDTH 420.0F

/** Pixels of indent per nesting level, so the shape of the frame is readable at a glance. */
#define GNY_TRACE_INDENT 14.0F

/** Spans drawn. A frame with more than this has a deeper story than a HUD panel should tell. */
#define GNY_TRACE_MAX_SPANS 48

/** A span taking at least this fraction of the frame's work is drawn in the warning colour. */
#define GNY_TRACE_HOT_FRACTION 0.25F

/** Seconds before the one-shot frame breakdown is logged. Late enough that asset loads and the first
 *  swapchain resize are behind it, so the report reflects steady state, not startup. */
#define GNY_TRACE_LOG_AFTER_S 4.0F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MENUS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define GNY_MENU_FONT       NYA_ASSET_FONTS_ALDRICH_TTF
#define GNY_MENU_TITLE_SIZE 44.0F
#define GNY_MENU_ITEM_SIZE  22.0F

/* The panel is centred and sized from its contents, so a menu with three items is not padded out to
 * the height of one with five. */
#define GNY_MENU_WIDTH       360.0F
#define GNY_MENU_ITEM_HEIGHT 42.0F
#define GNY_MENU_PADDING     28.0F
#define GNY_MENU_TITLE_GAP   22.0F

#define GNY_MENU_SCRIM     ((NYA_Color){ 0.02F, 0.02F, 0.04F, 0.72F })
#define GNY_MENU_PANEL     ((NYA_Color){ 0.05F, 0.06F, 0.09F, 0.94F })
#define GNY_MENU_BORDER    ((NYA_Color){ 0.35F, 0.62F, 0.42F, 0.70F })
#define GNY_MENU_TITLE     ((NYA_Color){ 0.94F, 0.96F, 0.98F, 1.0F })
#define GNY_MENU_SUBTITLE  ((NYA_Color){ 0.52F, 0.57F, 0.65F, 1.0F })
#define GNY_MENU_ITEM      ((NYA_Color){ 0.70F, 0.75F, 0.82F, 1.0F })
#define GNY_MENU_ITEM_ON   ((NYA_Color){ 0.06F, 0.08F, 0.06F, 1.0F })
#define GNY_MENU_HIGHLIGHT ((NYA_Color){ 0.45F, 0.78F, 0.53F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * 3D DEMO
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Metres, not pixels. The 2D world converts through NYA_PHYSICS2D_PIXELS_PER_METER at thirty-two; the
 * 3D one has no pixel scale, so a unit here is a metre and the numbers are the sizes of real things. A
 * one metre cube is a crate.
 */

/** Full edge length of the cube, in metres. */
#define GNY_CUBE3D_SIZE 1.0F

/** How far above the ground it starts, so the first thing the scene shows is the solver working. */
#define GNY_CUBE3D_DROP_HEIGHT 4.0F

#define GNY_CUBE3D_GROUND_SIZE      16.0F
#define GNY_CUBE3D_GROUND_THICKNESS 0.5F

/** Where the orbit starts: yaw and pitch in radians, range in metres. The range was seven, putting the
 *  camera *inside* a sixteen metre landscape; twenty is roughly the far corner, so the whole basin is
 *  in shot at the start. */
#define GNY_CUBE3D_ORBIT_YAW   0.7F
#define GNY_CUBE3D_ORBIT_PITCH 0.45F
#define GNY_CUBE3D_ORBIT_RANGE 20.0F

/** Radians of orbit per pixel of mouse motion. */
#define GNY_CUBE3D_ORBIT_SENSITIVITY 0.006F

#define GNY_CUBE3D_ZOOM_STEP 1.12F
#define GNY_CUBE3D_RANGE_MIN 2.5F
#define GNY_CUBE3D_RANGE_MAX 45.0F

/** Angular impulse per pixel of drag. Small, since an impulse is a change in angular *momentum* and the
 *  cube's is low (a metre cube at 400 kg/m³); ten times this and a flick sends it tumbling off. */
#define GNY_CUBE3D_SPIN_STRENGTH 0.02F

/** How far the picking ray reaches, in metres. Past the far edge of the ground. */
#define GNY_CUBE3D_PICK_RANGE 100.0F

/*
 * A flat cartoon palette: saturated objects on a light ground. The ground used to be near-black, which
 * hid how little light the physically based shader it was chosen for actually put on anything; under
 * the banded model objects keep their own colour, so a light ground reads as a lit room, not a void.
 */
#define GNY_CUBE3D_COLOR        ((NYA_Color){ 0.95F, 0.52F, 0.24F, 1.0F })
#define GNY_CUBE3D_HELD_COLOR   ((NYA_Color){ 0.99F, 0.82F, 0.34F, 1.0F })
#define GNY_CUBE3D_GROUND_COLOR ((NYA_Color){ 0.74F, 0.78F, 0.71F, 1.0F })
#define GNY_CUBE3D_GRID_COLOR   ((NYA_Color){ 0.60F, 0.64F, 0.58F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TERRAIN 3D
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * The 3D scene's ground: a heightmap from fBm noise, drawn as flat triangles and collided against as a
 * triangle mesh. Replaces the flat plane the scene used to stand on, a useless test exercising one
 * contact normal and nothing else.
 */

/**
 * Cells per side (vertex grid is one larger each direction). Thirty-two is where two limits meet: the
 * draw is 2048 flat triangles (6144 of the 3D batch's 16384, leaving room for cubes in the same flush),
 * doubled since the scene draws twice per frame for the shadow map. Sixty-four would be 24576 vertices
 * and split into two draw calls before a single cube was queued.
 * */
#define GNY_TERRAIN3D_RES 32

/** Metres across, matching the ground the scene was built around so the camera limits still fit. */
#define GNY_TERRAIN3D_EXTENT GNY_CUBE3D_GROUND_SIZE

#define GNY_TERRAIN3D_CELL (GNY_TERRAIN3D_EXTENT / (f32)GNY_TERRAIN3D_RES)

/**
 * How far a terrain chunk has to be from the viewer before it drops a detail level, doubling per level.
 *
 * ⚠ **In chunk widths, not in fractions of the world.** This was a quarter of the ground's width — four
 * units on a sixteen-unit scene — which is *half a chunk*, so every chunk including the one under the
 * camera was already two or three levels down and the whole surface drew at its coarsest. The scale
 * that decides when a cell stops being worth resolving is the size of a chunk against the viewing
 * distance; the extent of the world has nothing to do with it.
 *
 * Four chunk widths keeps the whole basin at full detail at the default orbit and only coarsens once
 * the camera is pulled well out, which is where there is something to save.
 * */
#define GNY_TERRAIN3D_LOD_DISTANCE (GNY_TERRAIN3D_CELL * (f32)NYA_TERRAIN3D_CHUNK_CELLS * 4.0F)

/** Samples along one edge of the vertex grid. */
#define GNY_TERRAIN3D_VERTS (GNY_TERRAIN3D_RES + 1)

/**
 * Metres from lowest point to highest, roughly — fBm isn't bounded to its nominal range, so a seed's
 * extremes land a little inside or outside this. Two and a half over sixteen is gentle: enough slope
 * that a cube settles differently each time, shallow enough it doesn't slide off the edge.
 * */
#define GNY_TERRAIN3D_AMPLITUDE 2.5F

/**
 * Where the rim starts lifting, as a fraction of the way from centre to edge. Everything inside is
 * free noise; past it the noise fades and the rim comes up, so the two never fight over the same ground.
 * */
#define GNY_TERRAIN3D_RIM_START 0.55F

/** How high the rim stands, in units of GNY_TERRAIN3D_AMPLITUDE. Above one, so it clears the highest
 *  the noise can reach and has no gap a cube can roll through — keeps the pile in without invisible walls. */
#define GNY_TERRAIN3D_RIM_HEIGHT 1.15F

/** How much world distance one unit of noise input covers. Lower is broader hills. */
#define GNY_TERRAIN3D_FREQUENCY 0.09F

#define GNY_TERRAIN3D_OCTAVES    4
#define GNY_TERRAIN3D_LACUNARITY 2.0F
#define GNY_TERRAIN3D_GAIN       0.5F

/** How much a cube sticks to a slope. High, so a landing settles rather than sliding to the edge. */
#define GNY_TERRAIN3D_FRICTION 0.85F

/**
 * The bands the surface is coloured in, low to high. Flat bands, not a gradient, matching the banded
 * shading: colour is chosen per triangle, so the boundary falls on a triangle edge and reads as a
 * facet rather than a contour line.
 */
#define GNY_TERRAIN3D_COLOR_LOW  ((NYA_Color){ 0.42F, 0.56F, 0.38F, 1.0F })
#define GNY_TERRAIN3D_COLOR_MID  ((NYA_Color){ 0.55F, 0.67F, 0.42F, 1.0F })
#define GNY_TERRAIN3D_COLOR_HIGH ((NYA_Color){ 0.72F, 0.74F, 0.58F, 1.0F })
/*
 * Pale dune. Darker than wanted for a while as a workaround: mesh3d.frag used to clamp output at 1.0,
 * so a fully lit pale surface reached the same value as an emissive lamp and no bloom threshold could
 * separate them. mesh3d_tonemap put the headroom back (lit 0.86 lands near 0.79, emissive near 0.99),
 * freeing the colour to suit the palette again.
 */
#define GNY_TERRAIN3D_COLOR_PEAK ((NYA_Color){ 0.86F, 0.83F, 0.71F, 1.0F })

/** Where each band starts, as a fraction of the height range. */
#define GNY_TERRAIN3D_BAND_MID  0.35F
#define GNY_TERRAIN3D_BAND_HIGH 0.62F
#define GNY_TERRAIN3D_BAND_PEAK 0.84F

/**
 * How much a triangle's colour is jittered from its band, either side — stops a band reading as a flat
 * sheet. Hashed from the cell index, not sampled from noise, so it's stable across frames; a shimmering
 * facet would undo the point.
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

/** Below this the cube has fallen off the world and is put back at the top — recycled, not despawned,
 *  since the pool is fixed. */
#define GNY_TERRAIN3D_CUBE_KILL_Y (-12.0F)

/*
 * ── Glass cubes ──
 *
 * Some of the pile is glass: a translucent *solid* is the case sorted transparency exists for — six
 * faces per cube, several overlapping, all moving, drawing visibly wrong the moment ordering is off.
 * What reads them as glass, not coloured cellophane, is the cel shader's own terms — a tight bright
 * highlight and strong rim, as an artist would draw a glass edge.
 */

/** One in this many cubes is glass. Four leaves enough opaque ones for the glass to be seen against. */
#define GNY_CUBE3D_GLASS_EVERY 4

/**
 * Pale and barely tinted, since saturated glass reads as plastic. Near-neutral rather than blue,
 * deliberately: the basin's water is blue, and a similarly tinted glass cube would merge into it as one
 * cyan haze. Alpha routes it into the sorted transparent pass; a quarter shows the far wall through the
 * near one — most of what makes it look solid, not hollow — and is low enough that three stacked don't
 * turn opaque.
 * */
#define GNY_CUBE3D_GLASS_COLOR ((NYA_Color){ 0.80F, 0.88F, 0.86F, 0.26F })

/**
 * A glass material: hard highlight, tight bands, strong rim, no edge darkening. `metallic` is
 * highlight strength here, not metalness; `roughness` is band softness, low for a crisp terminator.
 * */
#define GNY_CUBE3D_GLASS_METALLIC    0.95F
#define GNY_CUBE3D_GLASS_ROUGHNESS   0.12F
#define GNY_CUBE3D_GLASS_REFLECTANCE 1.0F

/** How far the glass bends what's behind it. See NYA_Render3DMaterial.refraction. Small: it's a
 *  screen-space offset, not a traced ray, so a large value looks like image tearing, not thicker glass. */
#define GNY_CUBE3D_GLASS_REFRACTION 0.45F

/** Blur levels the glass cubes cycle through: clear, lightly frosted, heavily frosted. Three, not one,
 *  since a single value can't show what the knob does. Cycled by index, so R replays the arrangement. */
#define GNY_CUBE3D_GLASS_BLURS \
    { 0.0F, 0.35F, 0.85F }

/** The palette the pile is coloured from. Saturated, so they read against the muted ground. */
#define GNY_TERRAIN3D_CUBE_COLORS                                                                                                            \
    {                                                                                                                                        \
        { 0.95F, 0.52F, 0.24F, 1.0F }, { 0.36F, 0.68F, 1.00F, 1.0F }, { 0.96F, 0.56F, 0.52F, 1.0F }, { 0.99F, 0.82F, 0.34F, 1.0F },           \
        { 0.55F, 0.82F, 0.55F, 1.0F }, { 0.72F, 0.56F, 0.92F, 1.0F },                                                                         \
    }

/** How far a sound gets from the 3D camera before it fades, in metres. About half the terrain, so a
 *  cube on the far rim is audibly further than one at the viewer's feet without the near ones deafening. */
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
 * Below the horizon: distant land seen through haze, not a void. A dark slate was tried on the theory
 * the world should read as sitting *on* something, but with terrain only sixteen metres across and the
 * camera pitched down, a dark ground made a small lit island float in a black field. A desaturated
 * green-grey close to the terrain's low band reads as more landscape receding — what haze looks like.
 * */
#define GNY_SKY3D_GROUND ((NYA_Color){ 0.34F, 0.39F, 0.35F, 1.0F })

/** Angular radius of the disc, radians. Life-size is 0.0087, which reads as a dot; this is a shape. */
#define GNY_SKY3D_SUN_ANGLE 0.055F

#define GNY_SKY3D_SUN_INTENSITY  1.0F
#define GNY_SKY3D_MOON_INTENSITY 0.55F

/** Halo exponent. Low is a wide atmospheric glow, high is a tight ring — see NYA_Render3DSky.sun_halo. */
#define GNY_SKY3D_SUN_HALO  48.0F
#define GNY_SKY3D_MOON_HALO 320.0F

/** Above one, so the horizon colour holds further up the sky than a linear ramp would put it. */
#define GNY_SKY3D_HORIZON_SOFTNESS 1.4F

/** How wide the fade into the ground half is, in sine-of-elevation units — wide enough to read as haze,
 *  not a drawn line; a narrow band gives a hard horizon, wrong for a world that simply stops. */
#define GNY_SKY3D_GROUND_BLEND 0.14F

/** Ink width around the loaded models, in world units. See nya_render3d_outline_set. Small: the hull
 *  expands by this in *world* space, so a line thicker than a model's features closes up its concavities. */
#define GNY_CUBE3D_OUTLINE_THICKNESS 0.05F

/** Near-black rather than black, so the ink sits in the palette instead of punching a hole in it. */
#define GNY_CUBE3D_OUTLINE_COLOR ((NYA_Color){ 0.10F, 0.09F, 0.12F, 1.0F })

/*
 * ── Water ──
 *
 * The scene's only translucent geometry, and it is here to give the transparency ordering something to
 * order. See the note at its draw.
 */

/** Where the surface sits. Below the terrain's mid height, so it pools in the basin rather than flooding. */
#define GNY_CUBE3D_WATER_LEVEL (-0.55F)

/** Three panes, so the sort has more than one pair to get right. */
#define GNY_CUBE3D_WATER_LAYERS 3

/** How far apart they sit. Small: the point is that they overlap on screen, not that they are separate. */
#define GNY_CUBE3D_WATER_GAP 0.10F

/** Narrower than the terrain, so the basin's rim is visibly above the surface. */
#define GNY_CUBE3D_WATER_SIZE (GNY_TERRAIN3D_EXTENT * 0.62F)

/** Alpha well below one, routing it into the sorted stream. Low because there are three panes: each
 *  blends over the last, so a fifth each lands the stack at about half — water, not paint. */
#define GNY_CUBE3D_WATER_COLOR ((NYA_Color){ 0.26F, 0.58F, 0.76F, 0.20F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FIRE AND SMOKE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * A plume made of billboards — what a volumetric effect is in a renderer with no depth prepass or
 * compute stage. Fire adds, smoke blends; see nya_render3d_billboard for why those are two systems.
 */

/** The soft radial sprite the plume's billboards use. White with a smooth alpha falloff, so particle
 *  colour comes entirely from the burst and one texture serves both fire and smoke. */
#define GNY_CUBE3D_PUFF_TEXTURE NYA_ASSET_TEXTURES_PUFF_PNG

/** Where the plume stands: up on the rim, clear of where the cubes land — the pile scatters over
 *  GNY_TERRAIN3D_CUBE_SPREAD, so anywhere inside that a crate could land on the fire itself, reading as
 *  broken rather than in the way. */
#define GNY_CUBE3D_PLUME_X (-6.2F)
#define GNY_CUBE3D_PLUME_Z 6.2F

/** How often a puff is emitted, in seconds. Short: a plume is a continuous feed, not a burst. */
#define GNY_CUBE3D_PLUME_INTERVAL_S 0.045F

#define GNY_CUBE3D_FIRE_POOL  256
#define GNY_CUBE3D_SMOKE_POOL 256

/** Per emission. Few, because they are emitted often. */
#define GNY_CUBE3D_FIRE_COUNT  2
#define GNY_CUBE3D_SMOKE_COUNT 2

/** Fire is short-lived and small; smoke outlives it and grows. That contrast is most of the effect. */
#define GNY_CUBE3D_FIRE_LIFETIME  ((f32x2){ 0.35F, 0.7F })
#define GNY_CUBE3D_SMOKE_LIFETIME ((f32x2){ 1.4F, 2.6F })

/*
 * Sized to read from the default orbit, twenty metres out — a plume authored at arm's length would
 * disappear at that range. The numbers below are a bonfire, not a candle, matching crates up to a
 * metre across.
 */
#define GNY_CUBE3D_FIRE_SIZE      ((f32x2){ 0.35F, 0.7F })
#define GNY_CUBE3D_FIRE_SIZE_END  ((f32x2){ 0.05F, 0.15F })
#define GNY_CUBE3D_SMOKE_SIZE     ((f32x2){ 0.7F, 1.2F })
#define GNY_CUBE3D_SMOKE_SIZE_END ((f32x2){ 2.2F, 3.4F })

#define GNY_CUBE3D_FIRE_SPEED  ((f32x2){ 1.4F, 3.0F })
#define GNY_CUBE3D_SMOKE_SPEED ((f32x2){ 0.7F, 1.6F })

/** Upward, with a narrow cone. A wide spread reads as an explosion rather than a fire. */
#define GNY_CUBE3D_PLUME_SPREAD 0.35F

/** Negative gravity: both rise, since hot air is what a plume is. Smoke rises more slowly than fire
 *  since it has cooled — the same reason it lasts longer and spreads wider. */
#define GNY_CUBE3D_FIRE_GRAVITY  ((f32x3){ 0.0F, 2.2F, 0.0F })
#define GNY_CUBE3D_SMOKE_GRAVITY ((f32x3){ 0.0F, 0.9F, 0.0F })

/**
 * Fire colours, above one on purpose: additive blending adds these straight into the target, and the
 * tonemap's shoulder keeps a stack from clipping, so a value past one stays saturated where several
 * overlap rather than washing to white — and puts the plume over the bloom threshold. Only just past
 * one: the first version used 1.6 and every overlap went to white, since additive stacking is
 * multiplicative and a plume is nothing but overlap. Tune this, not the bloom, when fire looks like a
 * searchlight.
 * */
#define GNY_CUBE3D_FIRE_COLOR_START ((NYA_Color){ 1.15F, 0.52F, 0.14F, 1.0F })
#define GNY_CUBE3D_FIRE_COLOR_END   ((NYA_Color){ 0.55F, 0.09F, 0.02F, 0.0F })

/** Smoke: alpha well below one, which is what routes it into the sorted transparent stream. */
#define GNY_CUBE3D_SMOKE_COLOR_START ((NYA_Color){ 0.26F, 0.24F, 0.24F, 0.55F })
#define GNY_CUBE3D_SMOKE_COLOR_END   ((NYA_Color){ 0.46F, 0.46F, 0.48F, 0.0F })

/*
 * ── Still missing ──
 *
 * A soft-particle fade: a billboard intersecting the ground shows a hard cut line, since nothing tells
 * it how close the geometry behind it is — needs scene depth as a texture, which this renderer doesn't
 * yet produce (the refraction capture is colour only).
 */

/*
 * ── Reverb ──
 *
 * The basin is a small hard-walled bowl, so a short bright-ish tail is what an impact produces. See
 * NYA_AudioReverb for why room size isn't a time in seconds.
 */

/** Around half is a room rather than a hall. Past 0.9 the basin would sound like a cathedral. */
#define GNY_CUBE3D_REVERB_ROOM 0.62F

/** Middling: stone and sand absorb some treble, not all of it. Zero would be tiled and unnatural. */
#define GNY_CUBE3D_REVERB_DAMPING 0.45F

/** Enough to hear, little enough that twenty-four cubes landing at once do not turn into a wash. */
#define GNY_CUBE3D_REVERB_WET 0.26F

/** Full: the impacts themselves should not get quieter for being in a room. */
#define GNY_CUBE3D_REVERB_DRY 1.0F

/*
 * ── Audio occlusion ──
 *
 * What a hill between a sound and the camera does to it. See nya_audio_occlusion_set for why the
 * engine takes a callback instead of raycasting itself.
 */

/** Cutoff at full occlusion. Around 700 reads as solid ground rather than as a curtain. */
#define GNY_CUBE3D_OCCLUSION_HZ 700.0F

/** Gain at full occlusion. Muffling does most of the work; this stops a blocked sound also being loud. */
#define GNY_CUBE3D_OCCLUSION_GAIN 0.45F

/** How fast the filter follows. Short enough to track a rolling cube, long enough not to click. */
#define GNY_CUBE3D_OCCLUSION_GLIDE_MS 90.0F

/** How far the two extra rays are offset, as a fraction of the distance. An angular spread, not a length. */
#define GNY_CUBE3D_OCCLUSION_SPREAD 0.06F

/** Seeds the per-cube size, position and colour hash. Arbitrary; changing it reshuffles the pile. */
#define GNY_TERRAIN3D_CUBE_SEED 0xC0BE5

/** The terrain's own hash seed, mixed with the world seed so R gives a genuinely different landscape. */
#define GNY_TERRAIN3D_SEED 0x7E44A1

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SPARKS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Live sparks at once, across every impact. A ceiling, not a growable pool, for the same reason
 *  NYA_PHYSICS2D_MAX_HITS is one: past this they're dropped and counted, the right behaviour under load. */
#define GNY_SPARK_POOL 2048

/** Sparks per impact, from the quietest that qualifies to the loudest. */
#define GNY_SPARK_MIN 12
#define GNY_SPARK_MAX 64

/** Half angle of the spray, in radians. About fifty degrees either side of straight up. */
#define GNY_SPARK_SPREAD 0.9F

/** Pulls them back down. Stronger than world gravity, so they arc tightly rather than drifting. */
#define GNY_SPARK_GRAVITY 900.0F

/** How big one spark starts and ends, in world units. Ends above zero so it fades rather than pops. */
#define GNY_SPARK_SIZE_START ((f32x2){ 3.5F, 8.0F })
#define GNY_SPARK_SIZE_END   ((f32x2){ 0.5F, 2.0F })

#define GNY_SPARK_COLOR ((NYA_Color){ 1.0F, 0.85F, 0.45F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIGHTING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How lit the world is where no crate reaches, as a multiplier on what was drawn. Not zero: a scene
 *  with zero ambient renders as a black rectangle, indistinguishable from a broken renderer. */
#define GNY_AMBIENT_LIGHT ((NYA_Color){ 0.34F, 0.36F, 0.46F, 1.0F })

/** World units a crate's glow reaches. A few crate widths, so overlapping ones pool. */
#define GNY_BOX_LIGHT_RADIUS 190.0F

/** Above one, so the crate itself over-brightens and reads as the source rather than as lit. */
#define GNY_BOX_LIGHT_INTENSITY 1.35F

#define GNY_BOX_LIGHT_COLOR ((NYA_Color){ 1.0F, 0.82F, 0.55F, 1.0F })

/*
 * ── 3D demo dust ──
 *
 * Metres, like everything else in that scene. A "spark" here is a chip of ground the cube kicks up,
 * centimetre-scale versus the 2D world's pixel-scale — the same numbers in both would be either
 * invisible or the size of the cube.
 */

/** Live dust particles at once. Small: one cube makes one impact at a time. */
#define GNY_CUBE3D_DUST_POOL 512

/** Particles per landing, from the softest that registers to the hardest. */
#define GNY_CUBE3D_DUST_MIN 10
#define GNY_CUBE3D_DUST_MAX 48

/** Metres per second. Fast enough to leave the contact point before gravity takes them. */
#define GNY_CUBE3D_DUST_SPEED ((f32x2){ 1.2F, 4.5F })

/** Metres. A chip of ground, not a boulder. */
#define GNY_CUBE3D_DUST_SIZE ((f32x2){ 0.03F, 0.09F })

/** Positive y is up in a 3D scene, so gravity here is negative — the opposite of the 2D world's. */
#define GNY_CUBE3D_DUST_GRAVITY -9.81F

#define GNY_CUBE3D_DUST_COLOR ((NYA_Color){ 0.72F, 0.68F, 0.60F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TILEMAP
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where the demo map's tile (0, 0) sits. The map is 20x12 tiles of 32 units (640x384), centred on x,
 *  placed so its solid bottom rows land just above GNY_TERRAIN_BASE_Y — a floor inside the opening view. */
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

/** Full width and height of a ledge, world units. Wide enough to land a crate on, thin enough to read as a shelf. */
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

/** High, so a crate riding the moving platform goes with it rather than being slid out from under. */
#define GNY_LEDGE_FRICTION 0.95F

/**
 * How long a drop-through window stays open, seconds.
 *
 * Long enough for a free fall to clear the ledge's thickness several times over — a window that
 * closes with the crate still inside the platform turns the contact solid again and pops it back out
 * on top, which reads as the key not working.
 * */
#define GNY_LEDGE_DROP_SECONDS 0.45F

#define GNY_LEDGE_COLOR         ((NYA_Color){ 0.36F, 0.40F, 0.52F, 1.0F })
#define GNY_LEDGE_COLOR_MOVING  ((NYA_Color){ 0.46F, 0.52F, 0.68F, 1.0F })
#define GNY_LEDGE_EDGE_COLOR    ((NYA_Color){ 0.82F, 0.88F, 1.0F, 1.0F })
#define GNY_LEDGE_EDGE_THICKNESS 2.5F

/** The marker parented to the moving ledge: how far under it, how big, and what colour. */
#define GNY_LEDGE_MARKER_LIFT   22.0F
#define GNY_LEDGE_MARKER_RADIUS 7.0F
#define GNY_LEDGE_MARKER_COLOR  ((NYA_Color){ 1.0F, 0.78F, 0.35F, 1.0F })

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
 * Drawn at its own size. One, not a shrink factor: ufbx converts to metres on load, and both models
 * span roughly minus one to one. The 0.01 here first was a guess from the file being 356 KB, and drew
 * both models two hundredths of a unit across — present, lit, and far too small to notice.
 * test_asset_mesh asserts the models are unit-sized, so a re-export that changes that fails the test
 * rather than quietly emptying the scene.
 * */
#define GNY_CUBE3D_MODEL_SCALE 1.0F

/** How far to one side of the cube it stands, so the two are both visible rather than intersecting. */
#define GNY_CUBE3D_MODEL_OFFSET 2.5F

/** Lifted clear of the ground plane at y zero. One, because the model's origin is its centre and its
 *  lowest vertex is a unit below that — half of it would be under the floor otherwise. */
#define GNY_CUBE3D_MODEL_LIFT 1.0F

/** Radians per second about the world's up axis, so every side of it is visible without being dragged. */
#define GNY_CUBE3D_MODEL_SPIN 0.6F

#define GNY_CUBE3D_MODEL_COLOR ((NYA_Color){ 0.42F, 0.63F, 0.88F, 1.0F })

/*
 * The second model, on the other side of the cube. Two, not one: one proves the loader runs, two prove
 * it is a loader — separate assets with separate triangle counts into the same batch, light and draw
 * call, so a path that reused one buffer for both would show up as two identical shapes.
 */
#define GNY_CUBE3D_PILL NYA_ASSET_MODELS_PILL_FBX

/** Its own scale, since nothing guarantees two FBX files were authored in the same units. */
#define GNY_CUBE3D_PILL_SCALE 1.0F

/** Mirrored across the cube from GNY_CUBE3D_MODEL_OFFSET, so the three stand in a row. */
#define GNY_CUBE3D_PILL_OFFSET (-GNY_CUBE3D_MODEL_OFFSET)

/**
 * Half the pill's height, not the cube's. pill.fbx spans about 1.73 either side of its origin on y once
 * its node transform is applied — a capsule stretched along one axis by the node, not its vertex data.
 * Lifting it by one, as the other model is, put its lower third through the floor.
 */
#define GNY_CUBE3D_PILL_LIFT 1.75F

/** Turned the other way, so the two are visibly independent rather than looking like one object. */
#define GNY_CUBE3D_PILL_SPIN (-0.45F)

#define GNY_CUBE3D_PILL_COLOR ((NYA_Color){ 0.96F, 0.56F, 0.52F, 1.0F })

/*
 * ─────────────────────────────────────────────────────────
 * THE 3D SCENE'S LIGHTS
 * ─────────────────────────────────────────────────────────
 */

/** Two lamps orbiting the scene, so the point lights are visibly lights, not a tint. Moving, not static:
 *  a stationary coloured light is indistinguishable from coloured ambient. */
#define GNY_CUBE3D_LAMP_COUNT  2
#define GNY_CUBE3D_LAMP_RADIUS 4.0F
#define GNY_CUBE3D_LAMP_HEIGHT 2.2F
#define GNY_CUBE3D_LAMP_SPEED  0.7F

/** How far each lamp reaches. Comfortably less than the ground plane, so the falloff is visible on it. */
#define GNY_CUBE3D_LAMP_RANGE 7.0F

#define GNY_CUBE3D_LAMP_INTENSITY 2.4F

/** Warm and cool, so the two are told apart by colour rather than only by position. */
#define GNY_CUBE3D_LAMP_A_COLOR ((NYA_Color){ 1.00F, 0.62F, 0.28F, 1.0F })
#define GNY_CUBE3D_LAMP_B_COLOR ((NYA_Color){ 0.36F, 0.68F, 1.00F, 1.0F })

/** The little sphere drawn at each lamp, and how strongly it glows. See NYA_Render3DMaterial.emission. */
#define GNY_CUBE3D_LAMP_MARKER_RADIUS 0.16F
#define GNY_CUBE3D_LAMP_EMISSION      1.6F

/** How strongly curved edges are inked in. See NYA_Render3DMaterial.edge. Shows on the two models and
 *  sphere markers; the generated cube has hard faces and gets nothing from it — a limit of the
 *  technique, not a tuning problem. */
#define GNY_CUBE3D_EDGE 0.55F

/*
 * ─────────────────────────────────────────────────────────
 * THE 3D SCENE'S SHADOWS
 * ─────────────────────────────────────────────────────────
 */

/** How dark a shadowed surface goes. Well under one on purpose: a shadow at one lands on black and
 *  reads as a hole in the floor. */
#define GNY_CUBE3D_SHADOW_STRENGTH 0.45F
/**
 * The *nearest* cascade's half-width; the others widen from it by NYA_RENDER3D_SHADOW_CASCADE_RATIO.
 * Much smaller than it was, since it no longer covers the whole terrain alone: three cascades at a
 * ratio of 2.5 reach 0.16, 0.4 and 1.0 of the extent, so the last still covers the rim while the first
 * spends its full resolution on the pile in the middle.
 * */
#define GNY_CUBE3D_SHADOW_EXTENT   (GNY_TERRAIN3D_EXTENT * 0.16F)

/** World units per second a networked player moves. See gny_net_apply_command. */
#define GNY_PLAYER_SPEED 220.0F

/** How far apart players spawn, so two joining at once do not start inside each other. */
#define GNY_PLAYER_SPAWN_SPACING 64.0F

/**
 * How large a player is drawn, per side. A drawing constant rather than a collision size: a player has
 * no physics body, so nothing reads this but gny_net_player_on_render. Kept under
 * GNY_PLAYER_SPAWN_SPACING so two players spawning at once are visibly apart rather than touching.
 * */
#define GNY_PLAYER_SIZE 24.0F
