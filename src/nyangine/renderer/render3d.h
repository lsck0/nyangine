/**
 * @file render3d.h
 *
 * Drawing solid geometry, through a 3D camera, into the same render pass render2d draws into.
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
 *
 * **2D and 3D share one render pass**; there is no mode to switch. The depth buffer is attached
 * unconditionally, the 3D pipeline tests and writes it and the 2D pipelines do neither, so 3D occludes
 * itself while 2D lands on top in submission order — what a HUD over a scene needs. The one rule is
 * ordering, and since a batch is drawn only when flushed, "drawn later" means "flushed later":
 * nya_render3d_begin flushes queued 2D so a background stays behind, and nya_render3d_end flushes the
 * 3D batch so later 2D lands in front. Interleaving further works, at a draw call per switch.
 *
 * **Shading** is flat-shaded cartoon: a wrapped diffuse quantised into three bands, one hard highlight
 * and a rim. Base colour is per vertex so one batch holds many differently coloured objects; the rest is
 * NYA_Render3DMaterial, set per flush — a constraint matching how a renderer wants to sort anyway.
 *
 * **Not physically based, despite the parameter names.** It was — Cook-Torrance metallic-roughness with
 * a Reinhard tonemap — and it made every flat colour dark: an 0.85 white lands at 0.21 once the diffuse
 * is divided by pi and tonemapped, and a face turned from the light kept only ambient and read as a
 * hole. Physical plausibility is given up so a caller's colour is very nearly what appears. Reasoning at
 * the top of mesh3d.frag.hlsl; per-field effects on NYA_Render3DMaterial.
 *
 * Deliberately absent, each a real addition rather than a tweak: texture maps, shadow maps, an outline
 * pass over the depth and normal buffers, and more than one light.
 *
 * **There is no mesh asset and no model format.** The batch takes triangles with a position, a colour
 * and a normal; nya_render3d_cube and friends generate those. A loader belongs on top of this — putting
 * it inside is how a renderer ends up owning a scene graph.
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
 *
 * Four, as a budget rather than a limit of the technique: every light is a term in the fragment loop and
 * the whole set is pushed per draw call, so raising it costs fragment time on every pixel whether or not
 * a light is near it. A scene wanting dozens wants a tiled or clustered renderer, not a bigger array.
 *
 * Must match MESH3D_MAX_POINT_LIGHTS in mesh3d_shading.hlsli and the arrays in NYA_ShaderMesh3DUniform.
 * */
#define NYA_RENDER3D_MAX_POINT_LIGHTS 4

/**
 * The reach a point light gets when it names none, in world units.
 *
 * Ten, which is a room. A light with no range at all would light nothing and read as broken, and the
 * physical answer — infinite reach with inverse-square falloff — is exactly what the windowed falloff
 * exists to avoid.
 * */
#ifndef NYA_RENDER3D_POINT_LIGHT_RANGE
#define NYA_RENDER3D_POINT_LIGHT_RANGE 10.0F
#endif

#define NYA_RENDER3D_PIPELINE_MESH "nya_mesh3d_pipeline"

/**
 * The same pipeline with a sampled base colour texture. Used by nya_render3d_mesh for a textured model.
 *
 * Two pipelines rather than one with a branch, because a shader that declares a sampler must have one
 * bound on every draw. See the note at the top of mesh3d_textured.frag.hlsl.
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
 *
 * A gizmo is not part of the scene. A translate handle inside the object it moves must still be
 * grabbable, and a selection outline that vanishes where the model is nearer reads as broken rather than
 * as depth working. Blending stays on, so "this handle is behind the object" is shown by drawing it
 * twice at two alphas rather than by letting depth hide it.
 * */
/**
 * The skinned mesh pipeline. See nya_render3d_skinned_mesh.
 *
 * Its own pipeline rather than a variant of the mesh one because the vertex *layout* differs — a
 * skinned vertex carries bone indices and weights, and a layout is baked into a pipeline at creation.
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
 *
 * Selected automatically when the current material has a non-zero NYA_Render3DMaterial.refraction and the
 * scene is being drawn into a render texture. Opaque, because it *replaces* the pixel with its own view of
 * what is behind rather than blending toward it — blending a sample of the destination back over the
 * destination would count it twice.
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
 *
 * A thousand and twenty-four over a volume a few dozen units across gives a texel a couple of
 * centimetres wide, which is what a soft contact shadow needs — the penumbra is a few texels, so a finer
 * map makes the shadow *harder*, not better. Raising it is how you cover a larger volume at the same
 * sharpness, not how you improve a small one.
 * */
/**
 * Cascades the shadow map is split into. Between one and four.
 *
 * One map over the whole view must choose between covering the scene and being sharp: a volume wide
 * enough for the distance spreads its texels so thinly the shadow at your feet is a blur. Cascades cover
 * near and far with separate maps at the same resolution over different extents.
 *
 * Four because they pack into one texture as a two-by-two atlas, which keeps this to one sampler binding
 * and no texture-array support to depend on. One cascade is the single-map behaviour exactly.
 *
 * Each cascade is a *separate pass* over the scene, so this multiplies how often a frame's geometry is
 * emitted. Three is the usual balance; four is for a very deep view.
 * */
#ifndef NYA_RENDER3D_SHADOW_CASCADES
#define NYA_RENDER3D_SHADOW_CASCADES 3
#endif

static_assert(NYA_RENDER3D_SHADOW_CASCADES >= 1 && NYA_RENDER3D_SHADOW_CASCADES <= 4,
              "the shadow atlas is two by two, so it holds between one and four cascades");

/**
 * How much wider each cascade is than the one before it.
 *
 * Geometric rather than a linear split, because perspective is: a texel's world size grows with distance,
 * so equal *distance* slices would give the far cascade far less relative detail than the near one. Two is
 * the standard ratio and makes three cascades cover four times the near extent.
 * */
#ifndef NYA_RENDER3D_SHADOW_CASCADE_RATIO
#define NYA_RENDER3D_SHADOW_CASCADE_RATIO 2.5F
#endif

