// Shading shared by the textured and untextured 3D mesh pipelines.
//
// Not a shader: `.hlsli`, so the build's shader discovery — which keys on `.hlsl` — walks past it and
// only the two files that include it are compiled.
//
// It exists because those two pipelines differ in exactly one expression, where the base colour comes
// from, and everything after that is identical. They were full copies of each other for one change, and
// the first change to touch the shading model had to be made twice. A shared include is the smaller
// evil: the alternative is one pipeline with a sampler always bound, which means a real texture fetch
// per fragment on every untextured cube in the scene.
//
// ## The model
//
// Flat-shaded cartoon lighting: a wrapped diffuse quantised into bands, an optional hard highlight, a
// rim, and any number of point lights up to NYA_RENDER3D_MAX_POINT_LIGHTS. **Not physically based** —
// see the long note in render3d.h for why the Cook-Torrance version was replaced, which comes down to
// energy conservation making flat colour impossible to keep flat.

/*
 * No texture is declared here, deliberately.
 *
 * It used to declare the base colour map at t0/space2 on the reasoning that an unused binding is
 * dead-stripped. That was wrong twice over. The untextured shader binds the *shadow* map at t0, so the two
 * declarations collided at the same register; and the sampler counts the pipelines declared no longer
 * matched what the shaders sampled. The result was a shader reading an unbound descriptor — a GPUVM fault
 * and a lost device, not a warning.
 *
 * So every texture is declared by the shader that uses it, at a register that shader chooses, and passed
 * into the helpers below. The registers genuinely differ between the two pipelines: the untextured one has
 * the shadow map first because it has nothing else, and the textured one has it second.
 */

/** Has to match NYA_RENDER3D_MAX_POINT_LIGHTS and NYA_ShaderMesh3DUniform. */
#define MESH3D_MAX_POINT_LIGHTS 4

/**
 * Has to match NYA_RENDER3D_SHADOW_CASCADES.
 *
 * The array is declared at the maximum the atlas can hold rather than at the count in use, because a
 * cbuffer's layout is fixed at compile time and the count is a runtime value. `cascade_count` bounds
 * which entries are read.
 * */
#define MESH3D_SHADOW_CASCADES 3

/** The atlas is two by two, so a cascade occupies half of each axis. */
#define MESH3D_SHADOW_ATLAS_SPLIT 2.0

cbuffer Uniforms : register(b0, space3) {
  // Points *from* the surface *toward* the light, already normalized by the CPU side. Naming it this
  // way round is worth the confusion it avoids: the other convention needs a negation exactly here, and
  // forgetting it lights the shape from behind, which reads as the light being in the wrong place
  // rather than as a sign error.
  float3 light_direction;

  // How bright the side facing away from every light is, as a fraction of full. Higher than a physical
  // renderer would want — around 0.6 keeps a cartoon shadow reading as a *shade of the object*.
  float ambient;

  float3 light_color;
  float intensity;

  // Needed for the highlight and the rim, both of which depend on where the surface is seen from.
  float3 camera_position;

  // Highlight strength. Not metalness; see NYA_Render3DMaterial.
  float metallic;

  // Band softness: low is a crisp cel terminator, high is nearly a gradient.
  float roughness;

  // Rim strength on the silhouette.
  float reflectance;

  // How much of the base colour is added regardless of any light. See NYA_Render3DMaterial.emission.
  float emission;

  // How many of the point lights below are live. A float because a cbuffer row is four floats and an
  // int here would pack the same but read worse next to three of them.
  float point_light_count;

  /*
   * Point lights, two rows each: position and range, then colour and intensity.
   *
   * A fixed array rather than a count-driven buffer because the whole uniform block is pushed per draw
   * call, and a structured buffer would be a second binding to manage for four lights. Four is the
   * budget; the fifth light a caller adds is dropped with a warning rather than silently ignored.
   */
  float4 point_light_position_range[MESH3D_MAX_POINT_LIGHTS];
  float4 point_light_color_intensity[MESH3D_MAX_POINT_LIGHTS];

  // How strongly curved edges are darkened. See mesh3d_edge below and NYA_Render3DMaterial.edge.
  float edge;

  // How dark a shadowed surface goes, in [0, 1]. Zero disables the lookup entirely.
  float shadow_strength;

  // One shadow map texel in UV, so the filter kernel can step by texels without knowing the resolution.
  float shadow_texel;

  // Depth slack, in light-space depth units. See mesh3d_shadow for why it cannot be zero.
  float shadow_bias;

  /** One light view-projection per cascade, as used by each shadow pass. Has to match NYA_RENDER3D_SHADOW_CASCADES. */
  float4x4 light_view_projection[MESH3D_SHADOW_CASCADES];

  /**
   * Each cascade's half-width in world units.
   *
   * No longer what picks the cascade — see mesh3d_cascade_for for why that was the bug. It is still
   * needed to turn a shadow-map texel into a world distance, which is what the normal offset below is
   * measured in.
   * */
  float4 cascade_extent;

  /** How many cascades ran this frame. Zero means none; the lookup then returns lit. */
  float cascade_count;
  float3 cascade_pad;
};

