/**
 * @file constants.h
 *
 * Every tunable number and colour in gnyame, in one place.
 *
 * Grouped by what they affect rather than by which file used to hold them, so a tuning pass reads
 * top to bottom: the world, then how it is played, then how it looks, then how it sounds.
 *
 * ## What belongs here
 *
 * Anything a person would change to make the game feel or look different. What does *not* belong is
 * anything the code derives or depends on structurally — layer ids are pointers to string literals
 * whose address is their identity, not constants, and stay with the layers they name.
 *
 * ## Why this is not the engine's arrangement
 *
 * nyangine deliberately does the opposite: its thirty-odd tunables each sit beside the mechanism
 * they govern, `#ifndef` guarded so a game can override them with a `-D`. That suits a library,
 * where a constant is part of the contract of the module it lives in and the person reading it is
 * usually reading that module. A game is tuned as a whole — the terrain amplitude, the camera speed
 * and the bloom threshold are adjusted against each other in one sitting — so the numbers are
 * collected instead.
 *
 * The cost of collecting them is real and worth naming: a colour read three lines from where it is
 * used documents itself, and the same name three hundred lines away in another file does not. The
 * section comments below are what pays that back.
 * */
#pragma once

#include "nyangine/nyangine.h"

// Both the HUD and the menus name a font, and the handles come from the generated asset index.
#include "assets/assets.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WORLD
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The ground is a polyline sampled at a fixed spacing, so these decide both how it collides and how
 * it draws. Wider spacing is cheaper and more angular; the crates notice.
 */

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
 * How far a sound gets from the camera before it starts fading, in world units.
 *
 * About a third of the view, so an impact at the edge of the screen is audibly further away than one
 * in the middle. See NYA_AudioListener.reference_distance.
 * */
#define GNY_CAMERA_EAR_DISTANCE 400.0F

/**
 * How quickly the camera closes on what it is following, as a fraction of the remaining gap per tick.
 *
 * Exponential easing rather than a constant speed: it starts fast when the target is far and settles
 * without overshoot, and it never has to know how fast the target is moving. Around 0.1 reads as a
 * camera that is keeping up rather than one welded to the thing.
 * */
#define GNY_CAMERA_FOLLOW_EASING 0.12F

/**
 * How fast a player-controlled entity moves, in world units per second.
 *
 * Separate from the camera's pan speed because the two are in different terms: a camera's speed is
 * relative to the view and scales with the zoom, while a crate's is a speed in the world.
 * */
#define GNY_PLAYER_MOVE_SPEED 420.0F

/*
 * ── Secondary views ──
 *
 * A camera that is not primary renders into its own texture and is composited into a viewport, which
 * is the picture-in-picture case: a window onto somewhere else in the world.
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
 * Luminance a pixel needs before it glows, in the 2D world.
 *
 * This and the two below were badly out, and the reason is worth recording: the bloom pass was running
 * with an unbound uniform buffer — see gny_bloom_pipeline_ensure — so its cbuffer read as zeros and the
 * intensity was zero. The pass was a pass-through, whatever these said. They had never actually been
 * seen, and the moment the binding was fixed the world whited out.
 *
 * Half is roughly where a lit crate sits. The terrain fill is around 0.15 and stays well out of it, which
 * is what keeps the ground from hazing over, and a settled crate darkened by the sleep tint drops below
 * the threshold and stops glowing — which reads as it going to sleep rather than as a bug.
 * */
#define GNY_BLOOM_2D_THRESHOLD 0.50F

/**
 * How hard the glow is added back.
 *
 * Below one, so a glowing crate keeps its edges and its colour instead of becoming a white blob. This was
 * 2.2 on the reasoning that what got past a threshold of 0.22 was dim to begin with — true of the bright
 * pass, and not true of a threshold that no longer catches the whole scene.
 * */
#define GNY_BLOOM_2D_INTENSITY 0.70F

/**
 * How far apart the kernel's samples sit, in pixels.
 *
 * effect_bloom.frag.hlsl names its uniform `texel` and its five by five kernel steps `±2` of them, so
 * handing it the true texel size produces a halo two pixels wide — real, correct, and completely
 * invisible at any window size anyone uses. Feeding it a multiple spreads the same twenty five taps over
 * a radius worth seeing.
 *
 * The honest cost: the samples get sparser as this grows rather than the kernel getting denser, so a very
 * large value shows the individual taps as a faint boxy star around small bright shapes. Three is a halo
 * that reads as a glow without the taps separating.
 * */
