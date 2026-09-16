// Shading shared by the textured and untextured 3D mesh pipelines, which differ only in where the base
// colour comes from. An include rather than one pipeline with a sampler always bound, which would fetch a
// texture per fragment on every untextured cube. `.hlsli`, so shader discovery skips it.
//
// Flat-shaded cartoon lighting: a wrapped diffuse quantised into bands, an optional hard highlight, a rim, and
// up to NYA_RENDER3D_MAX_POINT_LIGHTS point lights. Not physically based, since energy conservation cannot
// keep flat colour flat.

/*
 * No texture is declared here. The shadow map sits at t0 in the untextured pipeline and t1 in the textured one,
 * and a shared declaration would mismatch one of them, which is a lost device rather than a warning. Each
 * shader declares its textures and passes them in.
 */

/** Has to match NYA_RENDER3D_MAX_POINT_LIGHTS and NYA_ShaderMesh3DUniform. */
#define MESH3D_MAX_POINT_LIGHTS 4

/**
 * Has to match NYA_RENDER3D_SHADOW_CASCADES. Declared at the maximum, since cbuffer layout is fixed at
 * compile time; `cascade_count` bounds which entries are read.
 * */
#define MESH3D_SHADOW_CASCADES 3

/** The atlas is a strip of MESH3D_SHADOW_CASCADES cascades across and one tall. */
#define MESH3D_SHADOW_ATLAS_SPLIT ((float)MESH3D_SHADOW_CASCADES)