/*
 * Where edge darkening starts and saturates, in normal-change per pixel.
 *
 * A unit normal turning through ninety degrees over a single pixel gives a `fwidth` length near root
 * two, and a gently curved surface gives well under a tenth — so the band below picks out the tight
 * shoulder of a rounded edge and leaves broad curvature alone. Constants rather than uniforms because
 * they are a property of what "an edge" means, not of a material; NYA_Render3DMaterial.edge is the knob.
 */
static const float EDGE_LO = 0.20;
static const float EDGE_HI = 0.90;

/**
 * How many steps the diffuse is quantised into.
 *
 * Three: a lit side, a mid tone and a shadow side. Two loses the mid tone that makes a curved surface
 * read as curved, and more than four stops looking deliberate and starts looking like a gradient with
 * banding artefacts — which is what a cel shader is trying not to be mistaken for.
 */
static const float BANDS = 3.0;

/**
 * Wrapped, banded diffuse for one light direction.
 *
 * Wrapped rather than clamped: the dot product is mapped from [-1, 1] onto [0, 1] instead of being cut
 * off at zero. This is the single most important line for the look — a clamped Lambert term makes
 * everything past ninety degrees from the light identically unlit, so half of every object is one flat
 * dark value and the silhouette is all that remains of its shape.
 *
 * Quantised with the transition between steps smoothed rather than hard: `floor` alone gives aliased
 * terminators that crawl as the object turns, because the band edge is a step function sampled once per
 * pixel.
 * */
float mesh3d_banded(float3 normal, float3 light, float softness) {
  float wrapped = dot(normal, light) * 0.5 + 0.5;

  float scaled = wrapped * BANDS;
  float step_index = floor(scaled);
  float within_step = scaled - step_index;

  // Named `width`, not `edge`: there is a cbuffer member called `edge` now, and a local shadowing it
  // compiles quietly while making the two impossible to tell apart at a glance.
  float width = clamp(softness * 0.5, 0.02, 0.5);

  return saturate((step_index + smoothstep(0.5 - width, 0.5 + width, within_step)) / BANDS);
}

/**
 * How much this fragment sits on a curved edge, in [0, 1].
 *
 * `fwidth` is the sum of the absolute screen-space derivatives, so this is how fast the interpolated
 * normal is turning per pixel — large on the tight shoulder of a rounded edge, near zero on a broad
 * face or a gentle curve.
 *
 * ## What this does and does not catch
 *
 * It finds **curvature**, not creases. Derivatives are computed across a 2x2 pixel quad *within one
 * triangle*, so two flat faces meeting at a hard angle each report a constant normal and no change: a
 * hard-edged cube gets nothing from this. What it does catch is exactly the geometry it was added for —
 * a rounded cube's fillets, a capsule's caps, and any smooth-shaded model's tight radii — plus a little
 * extra at grazing angles, where the normal turns quickly in screen space for the same reason.
 *
 * A hard crease genuinely needs neighbour information this pass does not have: an inverted hull drawn
 * with front faces culled, or a screen-space pass over depth and normal buffers. Both are real additions
 * rather than a few lines here.
 *
 * Scale independence comes free. The derivative is per *pixel*, so a model at the back of the scene has
 * its edges picked out by the same band as one at the front, without a distance term.
 * */
float mesh3d_edge(float3 normal) {
  return smoothstep(EDGE_LO, EDGE_HI, length(fwidth(normal)));
}

/** How far the percentage-closer kernel reaches from its centre tap, in shadow map texels. */
static const float MESH3D_SHADOW_PCF_SPREAD = 1.5;