/**
 * The default sun's direction: upper front left, the one that makes a cube read as a cube by lighting
 * three faces differently. Straight down lights one and leaves four identical.
 *
 * Named rather than written out inside the default light, because the shadow fit needs the same
 * answer when a caller passes no direction and the two must not drift.
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
 *
 * Smaller than the 2D batch's, because a 3D vertex is forty-eight bytes against twenty and this is
 * uploaded in full every frame. Sixteen thousand is around two thousand cubes in one draw call,
 * which is well past where a game should be reaching for instancing instead.
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
 *
 * Eighty bytes each, so a thousand is eighty kilobytes per pass — against the two hundred and fifty
 * thousand vertices those copies would have cost the immediate batch, which could not have held them.
 *
 * A frame asking for more logs and drops the extra rather than growing, for the reason the vertex batch
 * has a ceiling: a renderer that quietly allocates under load hides the moment it stopped being affordable.
 * */
#ifndef NYA_RENDER3D_MAX_INSTANCES
#define NYA_RENDER3D_MAX_INSTANCES 1024
#endif

/**
 * Distinct meshes one frame can draw instanced.
 *
 * Not the number of copies — the number of different *models*. A forest of one tree is one of these and a
 * thousand instances; a scene of sixty-four unique props is sixty-four of these.
 * */
#ifndef NYA_RENDER3D_MAX_MESH_GROUPS
#define NYA_RENDER3D_MAX_MESH_GROUPS 64
#endif

/**
 * Meshes a caller can register at once. See nya_render3d_mesh_register.
 *
 * These are generated surfaces — a terrain, a cave, a marching-cubes chunk — not a model library.
 *
 * It used to be 32, on the reasoning that a scene wanting hundreds of them wants a chunked streaming
 * system rather than a bigger table. That system now exists: chunked terrain registers one mesh per
 * chunk (see core_terrain3d.h), and an eight-by-eight chunking is already 64. The table is a pointer
 * and a handle per entry, so this costs kilobytes.
 * */
#ifndef NYA_RENDER3D_MAX_REGISTERED_MESHES
#define NYA_RENDER3D_MAX_REGISTERED_MESHES 256
#endif

/** Segments around a sphere's equator. Halved for the rings from pole to pole. */
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
 *
 * Deliberately opaque-ish: the members are the backend's, and the only thing a caller does with one is
 * pass it back. It exists so a loop drawing hundreds of billboards resolves the handle once instead of
 * once per billboard.
 * */
struct NYA_Render3DTextureBinding {
    void* texture;
    void* sampler;
};

/**
 * How the transparent pass combines with what is already there.
 *
 * Its own enum rather than core_asset.h's NYA_BlendMode, which describes the same two things. Reaching for
 * that one would mean this header including the asset system's, and the asset system already includes this
 * one — a cycle for a two-value enum. It is also the more honest type: this names what a *scene* does, and
 * the pipeline's blend state is an implementation of it.
 * */
/**
 * Whether geometry takes part in depth at all.
 *
 * The two are not degrees of the same thing. Default geometry *is* the scene and is occluded by it;
 * an overlay is drawn on top of the scene and is not part of it — a gizmo, a selection outline, a
 * debug axis. Nothing between the two is useful, which is why this is a pair and not a set of flags.
 * */
enum NYA_Render3DDepth {
    /** Tested and written. What every piece of world geometry wants. */
    NYA_RENDER3D_DEPTH_DEFAULT,

    /**
     * Neither tested nor written: always visible, and invisible to everything drawn after it.
     *
     * Draw order alone decides what covers what, so a gizmo drawn later sits over one drawn earlier
     * and nothing in the scene can hide any of them.
     * */
    NYA_RENDER3D_DEPTH_OVERLAY,
};

enum NYA_Render3DBlend {
    /** Blend over what is behind, by alpha. Sorted back to front. The default, and the zero value. */
    NYA_RENDER3D_BLEND_ALPHA = 0,

    /**
     * Add to what is behind. For anything that is *emitting* rather than occluding.
     *
     * Fire, sparks, glow and magic. Overlapping additive geometry brightens toward white, which is what
     * overlapping light does — where alpha would average them into a grey smudge. Needs no sorting, since
     * addition does not depend on order.
     * */
    NYA_RENDER3D_BLEND_ADDITIVE,

    NYA_RENDER3D_BLEND_COUNT,
};

/**
 * A procedural sky: a vertical gradient, a ground half, and a sun.
 *
 * Unlike a 2D backdrop, which is screen space and stays put when the camera turns, this is shaded from a
 * *view ray* per pixel and so rotates with the camera as the world does. Orbiting a hill and finding the
 * sun still in the corner of the screen is what a backdrop looks like in a genuinely 3D scene.
 *
 * No cube map and no texture — a fit to the style, since the reference is bands of flat colour, and it
 * buys what a cube map cannot: the whole sky is six colours and a direction, so a day cycle is a function
 * of one number rather than six images per time of day. What it cannot do is clouds with shape.
 *
 * Every field has a usable default at zero, so `(NYA_Render3DSky){ .sun_direction = ... }` is a sky.
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
     *
     * The same convention NYA_Render3DLight.direction is the negation of, and the reason to state it: a
     * caller almost always has the light's direction to hand, and passing it unnegated puts the sun
     * exactly opposite where the scene is lit from. Zero is read as unspecified and puts it overhead.
     * */
    f32x3 sun_direction;

    NYA_Color sun_color;

    /**
     * Angular radius of the disc, in radians. Zero becomes about half a degree, which is life-size.
     *
     * Life-size is small — the real sun is half the width of a fingernail at arm's length — so a stylised
     * sky usually wants several times this.
     * */
    f32 sun_angle;

    /** How bright the disc and its halo are. Zero becomes one. */
    f32 sun_intensity;

    /**
     * How tightly the halo hugs the disc. Zero becomes a moderate spread.
     *
     * An exponent on the alignment, so small numbers glow across most of the sky and large ones make a
     * ring. This is the knob that separates a hazy afternoon from a hard winter sun.
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
 *
 * A property of the frame rather than of a draw, because it is a fragment uniform and a uniform is
 * per draw call — a light per cube would be a draw call per cube. Set it once after
 * nya_render3d_begin; changing it mid-frame flushes and costs one draw call, which is the honest
 * price and occasionally worth paying.
 * */