#define GNY_BLOOM_2D_SPREAD 3.0F

/*
 * ── The 3D scene's own bloom ──
 *
 * Separate numbers, because the two scenes are not equally bright and one threshold cannot serve both.
 * The 2D world is a dim side-on scene whose crates darken further as they settle, which is why the values
 * above are a very low threshold and a large intensity. The 3D scene is a lit landscape in daylight with
 * saturated objects on it: at a threshold of 0.22 almost every pixel is "bright", and multiplying that by
 * 2.2 whites out the frame.
 *
 * These were never caught because the bloom uniform was not being bound at all — see the note in
 * gny_bloom_pipeline_ensure. The pass ran with a zeroed cbuffer, which is an intensity of zero, so what
 * was on screen was the scene passed through untouched and the numbers had never actually been used.
 */

/**
 * Just under where the brightest *lit* surfaces land, not above them.
 *
 * mesh3d_tonemap is identity up to 0.6 and compresses above, so everything lit lands under about 0.86 and
 * anything emissive above 0.95. The first value here was 0.90 — in the gap between those, which is
 * arithmetically correct and made the pass almost invisible: toggling it changed a third of one percent of
 * the frame, all of it the fire and two lamp beads.
 *
 * That was an over-correction. The tonemap had already solved the problem the high threshold was for —
 * before it, everything clamped to one and any threshold blew the frame out — and setting this above the
 * lit range as well meant solving it twice.
 *
 * Now it sits *inside* the top of the lit range, so the pale rim of the basin, the sun's highlights and the
 * horizon catch a little of the glow, and emissive things still clear it by a wide margin. Saturated cubes
 * stay out of it: an orange crate's luma is around 0.6 even in full sun.
 * */
#define GNY_BLOOM_3D_THRESHOLD 0.78F

/**
 * Around one, so what does clear the threshold is actually visible.
 *
 * Paired with the threshold above rather than tuned alone. At 0.6 the little that got through was added
 * back at well under half strength, which is most of why the pass read as switched off.
 * */
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
 * Where the music slider starts, before the player has touched it.
 *
 * Separate from GNY_MUSIC_GAIN, which is the track's own level in the mix. This one is a *setting*:
 * gny_actions_init writes it into the settings system, the pause menu edits it, and the settings file
 * remembers it. A constant cannot do any of those, which is why the two are not one number.
 * */
#define GNY_MUSIC_VOLUME_DEFAULT 0.7F

/** How much one press of left or right on a volume row moves it. */
#define GNY_VOLUME_STEP 0.05F

/** Long enough that the track arrives rather than starting, since it has no lead-in of its own. */
#define GNY_MUSIC_FADE_IN_MS 1500

/**
 * Whether the background track starts silent.
 *
 * It is still loaded and still started, then paused immediately — rather than not played at all,
 * because `m` resumes the track and there is nothing to resume if it never began. The HUD reads
 * "paused" from the first frame, which is the honest description of that state.
 * */
#define GNY_MUSIC_START_MUTED true

/*
 * ── Impact audio ──
 *
 * Everything below the physics hit threshold never reaches the game at all; see physics2d.h.
 * These decide what the ones that do sound like.
 */

/**
 * Impacts given a voice per frame.
 *
 * There are sixteen voices in total and a collapsing stack can produce dozens of hits in one frame,
 * so this is a budget rather than a limit on what the world may do.
 *
 * Per frame rather than per tick because the observer that spends it sees the whole frame at once,
 * and can therefore spend it on the *loudest* impacts rather than on whichever happened to be
 * simulated first. See sim.h.
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

/**
 * Seeds for nya_ihash2, which is what gives the motes their positions and the crates their sizes.
 *
 * Arbitrary, and different from each other only so the two do not produce correlated sequences from
 * the same indices. Changing one reshuffles what it seeds and nothing else.
 * */
#define GNY_MOTE_SEED 0x5EED
#define GNY_BOX_SEED  0xC4A7E

/*
 * ── The day/night cycle ──
 *
 * GNY_SKY_TOP and GNY_SKY_BOTTOM are gone: the gradient's two colours now come from the keyframe table in
 * system_sky.c, because they change through the day. What is left here is the shape of the cycle rather
 * than its colours.
 */

/** How long a full day takes, in seconds. Short enough to see the whole cycle without waiting. */
#define GNY_DAY_LENGTH_S 120.0F

/** Where the demo starts in the day. 0.32 is mid morning, so the first frame is lit and not black. */
#define GNY_SKY_START_PHASE 0.32F