/**
 * How far a sample is lifted along the surface normal before it is compared, in cascade texels.
 *
 * The companion to the depth bias and the reason it can stay small. Acne is a *lateral* sampling error —
 * the map recorded the depth of a point up to a texel away across the surface — so moving the sample
 * along the normal by about that texel cancels it at its source, where depth bias can only paper over it
 * by pushing the whole surface away from the light and detaching it from its own shadow.
 * */
static const float MESH3D_SHADOW_NORMAL_OFFSET = 1.25;

/** `cascade_extent` is a float4 for packing, so indexing it costs this. See its declaration. */
float mesh3d_cascade_extent_at(int index) {
  return index == 0 ? cascade_extent.x : (index == 1 ? cascade_extent.y : (index == 2 ? cascade_extent.z : cascade_extent.w));
}

/**
 * Which cascade covers this point, and where it lands in that cascade's clip space.
 *
 * ⚠ **By where the cascade's volume actually is, not by distance from the camera.** Distance was the bug
 * behind shadows that grew as the camera approached: a cascade is *not* a sphere around the viewer. Its
 * volume is pushed a half-extent down the view direction — see nya_render3d_shadow_for_camera — so it
 * runs from the camera to twice the extent ahead, and testing `distance <= extent` therefore rejected
 * most of what the map had actually recorded. The shadowed region was a sphere of the last cascade's
 * extent centred on the camera, which swept over the world as the camera moved and let shadows appear,
 * grow and vanish with nothing in the scene having changed.
 *
 * Projecting into each cascade in turn is the exact test and costs three matrix multiplies. Cascades are
 * ordered smallest first, so the first that contains the point is also the sharpest that does.
 *
 * The acceptance is inset by the filter's reach so a fragment near a cascade's edge falls through to the
 * next one rather than having its kernel clamped against the seam. `shadow_texel` is in UV and clip space
 * is twice that per unit, hence the doubling.
 *
 * Returns the count when nothing covers it — a fragment beyond the last cascade — which the caller reads
 * as "no shadow information", not as "shadowed".
 * */
int mesh3d_cascade_for(float3 world_position, out float3 out_projected) {
  int count = min((int)cascade_count, MESH3D_SHADOW_CASCADES);

  float limit = 1.0 - (shadow_texel * MESH3D_SHADOW_PCF_SPREAD * 2.0);

  out_projected = float3(0.0, 0.0, 0.0);

  for (int i = 0; i < count; i++) {
    float4 clip = mul(light_view_projection[i], float4(world_position, 1.0));

    // Orthographic, so w is one and this is a formality — kept so a perspective light needs no edit here.
    float3 projected = clip.xyz / clip.w;

    if (all(abs(projected.xy) <= limit) && projected.z >= 0.0 && projected.z <= 1.0) {
      out_projected = projected;
      return i;
    }
  }

  return count;
}

/**
 * How lit this fragment is by the directional light, in [0, 1]. One is fully lit.
 *
 * The texture and sampler are parameters rather than globals, which is the one awkward part of sharing
 * this file: the two pipelines bind a different number of textures, so the shadow map does not sit at the
 * same register in both. The untextured shader has it at t0 and the textured one at t1, and each passes
 * its own binding in here.
 *
 * The surface normal is taken whole rather than as a dot product with the light, because the offset below
 * needs the direction and not only the angle.
 *
 * ## Softness
 *
 * A three by three tap, spaced by `shadow_texel * spread`, averaged. That is a small percentage-closer
 * filter: each tap is a hard in-or-out comparison and the *average* is what makes the edge soft. It is
 * deliberately a contact shadow rather than a long one — nine taps at a couple of texels' spread gives a
 * penumbra a few pixels wide, which reads as an object sitting on a surface, and does not pretend to be
 * an area light.
 *
 * ## Bias, and why there are two of them
 *
 * A surface compared against its own depth fails the test on about half its pixels, because the value in
 * the map was rasterised from the light's point of view at a different sample position — the result is
 * "shadow acne", a moiré of dark speckles over every lit face.
 *
 * Depth bias alone fixes that by pushing the comparison away from the light, and pays for it by lifting
 * every shadow off its caster: enough slack to clear the acne on a steeply lit floor is enough to leave a
 * visible gap between an object and the shadow it casts. That gap is the thing that stops a scene reading
 * as physical, whatever else is stylised about it.
 *
 * So most of the work is done by the normal offset instead — see MESH3D_SHADOW_NORMAL_OFFSET, which
 * cancels the error where it comes from — and the depth bias is left as the small remainder that catches
 * what the offset cannot, still scaled by how obliquely the surface faces the light.
 * */