struct NYA_Render3DLight {
    /**
     * Which way the light travels, in world space. Normalized on the way in.
     *
     * The direction light *goes*, not where it comes from — `{ 0, -1, 0 }` is midday sun. The shader
     * wants the opposite of this and negates it there, once, rather than making every caller do it.
     * Zero is read as straight down.
     * */
    f32x3 direction;

    NYA_Color color;

    /**
     * How lit a surface facing away from the light still is, in [0, 1].
     *
     * Zero renders unlit faces pure black, which reads as a hole rather than a shadow — nothing outdoors
     * is that dark, because the sky is also a light. Deliberately higher than a physical renderer would
     * want: this is the darkest an object reaches and a flat colour must still read as itself there.
     * Around 0.6 suits the cartoon model; 0.25 is a dramatically lit room and leaves half of it muddy.
     * */
    f32 ambient;

    /** Scales the lit term only. One is neutral; zero leaves everything at the ambient. */
    f32 intensity;
};

/**
 * How a surface responds to light: the metallic-roughness half of a glTF material.
 *
 * Base colour is not here — it is per vertex, which lets one material cover a hundred differently
 * coloured cubes in one draw call. Every batching renderer ends up making this split, because colour
 * varies per object and response varies per *kind* of object.
 *
 * Set per flush with nya_render3d_material_set. Changing it costs a draw call, which is why a scene is
 * drawn material by material rather than object by object.
 * */
/*
 * The names are historical, and each field says what it now controls.
 *
 * These were the glTF metallic-roughness parameters. They were kept rather than renamed because
 * renaming them would touch the struct, the uniform packing and every call site, for a shading model
 * that may change again; what matters is that the meaning is written down where a caller reads it.
 */
struct NYA_Render3DMaterial {
    /**
     * How strong the single hard-edged highlight is, in [0, 1]. Zero for none.
     *
     * **Not metalness.** Nothing here reflects its surroundings, and one is a bright distinct spot
     * rather than a mirror. It is the cartoon idiom of a highlight as a *shape* rather than a falloff,
     * so it appears and disappears rather than fading.
     * */
    f32 metallic;

    /**
     * How soft the transitions between shading bands are, in [0, 1].
     *
     * Low is a crisp cel terminator, high is close to a smooth gradient. It happens to read like
     * roughness — a rougher surface does have softer terminators — but the value is a smoothstep width
     * rather than a GGX alpha.
     *
     * Never quite zero; it is clamped slightly above, because a band edge with no width aliases into a
     * jagged line that crawls as the object turns.
     * */
    f32 roughness;

    /**
     * How strong the rim light on the silhouette is, in [0, 1]. Zero for none.
     *
     * What separates a shape from the background without an outline pass. Zero is read as 0.5, a faint
     * edge: enough to keep a dark object off a dark background, not enough to look like a glow.
     * */
    f32 reflectance;

    /**
     * How far this surface bends what is behind it, in [0, 1]. Zero for none.
     *
     * Non-zero turns a translucent surface into *glass*: it samples the opaque scene at an offset along
     * the surface normal and tints that, so a facetted shape distorts the view flat where a face points
     * at the camera and strongest where one turns away.
     *
     * A screen-space offset rather than a traced ray, so it does not obey Snell's law, cannot show what
     * is hidden behind the object's own silhouette, and refracts only geometry drawn *before* it — the
     * opaque pass. Glass behind glass shows the further pane undistorted.
     *
     * ⚠ **Needs the scene drawn into a render texture.** The capture is taken by resolving the current
     * colour target mid-frame, which only happens for a render texture; drawn straight to the window,
     * refraction is ignored and the surface falls back to ordinary blending.
     * */
    f32 refraction;

    /**
     * How much the view through this surface is blurred, in [0, 1]. Zero is clear glass.
     *
     * Frosted glass, as a per-material property rather than a separate shader: it widens the kernel the
     * refraction lookup samples with.
     *
     * ⚠ A fixed tap count with a widening radius, not a mip chain — so a very heavy blur shows its
     * individual taps as a faint grid rather than getting smoother. Same trade GNY_BLOOM_2D_SPREAD
     * documents, and the same fix: a downsampled chain.
     *
     * Does nothing unless `refraction` is also non-zero — there is no capture to blur otherwise.
     * */
    f32 blur;

    /**
     * How much of the surface's own colour is added regardless of any light. Zero for none.
     *
     * **Unlit, which is the point**: an emissive surface must not darken on the side facing away from the
     * sun, and folding it into the shaded term would make a glowing panel look like a painted one.
     *
     * It lights nothing else — that needs a NYA_Render3DPointLight at the same place. Nothing derives one
     * here, because a renderer guessing light positions from emissive geometry would be guessing.
     *
     * Values above one still mean something despite the clamp: they lift the *whole* surface past a bloom
     * threshold rather than only its lit half. See GNY_BLOOM_3D_THRESHOLD.
     * */
    f32 emission;

    /**
     * How strongly curved edges are darkened, in [0, 1]. Zero for none.
     *
     * Reads as an ink line along a rounded edge, and gives a flat-coloured model definition where shading
     * alone leaves it looking like a decal.
     *
     * ⚠ **Curvature, not creases.** It comes from how fast the interpolated normal turns per pixel — large
     * on a tight fillet, exactly zero across a flat face — so a rounded cube shows it strongly and a
     * hard-edged cube shows none at all. A hard crease needs neighbour information this pass does not
     * have; see mesh3d_edge in mesh3d_shading.hlsli.
     *
     * No distance term: the derivative is per pixel, so a model at the back gets the same treatment as one
     * at the front.
     * */
    f32 edge;
};

/**
 * A light at a point in the world, falling off with distance. What a lamp, a torch or a muzzle flash is.
 *
 * Additive on top of the directional light rather than replacing it: a surface lit by the sun and a lamp
 * is brighter than one lit by either, and a scene with no directional light simply sets its intensity to
 * zero. Each is banded like the directional light is, so a lamp casts the same style of terminator as
 * the sun and the look stays coherent.
 *
 * Frame state like NYA_Render3DLight, not per draw: the whole set goes into the fragment uniform, so
 * changing it costs a draw call. Set them once after nya_render3d_begin.
 * */