/**
 * The lowest the sun is allowed to sit, as a sine of its arc.
 *
 * A perfectly horizontal light lands along the ground plane and lights nothing on it, and makes the shadow
 * volume degenerate. Keeping a little elevation at the horizon costs nothing and avoids both.
 * */
#define GNY_SKY_MIN_ELEVATION 0.12F

/** The disc: its size as a fraction of the window, and the path it travels. */
#define GNY_SKY_DISC_RADIUS  0.035F
#define GNY_SKY_DISC_HORIZON 0.62F
#define GNY_SKY_DISC_RISE    0.44F

/** Two flat rings under the disc. A cartoon glow has an edge, which is the point of the style. */
#define GNY_SKY_HALO_ALPHA 0.10F
#define GNY_SKY_HALO_INNER 1.9F
#define GNY_SKY_HALO_OUTER 3.2F

/**
 * How much the moon's craters darken it, and how much less it glows than the sun.
 *
 * There is no crescent constant any more: see _gny_sky_disc_draw for why the occlusion trick that needed
 * one cannot work over a halo.
 * */
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
 * Opaque, and it has to be.
 *
 * A cloud here is three circles and a rectangle overlapping, and translucent parts *double-blend* where
 * they overlap — so every seam between them draws itself and the cloud reads as a pile of shapes rather
 * than as one silhouette. At 0.85 that was plainly visible.
 *
 * Softness comes from GNY_SKY_CLOUD_TINT instead: mixing the colour toward the horizon is what
 * translucency was there to achieve, and a flat style gets the same result from a paler colour as from a
 * transparent one. Composing a genuinely translucent cloud from parts would mean drawing it into its own
 * target first and compositing that once.
 * */
#define GNY_SKY_CLOUD_ALPHA  1.0F
#define GNY_SKY_CLOUD_COLOR  ((NYA_Color){ 1.0F, 0.99F, 0.97F, 1.0F })

/**
 * How far the clouds are tinted toward the horizon colour.
 *
 * Two jobs. A white cloud at dusk is the one thing that gives away a static backdrop, because everything
 * around it has gone orange — and since the clouds are opaque, this is also the only thing keeping them
 * from reading as cut-out paper. Raised when the alpha went to one; see GNY_SKY_CLOUD_ALPHA.
 * */
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

/**
 * Seconds before the one-shot frame breakdown is written to the log.
 *
 * Late enough that the asset loads and the first swapchain resize are behind it, so the frame it
 * reports is a steady state one rather than startup. Logged once, so a run leaves evidence of what
 * the frame actually cost without anyone having to be at the keyboard to press `t`.
 * */
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
 * Metres, not pixels. The 2D world converts through NYA_PHYSICS2D_PIXELS_PER_METER at thirty-two;
 * the 3D one has no pixel scale at all, so a unit here is a metre and the numbers are the sizes of
 * real things. A one metre cube is a crate.
 */

/** Full edge length of the cube, in metres. */
#define GNY_CUBE3D_SIZE 1.0F

/** How far above the ground it starts, so the first thing the scene shows is the solver working. */
#define GNY_CUBE3D_DROP_HEIGHT 4.0F

#define GNY_CUBE3D_GROUND_SIZE      16.0F
#define GNY_CUBE3D_GROUND_THICKNESS 0.5F

/**
 * Where the orbit starts: yaw and pitch in radians, range in metres.
 *
 * The range was seven, which framed a one metre cube on a flat plane and puts the camera *inside* a
 * sixteen metre landscape — the ground fills the frame and the models beside it are the size of hills.
 * Twenty is roughly the far corner of the terrain, so the whole basin is in shot at the start.
 * */
#define GNY_CUBE3D_ORBIT_YAW   0.7F
#define GNY_CUBE3D_ORBIT_PITCH 0.45F
#define GNY_CUBE3D_ORBIT_RANGE 20.0F

/** Radians of orbit per pixel of mouse motion. */
#define GNY_CUBE3D_ORBIT_SENSITIVITY 0.006F

#define GNY_CUBE3D_ZOOM_STEP 1.12F
#define GNY_CUBE3D_RANGE_MIN 2.5F
#define GNY_CUBE3D_RANGE_MAX 45.0F

/**
 * Angular impulse per pixel of drag.
 *
 * Small, because an impulse is a change in angular *momentum* and the cube's is low — a metre cube
 * at 400 kg/m³. Ten times this and a flick sends it tumbling off the ground.
 * */
#define GNY_CUBE3D_SPIN_STRENGTH 0.02F