/**
 * One cascade's percentage-closer lookup. Visibility in [0, 1], one being fully lit.
 *
 * Split out of mesh3d_shadow so the cascade boundary can be crossfaded: the blend needs the same
 * lookup run against two cascades, and a copy of it per cascade is how the seam gets fixed in one and
 * not the other.
 * */
float mesh3d_shadow_in_cascade(Texture2D map, SamplerState smp, int cascade, float3 world_position, float3 normal, float slope) {
  /*
   * Re-projected from a point lifted off the surface along its normal.
   *
   * The cascade is chosen from the unlifted position — the lift is under two texels and cannot move a
   * fragment out of the volume the selection's inset already reserved, so choosing first and lifting
   * after keeps the choice stable along a surface rather than making it depend on which way it faces.
   */
  float extent      = mesh3d_cascade_extent_at(cascade);
  float texel_world = 2.0 * extent * shadow_texel;

  float3 lifted = world_position + normal * (texel_world * MESH3D_SHADOW_NORMAL_OFFSET * (0.25 + slope));

  float4 lifted_clip = mul(light_view_projection[cascade], float4(lifted, 1.0));

  float3 projected = lifted_clip.xyz / lifted_clip.w;

  /*
   * Clip space is [-1, 1] in x and y and the texture is [0, 1], and y is flipped between them.
   *
   * The flip is the same one the mesh loader applies to its UVs and for the same reason: clip space puts
   * y up and a texture puts it down.
   */
  float2 uv = float2(projected.x * 0.5 + 0.5, -projected.y * 0.5 + 0.5);

  /*
   * Folded into this cascade's quadrant of the atlas.
   *
   * Kept in [0, 1] first so the taps are clamped against the cascade's own volume rather than against the
   * atlas — a fragment at a cascade's edge has to read its own map, not whatever is in the neighbouring
   * quadrant.
   */
  float2 cascade_origin = float2((float)(cascade % 2), (float)(cascade / 2)) / MESH3D_SHADOW_ATLAS_SPLIT;

  // Nothing past the far plane was recorded, and unrecorded is lit. Nor is anything outside the volume,
  // which the blend below can ask for even though the selection would not have.
  if (projected.z > 1.0 || projected.z < 0.0) return 1.0;
  if (any(abs(projected.xy) > 1.0)) return 1.0;

  float bias = shadow_bias * (1.0 + slope * 4.0);

  /*
   * The kernel's reach in *world* units is held constant across cascades, not its reach in texels.
   *
   * A fixed texel spread makes the penumbra as wide as the cascade is: the far cascade's texels are
   * several times the near one's, so the same nine taps blur a shadow several times as much. A patch of
   * ground that changes cascade — which is what happens whenever the camera turns — then visibly changes
   * how soft its shadow is, and that is a large part of "the shadows move when I turn".
   *
   * Scaled against cascade zero, so the near cascade keeps exactly the contact shadow it was tuned for
   * and the far ones match it as closely as their resolution allows. Floored at one texel: below that the
   * nine taps collapse onto the same texel and the filter stops filtering, which trades a soft edge for
   * an aliased one and is worse than a slightly wide penumbra.
   */
  float spread = max(MESH3D_SHADOW_PCF_SPREAD * (cascade_extent.x / max(extent, 1e-4)), 1.0);

  float visibility = 0.0;

  [unroll]
  for (int y = -1; y <= 1; y++) {
    [unroll]
    for (int x = -1; x <= 1; x++) {
      float2 offset = float2((float)x, (float)y) * shadow_texel * spread;

      /*
       * Clamped inside the quadrant before the atlas offset is applied.
       *
       * This is the one real cost of an atlas over a texture array: at the very edge of a cascade the
       * filter kernel would otherwise step across the seam and average in a neighbouring cascade's depths,
       * which reads as a bright or dark fringe along the boundary. A texel of inset is enough, because the
       * kernel reaches `spread` texels and the sample is clamped rather than the kernel shrunk.
       */
      float2 tap = clamp(uv + offset, shadow_texel, 1.0 - shadow_texel) / MESH3D_SHADOW_ATLAS_SPLIT;

      // The map holds the depth of whatever the light saw first. Anything further away is behind it.
      float occluder = map.Sample(smp, cascade_origin + tap).r;

      visibility += (projected.z - bias) <= occluder ? 1.0 : 0.0;
    }
  }

  return visibility / 9.0;
}

