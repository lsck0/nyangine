/**
 * @file render3d.h
 *
 * ```c
 * void layer_on_render(NYA_Window* window) {
 *     nya_render3d_begin(window, (NYA_Camera3DPerspective){ .position = { 4, 3, 6 }, .target = { 0, 0, 0 } });
 *
 *     nya_render3d_cube(window, (f32x3){ 0, 0, 0 }, (f32x3){ 1, 1, 1 }, entity->rotation, NYA_COLOR_ORANGE);
 *     nya_render3d_grid(window, 10, 1.0F, NYA_COLOR_GRAY);
 *
 *     nya_render3d_end(window);
 *
 *     // Screen pixels again, over the top of the scene, with no camera and no depth test.
 *     nya_render2d_text(window, "hold to spin", 16.0F, 16.0F, NYA_COLOR_WHITE);
 * }
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"
#include "nyangine/math/math_quaternion.h"
#include "nyangine/math/math_vector.h"
#include "nyangine/renderer/render_camera.h"
#include "nyangine/renderer/render_color.h"

typedef struct NYA_Window NYA_Window;

// this file includes nothing that includes it, and nya_render3d_occlusion only takes a pointer.
typedef struct NYA_OcclusionBuffer NYA_OcclusionBuffer;

// defined in renderer.h, which includes this file first; nya_render3d_grass only takes a pointer to it.
typedef struct NYA_Render3DInstance NYA_Render3DInstance;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The handle the built in 3D pipeline is registered under. Shared by every window. */
/**
 * How many point lights one draw call can be lit by.
 * */
#define NYA_RENDER3D_MAX_POINT_LIGHTS 4

/** The vertical field of view, in radians, of a camera that sets none, unless NYA_RenderOptions.fov_y does. */
#define NYA_RENDER3D_FOV_Y ((f32)M_PI / 3.0F)

/**
 * What NYA_Render3DFog.color falls back to: a pale desaturated blue.
 * */
#define NYA_RENDER3D_FOG_COLOR ((NYA_Color){ 0.66F, 0.72F, 0.78F, 1.0F })

/**
 * The reach a point light gets when it names none, in world units.
 * */
#ifndef NYA_RENDER3D_POINT_LIGHT_RANGE
#define NYA_RENDER3D_POINT_LIGHT_RANGE 10.0F
#endif

#define NYA_RENDER3D_PIPELINE_MESH "nya_mesh3d_pipeline"

/**
 * The same pipeline with a sampled base colour texture. Used by nya_render3d_mesh for a textured model.
 * */
#define NYA_RENDER3D_PIPELINE_MESH_TEXTURED "nya_mesh3d_textured_pipeline"

/** The depth-only pipeline the shadow cascades draw with. See nya_render3d_shadow_set. */
#define NYA_RENDER3D_PIPELINE_SHADOW "nya_mesh3d_shadow_pipeline"

// Retained mesh pipelines: they differ only in the vertex stage (model-space vertices plus a per-instance transform); the shared fragment stages keep a model looking the same either way.

/** Instanced, untextured. */
#define NYA_RENDER3D_PIPELINE_INSTANCED "nya_mesh3d_instanced_pipeline"

/** Instanced, with a sampled base colour texture. */
#define NYA_RENDER3D_PIPELINE_INSTANCED_TEXTURED "nya_mesh3d_instanced_textured_pipeline"

/** Instanced, depth only, for the shadow pass. */
#define NYA_RENDER3D_PIPELINE_INSTANCED_SHADOW "nya_mesh3d_instanced_shadow_pipeline"

/** The fullscreen sky. See nya_render3d_sky_draw. */
#define NYA_RENDER3D_PIPELINE_SKY "nya_sky3d_pipeline"

// Transparent pass: the opaque shaders with depth tested but not written, so a pane does not hide the one behind; alpha below one goes here, sorted back to front (see NYA_Render3DStream).

/** Untextured, depth-tested, no depth write. */
/**
 * What nya_render3d_mesh leaves where a model should have been and is not: a magenta box at the scale
 * the caller asked for, outlined rather than solid so a missing prop does not also hide the scene.
 * */
#define NYA_RENDER3D_MISSING_MESH_COLOR     ((NYA_Color){ 1.0F, 0.0F, 1.0F, 1.0F })
#define NYA_RENDER3D_MISSING_MESH_THICKNESS 0.02F

/** The gizmo pipeline: transparent, with neither depth testing nor writing. */
/**
 * The skinned mesh pipeline. See nya_render3d_skinned_mesh.
 * */
#define NYA_RENDER3D_PIPELINE_SKINNED "nya_mesh3d_skinned_pipeline"

/** The same, sampling a base colour texture, for a posed mesh whose parts name one. */
#define NYA_RENDER3D_PIPELINE_SKINNED_TEXTURED "nya_mesh3d_skinned_textured_pipeline"

/** The depth-only skinned pipeline, so a skinned mesh casts a shadow. See nya_render3d_skinned_mesh. */
#define NYA_RENDER3D_PIPELINE_SKINNED_SHADOW "nya_mesh3d_skinned_shadow_pipeline"

/**
 * Wind-swayed foliage. Model-space vertices bent about their base in the vertex stage, so it carries a
 * per-object pivot and the sampled wind that the fully-baked batch cannot. See nya_render3d_foliage.
 * */
#define NYA_RENDER3D_PIPELINE_FOLIAGE "nya_foliage_pipeline"

/**
 * Instanced wind-swayed foliage: the foliage bend on a per-instance model matrix, so a whole field of
 * blades sways from one instanced draw. The instanced counterpart to the foliage pipeline, exactly as the
 * instanced mesh pipeline is to the immediate one. See nya_render3d_grass.
 * */
#define NYA_RENDER3D_PIPELINE_FOLIAGE_INSTANCED "nya_foliage_instanced_pipeline"

#define NYA_RENDER3D_PIPELINE_OVERLAY "nya_mesh3d_overlay_pipeline"

#define NYA_RENDER3D_PIPELINE_TRANSPARENT "nya_mesh3d_transparent_pipeline"

/** Textured, depth-tested, no depth write. */
#define NYA_RENDER3D_PIPELINE_TRANSPARENT_TEXTURED "nya_mesh3d_transparent_textured_pipeline"

/** Instanced, depth-tested, no depth write. For a retained mesh drawn with a translucent tint. */
#define NYA_RENDER3D_PIPELINE_INSTANCED_TRANSPARENT "nya_mesh3d_instanced_transparent_pipeline"

/** Instanced and textured, depth-tested, no depth write. */
#define NYA_RENDER3D_PIPELINE_INSTANCED_TRANSPARENT_TEXTURED "nya_mesh3d_instanced_transparent_textured_pipeline"