/** How far the picking ray reaches, in metres. Past the far edge of the ground. */
#define GNY_CUBE3D_PICK_RANGE 100.0F

/*
 * A flat cartoon palette: saturated objects on a light ground.
 *
 * The ground used to be near-black, which suited the physically based shader it was chosen for — a dark
 * floor hid how little light that shader put on anything. Under the banded model the objects keep their
 * own colour, so a light ground reads as a lit room instead of as a void, and the silhouettes stop
 * needing the rim light to be visible at all.
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
 * triangle mesh. It replaces the flat plane the scene used to stand on, which was a fine backdrop and a
 * useless test — a box landing on a flat floor exercises one contact normal and nothing else.
 */

/**
 * Cells per side. The vertex grid is one larger in each direction.
 *
 * Thirty-two is where two limits meet. The draw is 2048 flat triangles at three vertices each, which is
 * 6144 of the 3D batch's 16384 — leaving room for the cubes in the same flush — and the scene is drawn
 * twice per frame, once for the shadow map, so the real cost is double that. Sixty-four would be 24576
 * vertices and would split into two draw calls before a single cube was queued.
 * */
#define GNY_TERRAIN3D_RES 32

/** Metres across, matching the ground the scene was built around so the camera limits still fit. */
#define GNY_TERRAIN3D_EXTENT GNY_CUBE3D_GROUND_SIZE

#define GNY_TERRAIN3D_CELL (GNY_TERRAIN3D_EXTENT / (f32)GNY_TERRAIN3D_RES)

/** Samples along one edge of the vertex grid. */
#define GNY_TERRAIN3D_VERTS (GNY_TERRAIN3D_RES + 1)

/**
 * Metres from the lowest point to the highest, roughly.
 *
 * fBm is not bounded to its nominal range, so the extremes of any one seed land a little inside or
 * outside this. Two and a half metres over sixteen is a gentle landscape — enough slope that a cube
 * rolls and settles somewhere different each time, shallow enough that one dropped in the middle does
 * not simply slide off the edge.
 * */
#define GNY_TERRAIN3D_AMPLITUDE 2.5F

/**
 * Where the rim starts lifting, as a fraction of the way from the centre to the edge.
 *
 * Everything inside this is free noise. Past it the noise fades out and the rim comes up, so the two
 * never fight over the same ground.
 * */
#define GNY_TERRAIN3D_RIM_START 0.55F

/**
 * How high the rim stands, in units of GNY_TERRAIN3D_AMPLITUDE.
 *
 * Above one, so it clears the highest the noise can reach and the ring has no gap a cube can roll
 * through. This is what keeps the pile on the terrain without invisible walls around it.
 * */
#define GNY_TERRAIN3D_RIM_HEIGHT 1.15F

/** How much world distance one unit of noise input covers. Lower is broader hills. */
#define GNY_TERRAIN3D_FREQUENCY 0.09F

#define GNY_TERRAIN3D_OCTAVES    4
#define GNY_TERRAIN3D_LACUNARITY 2.0F
#define GNY_TERRAIN3D_GAIN       0.5F

/** How much a cube sticks to a slope. High, so a landing settles rather than sliding to the edge. */
#define GNY_TERRAIN3D_FRICTION 0.85F

/**
 * The bands the surface is coloured in, low to high.
 *
 * Flat bands rather than a gradient, for the reason the shading is banded: the look is areas of colour
 * with a visible edge between them. The boundary falls on a triangle edge because the colour is chosen
 * per triangle, which is what makes it read as a facet rather than as a contour line.
 */
#define GNY_TERRAIN3D_COLOR_LOW  ((NYA_Color){ 0.42F, 0.56F, 0.38F, 1.0F })
#define GNY_TERRAIN3D_COLOR_MID  ((NYA_Color){ 0.55F, 0.67F, 0.42F, 1.0F })
#define GNY_TERRAIN3D_COLOR_HIGH ((NYA_Color){ 0.72F, 0.74F, 0.58F, 1.0F })
/*
 * Pale dune.
 *
 * This spent a while darker than wanted, as a workaround: mesh3d.frag used to clamp its output at 1.0, so
 * a fully lit pale surface reached exactly the value an emissive lamp did and no bloom threshold could
 * separate them. mesh3d_tonemap put the headroom back — a lit 0.86 now lands near 0.79 while an emissive
 * surface lands near 0.99 — so the colour is free to be whatever suits the palette again.
 */
#define GNY_TERRAIN3D_COLOR_PEAK ((NYA_Color){ 0.86F, 0.83F, 0.71F, 1.0F })