/** How much of a cascade's outer edge is spent fading into the next one, as a fraction of its half-width. */
static const float MESH3D_SHADOW_CASCADE_FADE = 0.15;

/**
 * How lit this fragment is by the directional light, in [0, 1]. One is fully lit.
 *
 * The texture and sampler are parameters rather than globals, which is the one awkward part of sharing
 * this file: the two pipelines bind a different number of textures, so the shadow map does not sit at the
 * same register in both. The untextured shader has it at t0 and the textured one at t1, and each passes
 * its own binding in here.
 *
 * The surface normal is taken whole rather than as a dot product with the light, because the offset in
 * the lookup needs the direction and not only the angle.
 *
 * ## Softness
 *
 * A three by three tap, averaged. That is a small percentage-closer filter: each tap is a hard in-or-out
 * comparison and the *average* is what makes the edge soft. It is deliberately a contact shadow rather
 * than a long one — nine taps at a couple of texels' spread gives a penumbra a few pixels wide, which
 * reads as an object sitting on a surface, and does not pretend to be an area light.
 *
 * ## The cascade seam
 *
 * Cascades tile the view, so a patch of ground changes cascade whenever the camera moves or turns, and
 * the two cascades either side of that boundary record the same world at very different resolutions.
 * Switching between them at a hard line makes the shadow jump — a stationary object's shadow visibly
 * shifts and re-softens as the viewer turns, which is the artefact this crossfade removes. The last
 * cascade has nothing to fade into, so it keeps its hard outer edge, which is the range limit and is
 * meant to be there.
 *
 * ## Bias, and why there are two of them
 *
 * A surface compared against its own depth fails the test on about half its pixels, because the value in
 * the map was rasterised from the light's point of view at a different sample position — the result is
 * "shadow acne", a moiré of dark speckles over every lit face.
 *
 * Depth bias alone fixes that by pushing the comparison away from the light, and pays for it by lifting
 * every shadow off its caster: enough slack to clear the acne on a steeply lit floor is enough to leave a
 * visible gap between an object and the shadow it casts. That gap is the thing that stops a scene reading
 * as physical, whatever else is stylised about it.
 *
 * So most of the work is done by the normal offset instead — see MESH3D_SHADOW_NORMAL_OFFSET, which
 * cancels the error where it comes from — and the depth bias is left as the small remainder that catches
 * what the offset cannot, still scaled by how obliquely the surface faces the light.
 * */
float mesh3d_shadow(Texture2D map, SamplerState smp, float3 world_position, float3 normal) {
  if (shadow_strength <= 0.0) return 1.0;

  int count = min((int)cascade_count, MESH3D_SHADOW_CASCADES);

  float3 projected;
  int cascade = mesh3d_cascade_for(world_position, projected);

  // Past the last cascade there is nothing recorded, and unrecorded is lit — the same answer as being
  // outside a single map's volume, and for the same reason: a hard dark edge at the limit of the shadow
  // range is far more obvious than the missing shadows beyond it.
  if (cascade >= count) return 1.0;

  // Steeper angles need more slack, in both of the ways slack is applied in the lookup; a surface facing
  // the light square needs almost none.
  float slope = saturate(1.0 - saturate(dot(normal, light_direction)));

  float visibility = mesh3d_shadow_in_cascade(map, smp, cascade, world_position, normal, slope);

  /*
   * Faded into the next cascade over the outer edge of this one.
   *
   * `edge` is how far out in this cascade's own clip space the fragment sits, one being the boundary. The
   * fade starts at MESH3D_SHADOW_CASCADE_FADE from that boundary and reaches the next cascade's lookup by
   * the time it arrives, so nothing changes cascade at a visible line. Consecutive cascades overlap
   * generously — they are bounding spheres of adjacent frustum slices — so the fragment being faded to is
   * comfortably inside the next one rather than at *its* edge.
   */
  if (cascade + 1 < count) {
    float edge = max(abs(projected.x), abs(projected.y));

    float blend = smoothstep(1.0 - MESH3D_SHADOW_CASCADE_FADE, 1.0, edge);

    if (blend > 0.0) {
      float next = mesh3d_shadow_in_cascade(map, smp, cascade + 1, world_position, normal, slope);

      visibility = lerp(visibility, next, blend);
    }
  }

  /*
   * Raw visibility, with the strength applied by the caller.
   *
   * This used to fold `shadow_strength` in here and return a multiplier. Separating them is what lets the
   * caller decide *what* the factor attenuates — see mesh3d_shade, where it turned out to matter a great
   * deal whether the ambient was included.
   */
  return visibility;
}

