// The 3D sky: a gradient and a sun, shaded per pixel from a reconstructed view ray.
//
// Drawn as one fullscreen triangle through nya_render2d_procedural, before anything else in the scene
// and with no depth involvement at all. That is what makes it a *background* rather than a piece of
// geometry: nothing is written to the depth buffer, so everything drawn afterwards simply covers it,
// and there is no far-plane distance to keep the sky beyond.
//
// ## Why a ray and not a cube map
//
// A cube map is an image, and this style has no image — the reference is bands of flat colour. Shading
// the gradient analytically means it costs one texture fetch fewer than a skybox, needs no asset, and
// can be driven straight from a day phase: the sun moves by changing a direction, not by re-rendering
// six faces.
//
// The thing it cannot do is clouds with shape, or anything an artist wants to paint. That is when a
// cube map earns its place.

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

// Matches NYA_ShaderSkyUniform. Every member is padded to a full float4 because HLSL will not split
// one across a sixteen-byte boundary; see the note at the top of uniforms.h.
cbuffer SkyUniform : register(b0, space3) {
  // The camera basis, as unit vectors in world space.
  float3 camera_right;
  float tangent;  // tan(fov_y / 2), which sets how wide the reconstructed rays fan out.

  float3 camera_up;
  float aspect;

  float3 camera_forward;
  float horizon_softness;

  // Points *from* the surface *toward* the sun, matching the convention the mesh shading uses.
  float3 sun_direction;
  float sun_size;

  float3 zenith_color;
  float sun_sharpness;

  float3 horizon_color;
  float ground_blend;

  float3 sun_color;
  float sun_intensity;

  float3 ground_color;
  float pad;
};

/**
 * The world-space direction this pixel looks along.
 *
 * Rebuilt from the camera basis rather than by inverting a projection, for the reason
 * nya_render3d_screen_ray gives at length: the basis is cheaper and far better conditioned than the
 * inverse of a matrix that is nearly singular near the near plane.
 *
 * `uv` grows *upward*, which is the one thing to get right here and is not the texture convention.
 * procedural.vert.hlsl builds it as `(clip * 0.5) + 0.5` straight from the clip-space corner, and clip y
 * points up — so uv.y is 1 at the top of the screen, not 0.
 *
 * Assuming the texture convention and negating this term flips the sky: the ground half ends up overhead
 * and the gradient runs the wrong way, with no error anywhere to say so.
 * */
float3 view_ray(float2 uv) {
  float2 ndc = (uv * 2.0) - 1.0;

  return normalize(camera_forward + (camera_right * ndc.x * tangent * aspect) + (camera_up * ndc.y * tangent));
}

float4 main(FragInput input) : SV_Target {
  float3 ray = view_ray(input.uv);

  /*
   * Height above the horizon, in [-1, 1], as the whole basis of the gradient.
   *
   * The y component of a unit direction *is* the sine of its elevation, so no trigonometry is needed and
   * the gradient is naturally denser near the horizon — which is what the real sky does and what makes a
   * linear ramp in this quantity read correctly rather than needing a curve.
   */
  float elevation = ray.y;

  /*
   * Sky above, ground below, with a soft band where they meet.
   *
   * The ground half matters more than it sounds. A sky that simply stops at the horizon leaves whatever
   * was on screen underneath it — the clear colour, or last frame — visible below, and orbiting a camera
   * down past level is the most ordinary thing a 3D viewer does.
   */
  float3 sky = lerp(horizon_color, zenith_color, saturate(pow(saturate(elevation), horizon_softness)));

  float below = smoothstep(0.0, -ground_blend, elevation);

  float3 colour = lerp(sky, ground_color, below);

  /*
   * The sun, as a disc with a halo, and both are thresholds rather than a falloff.
   *
   * `sun_size` is the cosine of the disc's angular radius, so the comparison against the dot product is
   * exact and needs no acos. The halo is a second, much wider smoothstep at low intensity, which is what
   * separates a sun from a white dot — the real thing brightens the sky around it well past its own edge.
   *
   * Suppressed below the horizon rather than clipped at it: a sun setting should fade as it goes down,
   * and a hard cut at zero elevation reads as the disc being sliced in half.
   */
  float alignment = dot(ray, sun_direction);

  float disc = smoothstep(sun_size, sun_size + (1.0 - sun_size) * 0.15, alignment);
  float halo = pow(saturate(alignment), sun_sharpness);

  float above_horizon = smoothstep(-ground_blend, ground_blend, sun_direction.y);

  colour += sun_color * ((disc * sun_intensity) + (halo * sun_intensity * 0.35)) * above_horizon;

  // Opaque. The sky is the bottom of the frame and there is nothing behind it to blend with; an alpha
  // below one would let whatever the target was cleared to show through the gradient.
  return float4(colour, 1.0);
}