/** Where each band starts, as a fraction of the height range. */
#define GNY_TERRAIN3D_BAND_MID  0.35F
#define GNY_TERRAIN3D_BAND_HIGH 0.62F
#define GNY_TERRAIN3D_BAND_PEAK 0.84F

/**
 * How much a triangle's colour is jittered from its band, either side.
 *
 * The one thing that stops a band reading as a flat sheet. Hashed from the cell index rather than
 * sampled from noise, so it is stable across frames and costs nothing to recompute — a facet that
 * shimmered would undo the point of it.
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

/**
 * Below this the cube has fallen off the world and is put back at the top.
 *
 * A recycle rather than a despawn: the pool is fixed, and a scene that empties itself over a minute is
 * a worse demo than one that keeps going.
 * */
#define GNY_TERRAIN3D_CUBE_KILL_Y (-12.0F)

/*
 * ── Glass cubes ──
 *
 * Some of the pile is glass. They are here because a translucent *solid* is the case sorted transparency
 * exists for: six faces per cube, several cubes overlapping, all of it moving — which is the arrangement
 * that draws visibly wrong the moment the ordering is.
 *
 * There is no refraction yet, so these do not bend or blur what is behind them; see the review. What
 * makes them read as glass instead of as coloured cellophane is the cel shader's own terms — a tight
 * bright highlight and a strong rim, which is what an artist would draw on a glass edge anyway.
 */

/** One in this many cubes is glass. Four leaves enough opaque ones for the glass to be seen against. */
#define GNY_CUBE3D_GLASS_EVERY 4

/**
 * Pale and barely tinted, because a saturated glass reads as plastic.
 *
 * Near-neutral rather than blue, deliberately: the water in the same basin is blue, and a glass cube
 * sitting in it at a similar tint merges with it into one cyan haze instead of reading as an object in a
 * pool. The two translucent things in a scene have to be told apart by colour, since neither has an
 * outline.
 *
 * The alpha is what routes it into the sorted transparent pass at all. A quarter is enough to see the far
 * wall of the cube through the near one, which is most of what makes it look solid rather than hollow —
 * and low enough that three of them stacked do not turn opaque.
 * */
#define GNY_CUBE3D_GLASS_COLOR ((NYA_Color){ 0.80F, 0.88F, 0.86F, 0.26F })

/**
 * A glass material: hard highlight, tight bands, strong rim, no edge darkening.
 *
 * `metallic` is highlight strength in this shading model rather than metalness, and glass has a bright
 * specular; `roughness` is band softness, and low is the crisp terminator a hard surface gives. The rim is
 * pushed high because a glass edge catching the light is the single most recognisable thing about it. Edge
 * darkening is off: it stands in for ambient occlusion on a curved surface, and a transparent object does
 * not occlude itself.
 * */
#define GNY_CUBE3D_GLASS_METALLIC    0.95F
#define GNY_CUBE3D_GLASS_ROUGHNESS   0.12F
#define GNY_CUBE3D_GLASS_REFLECTANCE 1.0F

/**
 * How far the glass bends what is behind it. See NYA_Render3DMaterial.refraction.
 *
 * Small. It is a screen-space offset rather than a traced ray, so a large value does not look like thicker
 * glass — it looks like the image tearing, because the sample it fetches stops having anything to do with
 * what is geometrically behind the surface.
 * */
#define GNY_CUBE3D_GLASS_REFRACTION 0.45F

/**
 * The blur levels the glass cubes are cycled through: clear, lightly frosted, heavily frosted.
 *
 * Three rather than one, because a single value cannot show what the knob does. Cycled by index for the
 * reason the glass cubes themselves are chosen by index — the same press of R gives the same arrangement,
 * and an even spread means each level has the others nearby to be compared against.
 * */
#define GNY_CUBE3D_GLASS_BLURS \
    { 0.0F, 0.35F, 0.85F }

/** The palette the pile is coloured from. Saturated, so they read against the muted ground. */
#define GNY_TERRAIN3D_CUBE_COLORS                                                                                                            \
    {                                                                                                                                        \
        { 0.95F, 0.52F, 0.24F, 1.0F }, { 0.36F, 0.68F, 1.00F, 1.0F }, { 0.96F, 0.56F, 0.52F, 1.0F }, { 0.99F, 0.82F, 0.34F, 1.0F },           \
        { 0.55F, 0.82F, 0.55F, 1.0F }, { 0.72F, 0.56F, 0.92F, 1.0F },                                                                         \
    }