/**
 * The display curve: identity below `MESH3D_SHOULDER_KNEE`, a soft compression above it.
 *
 * This replaced a bare `saturate`, and the difference is the whole reason bloom was untunable. Clamping
 * maps *everything* at or above one onto exactly one, so an emissive lamp at 1.6 and a fully lit pale
 * surface at 1.0 come out as the same number — and no threshold can separate two identical values. The
 * symptom was a bloom pass that either missed the lamps or set fire to the ground, with nothing in
 * between, and a terrain band that had to be darkened to buy room that should not have been needed.
 *
 * ## Why not a filmic curve
 *
 * ACES and extended Reinhard both start compressing from zero: an ACES fit puts an input of 1.0 at about
 * 0.8 and pulls the mid tones down with it. That is correct for a physically based renderer and wrong
 * here — the entire look is areas of *flat, authored* colour, and a curve that moves a mid tone at all
 * means the colour on screen is no longer the colour in the palette.
 *
 * So: exactly identity below the knee, and a shoulder only above it. Everything an artist picks is
 * reproduced untouched, and the range above one — which only emission and stacked lights reach — is
 * folded into the remaining headroom instead of being thrown away.
 *
 * The shoulder is `knee + (1 - knee) * (1 - exp(-(x - knee) / (1 - knee)))`, which is continuous and has
 * matching slope at the knee, so there is no visible seam where it takes over, and asymptotes to one, so
 * the result never needs clamping and no input is ever bright enough to blow out.
 *
 * Per channel rather than on luminance. Above the knee a saturated colour therefore shifts toward white
 * as it gets brighter, which is what a very bright thing looks like and is the behaviour wanted for
 * emissive surfaces. Below the knee the question does not arise, because nothing is changed at all.
 * */
static const float MESH3D_SHOULDER_KNEE = 0.60;

float mesh3d_shoulder_channel(float x) {
  if (x <= MESH3D_SHOULDER_KNEE) return max(x, 0.0);

  float range = 1.0 - MESH3D_SHOULDER_KNEE;

  return MESH3D_SHOULDER_KNEE + range * (1.0 - exp(-(x - MESH3D_SHOULDER_KNEE) / range));
}

float3 mesh3d_tonemap(float3 colour) {
  return float3(mesh3d_shoulder_channel(colour.r), mesh3d_shoulder_channel(colour.g), mesh3d_shoulder_channel(colour.b));
}

/**
 * The whole shading model, given a base colour.
 *
 * Split out so the two pipelines differ only in what they pass here. `world_position` is the shaded
 * point, which the point lights and the view vector both need.
 * */