/**
 * Refractive glass: samples the captured opaque scene instead of blending over it.
 * */
#define NYA_RENDER3D_PIPELINE_GLASS "nya_mesh3d_glass_pipeline"

/**
 * A flowing water surface. Model-space vertices lifted into travelling waves in the vertex stage, then a
 * fragment stage that refracts the captured scene, blends deep to shallow, and foams the banks and crests.
 * Like foliage, one draw of one registered mesh with its own per-object uniforms. See nya_render3d_water.
 * */
#define NYA_RENDER3D_PIPELINE_WATER "nya_water_pipeline"

/**
 * How long, in seconds, a water surface's flow-map ripple layer takes to wrap before the other layer's copy
 * takes over. The two are half a cycle out of step, so this also sets the crossfade rate. See nya_water_flow.
 * */
#define NYA_RENDER3D_WATER_RIPPLE_CYCLE 6.0F

/**
 * The world-space water depth over which the depth-difference shoreline foam fades, from full at the waterline
 * to none this far down: how far the view ray may travel through water to the bed drawn behind it and still
 * count as shore. A stylized band a little under a metre. See NYA_Render3DWater.depth_foam and water.frag.hlsl.
 * */
#define NYA_RENDER3D_WATER_DEPTH_SHORE 0.75F

// Additive pass: emission like fire brightens toward white rather than averaging, and addition being commutative needs no sorting.

/** Untextured, additive, depth-tested, no depth write. */
#define NYA_RENDER3D_PIPELINE_ADDITIVE "nya_mesh3d_additive_pipeline"

/** Textured, additive, depth-tested, no depth write. The one a flame sprite goes through. */
#define NYA_RENDER3D_PIPELINE_ADDITIVE_TEXTURED "nya_mesh3d_additive_textured_pipeline"

/**
 * The most cascades a window's shadow can be split into, which sizes the uniform arrays. Between one and four.
 * */
#ifndef NYA_RENDER3D_SHADOW_CASCADES
#define NYA_RENDER3D_SHADOW_CASCADES 3
#endif

/**
 * Cascades used when NYA_Render3DShadowOptions.cascades is zero. Two, since the fit follows the frustum: a
 * third cost a scene pass and 0.3 ms a frame in the 3D demo for no visible gain.
 * */
#ifndef NYA_RENDER3D_SHADOW_CASCADES_DEFAULT
#define NYA_RENDER3D_SHADOW_CASCADES_DEFAULT 2
#endif

static_assert(NYA_RENDER3D_SHADOW_CASCADES >= 1 && NYA_RENDER3D_SHADOW_CASCADES <= 4,
              "MESH3D_SHADOW_CASCADES and the uniform's matrix array are sized for at most four");

/** The most passes one scene is drawn in: every cascade, then the camera. */
#define NYA_RENDER3D_PASSES (NYA_RENDER3D_SHADOW_CASCADES + 1)

/**
 * State changes one playback can record, a material, light or texture change each. Past it the scene recorded so far
 * is drawn early, which costs its cascades a second pass.
 * */
#ifndef NYA_RENDER3D_MAX_SEGMENTS
#define NYA_RENDER3D_MAX_SEGMENTS 128
#endif

/**
 * The default sun's direction: upper front left, the one that makes a cube read as a cube by lighting
 * three faces differently. Straight down lights one and leaves four identical.
 * */
#define NYA_RENDER3D_LIGHT_DIRECTION_DEFAULT (nya_vector_normalize((f32x3){ -0.4F, -1.0F, -0.6F }))

/** Texels per side of one cascade when NYA_Render3DShadowOptions.map_size is zero. */
#ifndef NYA_RENDER3D_SHADOW_MAP_SIZE
#define NYA_RENDER3D_SHADOW_MAP_SIZE 1024
#endif

/** The range NYA_Render3DShadowOptions.map_size is clamped into. */
#define NYA_RENDER3D_SHADOW_MAP_SIZE_MIN 256
#define NYA_RENDER3D_SHADOW_MAP_SIZE_MAX 4096

/** Half-width of the shadow volume when NYA_Render3DShadow.extent is zero, in world units. */
#ifndef NYA_RENDER3D_SHADOW_EXTENT
#define NYA_RENDER3D_SHADOW_EXTENT 12.0F
#endif

/** Depth slack when NYA_Render3DShadow.bias is zero. Tuned against a 1024 map over a 12 unit extent. */
#ifndef NYA_RENDER3D_SHADOW_BIAS
#define NYA_RENDER3D_SHADOW_BIAS 0.0015F
#endif

/**
 * Vertices the 3D batch can hold before the scene recorded so far is drawn early.
 * */
#ifndef NYA_RENDER3D_MAX_VERTICES
#define NYA_RENDER3D_MAX_VERTICES 16384
#endif

static_assert(NYA_RENDER3D_MAX_VERTICES <= 65536, "the 3D batch's indices are sixteen bits");

/** Indices the 3D batch can hold. Six per quad, so a cube of six quads is thirty-six. */
#ifndef NYA_RENDER3D_MAX_INDICES
#define NYA_RENDER3D_MAX_INDICES (NYA_RENDER3D_MAX_VERTICES * 3)
#endif

/**
 * Drawn copies of retained meshes one frame can hold.
 * */
#ifndef NYA_RENDER3D_MAX_INSTANCES
#define NYA_RENDER3D_MAX_INSTANCES 1024
#endif

/**
 * Grass blades one frame can hold across every nya_render3d_grass patch. Higher than the retained ceiling
 * because density is the whole point: a field is many blades in one instanced draw, on their own buffer so
 * they do not spend the retained mesh budget. Blades past this are counted and dropped, never drawn wrong.
 * Ceiling-registered. See nya_render3d_grass.
 * */
#ifndef NYA_RENDER3D_MAX_GRASS_INSTANCES
#define NYA_RENDER3D_MAX_GRASS_INSTANCES 16384
#endif

/**
 * Distinct meshes one frame can draw instanced.
 * */
#ifndef NYA_RENDER3D_MAX_MESH_GROUPS
#define NYA_RENDER3D_MAX_MESH_GROUPS 64
#endif

/**
 * Meshes a caller can register at once. See nya_render3d_mesh_register.
 * */
#ifndef NYA_RENDER3D_MAX_REGISTERED_MESHES
#define NYA_RENDER3D_MAX_REGISTERED_MESHES 256
#endif

/** Longest handle a registered mesh may have, terminator included. The registry keeps its own copy. */
#ifndef NYA_RENDER3D_MESH_HANDLE_MAX
#define NYA_RENDER3D_MESH_HANDLE_MAX 128
#endif