/**
 * How far a sound gets from the 3D camera before it starts fading, in metres.
 *
 * About half the terrain, so a cube landing on the far rim is audibly further away than one at the
 * viewer's feet without the near ones being deafening. The 2D world's equivalent is four hundred, which
 * is the same fraction of a world whose units are pixels — see GNY_CAMERA_EAR_DISTANCE.
 * */
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
 * Below the horizon: distant land seen through haze, not a void.
 *
 * This was a dark slate on the reasoning that the world should read as sitting *on* something. It does
 * the opposite in this scene — the terrain is sixteen metres across with nothing beyond it, and a camera
 * pitched down puts most of the frame below the horizon, so a dark ground makes a small lit island float
 * in a black field.
 *
 * A desaturated green-grey close to the terrain's own low band reads as more of the same landscape
 * receding, which is what haze looks like and what the eye expects past the edge of a scene.
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

/**
 * How wide the fade into the ground half is, in sine-of-elevation units.
 *
 * Wide enough to read as haze rather than as a drawn line. A narrow band gives a hard horizon, which is
 * right for a world with actual geometry out to the edge and wrong for one that simply stops.
 * */
#define GNY_SKY3D_GROUND_BLEND 0.14F

/**
 * Ink width around the loaded models, in world units. See nya_render3d_outline_set.
 *
 * Small: the hull is expanded by this in *world* space, so it is a real distance rather than a screen
 * width, and a line thicker than a model's own features closes up its concavities.
 * */
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

/**
 * Alpha well below one: that is what routes it into the sorted stream at all.
 *
 * Low, because there are three panes. Each blends over the last, so the stack is far more opaque than any
 * one of them — a fifth each lands the three together at about half, which is water rather than paint.
 * */
#define GNY_CUBE3D_WATER_COLOR ((NYA_Color){ 0.26F, 0.58F, 0.76F, 0.20F })

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FIRE AND SMOKE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * A plume made of billboards, which is what a volumetric effect is in a renderer without a depth prepass
 * or a compute stage. Fire adds, smoke blends; see nya_render3d_billboard for why those are two systems.
 */

/**
 * The soft radial sprite the plume's billboards are drawn with.
 *
 * White with a smooth alpha falloff, so the particle colour comes entirely from the burst and one texture
 * serves both the fire and the smoke. See the note where it is set.
 * */
#define GNY_CUBE3D_PUFF_TEXTURE NYA_ASSET_TEXTURES_PUFF_PNG

/**
 * Where the plume stands: up on the rim, clear of where the cubes land.
 *
 * The pile is scattered over GNY_TERRAIN3D_CUBE_SPREAD on each axis, so anywhere inside that is somewhere
 * a crate can land on top of the fire — which reads as the effect being broken rather than as a crate
 * being in the way. The rim starts at GNY_TERRAIN3D_RIM_START of the half extent and nothing is dropped
 * there.
 * */
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
 * Sized to read from the default orbit, which is twenty metres out.
 *
 * A plume authored at arm's length disappears at that range — half a metre of flame is a few pixels. The
 * numbers below are what a bonfire would be rather than a candle, which is also what the scene's scale
 * asks for: the crates beside it are up to a metre across.
 */
#define GNY_CUBE3D_FIRE_SIZE      ((f32x2){ 0.35F, 0.7F })
#define GNY_CUBE3D_FIRE_SIZE_END  ((f32x2){ 0.05F, 0.15F })
#define GNY_CUBE3D_SMOKE_SIZE     ((f32x2){ 0.7F, 1.2F })
#define GNY_CUBE3D_SMOKE_SIZE_END ((f32x2){ 2.2F, 3.4F })

#define GNY_CUBE3D_FIRE_SPEED  ((f32x2){ 1.4F, 3.0F })
#define GNY_CUBE3D_SMOKE_SPEED ((f32x2){ 0.7F, 1.6F })

/** Upward, with a narrow cone. A wide spread reads as an explosion rather than a fire. */
#define GNY_CUBE3D_PLUME_SPREAD 0.35F

/**
 * Negative gravity: both rise.
 *
 * Hot air is what a plume is, so the acceleration is upward rather than downward. Smoke rises more slowly
 * than fire because it has cooled, which is the same reason it lasts longer and spreads wider.
 * */
#define GNY_CUBE3D_FIRE_GRAVITY  ((f32x3){ 0.0F, 2.2F, 0.0F })
#define GNY_CUBE3D_SMOKE_GRAVITY ((f32x3){ 0.0F, 0.9F, 0.0F })