struct NYA_Render3DPointLight {
    f32x3 position;

    NYA_Color color;

    /**
     * How far the light reaches, in world units. Attenuation is exactly zero at this distance.
     *
     * Windowed rather than the physical inverse square, which never reaches zero and so leaves every
     * light faintly touching every surface — wasted work in a forward renderer that pushes all lights at
     * every fragment, and a washed out scene. A hard boundary is also the thing a level designer can
     * reason about: this lamp lights this room.
     *
     * Zero is read as unspecified and becomes NYA_RENDER3D_POINT_LIGHT_RANGE.
     * */
    f32 range;

    /** Scales the light. One is neutral; zero is off, which is cheaper expressed by not adding it. */
    f32 intensity;
};

/**
 * The parts of a shadow volume that are not derived from the camera. See nya_render3d_shadow_for_camera.
 *
 * Every field's zero is its default, so `(NYA_Render3DShadowFit){ .strength = 0.45F }` is the whole of
 * a normal call — `strength` is the one that has no useful default, because zero means "no shadows"
 * and that is a real thing to ask for.
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
     *
     * Each cascade's size is now *derived* — it is whatever covers its slice of the camera's frustum —
     * so the near cascade lands on whatever is nearest the viewer at any distance. See
     * nya_render3d_shadow_for_camera.
     * */
    f32 range;

    /**
     * The camera's aspect ratio, width over height. Zero is 16:9.
     *
     * Needed because the fit measures the frustum itself, and a frustum is as wide as the target it
     * is drawn to. Being a little wrong here only means a cascade slightly larger or smaller than the
     * ideal, never a wrong one, which is why it has a default at all.
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
     *
     * For looking at what snapping is worth, and for a camera that does not move — where the snap is
     * a no-op anyway. Not something to ship with: see the note on nya_render3d_shadow_for_camera.
     * */
    b8 no_texel_snap;
};

/**
 * What the shadow pass covers, and how the result looks.
 *
 * Only the directional light casts. A point light would need its own map — six faces of one, in the
 * general case — and four lights would be four more passes over the whole scene; that is a different
 * renderer rather than another field here.
 * */
struct NYA_Render3DShadow {
    /**
     * What the shadow volume is centred on, in world space.
     *
     * A directional light has no position, so the volume has to be placed by hand. Following the camera
     * is the usual choice and is the caller's to make: the engine cannot know whether the scene is a room
     * that never moves or a world that scrolls.
     * */
    f32x3 center;

    /**
     * Half the width of the volume, in world units. Everything outside it is lit.
     *
     * The single most important number for quality: it is the *whole* budget the map's resolution is
     * spread over, so doubling it halves the effective sharpness. Cover the scene and no more.
     *
     * Zero is read as NYA_RENDER3D_SHADOW_EXTENT.
     * */
    f32 extent;

    /**
     * How far along the light the volume reaches, in world units.
     *
     * Has to contain everything that should cast onto what is visible, including geometry behind the
     * camera. Too small and tall objects are clipped out of the map and stop casting; too large costs
     * depth precision. Zero is read as four times `extent`.
     * */
    f32 depth;

    /**
     * How dark a fully shadowed surface goes, in [0, 1]. Zero disables shadows outright.
     *
     * Not one, normally. It is how far toward the ambient a shadow reaches, and a cartoon shadow that
     * lands on black reads as a hole in the floor — the same reason NYA_Render3DLight.ambient is high.
     * Around 0.45 is a shadow you can see without it becoming the subject.
     *
     * Zero is a real value here and disables the lookup, so there is no "unspecified" default: a caller
     * that wants shadows says how strong.
     * */
    f32 strength;

    /**
     * Depth slack against shadow acne, in light-space depth units. Zero is read as
     * NYA_RENDER3D_SHADOW_BIAS.
     *
     * A surface compared against its own recorded depth fails on about half its pixels, because the map
     * was rasterised from elsewhere at different sample positions — the result is a moiré of dark
     * speckles across every lit face. This is the slack that fixes it, scaled in the shader by how
     * obliquely the surface faces the light. Too much detaches an object from its own shadow, which is
     * the trade rather than a bug.
     * */
    f32 bias;