float3 mesh3d_shade(float3 base_colour, float3 normal, float3 world_position, float shadow) {
  float3 view = normalize(camera_position - world_position);

  /*
   * The directional light sets the base shade, and the point lights add to it.
   *
   * Additive rather than a max or an average: a surface lit by the sun and a lamp is brighter than one
   * lit by either, and taking the brightest would make a lamp invisible wherever the sun already
   * reached. The band quantisation happens per light, so each keeps its own terminator instead of the
   * sum being banded once and losing them.
   */
  float lit = mesh3d_banded(normal, light_direction, roughness);

  /*
   * The shadow attenuates the sun *and* the ambient, but never the point lights.
   *
   * Attenuating the directional term alone is the tidy answer and it is nearly invisible here, for a
   * reason worth writing down: `ambient` is around 0.6 in this shading model, so the sun accounts for only
   * the remaining 0.4 of a lit surface. A shadow strength of 0.45 applied to that alone moves a fully lit
   * surface from 1.0 to 0.82 — an eighteenth of the range, which reads as a faint smudge rather than as a
   * shadow.
   *
   * Including the ambient is also the more defensible model, not just the more visible one: the ambient
   * term stands in for light from the sky, and geometry that blocks the sun blocks much of the sky too.
   * That is what an ambient occlusion term would do if there were one.
   *
   * The point lights stay out of it, which is the part that matters for the lamps: they are added below,
   * after this, so a lamp still lights ground that the sun cannot reach. Folding the factor in there
   * instead would make a lamp look broken inside a shadow.
   */
  float shade = saturate(ambient + (1.0 - ambient) * lit * intensity);

  shade *= lerp(1.0 - shadow_strength, 1.0, shadow);

  float3 colour = base_colour * light_color * shade;

  int count = min((int)point_light_count, MESH3D_MAX_POINT_LIGHTS);

  for (int i = 0; i < count; i++) {
    float3 to_light = point_light_position_range[i].xyz - world_position;
    float range = max(point_light_position_range[i].w, 1e-4);

    float distance = length(to_light);

    /*
     * Attenuation falls to zero *at* the range rather than asymptotically.
     *
     * The physical answer is inverse square, which never reaches zero and so leaves every light
     * faintly touching every surface — for a forward renderer pushing all lights in one uniform block
     * that is wasted work and a washed out scene. The smooth windowed falloff below is the standard
     * fix: inverse-square in the middle, driven to exactly zero at the edge, so a light has a
     * boundary a level designer can reason about.
     */
    float normalized = saturate(distance / range);
    float window = 1.0 - (normalized * normalized * normalized * normalized);
    float attenuation = (window * window) / max(distance * distance, 1e-4);

    if (attenuation <= 0.0) continue;

    float3 direction = to_light / max(distance, 1e-4);

    // Banded like the directional light, so a lamp casts the same style of terminator as the sun.
    float point_lit = mesh3d_banded(normal, direction, roughness);

    // The ambient floor is deliberately *not* applied here: it stands in for light from everywhere and
    // adding it once per lamp would brighten a scene simply for having lamps in it.
    colour += base_colour * point_light_color_intensity[i].rgb * point_light_color_intensity[i].w * point_lit * attenuation;
  }

  /*
   * One highlight, with a hard edge.
   *
   * Blinn-Phong rather than GGX, and thresholded into a shape rather than left as a falloff — the
   * cartoon idiom is a distinct bright patch, and a physically correct highlight is a soft gradient
   * that reads as a smudge at this saturation. Only from the directional light: a highlight per lamp
   * is four bright spots on a curved surface, which reads as an artefact.
   */
  float3 half_vector = normalize(view + light_direction);
  float highlight = pow(saturate(dot(normal, half_vector)), 48.0);

  // Attenuated by the shadow as well: a highlight is the directional light reflecting, and a surface in
  // shadow has none of it to reflect.
  colour += light_color * smoothstep(0.25, 0.35, highlight) * metallic * 0.35 * shadow;

  /*
   * A rim on the silhouette, which separates an object from the background without an outline pass.
   *
   * Cheap and approximate: a real outline needs the depth and normal buffers and a second pass. This one
   * only appears where the surface turns away from the viewer, so it catches the edge of a shape and
   * nothing inside it.
   */
  float rim = pow(1.0 - saturate(dot(normal, view)), 3.0);

  colour += light_color * smoothstep(0.55, 0.85, rim) * reflectance * 0.25;

  /*
   * Emission, added last and unlit.
   *
   * Unlit is the point: an emissive surface is its own light source and must not darken on the side
   * facing away from the sun, which is exactly what would happen if it were folded in above. It is what
   * gives the bloom pass something above its threshold to find — see GNY_BLOOM_THRESHOLD — so a value
   * over one is meaningful even though the result is clamped, because it lifts the *whole* surface past
   * the threshold rather than only its lit half.
   */
  /*
   * Edges darkened before emission, not after.
   *
   * Emission is a surface being its own light source, and shading an edge into it would make a glowing
   * bead look dented. Everything above emission is light *arriving*, which an edge legitimately occludes;
   * light *leaving* is not occluded by the shape it leaves from.
   *
   * Multiplied rather than subtracted so the darkening is proportional: a pale surface loses a little and
   * a saturated one loses a lot, which is what an artist drawing the same line in ink would do.
   */
  colour *= 1.0 - (mesh3d_edge(normal) * edge);

  colour += base_colour * emission;

  return colour;
}