/**
 * How many foliage disturbers one plant's vertex shader tests against. The shader loops over exactly
 * this many, so it is a fixed cost and must stay small; each plant is given the nearest this-many of
 * whatever was fed this frame. See nya_render3d_foliage_disturb.
 * */
#define NYA_RENDER3D_FOLIAGE_DISTURBERS 4

/**
 * How many disturbers a frame can be fed in total, before the nearest few are picked per plant. Bodies
 * past this are counted and dropped, never drawn wrong. Ceiling-registered.
 * */
#ifndef NYA_RENDER3D_FOLIAGE_DISTURBERS_MAX
#define NYA_RENDER3D_FOLIAGE_DISTURBERS_MAX 16
#endif

/** Segments around a sphere's equator. Halved for the rings from pole to pole. */
/**
 * The handle the shared unit sphere is registered under, by the first nya_render3d_sphere of the run.
 *
 * Reserved, and nya_render3d_mesh_register refuses it rather than trusting a caller to have read this.
 * */
/**
 * The shadow map's colour format: one normalized 16-bit channel. The map stores a depth in [0, 1], which
 * R16_UNORM covers evenly at half the memory of R32_FLOAT. The texture and three pipelines must agree on it.
 * */
#define NYA_RENDER3D_SHADOW_FORMAT SDL_GPU_TEXTUREFORMAT_R16_UNORM

/**
 * The largest a planar water reflection texture (see NYA_Render3DWater.reflection) gets on a side. The
 * reflection is a low-frequency mirrored sky sampled at the fragment's screen position, so half the target's
 * resolution capped here is ample and keeps the extra pass cheap.
 * */
#ifndef NYA_RENDER3D_REFLECTION_MAX
#define NYA_RENDER3D_REFLECTION_MAX 1024
#endif

/**
 * The scene normal buffer's format: the world normal in rgb and the distance from the camera in alpha, zero where
 * no opaque surface was drawn. What the screen-space passes read; see NYA_PostInk.
 *
 * Written by the scene pass itself as a second colour target, since a separate pass would draw the scene twice.
 * Only a render texture created with NYA_RenderTextureOptions.normals has one, so a scene drawn to the window pays
 * nothing. A pass needs pipelines built for its exact targets, so every 3D pipeline has a variant with this
 * target (`normals` on the load parameters) and the batch reopens the pass with the buffer attached when 3D draws
 * and without it when 2D does.
 *
 * Multisampled like the colour, and resolved once when the render texture ends. The resolve averages, so an edge
 * pixel holds a blend of both sides: the ink reads that as a softer edge and the occlusion's range check rejects
 * it. Half floats, because eight bits of distance cannot tell a crease from a step.
 * */
#define NYA_RENDER3D_NORMAL_FORMAT SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT

#define NYA_RENDER3D_MESH_UNIT_SPHERE "nya_unit_sphere"

#ifndef NYA_RENDER3D_SPHERE_SEGMENTS
#define NYA_RENDER3D_SPHERE_SEGMENTS 24
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_Render3DLight      NYA_Render3DLight;
typedef struct NYA_Render3DPointLight NYA_Render3DPointLight;
typedef struct NYA_Render3DShadowFit NYA_Render3DShadowFit;
typedef struct NYA_Render3DShadowOptions NYA_Render3DShadowOptions;
typedef struct NYA_Render3DShadow     NYA_Render3DShadow;
typedef struct NYA_Render3DMaterial NYA_Render3DMaterial;
typedef struct NYA_Render3DFog      NYA_Render3DFog;
typedef struct NYA_Render3DSky      NYA_Render3DSky;
typedef enum NYA_Render3DBlend      NYA_Render3DBlend;
typedef enum NYA_Render3DDepth      NYA_Render3DDepth;
typedef struct NYA_Render3DTextureBinding NYA_Render3DTextureBinding;
typedef enum NYA_FoliageStyle       NYA_FoliageStyle;
typedef struct NYA_Render3DFoliage  NYA_Render3DFoliage;
typedef struct NYA_Render3DWater    NYA_Render3DWater;

/* Forward declared: NYA_Vertex3D belongs to renderer.h, which includes this file. */
typedef struct NYA_Vertex3D NYA_Vertex3D;

/**
 * A texture and its sampler, already looked up. See nya_render3d_texture_resolve.
 * */
struct NYA_Render3DTextureBinding {
    void* texture;
    void* sampler;
};

/**
 * How the transparent pass combines with what is already there.
 * */
/**
 * Whether geometry takes part in depth at all.
 * */
enum NYA_Render3DDepth {
    /** Tested and written. What every piece of world geometry wants. */
    NYA_RENDER3D_DEPTH_DEFAULT,

    /** Neither tested nor written: always visible, and hidden from everything drawn after it. */
    NYA_RENDER3D_DEPTH_OVERLAY,
};

enum NYA_Render3DBlend {
    /** Blend over what is behind, by alpha. Sorted back to front. The default, and the zero value. */
    NYA_RENDER3D_BLEND_ALPHA = 0,

    /**
     * Add to what is behind, for anything emitting light rather than occluding. Geometry drawn under it is never
     * opaque, however solid its colour: a flame at full alpha still adds rather than writing depth.
     * */
    NYA_RENDER3D_BLEND_ADDITIVE,

    NYA_RENDER3D_BLEND_COUNT,
};

/**
 * A procedural sky: a vertical gradient, a ground half, and a sun.
 * */
struct NYA_Render3DSky {
    /** Straight up. Zero becomes a mid blue. */
    NYA_Color zenith;

    /** At eye level. Zero becomes a pale blue. */
    NYA_Color horizon;

    /** Below level: haze, sea, distant ground. Zero becomes a dim slate. */
    NYA_Color ground;

    /**
     * Where the sun is, as a direction pointing *toward* it from the viewer.
     * */
    f32x3 sun_direction;

    NYA_Color sun_color;

    /**
     * Angular radius of the disc, in radians. Zero becomes about half a degree, which is life-size.
     * */
    f32 sun_angle;

    /** How bright the disc and its halo are. Zero becomes one. */
    f32 sun_intensity;

    /**
     * How tightly the halo hugs the disc. Zero becomes a moderate spread.
     * */
    f32 sun_halo;

    /** Exponent on the gradient. Zero becomes one, a linear ramp from horizon to zenith. */
    f32 horizon_softness;

    /** How wide the fade between sky and ground is. Zero becomes a narrow band. */
    f32 ground_blend;
};
typedef struct NYA_Render3DRay      NYA_Render3DRay;

/**
 * The one directional light everything drawn through here is shaded by.
 * */
struct NYA_Render3DLight {
    /**
     * Which way the light travels, in world space. Normalized on the way in.
     * */
    f32x3 direction;

    NYA_Color color;