/**
 * Fire colours, and they are above one on purpose.
 *
 * Additive blending adds these straight into the target, and the tonemap's shoulder is what keeps a stack
 * of them from clipping — so a value past one is a tongue that stays saturated where several overlap
 * rather than washing to white immediately. It also puts the plume over the bloom threshold.
 *
 * Only just past one, though. The first version used 1.6 and every place two billboards overlapped went to
 * white — additive stacking is multiplicative in practice, and a plume is nothing but overlap. The number
 * to tune when a fire looks like a searchlight is this one, not the bloom.
 * */
#define GNY_CUBE3D_FIRE_COLOR_START ((NYA_Color){ 1.15F, 0.52F, 0.14F, 1.0F })
#define GNY_CUBE3D_FIRE_COLOR_END   ((NYA_Color){ 0.55F, 0.09F, 0.02F, 0.0F })

/** Smoke: alpha well below one, which is what routes it into the sorted transparent stream. */
#define GNY_CUBE3D_SMOKE_COLOR_START ((NYA_Color){ 0.26F, 0.24F, 0.24F, 0.55F })
#define GNY_CUBE3D_SMOKE_COLOR_END   ((NYA_Color){ 0.46F, 0.46F, 0.48F, 0.0F })

/*
 * ── What is still missing from this ──
 *
 * A soft-particle fade. A billboard intersecting the ground shows a hard cut line along the intersection,
 * because nothing tells it how close the geometry behind it is. That needs the scene depth as a texture,
 * which this renderer does not yet produce — the refraction capture is colour only.
 *
 * The sprite this note used to ask for now exists; see GNY_CUBE3D_PUFF_TEXTURE.
 */

/*
 * ── Reverb ──
 *
 * The basin is a small hard-walled bowl, so a short bright-ish tail is what an impact in it produces.
 * See NYA_AudioReverb for why room size is not a time in seconds.
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
 * What a hill between a sound and the camera does to it. See nya_audio_occlusion_set for why the engine
 * takes a callback rather than raycasting for itself.
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

/**
 * Live sparks at once, across every impact.
 *
 * A ceiling rather than a growable pool, for the same reason NYA_PHYSICS2D_MAX_HITS is one: a
 * collapsing stack produces a burst of impacts, and there are only so many sparks anyone can see.
 * Past this they are dropped and counted, which is the right behaviour under load.
 * */
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

/**
 * How lit the world is where no crate reaches, as a multiplier on what was drawn.
 *
 * Not zero. A scene with no lights and a zero ambient renders as a black rectangle, which looks
 * exactly like a broken renderer rather than like night — and every real night has a moon.
 * */
#define GNY_AMBIENT_LIGHT ((NYA_Color){ 0.34F, 0.36F, 0.46F, 1.0F })

/** World units a crate's glow reaches. A few crate widths, so overlapping ones pool. */
#define GNY_BOX_LIGHT_RADIUS 190.0F

/** Above one, so the crate itself over-brightens and reads as the source rather than as lit. */
#define GNY_BOX_LIGHT_INTENSITY 1.35F

#define GNY_BOX_LIGHT_COLOR ((NYA_Color){ 1.0F, 0.82F, 0.55F, 1.0F })

/*
 * ── 3D demo dust ──
 *
 * Metres, like everything else in that scene. A "spark" here is a chip of the ground the cube kicks
 * up, so it is centimetre-scale rather than the pixel-scale of the 2D world's — the same numbers in
 * two scenes would be either invisible or the size of the cube.
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

/**
 * Where the demo map's tile (0, 0) sits in the world.
 *
 * The map is 20x12 tiles of 32 units, so 640 by 384. Centred on x, and placed so its solid bottom
 * rows land just above GNY_TERRAIN_BASE_Y — which puts a floor a crate can pile up on inside the
 * camera's opening view rather than somewhere it has to be found.
 * */
#define GNY_TILEMAP_ORIGIN ((f32x2){ -320.0F, GNY_TERRAIN_BASE_Y - 384.0F - 30.0F })

/** The kind tilemap colliders are spawned as, so they can be found and cleared as a group. */
#define GNY_TILEMAP_COLLIDER_KIND GNY_ENTITY_TILEMAP

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
 * Drawn at its own size.
 *
 * One, not a shrink factor: ufbx is asked to convert to metres on load, and both models in this tree
 * come out spanning roughly minus one to one. The 0.01 that was here first was a guess made from the
 * file being 356 KB, and it drew both models two hundredths of a unit across — present, lit, and far
 * too small to notice. test_asset_mesh asserts the models really are unit-sized, so if a re-export
 * changes that, the test says so rather than the scene quietly emptying.
 * */
