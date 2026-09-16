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

// Same reason as NYA_Window: this file deliberately includes nothing that includes it back, and
// nya_render3d_occlusion only ever takes a pointer. render_occlusion.h has the definition.
typedef struct NYA_OcclusionBuffer NYA_OcclusionBuffer;

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

/** The depth-only pipeline the shadow pass draws with. See nya_render3d_shadow_begin. */
#define NYA_RENDER3D_PIPELINE_SHADOW "nya_mesh3d_shadow_pipeline"

/*
 * ── The retained mesh path ──
 *
 * Four more pipelines, differing from the three above only in their vertex stage: these read vertices in
 * model space plus a per-instance transform, where those read vertices already in world space.
 *
 * The fragment stages are shared with the immediate path, unchanged. That is not an accident of
 * implementation — it is what guarantees a model looks identical whichever path drew it, and it is why
 * the split can be an internal performance decision rather than something a caller has to think about.
 */

/** Instanced, untextured. */
#define NYA_RENDER3D_PIPELINE_INSTANCED "nya_mesh3d_instanced_pipeline"

/** Instanced, with a sampled base colour texture. */
#define NYA_RENDER3D_PIPELINE_INSTANCED_TEXTURED "nya_mesh3d_instanced_textured_pipeline"

/** Instanced, depth only, for the shadow pass. */
#define NYA_RENDER3D_PIPELINE_INSTANCED_SHADOW "nya_mesh3d_instanced_shadow_pipeline"

/** The fullscreen sky. See nya_render3d_sky_draw. */
#define NYA_RENDER3D_PIPELINE_SKY "nya_sky3d_pipeline"

/** The inverted-hull outline, for retained meshes. See nya_render3d_outline_set. */
#define NYA_RENDER3D_PIPELINE_OUTLINE "nya_mesh3d_outline_pipeline"

/*
 * ── The transparent pass ──
 *
 * The same shaders as the opaque pipelines with one state difference: depth is *tested* and not
 * *written*. A translucent surface is still behind the wall in front of it, so it has to test — and
 * writing would let the nearer of two translucent panes stop the further one being drawn at all.
 *
 * Anything drawn with an alpha below one is routed here automatically and sorted back to front within
 * the flush; there is nothing for a caller to switch on. See NYA_Render3DStream.
 */

/** Untextured, depth-tested, no depth write. */
/**
 * The gizmo pipeline: transparent, with depth *testing* off as well as depth writing.
 * */
/**
 * The skinned mesh pipeline. See nya_render3d_skinned_mesh.
 * */
#define NYA_RENDER3D_PIPELINE_SKINNED "nya_mesh3d_skinned_pipeline"

/** The depth-only skinned pipeline, so a skinned mesh casts a shadow. See nya_render3d_skinned_mesh. */
#define NYA_RENDER3D_PIPELINE_SKINNED_SHADOW "nya_mesh3d_skinned_shadow_pipeline"

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

/*
 * ── The additive pass ──
 *
 * Light adds, it does not occlude. Fire, sparks, magic and glow are *emission*, so overlapping them has
 * to brighten toward white rather than blending toward an average — which is what alpha does, and why a
 * stack of alpha-blended flame sprites reads as a grey smudge.
 *
 * Additive geometry needs no sorting at all, which is the other half of why it has its own pass:
 * addition is commutative, so the order it is drawn in cannot change the result.
 */

/** Untextured, additive, depth-tested, no depth write. */
#define NYA_RENDER3D_PIPELINE_ADDITIVE "nya_mesh3d_additive_pipeline"

/** Textured, additive, depth-tested, no depth write. The one a flame sprite goes through. */
#define NYA_RENDER3D_PIPELINE_ADDITIVE_TEXTURED "nya_mesh3d_additive_textured_pipeline"

/**
 * Resolution of the shadow map, per side.
 * */
/**
 * Cascades the shadow map is split into. Between one and four.
 * */
#ifndef NYA_RENDER3D_SHADOW_CASCADES
#define NYA_RENDER3D_SHADOW_CASCADES 3
#endif

static_assert(NYA_RENDER3D_SHADOW_CASCADES >= 1 && NYA_RENDER3D_SHADOW_CASCADES <= 4,
              "the shadow atlas is two by two, so it holds between one and four cascades");