cbuffer Uniforms : register(b0, space3) {
  // points from the surface toward the light, normalized on the CPU. the other convention needs a negation here,
  // and forgetting it lights shapes from behind.
  float3 light_direction;

  // brightness facing away from every light. around 0.6, higher than physical, so a cartoon shadow reads as a
  // shade of the object.
  float ambient;

  float3 light_color;
  float intensity;

  // for the highlight and the rim, which depend on the view.
  float3 camera_position;

  // Highlight strength. Not metalness; see NYA_Render3DMaterial.
  float metallic;

  // Band softness: low is a crisp cel terminator, high is nearly a gradient.
  float roughness;

  // Rim strength on the silhouette.
  float reflectance;

  // How much of the base colour is added regardless of any light. See NYA_Render3DMaterial.emission.
  float emission;

  // live point lights. a float, packing like the three floats beside it.
  float point_light_count;

  /*
   * Point lights, two rows each: position and range, then colour and intensity. A fixed array, since the
   * uniform block is pushed per draw and a structured buffer would be a second binding for four lights.
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

  // Fog. Two rows, matching NYA_ShaderMesh3DUniform exactly. See NYA_Render3DFog.
  float3 fog_color;
  float fog_density;

  float fog_height_falloff;
  float fog_height_base;
  float fog_sun_amount;
  float fog_pad;
};

/*
 * Where edge darkening starts and saturates, in normal change per pixel. A ninety degree turn across one pixel
 * gives about root two; gentle curvature stays under a tenth. Constants, since they define what an edge is;
 * NYA_Render3DMaterial.edge is the knob.
 */
static const float EDGE_LO = 0.20;
static const float EDGE_HI = 0.90;

/**
 * How many steps the diffuse is quantised into. Three: lit, mid tone, shadow. Two loses the mid tone that
 * reads as curvature, and more looks like a banded gradient.
 * */
static const float BANDS = 3.0;

/**
 * Wrapped, banded diffuse for one light direction.
 *
 * Wrapped: the dot product maps from [-1, 1] onto [0, 1] instead of clamping at zero, which would leave half of
 * every object one flat dark value. Band transitions are smoothed, because a bare `floor` makes terminators
 * crawl as objects turn.
 * */
float mesh3d_banded(float3 normal, float3 light, float softness) {
  float wrapped = dot(normal, light) * 0.5 + 0.5;

  float scaled = wrapped * BANDS;
  float step_index = floor(scaled);
  float within_step = scaled - step_index;

  // `width`, not `edge`, which is a cbuffer member.
  float width = clamp(softness * 0.5, 0.02, 0.5);

  return saturate((step_index + smoothstep(0.5 - width, 0.5 + width, within_step)) / BANDS);
}

/**
 * How much this fragment sits on a curved edge, in [0, 1]: how fast the interpolated normal turns per pixel.
 *
 * It finds curvature, not creases. Derivatives come from a 2x2 quad inside one triangle, so flat faces meeting
 * at a hard angle report nothing. It catches fillets, caps and tight radii on smooth models, plus a little at
 * grazing angles. Hard creases need an inverted hull or a screen-space pass. Being per pixel, it is scale
 * independent.
 * */
float mesh3d_edge(float3 normal) {
  return smoothstep(EDGE_LO, EDGE_HI, length(fwidth(normal)));
}

/** How far the percentage-closer kernel reaches from its centre tap, in shadow map texels. */
static const float MESH3D_SHADOW_PCF_SPREAD = 1.5;

/**
 * How far a sample is lifted along the surface normal before comparing, in cascade texels. Acne is a lateral
 * error of up to a texel, and lifting by that cancels it at its source, so the depth bias can stay small
 * instead of detaching surfaces from their shadows.
 * */
static const float MESH3D_SHADOW_NORMAL_OFFSET = 1.25;

/** `cascade_extent` is a float4 for packing, so indexing it goes through this. */
float mesh3d_cascade_extent_at(int index) {
  return index == 0 ? cascade_extent.x : (index == 1 ? cascade_extent.y : (index == 2 ? cascade_extent.z : cascade_extent.w));
}

/**
 * Which cascade covers this point, and where it lands in that cascade's clip space.
 *
 * Chosen by projecting into each cascade's volume, not by distance from the camera: a cascade's volume sits
 * ahead of the camera, and a distance test made shadows appear and vanish as the camera moved. Cascades are
 * ordered smallest first, so the first containing the point is the sharpest.
 *
 * The acceptance is inset by the filter's reach so a fragment near an edge uses the next cascade instead of a
 * clamped kernel. `shadow_texel` is in uv, and clip space is twice that per unit.
 *
 * Returns the count when nothing covers the point, which the caller reads as no shadow information.
 * */
int mesh3d_cascade_for(float3 world_position, out float3 out_projected) {
  int count = min((int)cascade_count, MESH3D_SHADOW_CASCADES);

  float limit = 1.0 - (shadow_texel * MESH3D_SHADOW_PCF_SPREAD * 2.0);

  out_projected = float3(0.0, 0.0, 0.0);

  for (int i = 0; i < count; i++) {
    float4 clip = mul(light_view_projection[i], float4(world_position, 1.0));

    // orthographic, so w is one; kept so a perspective light needs no change.
    float3 projected = clip.xyz / clip.w;

    if (all(abs(projected.xy) <= limit) && projected.z >= 0.0 && projected.z <= 1.0) {
      out_projected = projected;
      return i;
    }
  }

  return count;
}

/**
 * One cascade's percentage-closer lookup, visibility in [0, 1]. Separate so the cascade boundary can be
 * crossfaded with the same lookup on two cascades.
 * */
float mesh3d_shadow_in_cascade(Texture2D map, SamplerState smp, int cascade, float3 world_position, float3 normal, float slope) {
  /*
   * Re-projected from a point lifted along the normal. The cascade is chosen from the unlifted position, which
   * keeps the choice stable along a surface; the lift is under two texels and stays inside the inset.
   */
  float extent      = mesh3d_cascade_extent_at(cascade);
  float texel_world = 2.0 * extent * shadow_texel;

  float3 lifted = world_position + normal * (texel_world * MESH3D_SHADOW_NORMAL_OFFSET * (0.25 + slope));

  float4 lifted_clip = mul(light_view_projection[cascade], float4(lifted, 1.0));

  float3 projected = lifted_clip.xyz / lifted_clip.w;

  /* Clip space [-1, 1] to texture [0, 1], with y flipped as in the mesh loader's uvs. */
  float2 uv = float2(projected.x * 0.5 + 0.5, -projected.y * 0.5 + 0.5);

  /*
   * Folded into this cascade's slice of the atlas, clamped in [0, 1] first so taps read this cascade's map, not
   * the neighbour's.
   */
  float2 cascade_origin = float2((float)cascade / MESH3D_SHADOW_ATLAS_SPLIT, 0.0);

  // nothing past the far plane or outside the volume was recorded, and unrecorded is lit.
  if (projected.z > 1.0 || projected.z < 0.0) return 1.0;
  if (any(abs(projected.xy) > 1.0)) return 1.0;

  float bias = shadow_bias * (1.0 + slope * 4.0);

  /*
   * The kernel's reach is constant in world units across cascades, not in texels. A fixed texel spread makes far
   * cascades several times softer, so ground changing cascade as the camera turns visibly changes softness.
   * Scaled against cascade zero, and floored at one texel so the taps never collapse onto one texel.
   */
  float spread = max(MESH3D_SHADOW_PCF_SPREAD * (cascade_extent.x / max(extent, 1e-4)), 1.0);

  float visibility = 0.0;

  [unroll]
  for (int y = -1; y <= 1; y++) {
    [unroll]
    for (int x = -1; x <= 1; x++) {
      float2 offset = float2((float)x, (float)y) * shadow_texel * spread;

      /*
       * Clamped inside the cascade's slice before the atlas offset, so the kernel never averages in the neighbouring
       * cascade at a seam. One texel of inset is enough since the sample is clamped, not the kernel shrunk.
       */
      // x folds into this cascade's column; y spans the one-cascade-tall strip.
      float2 tap = clamp(uv + offset, shadow_texel, 1.0 - shadow_texel) / float2(MESH3D_SHADOW_ATLAS_SPLIT, 1.0);

      // the map holds the depth the light saw first; anything further is behind it.
      float occluder = map.Sample(smp, cascade_origin + tap).r;

      visibility += (projected.z - bias) <= occluder ? 1.0 : 0.0;
    }
  }

  return visibility / 9.0;
}

/** How much of a cascade's outer edge fades into the next one, as a fraction of its half-width. */
static const float MESH3D_SHADOW_CASCADE_FADE = 0.15;

/**
 * How lit this fragment is by the directional light, in [0, 1]. One is fully lit. The shadow map is a
 * parameter because its register differs between the two pipelines, and the normal is taken whole because the
 * lookup offsets along it.
 *
 * A three by three percentage-closer filter: each tap is in or out, and the average softens the edge into a
 * contact shadow a few pixels wide.
 *
 * A patch of ground changes cascade whenever the camera moves, and the two cascades record it at different
 * resolutions, so the boundary is crossfaded instead of switched. The last cascade keeps its hard edge at the
 * shadow range.
 *
 * Comparing a surface against its own depth fails on about half its pixels (acne). Depth bias alone fixes it by
 * lifting every shadow off its caster, so the normal offset (MESH3D_SHADOW_NORMAL_OFFSET) does most of the work
 * and the depth bias covers the remainder, scaled by how obliquely the surface faces the light.
 * */
float mesh3d_shadow(Texture2D map, SamplerState smp, float3 world_position, float3 normal) {
  if (shadow_strength <= 0.0) return 1.0;

  int count = min((int)cascade_count, MESH3D_SHADOW_CASCADES);

  float3 projected;
  int cascade = mesh3d_cascade_for(world_position, projected);

  // past the last cascade is unrecorded, and lit: a hard dark edge at the shadow range would be far more obvious
  // than the missing shadows beyond it.
  if (cascade >= count) return 1.0;

  // steeper angles need more slack; a surface facing the light square needs almost none.
  float slope = saturate(1.0 - saturate(dot(normal, light_direction)));

  float visibility = mesh3d_shadow_in_cascade(map, smp, cascade, world_position, normal, slope);

  /*
   * Faded into the next cascade over this one's outer edge. `edge` is how far out the fragment sits in this
   * cascade's clip space, one at the boundary. Adjacent cascades are bounding spheres of adjacent frustum slices
   * and overlap generously, so the fragment is well inside the next one.
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
   * Raw visibility; the caller applies the strength, since it decides what the shadow attenuates (see
   * mesh3d_shade).
   */
  return visibility;
}

/**
 * The display curve: identity below `MESH3D_SHOULDER_KNEE`, a soft shoulder above it.
 *
 * A bare `saturate` mapped emissive 1.6 and lit 1.0 to the same value, so no bloom threshold could separate
 * lamps from pale ground. ACES and Reinhard compress from zero and would shift the mid tones of flat, authored
 * colour, so below the knee nothing changes.
 *
 * The shoulder is `knee + (1 - knee) * (1 - exp(-(x - knee) / (1 - knee)))`: continuous with matching slope at
 * the knee and asymptotic to one. Per channel, so very bright saturated colours shift toward white, which suits
 * emission.
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

/*
 * Distance and height fog. See NYA_Render3DFog. Applied after mesh3d_tonemap, which leaves authored colours
 * alone below its knee. The shadow pass does not fog; it writes depth.
 */
float3 mesh3d_fog(float3 colour, float3 world_position) {
  // fog's whole cost when unused: one compare.
  if (fog_density <= 0.0) return colour;

  float3 to_camera = camera_position - world_position;

  /*
   * Height only thins fog above `fog_height_base`, clamped at zero below, where the exponential would run away
   * and the terrain basin has geometry. Sampled at the fragment rather than integrated along the ray, one exp
   * instead of a loop.
   */
  float above = max(world_position.y - fog_height_base, 0.0);
  float amount = saturate(1.0 - exp(-fog_density * length(to_camera) * exp(-fog_height_falloff * above)));

  /*
   * Aerial perspective: looking toward the light tints fog toward the light's colour. Squared, so the warmth stays
   * near the sun, as NYA_Render3DSky.sun_halo does.
   */
  float alignment = saturate(dot(-normalize(to_camera), light_direction));
  float3 tint = lerp(fog_color, light_color, fog_sun_amount * alignment * alignment);

  return lerp(colour, tint, amount);
}

/**
 * The whole shading model, given a base colour. `world_position` is the shaded point, for point lights and the
 * view vector.
 * */
float3 mesh3d_shade(float3 base_colour, float3 normal, float3 world_position, float shadow) {
  float3 view = normalize(camera_position - world_position);

  /*
   * The directional light sets the base shade and point lights add to it, so a lamp still shows where the sun
   * reaches. Each light is banded separately and keeps its own terminator.
   */
  float lit = mesh3d_banded(normal, light_direction, roughness);

  /*
   * The shadow attenuates the sun and the ambient, never the point lights.
   *
   * Ambient is about 0.6 here, so shadowing the sun term alone moved a lit surface from 1.0 to 0.82, a faint
   * smudge. Ambient stands in for sky light, which a sun blocker also blocks. Point lights are added after, so a
   * lamp still lights ground in shadow.
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
     * Attenuation reaches zero at the range. Inverse square never does, so every light would faintly touch every
     * surface. The windowed falloff is inverse square in the middle and exactly zero at the edge.
     */
    float normalized = saturate(distance / range);
    float window = 1.0 - (normalized * normalized * normalized * normalized);
    float attenuation = (window * window) / max(distance * distance, 1e-4);

    if (attenuation <= 0.0) continue;

    float3 direction = to_light / max(distance, 1e-4);

    // banded like the sun.
    float point_lit = mesh3d_banded(normal, direction, roughness);

    // no ambient per lamp, or a scene would brighten just for having lamps.
    colour += base_colour * point_light_color_intensity[i].rgb * point_light_color_intensity[i].w * point_lit * attenuation;
  }

  /*
   * One hard-edged highlight: Blinn-Phong, thresholded into a shape, since a soft physical highlight reads as a
   * smudge. Only from the directional light; four lamp highlights on a curve look like artefacts.
   */
  float3 half_vector = normalize(view + light_direction);
  float highlight = pow(saturate(dot(normal, half_vector)), 48.0);

  // shadowed too: a surface in shadow has no sunlight to reflect.
  colour += light_color * smoothstep(0.25, 0.35, highlight) * metallic * 0.35 * shadow;

  /*
   * A rim on the silhouette, separating the object from the background where the surface turns away from the
   * viewer. A real outline needs depth and normal buffers.
   */
  float rim = pow(1.0 - saturate(dot(normal, view)), 3.0);

  colour += light_color * smoothstep(0.55, 0.85, rim) * reflectance * 0.25;

  /*
   * Emission, added last and unlit, so an emissive surface does not darken away from the sun. A value above one
   * lifts the whole surface past the bloom threshold (GNY_BLOOM_THRESHOLD).
   */
  /*
   * Edges are darkened before emission: an edge occludes arriving light, not light the surface emits. Multiplied,
   * so pale surfaces lose a little and saturated ones more, as ink would.
   */
  colour *= 1.0 - (mesh3d_edge(normal) * edge);

  colour += base_colour * emission;

  return colour;
}