#define GNY_CUBE3D_MODEL_SCALE 1.0F

/** How far to one side of the cube it stands, so the two are both visible rather than intersecting. */
#define GNY_CUBE3D_MODEL_OFFSET 2.5F

/**
 * Lifted clear of the ground plane, which sits at y zero.
 *
 * One, because the model's own origin is its centre and its lowest vertex is a unit below that — half
 * of it would be under the floor otherwise.
 * */
#define GNY_CUBE3D_MODEL_LIFT 1.0F

/** Radians per second about the world's up axis, so every side of it is visible without being dragged. */
#define GNY_CUBE3D_MODEL_SPIN 0.6F

#define GNY_CUBE3D_MODEL_COLOR ((NYA_Color){ 0.42F, 0.63F, 0.88F, 1.0F })

/*
 * The second model, on the other side of the cube.
 *
 * Two models rather than one, because one proves the loader runs and two prove it is a loader: they are
 * separate assets with separate triangle counts going into the same batch, the same light and the same
 * draw call. A path that quietly reused one buffer for both would show up here as two identical shapes.
 */
#define GNY_CUBE3D_PILL NYA_ASSET_MODELS_PILL_FBX

/** Its own scale, since nothing guarantees two FBX files were authored in the same units. */
#define GNY_CUBE3D_PILL_SCALE 1.0F

/** Mirrored across the cube from GNY_CUBE3D_MODEL_OFFSET, so the three stand in a row. */
#define GNY_CUBE3D_PILL_OFFSET (-GNY_CUBE3D_MODEL_OFFSET)

/*
 * Half the pill's height, not half the cube's.
 *
 * pill.fbx spans about 1.73 either side of its origin on y once its node transform is applied — it is a
 * capsule, stretched along one axis by the node rather than by its vertex data. Lifting it by one, as
 * the other model is, put its lower third through the floor.
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

/**
 * Two lamps orbiting the scene, so the point lights are visibly lights and not a tint.
 *
 * Moving rather than static on purpose: a stationary coloured light is indistinguishable from a coloured
 * ambient, and the thing worth seeing is the terminator sweeping across a curved surface.
 * */
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

/**
 * How strongly curved edges are inked in. See NYA_Render3DMaterial.edge.
 *
 * Shows on the two models, whose edges are rounded geometry, and on the sphere markers. The generated
 * cube has hard faces and gets nothing from it, which is the documented limit of the technique rather
 * than a tuning problem.
 * */
#define GNY_CUBE3D_EDGE 0.55F

/*
 * ─────────────────────────────────────────────────────────
 * THE 3D SCENE'S SHADOWS
 * ─────────────────────────────────────────────────────────
 */

/**
 * How dark a shadowed surface goes, and how far the shadow volume reaches.
 *
 * The strength is well under one on purpose: it is how far toward the ambient a shadow reaches, and a
 * cartoon shadow that lands on black reads as a hole in the floor. The extent is the whole budget the
 * map's resolution is spread across, so it is sized to the ground plane and no further — see
 * NYA_Render3DShadow.extent.
 * */
#define GNY_CUBE3D_SHADOW_STRENGTH 0.45F
/**
 * The *nearest* cascade's half-width. The others widen from it by NYA_RENDER3D_SHADOW_CASCADE_RATIO.
 *
 * Much smaller than it was, because it no longer has to cover the whole terrain on its own: three
 * cascades at a ratio of 2.5 reach 0.16, 0.4 and 1.0 of the extent, so the last one still covers the rim
 * while the first spends its full resolution on the pile in the middle.
 * */
#define GNY_CUBE3D_SHADOW_EXTENT   (GNY_TERRAIN3D_EXTENT * 0.16F)

/** World units per second a networked player moves. See gny_net_apply_command. */
#define GNY_PLAYER_SPEED 220.0F

/** How far apart players spawn, so two joining at once do not start inside each other. */
#define GNY_PLAYER_SPAWN_SPACING 64.0F

/**
 * How large a player is drawn, per side.
 *
 * A drawing constant rather than a collision size: a player has no physics body, so nothing reads this
 * but gny_net_player_on_render. Kept under GNY_PLAYER_SPAWN_SPACING so two players spawning at once are
 * visibly apart rather than touching.
 * */
#define GNY_PLAYER_SIZE 24.0F