    /**
     * How lit a surface facing away from the light still is, in [0, 1].
     * */
    f32 ambient;

    /** Scales the lit term only. One is neutral; zero leaves everything at the ambient. */
    f32 intensity;

    /**
     * The ambient's colour from above and from below, blended by which way a surface faces: sky light on tops,
     * bounce light on undersides. Zero alpha takes `color`, the flat ambient.
     * */
    NYA_Color sky;
    NYA_Color ground;
};

/**
 * Distance and height fog, the depth cue a flat-shaded renderer otherwise lacks.
 *
 * ```c
 * nya_render3d_fog_set(window, (NYA_Render3DFog){ .color = sky.horizon, .density = 0.02F, .aerial = 0.75F });
 * ```
 * */
// @reflect
struct NYA_Render3DFog {
    /**
     * What distance fades toward. Zero becomes NYA_RENDER3D_FOG_COLOR.
     * */
    NYA_Color color;

    /** How quickly fog closes in, per world unit. Zero disables fog, shader branch included. */
    f32 density;

    /**
     * How fast fog thins above `height_base`, per world unit. Zero makes it uniform.
     * */
    f32 height_falloff;

    /** The world y at which `height_falloff` starts thinning. Zero is the origin plane. */
    f32 height_base;

    /**
     * How far fog tints toward the light's own colour when looking into it, in [0, 1]. Zero is off.
     * */
    f32 sun_amount;

    /**
     * Aerial perspective: how many times faster than the fog itself distance draws a surface's hue toward the fog's,
     * keeping its brightness, so far ground reads as far without washing out. Zero is off.
     * */
    f32 aerial;
};

/**
 * How a surface responds to light: the metallic-roughness half of a glTF material.
 * */
/* The names follow glTF; each field says what it controls here. */
struct NYA_Render3DMaterial {
    /**
     * How strong the single hard-edged highlight is, in [0, 1]. Zero for none.
     * */
    f32 metallic;

    /**
     * How soft the transitions between shading bands are, in [0, 1]. From one half up the bands fade into a smooth
     * gradient, reached at one.
     * */
    f32 roughness;

    /**
     * How strong the rim light on the silhouette is, in [0, 1]. Zero for none.
     * */
    f32 reflectance;

    /**
     * How far this surface bends what is behind it, in [0, 1]. Zero for none.
     *
     * Only works when the scene is drawn into a render texture, which is the only target resolved mid-frame.
     * Drawn to the window, the surface falls back to plain blending.
     * */
    f32 refraction;

    /**
     * How much the view through this surface is blurred, in [0, 1]. Zero is clear glass.
     *
     * A fixed tap count with a widening radius, so a very heavy blur shows its taps as a faint grid. A
     * downsampled chain would fix it, as for GNY_BLOOM_2D_SPREAD.
     * */
    f32 blur;

    /**
     * How much of the surface's own colour is added regardless of any light. Zero for none.
     * */
    f32 emission;

    /**
     * How strongly curved edges are darkened, in [0, 1]. Zero for none.
     *
     * It measures how fast the interpolated normal turns per pixel, so a rounded cube shows it and a hard-edged
     * cube does not. Hard creases need neighbour information this pass lacks, which NYA_PostInk has.
     * */
    f32 edge;
};

/**
 * A light at a point in the world, falling off with distance. What a lamp, a torch or a muzzle flash is.
 * */
struct NYA_Render3DPointLight {
    f32x3 position;

    NYA_Color color;

    /**
     * How far the light reaches, in world units. Attenuation is exactly zero at this distance.
     * */
    f32 range;

    /** Scales the light. One is neutral; zero is off, which is cheaper expressed by not adding it. */
    f32 intensity;
};

/**
 * A window's shadow quality: what the atlas is allocated for. Zeroed fields take the defaults, so a zeroed struct
 * is a valid one. See nya_render3d_shadow_options_set.
 * */
struct NYA_Render3DShadowOptions {
    /** How many slices of the view get their own map, up to NYA_RENDER3D_SHADOW_CASCADES. Each costs a scene pass. */
    u32 cascades;

    /** Texels per side of one cascade, rounded up to a power of two. Memory grows with its square. */
    u32 map_size;

    /**
     * The hue shadowed and dark banded surfaces lean toward, with alpha as how far. Brightness is kept, so a cool
     * colour makes shade blue rather than darker. Zero alpha leaves shade a plain darkening.
     * */
    NYA_Color color;
};

/**
 * The parts of a shadow volume that are not derived from the camera. See nya_render3d_shadow_for_camera.
 * */
struct NYA_Render3DShadowFit {
    /**
     * How far from the camera shadows are cast, in world units. Zero is NYA_RENDER3D_SHADOW_EXTENT. This is a
     * distance down the view that the cascades divide, not the size of one cascade.
     * */
    f32 range;

    /**
     * Where shadow casting starts, as a distance down the view. Zero is the camera's near plane.
     *
     * Named `near_distance` because `near` and `far` are macros in the Windows headers MinGW still ships.
     *
     * Set it whenever the camera is further from its subject than the subject is wide, usually the distance to
     * the scene's centre minus its radius. Otherwise the sharp near cascades cover empty air and the whole scene
     * falls into the coarsest one.
     * */
    f32 near_distance;

    /**
     * The camera's aspect ratio, width over height. Zero is the render target's through nya_render3d_shadow_set, and
     * 16:9 for nya_render3d_shadow_for_camera.
     * */
    f32 aspect;

    /** How dark a shadow goes. Zero disables them, so this is the field a caller must set. */
    f32 strength;

    /** Passed through. Zero is NYA_RENDER3D_SHADOW_BIAS. */
    f32 bias;

    /** Passed through. Zero is four times the cascade's extent. */
    f32 depth;

    /**
     * Leave the volume where the camera puts it, unsnapped.
     * */
    b8 no_texel_snap;
};

/**
 * What the shadow pass covers, and how the result looks.
 * */
struct NYA_Render3DShadow {
    /**
     * What the shadow volume is centred on, in world space.
     * */
    f32x3 center;

    /**
     * Half the width of the volume, in world units. Everything outside it is lit.
     * */
    f32 extent;

    /**
     * How far along the light the volume reaches, in world units.
     * */
    f32 depth;

    /**
     * How dark a fully shadowed surface goes, in [0, 1]. Zero disables shadows outright.
     * */
    f32 strength;

    /**
     * Depth slack against shadow acne, in light-space depth units. Zero is read as
     * NYA_RENDER3D_SHADOW_BIAS.
     * */
    f32 bias;

    /** Which cascade this volume covers, from zero for the nearest. */
    u32 cascade;
};

/** A ray in world space. What nya_render3d_screen_ray produces and a physics raycast consumes. */
struct NYA_Render3DRay {
    f32x3 origin;