    /**
     * Which cascade this pass is filling, from zero for the nearest.
     *
     * The caller runs the loop, because only the caller can draw the scene again — this renderer keeps no
     * geometry between flushes, which is the same reason the shadow pass has always been the game drawing
     * its scene twice rather than the engine replaying it.
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
     *
     * `extent` is the *nearest* cascade's extent; each one after it is
     * NYA_RENDER3D_SHADOW_CASCADE_RATIO times wider. A caller wanting one map passes cascade zero and
     * never loops, which is what the single-cascade case has always been.
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
 *
 * Flushes whatever render2d has queued first, so anything drawn in 2D before this stays behind the
 * scene. The aspect ratio comes from the current target, so a window resize needs no camera change.
 *
 * Zero fields are read as sensible defaults — see NYA_Camera3DPerspective — so the shortest useful
 * camera is a position and a target.
 * */
NYA_API void nya_render3d_begin(NYA_Window* window, NYA_Camera3DPerspective camera);

/** The same, through an orthographic camera. See NYA_Camera3DOrthographic. */
NYA_API void nya_render3d_begin_orthographic(NYA_Window* window, NYA_Camera3DOrthographic camera);

/**
 * Draws everything queued and stops drawing in 3D.
 *
 * Anything drawn through render2d afterwards lands in front of the scene, because the 2D pipelines
 * do not test depth and this flush has already happened. Calling it twice is harmless.
 * */
NYA_API void nya_render3d_end(NYA_Window* window);

/** Whether nya_render3d_begin has been called and not yet ended. */
NYA_API b8 nya_render3d_active(NYA_Window* window) __attr_no_discard;

/**
 * Replaces the directional light. Flushes, because the light is a per draw call uniform.
 *
 * Valid only between begin and end. The default — a light from the upper front left, quarter
 * ambient, neutral white — is restored by every nya_render3d_begin, so a scene that never calls this
 * still looks like a lit scene.
 * */
/**
 * Draws the sky behind everything else in the current 3D scene.
 *
 * Call it immediately after nya_render3d_begin and before any geometry. It writes no depth and tests
 * none, so it is a background in the plainest sense: whatever is drawn afterwards simply covers it.
 * Calling it *after* geometry paints over the scene, which is a plain ordering mistake rather than
 * something the renderer can detect.
 *
 * ```c
 * nya_render3d_begin(window, camera);
 * nya_render3d_sky_draw(window, (NYA_Render3DSky){ .sun_direction = -sun.direction, .sun_angle = 0.04F });
 * // ... the scene ...
 * nya_render3d_end(window);
 * ```
 *
 * One fullscreen triangle and no vertex buffer, through the same procedural path a post-process effect
 * uses. Costs one draw call and no geometry at all.
 *
 * Does nothing when no 3D camera is active — the sky is shaded from the camera basis, and there is no
 * sensible sky to draw without one.
 * */
NYA_API void nya_render3d_sky_draw(NYA_Window* window, NYA_Render3DSky sky);

/**
 * Draws an ink outline around every retained mesh, as an inverted hull.
 *
 * The thing mesh3d_shading.hlsli's edge term cannot do, and says so: `fwidth` finds *curvature*, because
 * screen-space derivatives are taken across a 2x2 quad inside one triangle. Two flat faces meeting at a
 * hard angle each report a constant normal, so a cube's edges produce nothing at all. What comes out of
 * that term is fillets and grazing angles — useful, and not an outline.
 *
 * ## How this one works
 *
 * The mesh is drawn a second time, expanded along its own normals, with the *front* faces culled. Only
 * the back of the expanded shell survives, and it survives exactly where it pokes out past the silhouette
 * of the real model — which is a band of constant width around it. Drawing it before the model means the
 * model covers the rest.
 *
 * Its virtues are that it needs no depth or normal buffer, no second render target, and no screen-space
 * pass, and that it costs one extra instanced draw per mesh rather than one per frame per pixel.
 *
 * Its limits are worth knowing before reaching for it. It outlines the **silhouette**, not creases
 * *inside* the silhouette — a cube's far edges get no line. A model with hard normals splits at its own
 * edges, so the shell splits there too and the outline shows gaps at sharp corners; a smooth-shaded or
 * averaged-normal model does not. And it applies to meshes only, not to the generated primitives, which
 * have no second copy to expand.
 *
 * The real answer to interior creases is a depth-and-normal edge detect over the finished frame, which is
 * a screen-space pass and a second target. This is the version that fits a forward renderer with neither.
 *
 * `thickness` is in world units at the model's own scale, and zero switches the pass off — which is the
 * resting state, so nothing pays for this until it is asked for.
 * */
NYA_API void nya_render3d_outline_set(NYA_Window* window, f32 thickness, NYA_Color color);

/**
 * Culls this pass against an occlusion buffer as well as the frustum. Null turns it back off.
 *
 * Set after `nya_render3d_begin`, which clears it — the same rule the light and the material follow,
 * and for the same reason: a buffer built for the camera of two frames ago would keep hiding things
 * for this one. See render_occlusion.h, which is where the buffer comes from and where the argument
 * for doing this in software rather than with GPU queries is made.
 *
 * The buffer is not copied and not written to; it must outlive the pass.
 * */
NYA_API void nya_render3d_occlusion(NYA_Window* window, const NYA_OcclusionBuffer* buffer);

/**
 * The camera matrix this pass is drawing with, for handing to nya_occlusion_begin.
 *
 * Read only, and read back rather than recomputed: the batch built it from the target size at
 * `nya_render3d_begin`, and a caller rebuilding it from the camera would have to know the same
 * aspect ratio and get the same answer.
 * */
NYA_API f32_4x4 nya_render3d_view_projection(NYA_Window* window) __attr_no_discard;

/**
 * Switches later translucent geometry between alpha blending and adding.
 *
 * Batch state like the material is, so changing it costs a draw call and a scene should group by it.
 * NYA_BLEND_NONE and NYA_BLEND_ALPHA both mean the ordinary sorted alpha pass; NYA_BLEND_ADDITIVE means
 * the additive one.
 *
 * Only affects geometry that is *already* translucent — the stream is chosen by the colour's alpha, and
 * this decides what the transparent stream does once it gets there. An opaque cube drawn while additive is
 * set is still an opaque cube.
 *
 * Reset to alpha by nya_render3d_begin, like the light and the material, so a frame cannot inherit it.
 * */
NYA_API void nya_render3d_blend_set(NYA_Window* window, NYA_Render3DBlend blend);

/**
 * Whether what follows is occluded by the scene or drawn over it. See NYA_Render3DDepth.
 *
 * Batch state like the blend mode, so changing it flushes — it selects the pipeline, and a pipeline
 * is per draw call. Set it back to NYA_RENDER3D_DEPTH_DEFAULT when the gizmos are done, or the rest
 * of the frame draws on top of everything.
 * */
NYA_API void nya_render3d_depth_set(NYA_Window* window, NYA_Render3DDepth depth);

NYA_API NYA_Render3DDepth nya_render3d_depth(NYA_Window* window) __attr_no_discard;

/**
 * A camera-facing quad at `center`, `size` across, spun by `rotation` radians in screen space.
 *
 * The primitive volumetric effects are made of, and the one this renderer did not have — which is why
 * nya_particles_draw was drawing small *cubes* in 3D and saying so in a comment. A cube reads correctly
 * from every angle and is the wrong shape for smoke, fire, a spark or a glow, all of which are a flat
 * image that always faces the viewer.
 *
 * ## Making volumetric effects out of these
 *
 * Real volumetrics — raymarching a density field — need a depth prepass and a compute stage this forward
 * renderer has neither of, and would look wrong against flat-shaded low-poly geometry anyway. What
 * actually produces smoke and fire in this style is a crowd of these:
 *
 * - **Smoke** is alpha-blended billboards, large and slow, drawn through the sorted transparent pass so
 *   the near ones layer over the far ones. Give them a soft texture and a low alpha.
 * - **Fire** is *additive* billboards — see nya_render3d_blend_set — small, fast, and short-lived, so
 *   overlapping tongues brighten instead of muddying.
 * - **Sparks and glow** are additive billboards at emissive colours, which the bloom pass then finds.
 *
 * `texture_handle` is what turns a square into a puff. Null draws a flat quad, which is a hard-edged
 * square by definition — the shape is the geometry, so an untextured billboard cannot be soft however its
 * colours are set. A soft radial sprite is the whole difference between a stack of squares and smoke.
 *
 * The one thing missing for the very best version of this is a soft-particle fade, where a billboard
 * fades out as it intersects solid geometry instead of showing a hard cut line along it. That needs the
 * scene depth as a texture, which this renderer does not yet produce.
 *
 * `rotation` turns the quad about the view axis, which is what stops a hundred identical puffs looking
 * stamped from one die. Does nothing when no 3D camera is active.
 * */
NYA_API void nya_render3d_billboard(NYA_Window* window, NYA_ConstCString texture_handle, f32x3 center, f32x2 size, f32 rotation,
                                    NYA_Color color);

/**
 * Resolves a texture handle once, for a caller about to draw many billboards with it.
 *
 * nya_render3d_billboard resolves its handle on every call, which is a dictionary lookup and a string
 * hash — fine for a handful and not for a particle system, where it is the same answer several hundred
 * times a frame per pass. Something drawing a crowd resolves once and passes the result.
 *
 * The value is opaque and only valid for the current frame: it names a GPU texture the asset system owns,
 * and a hot reload replaces it. Resolve per frame, never store it.
 *
 * Zero when the handle names nothing, names something that is not a texture, or names one still loading —
 * which nya_render3d_billboard_resolved draws untextured, exactly as the unresolved call does.
 * */
NYA_API NYA_Render3DTextureBinding nya_render3d_texture_resolve(NYA_ConstCString texture_handle) __attr_no_discard;

/** nya_render3d_billboard with the texture already resolved. See nya_render3d_texture_resolve. */
NYA_API void nya_render3d_billboard_resolved(NYA_Window* window, NYA_Render3DTextureBinding texture, f32x3 center, f32x2 size, f32 rotation,
                                             NYA_Color color);

NYA_API void nya_render3d_light_set(NYA_Window* window, NYA_Render3DLight light);

/** The light currently in effect. */
NYA_API NYA_Render3DLight nya_render3d_light(NYA_Window* window) __attr_no_discard;

/**
 * Adds a point light to the frame. Flushes, because the light set is a fragment uniform.
 *
 * Silently past the budget is the one behaviour worth avoiding, so the light beyond
 * NYA_RENDER3D_MAX_POINT_LIGHTS is dropped with a warning: a lamp that does nothing and says nothing is
 * indistinguishable from a lamp with the wrong colour.
 *
 * Not cleared by nya_render3d_end. A scene whose lights change per frame calls
 * nya_render3d_point_lights_clear first; one whose lights are fixed sets them once.
 * */
NYA_API void nya_render3d_point_light_add(NYA_Window* window, NYA_Render3DPointLight light);

/** Removes every point light. Flushes, for the same reason adding one does. */
NYA_API void nya_render3d_point_lights_clear(NYA_Window* window);

/**
 * Starts the shadow pass. Everything drawn until nya_render3d_shadow_end goes into the shadow map.
 *
 * **The scene has to be drawn twice.** The batch flushes as it fills rather than retaining a frame of
 * geometry, so there is nothing for the engine to replay from the light's point of view — the game calls
 * its own draw function once here and once inside nya_render3d_begin. That is the whole cost of this
 * design and it buys a shadow pass that needs no scene graph:
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
 *
 * The light direction comes from whatever NYA_Render3DLight is set, so set the light first. Materials,
 * colours and textures are all ignored during the pass — it writes depth and nothing else — so the draw
 * function needs no notion of which pass it is in.
 *
 * Does nothing when `strength` is zero, including not drawing, so a caller can leave the calls in place
 * and turn shadows off with one field.
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
 *
 * **Two things it does that placing the volume by hand does not.**
 *
 * ⭐ **The centre is pushed a half-extent down the camera's forward axis**, rather than sitting on the
 * camera. A volume centred on the viewer spends half its resolution on what is behind them, which is
 * the single cheapest doubling of shadow sharpness available — and it has to be done per cascade,
 * because each one is a different size and therefore wants a different push.
 *
 * ⭐ **The centre is snapped to whole shadow-map texels.** Without it the volume slides continuously
 * as the camera moves, every texel covers a slightly different patch of world each frame, and shadow
 * edges crawl and fizz — the most recognisable artifact cascaded shadows have, and one that looks
 * like a filtering problem rather than a placement one. Snapping makes the volume move in whole-texel
 * steps, so a static shadow edge stays on the same texels while the camera moves through it.
 *
 * `light_direction` is the way the light *travels*, matching NYA_Render3DLight.direction; a zero
 * vector is read as the default light's. The result is an ordinary NYA_Render3DShadow, so a caller
 * that wants to adjust one field can.
 * */
/**
 * The light's own axes: where it points, and an up that is not parallel to it.
 *
 * Public because the shadow pass and the volume fit have to agree exactly — the fit snaps a position
 * onto the grid the pass then rasterises, and a basis differing by a hair would put the snap on a
 * grid the map does not use.
 * */
NYA_API void nya_render3d_light_basis(f32x3 direction, OUT f32x3* out_forward, OUT f32x3* out_right, OUT f32x3* out_up);

/** The half-width of cascade `index`, from the nearest cascade's. Geometric — see the ratio's note. */
NYA_API f32 nya_render3d_cascade_extent(f32 near_extent, u32 index) __attr_no_discard;

/**
 * The matrix a shadow pass rasterises with, and the matrix the scene pass samples through.
 *
 * One function rather than the pass building its own, because the two have to be the *same* matrix and
 * for a while they were two copies of the same six lines. It is also what makes the volume's coverage
 * testable without a device: a fragment is inside cascade `i` exactly when this matrix maps it into
 * clip space's [-1, 1] box, which is the rule mesh3d_shading.hlsli now selects a cascade by.
 *
 * `center`, `extent` and `depth` are already resolved — this applies no cascade ratio and no defaults
 * beyond a zero `light_direction`, which is read as the default sun rather than normalized into NaN.
 *
 * `out_eye` is where the invented directional-light position ended up, which the pass needs for the
 * view vector it shades with. Null when the caller only wants the matrix.
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
 *
 * What something that must not be drawn into a shadow map tests. The distinction matters more than it
 * reads: `nya_render3d_shadow_active` answers "has one run", which is true for the whole camera pass
 * and false during the frame's first shadow pass, so using it to mean "am I in a shadow pass" both
 * draws the thing into the shadow map and hides it from the camera. That is precisely what happened to
 * particles, which vanished from the 3D scene entirely while still casting shadows.
 * */
NYA_API b8 nya_render3d_shadow_pass_active(NYA_Window* window) __attr_no_discard;

/** How many point lights the frame currently has, at most NYA_RENDER3D_MAX_POINT_LIGHTS. */
NYA_API u32 nya_render3d_point_light_count(NYA_Window* window) __attr_no_discard;

/**
 * Replaces the material everything drawn from here on responds with. Flushes.
 *
 * Valid only between begin and end. Every nya_render3d_begin restores the default — a fully rough
 * dielectric, which is matte plastic and the least surprising thing an unconfigured scene can be
 * made of.
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
 *
 * Drawn as thin boxes rather than as lines, because a line has no thickness in 3D and a
 * pixel-thin one shimmers as the camera moves. `thickness` is in world units.
 * */
NYA_API void nya_render3d_cube_outline(NYA_Window* window, f32x3 center, f32x3 size, NYA_Quaternion rotation, f32 thickness, NYA_Color color);

/** A UV sphere of `radius`, at NYA_RENDER3D_SPHERE_SEGMENTS around. */
NYA_API void nya_render3d_sphere(NYA_Window* window, f32x3 center, f32 radius, NYA_Color color);

/**
 * A flat quad, `size` wide and deep, lying in the xz plane and facing +y.
 *
 * The ground. One quad rather than a subdivided plane, because nothing here does per-vertex lighting
 * that would need the extra vertices.
 * */
NYA_API void nya_render3d_plane(NYA_Window* window, f32x3 center, f32x2 size, NYA_Color color);

/**
 * One flat-shaded triangle, wound counter-clockwise seen from the side it faces.
 *
 * The primitive with no shape of its own, and the reason it is public: everything else here is a shape
 * this renderer knows how to build, and a caller generating its own geometry — a terrain from a
 * heightmap, a mesh from a marching-cubes pass, a decal fitted to a surface — had no way in at all. It
 * went into the same batch as the rest, so procedural geometry costs no more draw calls than a cube does.
 *
 * The normal is the face normal, computed from the winding. That makes it flat shaded like every other
 * primitive here, which is the whole look — a smooth-shaded surface needs per-vertex normals and belongs
 * in a mesh asset, not in a call that takes three points.
 *
 * Get the winding backwards and the triangle vanishes rather than turning black: back faces are culled,
 * not shaded from behind.
 * */
NYA_API void nya_render3d_triangle(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, NYA_Color color);

/**
 * Two triangles sharing the a-c diagonal, with one normal taken from the first of them.
 *
 * The corners are expected in order around the quad and roughly coplanar; four points that are not give
 * a visible crease along the diagonal, because both halves are shaded with the first half's normal.
 * nya_render3d_triangle twice is the answer when they genuinely are not flat.
 *
 * Four vertices rather than six, which is what makes this worth having over two triangle calls on a
 * surface built from thousands of them.
 * */
NYA_API void nya_render3d_quad(NYA_Window* window, f32x3 a, f32x3 b, f32x3 c, f32x3 d, NYA_Color color);

/**
 * A line from `from` to `to`, as a square prism of `thickness` world units.
 *
 * The building block the outline and grid are made of. See nya_render3d_cube_outline for why this is
 * geometry rather than a line primitive.
 * */
NYA_API void nya_render3d_line(NYA_Window* window, f32x3 from, f32x3 to, f32 thickness, NYA_Color color);

/**
 * A wireframe grid on the xz plane, `half_extent` cells out from the origin in each direction.
 *
 * For orienting a scene during development. `cell_size` is in world units, so a grid of ten at one
 * unit spans twenty units across.
 * */
NYA_API void nya_render3d_grid(NYA_Window* window, u32 half_extent, f32 cell_size, NYA_Color color);

/**
 * Draws a model loaded as NYA_ASSET_TYPE_MESH, transformed and pushed into the batch like any other
 * shape.
 *
 * `handle` names the asset; a mesh that is not loaded yet draws nothing rather than stalling, which is
 * what makes it safe to call from the first frame while the load is still queued.
 *
 * ```c
 * nya_render3d_mesh(window, NYA_ASSET_MODELS_CUBIE_FBX, position, (f32x3){ 1, 1, 1 }, rotation, NYA_COLOR_WHITE);
 * ```
 *
 * **Smooth shaded, unlike every primitive above it.** The normals come from the file rather than from
 * the winding of each face, which is the whole reason a model looks like a model: nya_render3d_cube and
 * friends go through _nya_render3d_quad and get one flat normal per quad, and a model shaded that way
 * is faceted no matter how it was authored.
 *
 * Transformed on the CPU into the shared batch rather than drawn with a model matrix, for the same
 * reason the cube's corners are: a model matrix is a uniform, a uniform is per draw call, and a draw
 * call per model is what the batch exists to avoid. The cost is one rotate per vertex per frame, so
 * this is right for models of the size a scene has tens of and wrong for one with a hundred thousand
 * triangles — that one wants its own buffer and its own draw.
 *
 * A mesh larger than the batch can hold is dropped rather than partially drawn: half a model is a
 * harder thing to diagnose than none of it. See NYA_RENDER3D_MAX_VERTICES.
 * */
NYA_API void nya_render3d_mesh(NYA_Window* window, NYA_ConstCString handle, f32x3 center, f32x3 scale, NYA_Quaternion rotation, NYA_Color color);

/**
 * The axis-aligned bounds of a loaded mesh, in the model's own units. False when it is not loaded yet.
 *
 * Exists because a model's size is not knowable from anything else the engine hands back. The asset
 * carries its vertices and nothing that summarises them, so the only ways to fit anything to a model —
 * a collider, a camera framing it, a bounding check before drawing — were to walk the positions in game
 * code or to hardcode numbers measured by hand in a modelling package. The second is what usually
 * happens, and it is silently wrong the day the model is re-exported.
 *
 * **Scale is not applied.** These are the file's own coordinates; multiply by whatever scale is passed to
 * nya_render3d_mesh, which is the same thing that function does to each vertex.
 *
 * False and untouched outputs when the handle names nothing, names something that is not a mesh, or names
 * a mesh still loading. That last one is the ordinary case for a frame or two after nya_asset_load, and is
 * why this returns a boolean rather than a struct — a caller fitting a body to a model has to wait for the
 * load either way, and a zero-size box is not a usable stand-in.
 * */
NYA_API b8 nya_render3d_mesh_bounds(NYA_Window* window, NYA_ConstCString handle, OUT f32x3* out_min, OUT f32x3* out_max) __attr_no_discard;

/**
 * Uploads geometry the caller built and keeps it under `handle`, for nya_render3d_mesh to draw.
 *
 * The retained path without the asset system. It existed only for models read off disk, which left the
 * case that needs it most — *generated* geometry — going through the immediate batch, where every vertex
 * is transformed and uploaded again for the camera pass and for each shadow cascade. A terrain of two
 * thousand triangles is about four hundred kilobytes of that, four times a frame, for a surface that
 * changes only when its seed does.
 *
 * ```c
 * // Once, when the surface is generated.
 * nya_render3d_mesh_register(window, "my_terrain", vertices, count);
 *
 * // Every frame, from then on: one instanced draw and no vertex work at all.
 * nya_render3d_mesh(window, "my_terrain", f32x3_zero, (f32x3){ 1, 1, 1 }, nya_quaternion_identity, NYA_COLOR_WHITE);
 * ```
 *
 * `handle` is compared by pointer, like every other handle here, so it must outlive the registration — a
 * string literal or a `#define` from the asset index. Registering a handle that is already registered
 * replaces it, releasing the previous buffer, which is what regenerating a surface wants.
 *
 * The vertices are copied to the GPU and not kept on the CPU. They carry their own colours and normals, so
 * the tint passed to nya_render3d_mesh multiplies them exactly as a model's material colour is multiplied.
 *
 * **What this trades away.** Registered geometry is culled as one bounding sphere rather than per
 * triangle, so a large surface is either wholly in a pass or wholly out. That is the right trade at any
 * realistic size: the GPU processing a few thousand vertices nobody sees costs far less than the CPU
 * writing and uploading them, which is what the per-triangle version was doing.
 *
 * False when the table is full, the arguments are nonsense, or the upload failed — with a line in the log
 * saying which.
 * */
/**
 * Draws a skinned mesh, posed by `palette`. See core_skeleton.h for how a palette is built.
 *
 * ```c
 * nya_skeleton_animator_update(&animator, delta_time_s, &pose);
 * nya_skeleton_palette(skeleton, &pose, palette);
 * nya_render3d_skinned_mesh(window, NYA_ASSET_MODELS_BENDER_FBX, palette, skeleton->bone_count, placement, NYA_COLOR_WHITE);
 * ```
 *
 * **Its own draw call, and un-instanced.** Both follow from what skinning is rather than from a
 * limitation: the vertices move every frame and each by a different matrix, so there is nothing to
 * bake into a shared buffer, and two copies of a character are two poses rather than two instances.
 * Props stay on the retained instanced path, which is where the thousands are.
 *
 * Flushes the mesh batch, since it binds a different pipeline and vertex layout. Draws in the shadow
 * pass too, through a depth-only variant posed by the same palette — a shadow posed differently from
 * the surface it belongs to makes the model shadow itself in stripes.
 *
 * `model` places the whole character. It is folded into each palette entry rather than sent as its own
 * uniform, which is equivalent and costs a multiply per bone instead of one per vertex.
 *
 * Bones past NYA_SKELETON_MAX_BONES are ignored, and palette entries the caller did not fill are sent
 * as the identity — a vertex weighted to an unfilled bone would otherwise be multiplied by zeroes and
 * collapse to the origin, which is the loudest possible way to render a quiet mistake.
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
 * The 3D counterpart of nya_render2d_screen_to_world, and necessarily a different shape: in 2D the
 * screen *is* the world plane and a click names a point, while in 3D it names a line into the scene.
 * Feed the result to nya_physics3d_raycast to find out what is under the cursor.
 *
 * ```c
 * NYA_Render3DRay ray = nya_render3d_screen_ray(window, (f32x2){ mouse->x, mouse->y });
 * NYA_EntityHandle hit = nya_physics3d_raycast(ray.origin, ray.direction * 100.0F, nullptr, nullptr);
 * ```
 *
 * Valid only while a 3D camera is set — there is no ray without one. Outside that it answers a ray
 * at the origin pointing along -z, which hits nothing rather than hitting something wrong.
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
 *
 * The 3D counterpart of nya_render2d_frame_stats, and it did not exist — the counters were incremented,
 * never reset and never readable, so they were three write-only numbers growing for the life of the
 * window while the comment above them claimed they were per frame.
 *
 * `culled` is the one worth watching. Frustum culling is a cost when nothing is off screen and a saving
 * when things are, and the ratio between it and `vertices` is the only way to tell which a given scene is
 * without a stopwatch.
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
     *
     * Worth reading beside `culled` rather than instead of it: occlusion culling costs a rasterised
     * occluder and a rectangle scan per primitive, so a scene where this stays near zero is paying
     * for nothing. See render_occlusion.h.
     * */
    u32 occluded;

    /** Primitives too large for an empty batch, or past the instance ceiling. Any of these is a bug. */
    u32 dropped_draws;
};

/** What the 3D batch did this frame. Reset by nya_render_begin, so this is read before it. */
NYA_API NYA_Render3DFrameStats nya_render3d_frame_stats(NYA_Window* window) __attr_no_discard;