/**
 * The default sun's direction: upper front left, the one that makes a cube read as a cube by lighting
 * three faces differently. Straight down lights one and leaves four identical.
 * */
#define NYA_RENDER3D_LIGHT_DIRECTION_DEFAULT (nya_vector_normalize((f32x3){ -0.4F, -1.0F, -0.6F }))

#ifndef NYA_RENDER3D_SHADOW_MAP_SIZE
#define NYA_RENDER3D_SHADOW_MAP_SIZE 1024
#endif

/** Half-width of the shadow volume when NYA_Render3DShadow.extent is zero, in world units. */
#ifndef NYA_RENDER3D_SHADOW_EXTENT
#define NYA_RENDER3D_SHADOW_EXTENT 12.0F
#endif

/** Depth slack when NYA_Render3DShadow.bias is zero. Tuned against a 1024 map over a 12 unit extent. */
#ifndef NYA_RENDER3D_SHADOW_BIAS
#define NYA_RENDER3D_SHADOW_BIAS 0.0015F
#endif

/**
 * Vertices the 3D batch can hold before a draw is forced.
 * */
#ifndef NYA_RENDER3D_MAX_VERTICES
#define NYA_RENDER3D_MAX_VERTICES 16384
#endif

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

/** Segments around a sphere's equator. Halved for the rings from pole to pole. */
/**
 * The handle the shared unit sphere is registered under, by the first nya_render3d_sphere of the run.
 *
 * Reserved, and nya_render3d_mesh_register refuses it rather than trusting a caller to have read this.
 * */
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
typedef struct NYA_Render3DShadow     NYA_Render3DShadow;
typedef struct NYA_Render3DMaterial NYA_Render3DMaterial;
typedef struct NYA_Render3DFog      NYA_Render3DFog;
typedef struct NYA_Render3DSky      NYA_Render3DSky;
typedef enum NYA_Render3DBlend      NYA_Render3DBlend;
typedef enum NYA_Render3DDepth      NYA_Render3DDepth;
typedef struct NYA_Render3DTextureBinding NYA_Render3DTextureBinding;

/*
 * Forward declared rather than included. NYA_Vertex3D is renderer.h's, and renderer.h includes this file —
 * so reaching for it would close a cycle. Only the pointer is needed here; the caller building the
 * vertices already has the full definition.
 */
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

    /**
     * Neither tested nor written: always visible, and invisible to everything drawn after it.
     * */
    NYA_RENDER3D_DEPTH_OVERLAY,
};

enum NYA_Render3DBlend {
    /** Blend over what is behind, by alpha. Sorted back to front. The default, and the zero value. */
    NYA_RENDER3D_BLEND_ALPHA = 0,

    /**
     * Add to what is behind. For anything that is *emitting* rather than occluding.
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

    /** Below level — haze, sea, distant ground. Zero becomes a dim slate. */
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
};

/**
 * Distance and height fog: the depth cue a flat-shaded renderer has no other source of.
 *
 * ```c
 * nya_render3d_fog_set(window, (NYA_Render3DFog){ .color = sky.horizon, .density = 0.02F });
 * ```
 *
 * ⚠ The inverted-hull outline is not fogged: its fragment shader reads no uniforms at all, so ink stays
 * at full contrast while the surface recedes. Visible at heavy density.
 * */
struct NYA_Render3DFog {
    /**
     * What distance fades toward. Zero becomes NYA_RENDER3D_FOG_COLOR.
     * */
    NYA_Color color;

    /**
     * How quickly fog closes in, per world unit. **Zero disables fog**, shader branch included.
     * */
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
};

/**
 * How a surface responds to light: the metallic-roughness half of a glTF material.
 * */
/*
 * The names are historical, and each field says what it now controls.
 */
struct NYA_Render3DMaterial {
    /**
     * How strong the single hard-edged highlight is, in [0, 1]. Zero for none.
     * */
    f32 metallic;

    /**
     * How soft the transitions between shading bands are, in [0, 1].
     * */
    f32 roughness;

    /**
     * How strong the rim light on the silhouette is, in [0, 1]. Zero for none.
     * */
    f32 reflectance;

