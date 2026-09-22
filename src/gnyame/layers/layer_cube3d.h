/**
 * @file layer_cube3d.h
 *
 * The 3D demo's state: an orbit camera, a draggable cube, two loaded models, a pile of cubes on a
 * noise terrain, and particle effects.
 * */
#pragma once

#include "nyangine/nyangine.h"
#include "gnyame/constants.h"

/** One cube in the pile. The pool is fixed; see GNY_TERRAIN3D_CUBE_COUNT. */
typedef struct GNY_FallingCube {
    NYA_EntityHandle entity;

    /** Full edge length in metres, kept for drawing. */
    f32       size;
    NYA_Color color;

    /** Translucent, drawn in the sorted transparent pass, frosted by `blur` in [0, 1]. */
    b8  glass;
    f32 blur;
} GNY_FallingCube;

/** A mark a landing left on the ground. See GNY_CUBE3D_MARK_COUNT. */
typedef struct GNY_Cube3DMark {
    f32x3     position;
    f32       rotation;
    f32       size;
    NYA_Color color;
    u8        cell;

    /** Uptime the mark was made, zero for a slot never used, and how long it lasts. */
    f32 born_s;
    f32 lifetime_s;
} GNY_Cube3DMark;

/** One slab of the ring around the basin. See layer_cube3d_stones.c and GNY_CUBE3D_STONE_COUNT. */
typedef struct GNY_Cube3DStone {
    NYA_EntityHandle entity;

    /** Where the slab meets the ground, sunk by GNY_CUBE3D_STONE_SINK. The mesh stands on it. */
    f32x3 base;

    /** Full width, height and depth in metres. */
    f32x3 size;

    /** How far it is turned about y, radians. */
    f32 yaw;
} GNY_Cube3DStone;

typedef struct GNY_Cube3DScene {
    NYA_EntityHandle cube;
    NYA_EntityHandle model;
    NYA_EntityHandle pill;

    NYA_Terrain3D* terrain;

    GNY_FallingCube cubes[GNY_TERRAIN3D_CUBE_COUNT];
    u32             cube_count;

    /** Cubes that fell off the world and were put back. Shown in the HUD. */
    u32 cubes_recycled;

    /** Radians, with pitch clamped short of the poles. */
    f32 orbit_yaw;
    f32 orbit_pitch;

    /** Metres from the target. */
    f32 orbit_range;

    /** Left button went down on the cube, not merely down. */
    b8 dragging;

    /** The hint stops once the cube has been grabbed. */
    b8 grabbed_once;

    /**
     * Draws every collision shape over the scene. Off by default; the switchboard turns it on.
     *
     * Kept on the scene rather than in NYA_CONFIG.engine.renderer.features, because it is not one: the
     * renderer never asks about it and a saved config that came back with the hitboxes on would be a
     * puzzle rather than a setting.
     * */
    b8 show_hitboxes;

    NYA_ParticleSystem* dust;

    /** Fire adds and smoke blends, so they are two systems. */
    NYA_ParticleSystem* fire;
    NYA_ParticleSystem* smoke;

    /**
     * The simulated column over the same bonfire: a Navier-Stokes volume whose buoyancy carries the
     * heat the flames put into it. The billboards are the flames, this is the air above them.
     * */
    NYA_Fluid* plume;

    /** Seconds since the plume last emitted. */
    f32 plume_timer_s;

    /** The plume's crackle, looping. */
    NYA_SoundVoice fire_voice;

    /** The skinned bar's clock, and which of its clips it is on. */
    NYA_SkeletonAnimator bender;
    u32                  bender_clip;

    /** Stops the bar's clock where it is. */
    b8 bender_frozen;

    /** Zero until the model loads. */
    u32 bender_bone_count;

    /** A ring: the next landing writes `mark_next`. */
    GNY_Cube3DMark marks[GNY_CUBE3D_MARK_COUNT];
    u32            mark_next;

    /** Where the camera was last frame and when, for the speed lines. */
    f32x3 camera_previous;
    f32   camera_previous_s;

    /** The render feature switchboard is up. See layer_cube3d_features.c. */
    b8 features_open;

    /** The ring of standing stones. See layer_cube3d_stones.c. */
    GNY_Cube3DStone stones[GNY_CUBE3D_STONE_COUNT];
    u32             stone_count;

    /**
     * What the stones are rasterized into for the camera pass, tens of kilobytes, so it is allocated from the
     * world's arena once and kept across a visit like `terrain` rather than taken again each time.
     * */
    NYA_OcclusionBuffer* occlusion;
} GNY_Cube3DScene;

/**
 * Draws the render feature switchboard over the scene: one row per NYA_RenderFeature, writing
 * NYA_CONFIG.engine.renderer.features. Only while `features_open`; `0` toggles it.
 *
 * `show_hitboxes` is the scene's own flag and not one of those rows, which is why it is passed rather
 * than read from the config with the rest.
 * */
void gny_layer_cube3d_features_draw(NYA_UI* ui, NYA_Window* window, b8* show_hitboxes);

/**
 * Registers the standing stones' three meshes and their detail chain, once per run and again after a code
 * reload has emptied the LOD registry. Cheap enough to call every frame; it asks the registry, not a flag.
 * */
void gny_layer_cube3d_stones_register(NYA_Window* window);

/** Builds the ring: the meshes, the detail chain and one static body per slab. */
void gny_layer_cube3d_stones_create(NYA_Window* window);

/** Takes the bodies, the meshes and the chain back down. */
void gny_layer_cube3d_stones_destroy(NYA_Window* window);

/** Puts every slab back on the ground, for when `r` has regenerated the terrain under the ring. */
void gny_layer_cube3d_stones_place(void);

/**
 * Rasterizes each slab's far face into the scene's occlusion buffer and hands it to the camera pass, so what
 * the ring hides is rejected before it is recorded. Costs nothing while occlusion culling is off.
 * */
void gny_layer_cube3d_stones_occlude(NYA_Window* window, f32x3 eye);

/** Draws the ring. One call per slab, all resolving through the detail chain to the same few meshes. */
void gny_layer_cube3d_stones_draw(NYA_Window* window);

/**
 * The nearest and the farthest detail level any slab is drawn at from `eye`, for the HUD row. Both are
 * NYA_RENDER3D_LOD_LEVELS and zero when there is no ring.
 * */
void gny_layer_cube3d_stones_levels(f32x3 eye, OUT u32* out_nearest, OUT u32* out_farthest);