    /** Unit length. Multiply it by how far the query should reach. */
    f32x3 direction;
};

/**
 * The three foliage looks, all one shader and one wind field apart only in the parameters below. See
 * nya_render3d_foliage_style, which fills a NYA_Render3DFoliage with a sensible set for each.
 * */
enum NYA_FoliageStyle {
    /** A full, base-anchored low-frequency bend. Blades of grass, reeds, a wheat field. */
    NYA_FOLIAGE_GRASS = 0,

    /** A low-amplitude, high-frequency flutter with a per-instance phase. A canopy of leaves. */
    NYA_FOLIAGE_LEAVES,

    /** Stiff: a long wavelength and a small amplitude. Branches and trunks that barely give. */
    NYA_FOLIAGE_BRANCHES,

    NYA_FOLIAGE_STYLE_COUNT,
};

/**
 * How one plant sways. Passed by value to nya_render3d_foliage. The wind is sampled by the caller from a
 * NYA_WindField (see render_wind.h) so foliage stays independent of the field; the rest is the material
 * that makes the same authored mesh read as grass, a leaf, or a branch. Zeroed fields take their defaults,
 * so `(NYA_Render3DFoliage){ .wind = w }` is a valid light grass.
 * */
struct NYA_Render3DFoliage {
    /** The wind's displacement/force at the plant, world space. From nya_wind_sample. */
    f32x3 wind;

    /** The wind field's time, so the sway animates. From NYA_WindField.time. */
    f32 time;

    /** Tip sway as a fraction of the plant's height. Zero is read as a light default. */
    f32 amplitude;

    /** The primary bend rate. Zero is read as a default. */
    f32 frequency;

    /** How rigid the plant is, in [0, 1]: zero bends fully, one barely moves. */
    f32 stiffness;

    /** High-frequency flutter amplitude, for leaves. Zero for grass and branches. */
    f32 flutter;

    /** The flutter rate. Zero is read as a default when `flutter` is set. */
    f32 detail_frequency;

    /**
     * A per-object phase offset, so identical plants do not sway in lockstep. Any value; a hash of the
     * plant's position is a good source, and nya_render3d_foliage seeds one from the placement when this
     * is zero.
     * */
    f32 phase;

    /** Multiplied into the mesh's vertex colour. A zeroed colour is read as white. */
    NYA_Color tint;
};

/**
 * A flowing water surface. Passed by value to nya_render3d_water. Zeroed fields take sensible defaults, so
 * `(NYA_Render3DWater){ 0 }` is a plausible calm river along +x; the caller usually sets at least the flow and
 * the colours. The surface mesh (a flat strip the caller lays along the riverbed and registers, its vertices'
 * still height at y = 0) carries the shore weight in each vertex colour's alpha — 0 down the channel, 1 at the
 * banks — which drives both the deep-to-shallow colour and the shoreline foam.
 * */
struct NYA_Render3DWater {
    /** Which way the current flows, in world space. The horizontal part is used; a zero vector is read as +x. */
    f32x3 flow_direction;

    /** How fast the wave crests travel along the flow. Zero is read as a gentle default. */
    f32 flow_speed;

    /** Wave height in world units. Zero is read as a small default; the surface never lifts past a set multiple of it. */
    f32 wave_amplitude;

    /** Wave wavelength as a spatial frequency (larger is choppier, shorter waves). Zero is read as a default. */
    f32 wave_frequency;

    /**
     * How much the wind field bends the flow, in [0, 1]. Zero leaves the water on its own steady current; higher
     * lets a sampled wind (see `wind`) hurry the travel and lift the chop, so water shares one wind with foliage
     * and particles. Off by default, since a caller need not have a wind field.
     * */
    f32 wind_influence;

    /** The wind's horizontal push at the surface, from nya_wind_sample. Only read when `wind_influence` is set. */
    f32x3 wind;

    /** How sharply the crests pinch, in [0, 1]: zero is round swells, one is peaked chop. A light default when zero. */
    f32 choppiness;

    /** The deep channel colour. A zeroed colour is read as a deep blue-green. */
    NYA_Color deep_color;

    /** The shallow bank colour. A zeroed colour is read as a pale teal. */
    NYA_Color shallow_color;

    /** How opaque the surface is where it does not refract, in [0, 1]. Zero is read as a mostly-opaque default. */
    f32 opacity;

    /** How strongly the surface refracts the scene behind it, in [0, 1]. Only visible when the scene is a render
     *  texture, the same limit the glass material has; drawn to the window the body falls back to its colour. */
    f32 refraction;

    /** How wide the foam band along the banks is, as a fraction of the shore weight, in [0, 1]. Zero is a thin default. */
    f32 foam;

    /**
     * How strongly shoreline foam is driven by the true water depth over the bed rather than the authored shore
     * weight, in [0, 1]: zero (the default) foams from the mesh's vertex-alpha shore band alone, any positive value
     * foams wherever the water is shallow — where the scene drawn behind the surface sits close beneath it, so the
     * foam wraps the banks *and* rings any obstacle that rises near the surface, and its scale follows the real
     * geometry. The depth is read from the scene distance buffer the renderer records only for a render texture
     * created with NYA_RenderTextureOptions.normals; drawn to the window, or to a target without that buffer, the
     * surface falls back to the authored shore band. The value scales how much the depth foam replaces it.
     * */
    f32 depth_foam;