    /**
     * How far this surface bends what is behind it, in [0, 1]. Zero for none.
     *
     * ⚠ **Needs the scene drawn into a render texture.** The capture is taken by resolving the current
     * colour target mid-frame, which only happens for a render texture; drawn straight to the window,
     * refraction is ignored and the surface falls back to ordinary blending.
     * */
    f32 refraction;

    /**
     * How much the view through this surface is blurred, in [0, 1]. Zero is clear glass.
     *
     * ⚠ A fixed tap count with a widening radius, not a mip chain — so a very heavy blur shows its
     * individual taps as a faint grid rather than getting smoother. Same trade GNY_BLOOM_2D_SPREAD
     * documents, and the same fix: a downsampled chain.
     * */
    f32 blur;

    /**
     * How much of the surface's own colour is added regardless of any light. Zero for none.
     * */
    f32 emission;

    /**
     * How strongly curved edges are darkened, in [0, 1]. Zero for none.
     *
     * ⚠ **Curvature, not creases.** It comes from how fast the interpolated normal turns per pixel — large
     * on a tight fillet, exactly zero across a flat face — so a rounded cube shows it strongly and a
     * hard-edged cube shows none at all. A hard crease needs neighbour information this pass does not
     * have; see mesh3d_edge in mesh3d_shading.hlsli.
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
 * The parts of a shadow volume that are not derived from the camera. See nya_render3d_shadow_for_camera.
 * */
struct NYA_Render3DShadowFit {
    /**
     * How far from the camera shadows are cast, in world units. Zero is NYA_RENDER3D_SHADOW_EXTENT.
     *
     * ⚠ **This is a distance down the view, not a cascade's size.** It used to be the near cascade's
     * half-width, and that is what made shadows depend on how far the camera was from what it was
     * looking at: each cascade was a box of a fixed size sitting a fixed distance in front of the
     * camera, so a camera further away than the near cascade's reach spent that cascade on empty air
     * and shadowed the whole scene with the coarsest map it had. Moving the camera then moved patches
     * of ground between cascades of very different resolution, which is what "the shadows change when
     * I move" was.
     * */
    f32 range;

    /**
     * Where shadow casting *starts*, as a distance down the view. Zero is the camera's near plane.
     *
     * Named `near_distance` rather than `near`: `near` and `far` are legacy macros in the Windows headers
     * that MinGW still defines, so a field called `near` compiles on Linux and fails to parse on the
     * cross build.
     *
     * ⚠ **Set this whenever the camera is further from its subject than the subject is wide.** The
     * cascades split the span from here to `range`, so leaving it at the near plane spends the sharp
     * near cascades on the empty air between an orbit camera and what it is looking at, and drops the
     * whole scene into the coarsest map — which is the blurry, misplaced-looking shadow that fitting
     * the cascades to the frustum did not by itself fix.
     *
     * Measured, not guessed: the distance from the camera to the nearest caster. For a scene orbited
     * from outside, that is the distance to its centre minus its radius.
     * */
    f32 near_distance;

    /**
     * The camera's aspect ratio, width over height. Zero is 16:9.
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

    /**
     * Which cascade this pass is filling, from zero for the nearest.
     *
     * ```c
     * for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
     *     nya_render3d_shadow_begin(window, (NYA_Render3DShadow){
     *         .center = focus, .extent = 8.0F, .strength = 0.45F, .cascade = cascade,
     *     });
     *
     *     draw_scene(window);
     *
     *     nya_render3d_shadow_end(window);
     * }
     * ```
     * */
    u32 cascade;
};

/** A ray in world space. What nya_render3d_screen_ray produces and a physics raycast consumes. */
struct NYA_Render3DRay {
    f32x3 origin;

    /** Unit length. Multiply it by how far the query should reach. */
    f32x3 direction;
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
 * Draws an ink outline around every retained mesh, as an inverted hull.
 * */
NYA_API void nya_render3d_outline_set(NYA_Window* window, f32 thickness, NYA_Color color);

/**
 * Culls this pass against an occlusion buffer as well as the frustum. Null turns it back off.
 * */
NYA_API void nya_render3d_occlusion(NYA_Window* window, const NYA_OcclusionBuffer* buffer);

/**
 * The camera matrix this pass is drawing with, for handing to nya_occlusion_begin.
 * */
NYA_API f32_4x4 nya_render3d_view_projection(NYA_Window* window) __attr_no_discard;

/**
 * Switches later translucent geometry between alpha blending and adding.
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
 * Starts the shadow pass. Everything drawn until nya_render3d_shadow_end goes into the shadow map.
 *
 * ```c
 * nya_render3d_shadow_begin(window, (NYA_Render3DShadow){ .center = ..., .extent = 12.0F, .strength = 0.45F });
 * draw_the_scene(window);
 * nya_render3d_shadow_end(window);
 *
 * nya_render3d_begin(window, camera);
 * draw_the_scene(window);   // the same calls; the shadow map is sampled automatically
 * nya_render3d_end(window);
 * ```
 * */
NYA_API void nya_render3d_shadow_begin(NYA_Window* window, NYA_Render3DShadow shadow);

/**
 * A cascade's shadow volume, fitted to a camera and snapped to the shadow map's texel grid.
 *
 * ```c
 * for (u32 cascade = 0; cascade < NYA_RENDER3D_SHADOW_CASCADES; cascade++) {
 *     nya_render3d_shadow_begin(window, nya_render3d_shadow_for_camera(camera, sun, cascade,
 *                                                                     (NYA_Render3DShadowFit){ .strength = 0.45F }));
 *     draw_scene(window);
 *     nya_render3d_shadow_end(window);
 * }
 * ```
 * */
/**
 * The light's own axes: where it points, and an up that is not parallel to it.
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

NYA_API NYA_Render3DShadow nya_render3d_shadow_for_camera(
    NYA_Camera3DPerspective camera,
    f32x3                   light_direction,
    u32                     cascade,
    NYA_Render3DShadowFit   fit
) __attr_no_discard;

/** Ends the shadow pass and restores the previous render target. */
NYA_API void nya_render3d_shadow_end(NYA_Window* window);

/**
 * Whether a shadow pass has *already run* this frame, and so whether anything is being shadowed.
 *
 * ⚠ **Not "are we inside a shadow pass".** It goes true at nya_render3d_shadow_end and stays true for
 * the rest of the frame, which is the opposite of what a caller wanting to skip the shadow pass needs.
 * That caller wants nya_render3d_shadow_pass_active, and reaching for this one instead is a mistake
 * that has been made: see the note there.
 * */
NYA_API b8 nya_render3d_shadow_active(NYA_Window* window) __attr_no_discard;

/**
 * Whether a shadow pass is running *right now* — between nya_render3d_shadow_begin and its end.
 * */
NYA_API b8 nya_render3d_shadow_pass_active(NYA_Window* window) __attr_no_discard;

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
 * All of them take world-space positions and a rotation, and bake both into the vertices as they are
 * built. That is what keeps a hundred of them in one draw call; the cost is that moving one means
 * rebuilding it, which at these vertex counts is cheaper than the draw call it saves.
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
 * Draws a model loaded as NYA_ASSET_TYPE_MESH, transformed and pushed into the batch like any other
 * shape.
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

/** Draws what is queued. Called by nya_render3d_end and at frame end; a game does not call it. */
NYA_API void nya_render3d_flush(NYA_Window* window);

typedef struct NYA_Render3DFrameStats NYA_Render3DFrameStats;

/**
 * What the 3D batch did this frame.
 * */
struct NYA_Render3DFrameStats {
    /** Draw calls issued. Includes the outline and shadow passes, which are draws like any other. */
    u32 draw_calls;

    u32 vertices;
    u32 indices;

    /** Copies of retained meshes drawn. See NYA_Render3DInstance. */
    u32 instances;

    /** Primitives rejected by the frustum test before any vertex was written. */
    u32 culled;

    /**
     * Primitives that were on screen and hidden behind an occluder. Zero unless a buffer is set.
     * */
    u32 occluded;

    /** Primitives too large for an empty batch, or past the instance ceiling. Any of these is a bug. */
    u32 dropped_draws;
};

/** What the 3D batch did this frame. Reset by nya_render_begin, so this is read before it. */
NYA_API NYA_Render3DFrameStats nya_render3d_frame_stats(NYA_Window* window) __attr_no_discard;