    /**
     * Whether the surface mirrors a real planar reflection of the sky instead of the flat Fresnel tint, in
     * [0, 1]: zero (the default) keeps the cheap constant reflection tint, any positive value renders the sky
     * mirrored about the still surface into a bounded reflection texture (NYA_RENDER3D_REFLECTION_MAX) and
     * Fresnel-blends it in, distorted by the surface ripples. Only visible when the scene is a render texture,
     * the same limit refraction has; drawn straight to the window the surface falls back to the tint. The
     * value scales how far the reflection wins over the body colour, so a hazier surface can dial it down.
     * */
    f32 reflection;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * FRAME
 * ─────────────────────────────────────────────────────────
 */

/**
 * Starts drawing in 3D, through a perspective camera.
 * */
NYA_API void nya_render3d_begin(NYA_Window* window, NYA_Camera3DPerspective camera);

/** The same, through an orthographic camera. See NYA_Camera3DOrthographic. */
NYA_API void nya_render3d_begin_orthographic(NYA_Window* window, NYA_Camera3DOrthographic camera);

/**
 * Draws everything queued and stops drawing in 3D.
 * */
NYA_API void nya_render3d_end(NYA_Window* window);

/** Whether nya_render3d_begin has been called and not yet ended. */
NYA_API b8 nya_render3d_active(NYA_Window* window) __attr_no_discard;

/**
 * Replaces the directional light. Flushes, because the light is a per draw call uniform.
 * */
/**
 * Draws the sky behind everything else in the current 3D scene.
 *
 * ```c
 * nya_render3d_begin(window, camera);
 * nya_render3d_sky_draw(window, (NYA_Render3DSky){ .sun_direction = -sun.direction, .sun_angle = 0.04F });
 * // ... the scene ...
 * nya_render3d_end(window);
 * ```
 * */
NYA_API void nya_render3d_sky_draw(NYA_Window* window, NYA_Render3DSky sky);

/**
 * Culls this pass against an occlusion buffer as well as the frustum. Null turns it back off.
 * */
NYA_API void nya_render3d_occlusion(NYA_Window* window, const NYA_OcclusionBuffer* buffer);

/**
 * The camera matrix this pass is drawing with, for handing to nya_occlusion_begin.
 * */
NYA_API f32_4x4 nya_render3d_view_projection(NYA_Window* window) __attr_no_discard;

/**
 * Switches later geometry between alpha blending and adding. Under NYA_RENDER3D_BLEND_ADDITIVE everything drawn is
 * translucent, since an opaque draw would write depth instead of adding.
 * */
NYA_API void nya_render3d_blend_set(NYA_Window* window, NYA_Render3DBlend blend);

/**
 * Whether what follows is occluded by the scene or drawn over it. See NYA_Render3DDepth.
 * */
NYA_API void nya_render3d_depth_set(NYA_Window* window, NYA_Render3DDepth depth);

NYA_API NYA_Render3DDepth nya_render3d_depth(NYA_Window* window) __attr_no_discard;

/**
 * A camera-facing quad at `center`, `size` across, spun by `rotation` radians in screen space.
 * */
NYA_API void nya_render3d_billboard(NYA_Window* window, NYA_ConstCString texture_handle, f32x3 center, f32x2 size, f32 rotation,
                                    NYA_Color color);

/**
 * Resolves a texture handle once, for a caller about to draw many billboards with it.
 * */
NYA_API NYA_Render3DTextureBinding nya_render3d_texture_resolve(NYA_ConstCString texture_handle) __attr_no_discard;

/** nya_render3d_billboard with the texture already resolved. See nya_render3d_texture_resolve. */
NYA_API void nya_render3d_billboard_resolved(NYA_Window* window, NYA_Render3DTextureBinding texture, f32x3 center, f32x2 size, f32 rotation,
                                             NYA_Color color);

NYA_API void nya_render3d_light_set(NYA_Window* window, NYA_Render3DLight light);

/** The light currently in effect. */
NYA_API NYA_Render3DLight nya_render3d_light(NYA_Window* window) __attr_no_discard;

/**
 * Sets the frame's fog. Flushes, because the fog rides in the fragment uniform.
 *
 * ```c
 * nya_render3d_fog_set(window, (NYA_Render3DFog){ .color = sky.bottom, .density = 0.018F, .sun_amount = 0.4F });
 * ```
 * */
NYA_API void nya_render3d_fog_set(NYA_Window* window, NYA_Render3DFog fog);

/** The fog currently in effect. */
NYA_API NYA_Render3DFog nya_render3d_fog(NYA_Window* window) __attr_no_discard;

/**
 * Adds a point light to the frame. Flushes, because the light set is a fragment uniform.
 * */
NYA_API void nya_render3d_point_light_add(NYA_Window* window, NYA_Render3DPointLight light);

/** Removes every point light. Flushes, for the same reason adding one does. */
NYA_API void nya_render3d_point_lights_clear(NYA_Window* window);

/**
 * Casts the sun's shadow in every scene the window draws from now on. The scene is recorded once and drawn into each
 * cascade before the camera sees it, fitted to the camera by `fit` and cast along the light in effect when the scene
 * first draws something. Zero strength turns shadows off. Only perspective cameras cast them.
 *
 * ```c
 * nya_render3d_shadow_set(window, (NYA_Render3DShadowFit){ .strength = 0.45F });
 *
 * nya_render3d_begin(window, camera);
 * nya_render3d_light_set(window, sun);
 * draw_the_scene(window);
 * nya_render3d_end(window);
 * ```
 * */
NYA_API void nya_render3d_shadow_set(NYA_Window* window, NYA_Render3DShadowFit fit);

/** The fit as set. */
NYA_API NYA_Render3DShadowFit nya_render3d_shadow(NYA_Window* window) __attr_no_discard;

/**
 * Whether what is drawn from here on casts a shadow. On by default and at every begin. A translucent billboard
 * would cast a solid square, which is what turning it off is for. A change costs a draw call.
 * */
NYA_API void nya_render3d_shadow_cast_set(NYA_Window* window, b8 casts_shadow);

/** Whether what is drawn now casts a shadow, so a caller can restore what it found. */
NYA_API b8 nya_render3d_shadow_casts(NYA_Window* window) __attr_no_discard;

/**
 * The light's own axes: where it points, and an up that is not parallel to it. The direction is followed exactly,
 * so a turning sun moves the shadow map smoothly rather than in steps.
 * */
NYA_API void nya_render3d_light_basis(f32x3 direction, OUT f32x3* out_forward, OUT f32x3* out_right, OUT f32x3* out_up);

/**
 * The matrix a shadow pass rasterises with, and the matrix the scene pass samples through.
 * */
NYA_API f32_4x4 nya_render3d_shadow_view_projection(
    f32x3       center,
    f32x3       light_direction,
    f32         extent,
    f32         depth,
    OUT f32x3*  out_eye
);

/**
 * A cascade's shadow volume, fitted to a camera over the window's cascade count and snapped to its texel grid. What
 * nya_render3d_shadow_set fits every cascade with.
 * */
NYA_API NYA_Render3DShadow nya_render3d_shadow_for_camera(
    const NYA_Window*       window,
    NYA_Camera3DPerspective camera,
    f32x3                   light_direction,
    u32                     cascade,
    NYA_Render3DShadowFit   fit
) __attr_no_discard;

/**
 * Sets how many cascades the window's shadow uses and how large each is, with zeroes taking the defaults and the
 * rest clamped into range. A change releases the atlas and the next pass allocates it at the new size, so this is
 * cheap to call every frame from a config.
 *
 * ```c
 * nya_render3d_shadow_options_set(window, (NYA_Render3DShadowOptions){ .cascades = 2, .map_size = 2048 });
 * ```
 * */
NYA_API void nya_render3d_shadow_options_set(NYA_Window* window, NYA_Render3DShadowOptions options);

/** The window's shadow options with defaults and clamping applied. */
NYA_API NYA_Render3DShadowOptions nya_render3d_shadow_options(const NYA_Window* window) __attr_no_discard;

/** How many point lights the frame currently has, at most NYA_RENDER3D_MAX_POINT_LIGHTS. */
NYA_API u32 nya_render3d_point_light_count(NYA_Window* window) __attr_no_discard;

/**
 * Replaces the material everything drawn from here on responds with. Flushes.
 *
 * ```c
 * nya_render3d_material_set(window, (NYA_Render3DMaterial){ .metallic = 1.0F, .roughness = 0.25F });
 * ```
 * */
NYA_API void nya_render3d_material_set(NYA_Window* window, NYA_Render3DMaterial material);

/** The material currently in effect. */
NYA_API NYA_Render3DMaterial nya_render3d_material(NYA_Window* window) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * PRIMITIVES
 * ─────────────────────────────────────────────────────────
 *
 * All take world-space positions and a rotation and bake both into the vertices, which keeps a
 * hundred of them in one draw call. Moving one means rebuilding it, which at these vertex counts is
 * cheaper than the draw call saved.
 *
 * Every face is wound counter-clockwise seen from outside, so back-face culling works.
 */

/** A box centred on `center`, `size` being its full extents, turned by `rotation`. */
NYA_API void nya_render3d_cube(NYA_Window* window, f32x3 center, f32x3 size, NYA_Quaternion rotation, NYA_Color color);

/**
 * The same box as twelve edges rather than six faces.
 * */
NYA_API void nya_render3d_cube_outline(NYA_Window* window, f32x3 center, f32x3 size, NYA_Quaternion rotation, f32 thickness, NYA_Color color);

/** A UV sphere of `radius`, at NYA_RENDER3D_SPHERE_SEGMENTS around. */
NYA_API void nya_render3d_sphere(NYA_Window* window, f32x3 center, f32 radius, NYA_Color color);

/**
 * A flat quad, `size` wide and deep, lying in the xz plane and facing +y.
 * */
NYA_API void nya_render3d_plane(NYA_Window* window, f32x3 center, f32x2 size, NYA_Color color);

/**
 * One flat-shaded triangle, wound counter-clockwise seen from the side it faces.
 * */
NYA_API void nya_render3d_triangle(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, NYA_Color color);

/**
 * Two triangles sharing the a-c diagonal, with one normal taken from the first of them.
 * */
NYA_API void nya_render3d_quad(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color);

/**
 * A line from `from` to `to`, as a square prism of `thickness` world units.
 * */
NYA_API void nya_render3d_line(NYA_Window* window, f32x3 from, f32x3 to, f32 thickness, NYA_Color color);

/**
 * A wireframe grid on the xz plane, `half_extent` cells out from the origin in each direction.
 * */
NYA_API void nya_render3d_grid(NYA_Window* window, u32 half_extent, f32 cell_size, NYA_Color color);

/**
 * Draws a model loaded as NYA_ASSET_TYPE_MESH.
 *
 * ```c
 * nya_render3d_mesh(window, NYA_ASSET_MODELS_CUBIE_FBX, position, (f32x3){ 1, 1, 1 }, rotation, NYA_COLOR_WHITE);
 * ```
 * */
NYA_API void nya_render3d_mesh(NYA_Window* window, NYA_ConstCString handle, f32x3 center, f32x3 scale, NYA_Quaternion rotation, NYA_Color color);

/**
 * The axis-aligned bounds of a loaded mesh, in the model's own units. False when it is not loaded yet.
 * */
NYA_API b8 nya_render3d_mesh_bounds(NYA_Window* window, NYA_ConstCString handle, OUT f32x3* out_min, OUT f32x3* out_max) __attr_no_discard;

/**
 * Uploads geometry the caller built and keeps it under `handle`, for nya_render3d_mesh to draw.
 *
 * ```c
 * // Once, when the surface is generated.
 * nya_render3d_mesh_register(window, "my_terrain", vertices, count);
 *
 * // Every frame, from then on: one instanced draw and no vertex work at all.
 * nya_render3d_mesh(window, "my_terrain", f32x3_zero, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, NYA_COLOR_WHITE);
 * ```
 * */
/**
 * The world-space sphere a posed mesh occupies, for culling it.
 *
 * `palette` is the bone matrices as nya_render3d_skinned_mesh takes them, `model` the transform it
 * places them with, and `rest_min`/`rest_max` the mesh's own bounds before it was animated.
 *
 * The sphere is around the posed bone origins, padded by the rest model's radius: a skinned mesh is
 * wherever its bones are, and its rest bounds only say where it stands before it is animated. Loose on
 * purpose. A bound that is too big costs a draw that could have been skipped; one that is too small
 * takes a limb out of a shadow, and that is visible.
 *
 * False when there are no bones to bound, which is a draw with nothing to place.
 * */
NYA_API b8 nya_render3d_skinned_bounds(const f32_4x4* palette, u32 bone_count, f32_4x4 model, f32x3 rest_min, f32x3 rest_max,
                                       OUT f32x3* out_center, OUT f32* out_radius) __attr_no_discard;

/**
 * Draws a skinned mesh, posed by `palette`. See core_skeleton.h for how a palette is built.
 *
 * ```c
 * nya_skeleton_animator_update(&animator, delta_time_s, &pose);
 * nya_skeleton_palette(skeleton, &pose, palette);
 * nya_render3d_skinned_mesh(window, NYA_ASSET_MODELS_BENDER_FBX, palette, skeleton->bone_count, placement, NYA_COLOR_WHITE);
 * ```
 * */
NYA_API void nya_render3d_skinned_mesh(NYA_Window* window, NYA_ConstCString handle, const f32_4x4* palette, u32 bone_count,
                                       f32_4x4 model, NYA_Color tint);

NYA_API b8 nya_render3d_mesh_register(NYA_Window* window, NYA_ConstCString handle, const NYA_Vertex3D* vertices, u32 vertex_count);

/**
 * Draws a registered mesh as wind-swayed foliage: model-space vertices bent about their base in the
 * vertex stage before they are view-projected, so the plant anchors at its pivot and bends more toward
 * its tips. `handle` must have been registered with nya_render3d_mesh_register; author the plant with
 * its base at y = 0 and a flexibility weight up its height in the vertex colour's alpha (0 at the base,
 * 1 at the tips). Foliage is lit and receives shadows like any mesh, but does not itself cast one.
 *
 * ```c
 * NYA_Render3DFoliage grass = nya_render3d_foliage_style(NYA_FOLIAGE_GRASS);
 * grass.wind = nya_wind_sample(&wind, tuft_position);
 * grass.time = wind.time;
 * nya_render3d_foliage(window, "grass_tuft", tuft_position, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, grass);
 * ```
 * */
NYA_API void nya_render3d_foliage(NYA_Window* window, NYA_ConstCString handle, f32x3 position, f32x3 scale, NYA_Quaternion rotation,
                                  NYA_Render3DFoliage foliage);

/**
 * A NYA_Render3DFoliage filled with a sensible parameter set for one of the three looks. The caller
 * still sets `wind`, `time` and `tint`; this only chooses amplitude, frequency, stiffness and flutter.
 * */
NYA_API NYA_Render3DFoliage nya_render3d_foliage_style(NYA_FoliageStyle style) __attr_no_discard;

/**
 * Draws a whole field of wind-swayed blades in ONE instanced draw: a registered blade mesh placed once per
 * NYA_Render3DInstance, every copy bent about its base by the shared wind in `look`. This is
 * nya_render3d_foliage's instanced sibling — density is the point, so a dense field costs one draw call
 * rather than one per blade. Reuses NYA_Render3DInstance (a per-blade model matrix and tint); the shared
 * wind, sway and tint come from `look`, and each blade's `look.model`/`look.phase` are ignored.
 *
 * Each blade's sway phase is derived from its own world position — the same hash nya_render3d_foliage runs
 * when a caller leaves NYA_Render3DFoliage.phase unset — so a field of identical blades neither sways in
 * lockstep nor is billed a per-instance phase attribute. Feed `look.wind`/`look.time` from one NYA_WindField
 * so grass, leaves and water can all share it. Like foliage, grass is lit and receives shadows but casts
 * none, and disturbers fed this frame (nya_render3d_foliage_disturb) part the nearest blades of the patch.
 *
 * Blades past NYA_RENDER3D_MAX_GRASS_INSTANCES in a frame are counted and dropped, never drawn wrong.
 *
 * ```c
 * NYA_Render3DFoliage look = nya_render3d_foliage_style(NYA_FOLIAGE_GRASS);
 * look.wind = nya_wind_sample(&wind, patch_center);
 * look.time = wind.time;
 * nya_render3d_grass(window, "grass_tuft", blades, blade_count, look);
 * ```
 * */
NYA_API void nya_render3d_grass(NYA_Window* window, NYA_ConstCString blade_mesh, const NYA_Render3DInstance* instances, u32 count,
                                NYA_Render3DFoliage look);

/**
 * Adds a disturber for this frame: a sphere in world space that foliage bends away from, on top of the
 * wind. Feed one per dynamic body that might brush the plants — its position, how far its influence
 * reaches, and how hard it shoves — after nya_render3d_begin and before drawing foliage. Cleared every
 * frame at nya_render3d_begin, so a body that stops moving simply stops being fed. Each plant is bent by
 * the nearest NYA_RENDER3D_FOLIAGE_DISTURBERS of them.
 *
 * ```c
 * nya_render3d_begin(window, camera);
 * nya_render3d_foliage_disturb(window, nya_entity_render_position(creature), 1.2F, 1.0F);
 * // ... draw the foliage ...
 * ```
 * */
NYA_API void nya_render3d_foliage_disturb(NYA_Window* window, f32x3 position, f32 radius, f32 strength);

/**
 * Draws a registered mesh as a flowing water surface: model-space vertices lifted into travelling waves in
 * the vertex stage, then refracted, depth-blended and foamed in the fragment stage. Like foliage, one draw of
 * one registered mesh (nya_render3d_mesh_register) with its own per-object uniforms — the still surface at
 * y = 0 and the shore weight in each vertex colour's alpha (0 down the channel, 1 at the banks).
 *
 * Drawn in the camera pass, after the opaque scene, so it can refract what is behind it. The refraction only
 * shows when the scene is drawn into a render texture (through nya_post_begin, say), the same limit the glass
 * material has; drawn straight to the window the body falls back to its deep-to-shallow colour. Water is lit
 * and reflects the sun, but does not cast or receive a cast shadow.
 *
 * ```c
 * NYA_Render3DWater river = {
 *     .flow_direction = { 1, 0, 0.2F }, .flow_speed = 1.0F, .wave_amplitude = 0.15F, .wave_frequency = 0.6F,
 *     .deep_color = { 0.02F, 0.12F, 0.18F, 1 }, .shallow_color = { 0.10F, 0.35F, 0.38F, 1 }, .refraction = 0.5F,
 * };
 * river.wind = nya_wind_sample(&wind, surface_center);   // optional: share the one wind field
 * river.wind_influence = 0.5F;
 * nya_render3d_water(window, "river_surface", position, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, river);
 * ```
 * */
NYA_API void nya_render3d_water(NYA_Window* window, NYA_ConstCString handle, f32x3 position, f32x3 scale, NYA_Quaternion rotation,
                                NYA_Render3DWater water);

/** Releases a registered mesh's GPU buffer. Safe for a handle that was never registered. */
NYA_API void nya_render3d_mesh_release(NYA_Window* window, NYA_ConstCString handle);

/*
 * ─────────────────────────────────────────────────────────
 * PICKING
 * ─────────────────────────────────────────────────────────
 */

/**
 * The world-space ray a screen pixel points along, under the camera set by nya_render3d_begin.
 *
 * ```c
 * NYA_Render3DRay ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });
 * NYA_EntityHandle hit = nya_physics3d_raycast(ray.origin, ray.direction * 100.0F, nullptr, nullptr);
 * ```
 * */
NYA_API NYA_Render3DRay nya_render3d_screen_ray(NYA_Window* window, f32x2 screen) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM
 * ─────────────────────────────────────────────────────────
 */

/**
 * Ends the run of draws that share the current state, so the next draw can change it. Called by every setter that
 * changes shading; the scene draws at nya_render3d_end. A game does not call it.
 * */
NYA_API void nya_render3d_flush(NYA_Window* window);

typedef struct NYA_Render3DFrameStats NYA_Render3DFrameStats;

/**
 * What the 3D batch did this frame.
 * */
struct NYA_Render3DFrameStats {
    /** Draw calls issued. Includes the shadow passes, which are draws like any other. */
    u32 draw_calls;

    u32 vertices;
    u32 indices;

    /** Copies of retained meshes drawn. See NYA_Render3DInstance. */
    u32 instances;

    /** Primitives no pass could see, rejected before any vertex was written. */
    u32 culled;

    /**
     * Primitives that were on screen and hidden behind an occluder. Zero unless a buffer is set.
     * */
    u32 occluded;

    /** Primitives too large for an empty batch, or past the instance ceiling. Any of these is a bug. */
    u32 dropped_draws;

    /** Passes the scene was drawn in: each shadow cascade and the camera. */
    u32 passes;
};

/** What the 3D batch did this frame. Reset by nya_render_begin, so this is read before it. */
NYA_API NYA_Render3DFrameStats nya_render3d_frame_stats(NYA_Window* window) __attr_no_discard;
