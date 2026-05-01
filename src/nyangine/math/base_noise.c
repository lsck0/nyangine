#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define _NYA_F2 0.3660254037844386F  // (sqrt(3) - 1) / 2
#define _NYA_G2 0.21132486540518713F // (3 - sqrt(3)) / 6
#define _NYA_F3 (1.0F / 3.0F)
#define _NYA_G3 (1.0F / 6.0F)

NYA_INTERNAL f32 _nya_noise_fade(f32 t);
NYA_INTERNAL f32 _nya_noise_lerp(f32 a, f32 b, f32 t);
NYA_INTERNAL f32 _nya_noise_grad1(s32 hash, f32 x);
NYA_INTERNAL f32 _nya_noise_grad2(s32 hash, f32 x, f32 y);
NYA_INTERNAL f32 _nya_noise_grad3(s32 hash, f32 x, f32 y, f32 z);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — CREATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Noise nya_noise_create(NYA_RNG* rng) {
  nya_assert(rng != nullptr);

  NYA_Noise noise;

  // initialize with identity permutation
  for (u32 i = 0; i < 256; i++) noise.perm[i] = (u8)i;

  // Fisher-Yates shuffle
  for (u32 i = 255; i > 0; i--) {
    u32 j = nya_rng_sample_u32(
        rng,
        (NYA_RNGDistribution){
            .type    = NYA_RNG_DISTRIBUTION_UNIFORM,
            .uniform = { .min = 0, .max = i }
    }
    );
    u8 tmp        = noise.perm[i];
    noise.perm[i] = noise.perm[j];
    noise.perm[j] = tmp;
  }

  // double the table to avoid index wrapping
  for (u32 i = 0; i < 256; i++) noise.perm[256 + i] = noise.perm[i];

  return noise;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — PERLIN NOISE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 nya_noise_perlin1(NYA_Noise* noise, f32 x) {
  nya_assert(noise != nullptr);

  const u8* p  = noise->perm;
  s32       xi = (s32)floorf(x) & 255;
  f32       xf = x - floorf(x);
  f32       u  = _nya_noise_fade(xf);

  s32 a = p[xi];
  s32 b = p[xi + 1];

  return _nya_noise_lerp(_nya_noise_grad1(a, xf), _nya_noise_grad1(b, xf - 1.0F), u);
}

f32 nya_noise_perlin2(NYA_Noise* noise, f32 x, f32 y) {
  nya_assert(noise != nullptr);

  const u8* p  = noise->perm;
  s32       xi = (s32)floorf(x) & 255;
  s32       yi = (s32)floorf(y) & 255;
  f32       xf = x - floorf(x);
  f32       yf = y - floorf(y);
  f32       u  = _nya_noise_fade(xf);
  f32       v  = _nya_noise_fade(yf);

  s32 aa = p[p[xi] + yi];
  s32 ab = p[p[xi] + yi + 1];
  s32 ba = p[p[xi + 1] + yi];
  s32 bb = p[p[xi + 1] + yi + 1];

  f32 x1 = _nya_noise_lerp(_nya_noise_grad2(aa, xf, yf), _nya_noise_grad2(ba, xf - 1.0F, yf), u);
  f32 x2 = _nya_noise_lerp(_nya_noise_grad2(ab, xf, yf - 1.0F), _nya_noise_grad2(bb, xf - 1.0F, yf - 1.0F), u);

  return _nya_noise_lerp(x1, x2, v);
}

f32 nya_noise_perlin3(NYA_Noise* noise, f32 x, f32 y, f32 z) {
  nya_assert(noise != nullptr);

  const u8* p  = noise->perm;
  s32       xi = (s32)floorf(x) & 255;
  s32       yi = (s32)floorf(y) & 255;
  s32       zi = (s32)floorf(z) & 255;
  f32       xf = x - floorf(x);
  f32       yf = y - floorf(y);
  f32       zf = z - floorf(z);
  f32       u  = _nya_noise_fade(xf);
  f32       v  = _nya_noise_fade(yf);
  f32       w  = _nya_noise_fade(zf);

  s32 a  = p[xi] + yi;
  s32 aa = p[a] + zi;
  s32 ab = p[a + 1] + zi;
  s32 b  = p[xi + 1] + yi;
  s32 ba = p[b] + zi;
  s32 bb = p[b + 1] + zi;

  f32 l1 = _nya_noise_lerp(_nya_noise_grad3(p[aa], xf, yf, zf), _nya_noise_grad3(p[ba], xf - 1.0F, yf, zf), u);
  f32 l2 = _nya_noise_lerp(_nya_noise_grad3(p[ab], xf, yf - 1.0F, zf), _nya_noise_grad3(p[bb], xf - 1.0F, yf - 1.0F, zf), u);
  f32 l3 = _nya_noise_lerp(_nya_noise_grad3(p[aa + 1], xf, yf, zf - 1.0F), _nya_noise_grad3(p[ba + 1], xf - 1.0F, yf, zf - 1.0F), u);
  f32 l4 = _nya_noise_lerp(_nya_noise_grad3(p[ab + 1], xf, yf - 1.0F, zf - 1.0F), _nya_noise_grad3(p[bb + 1], xf - 1.0F, yf - 1.0F, zf - 1.0F), u);

  return _nya_noise_lerp(_nya_noise_lerp(l1, l2, v), _nya_noise_lerp(l3, l4, v), w);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — SIMPLEX NOISE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 nya_noise_simplex2(NYA_Noise* noise, f32 x, f32 y) {
  nya_assert(noise != nullptr);

  const u8* p = noise->perm;
  f32       s = (x + y) * _NYA_F2;
  s32       i = (s32)floorf(x + s);
  s32       j = (s32)floorf(y + s);

  f32 t  = (f32)(i + j) * _NYA_G2;
  f32 x0 = x - ((f32)i - t);
  f32 y0 = y - ((f32)j - t);

  s32 i1, j1;
  if (x0 > y0) {
    i1 = 1;
    j1 = 0;
  } else {
    i1 = 0;
    j1 = 1;
  }

  f32 x1 = x0 - (f32)i1 + _NYA_G2;
  f32 y1 = y0 - (f32)j1 + _NYA_G2;
  f32 x2 = x0 - 1.0F + 2.0F * _NYA_G2;
  f32 y2 = y0 - 1.0F + 2.0F * _NYA_G2;

  s32 ii = i & 255;
  s32 jj = j & 255;

  f32 n0 = 0, n1 = 0, n2 = 0;

  f32 t0 = 0.5F - x0 * x0 - y0 * y0;
  if (t0 >= 0.0F) {
    t0 *= t0;
    n0  = t0 * t0 * _nya_noise_grad2(p[ii + p[jj]], x0, y0);
  }

  f32 t1 = 0.5F - x1 * x1 - y1 * y1;
  if (t1 >= 0.0F) {
    t1 *= t1;
    n1  = t1 * t1 * _nya_noise_grad2(p[ii + i1 + p[jj + j1]], x1, y1);
  }

  f32 t2 = 0.5F - x2 * x2 - y2 * y2;
  if (t2 >= 0.0F) {
    t2 *= t2;
    n2  = t2 * t2 * _nya_noise_grad2(p[ii + 1 + p[jj + 1]], x2, y2);
  }

  return 70.0F * (n0 + n1 + n2);
}

f32 nya_noise_simplex3(NYA_Noise* noise, f32 x, f32 y, f32 z) {
  nya_assert(noise != nullptr);

  const u8* p = noise->perm;
  f32       s = (x + y + z) * _NYA_F3;
  s32       i = (s32)floorf(x + s);
  s32       j = (s32)floorf(y + s);
  s32       k = (s32)floorf(z + s);

  f32 t  = (f32)(i + j + k) * _NYA_G3;
  f32 x0 = x - ((f32)i - t);
  f32 y0 = y - ((f32)j - t);
  f32 z0 = z - ((f32)k - t);

  s32 i1, j1, k1, i2, j2, k2;
  if (x0 >= y0) {
    if (y0 >= z0) {
      i1 = 1;
      j1 = 0;
      k1 = 0;
      i2 = 1;
      j2 = 1;
      k2 = 0;
    } else if (x0 >= z0) {
      i1 = 1;
      j1 = 0;
      k1 = 0;
      i2 = 1;
      j2 = 0;
      k2 = 1;
    } else {
      i1 = 0;
      j1 = 0;
      k1 = 1;
      i2 = 1;
      j2 = 0;
      k2 = 1;
    }
  } else {
    if (y0 < z0) {
      i1 = 0;
      j1 = 0;
      k1 = 1;
      i2 = 0;
      j2 = 1;
      k2 = 1;
    } else if (x0 < z0) {
      i1 = 0;
      j1 = 1;
      k1 = 0;
      i2 = 0;
      j2 = 1;
      k2 = 1;
    } else {
      i1 = 0;
      j1 = 1;
      k1 = 0;
      i2 = 1;
      j2 = 1;
      k2 = 0;
    }
  }

  f32 x1 = x0 - (f32)i1 + _NYA_G3;
  f32 y1 = y0 - (f32)j1 + _NYA_G3;
  f32 z1 = z0 - (f32)k1 + _NYA_G3;
  f32 x2 = x0 - (f32)i2 + 2.0F * _NYA_G3;
  f32 y2 = y0 - (f32)j2 + 2.0F * _NYA_G3;
  f32 z2 = z0 - (f32)k2 + 2.0F * _NYA_G3;
  f32 x3 = x0 - 1.0F + 3.0F * _NYA_G3;
  f32 y3 = y0 - 1.0F + 3.0F * _NYA_G3;
  f32 z3 = z0 - 1.0F + 3.0F * _NYA_G3;

  s32 ii = i & 255;
  s32 jj = j & 255;
  s32 kk = k & 255;

  f32 n0 = 0, n1 = 0, n2 = 0, n3 = 0;

  f32 t0 = 0.6F - x0 * x0 - y0 * y0 - z0 * z0;
  if (t0 >= 0.0F) {
    t0 *= t0;
    n0  = t0 * t0 * _nya_noise_grad3(p[ii + p[jj + p[kk]]], x0, y0, z0);
  }

  f32 t1 = 0.6F - x1 * x1 - y1 * y1 - z1 * z1;
  if (t1 >= 0.0F) {
    t1 *= t1;
    n1  = t1 * t1 * _nya_noise_grad3(p[ii + i1 + p[jj + j1 + p[kk + k1]]], x1, y1, z1);
  }

  f32 t2 = 0.6F - x2 * x2 - y2 * y2 - z2 * z2;
  if (t2 >= 0.0F) {
    t2 *= t2;
    n2  = t2 * t2 * _nya_noise_grad3(p[ii + i2 + p[jj + j2 + p[kk + k2]]], x2, y2, z2);
  }

  f32 t3 = 0.6F - x3 * x3 - y3 * y3 - z3 * z3;
  if (t3 >= 0.0F) {
    t3 *= t3;
    n3  = t3 * t3 * _nya_noise_grad3(p[ii + 1 + p[jj + 1 + p[kk + 1]]], x3, y3, z3);
  }

  return 32.0F * (n0 + n1 + n2 + n3);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — VALUE NOISE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 nya_noise_value2(NYA_Noise* noise, f32 x, f32 y) {
  nya_assert(noise != nullptr);

  u32 seed = (u32)noise->perm[0] | ((u32)noise->perm[1] << 8) | ((u32)noise->perm[2] << 16) | ((u32)noise->perm[3] << 24);
  s32 xi   = (s32)floorf(x);
  s32 yi   = (s32)floorf(y);
  f32 xf   = x - (f32)xi;
  f32 yf   = y - (f32)yi;
  f32 u    = _nya_noise_fade(xf);
  f32 v    = _nya_noise_fade(yf);

  f32 aa = nya_ihash2(xi, yi, seed);
  f32 ba = nya_ihash2(xi + 1, yi, seed);
  f32 ab = nya_ihash2(xi, yi + 1, seed);
  f32 bb = nya_ihash2(xi + 1, yi + 1, seed);

  return _nya_noise_lerp(_nya_noise_lerp(aa, ba, u), _nya_noise_lerp(ab, bb, u), v);
}

f32 nya_noise_value3(NYA_Noise* noise, f32 x, f32 y, f32 z) {
  nya_assert(noise != nullptr);

  u32 seed = (u32)noise->perm[0] | ((u32)noise->perm[1] << 8) | ((u32)noise->perm[2] << 16) | ((u32)noise->perm[3] << 24);
  s32 xi   = (s32)floorf(x);
  s32 yi   = (s32)floorf(y);
  s32 zi   = (s32)floorf(z);
  f32 xf   = x - (f32)xi;
  f32 yf   = y - (f32)yi;
  f32 zf   = z - (f32)zi;
  f32 u    = _nya_noise_fade(xf);
  f32 v    = _nya_noise_fade(yf);
  f32 w    = _nya_noise_fade(zf);

  f32 aaa = nya_ihash3(xi, yi, zi, seed);
  f32 baa = nya_ihash3(xi + 1, yi, zi, seed);
  f32 aba = nya_ihash3(xi, yi + 1, zi, seed);
  f32 bba = nya_ihash3(xi + 1, yi + 1, zi, seed);
  f32 aab = nya_ihash3(xi, yi, zi + 1, seed);
  f32 bab = nya_ihash3(xi + 1, yi, zi + 1, seed);
  f32 abb = nya_ihash3(xi, yi + 1, zi + 1, seed);
  f32 bbb = nya_ihash3(xi + 1, yi + 1, zi + 1, seed);

  f32 l1 = _nya_noise_lerp(_nya_noise_lerp(aaa, baa, u), _nya_noise_lerp(aba, bba, u), v);
  f32 l2 = _nya_noise_lerp(_nya_noise_lerp(aab, bab, u), _nya_noise_lerp(abb, bbb, u), v);

  return _nya_noise_lerp(l1, l2, w);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — FBM
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 nya_noise_fbm1(NYA_Noise* noise, f32 x, NYA_NoiseParams p) {
  nya_assert(noise != nullptr);

  f32 sum     = 0.0F;
  f32 amp     = 1.0F;
  f32 freq    = 1.0F;
  f32 max_amp = 0.0F;

  for (u32 i = 0; i < p.octaves; i++) {
    sum     += nya_noise_perlin1(noise, x * freq) * amp;
    max_amp += amp;
    amp     *= p.gain;
    freq    *= p.lacunarity;
  }

  return sum / max_amp;
}

f32 nya_noise_fbm2(NYA_Noise* noise, f32 x, f32 y, NYA_NoiseParams p) {
  nya_assert(noise != nullptr);

  f32 sum     = 0.0F;
  f32 amp     = 1.0F;
  f32 freq    = 1.0F;
  f32 max_amp = 0.0F;

  for (u32 i = 0; i < p.octaves; i++) {
    sum     += nya_noise_perlin2(noise, x * freq, y * freq) * amp;
    max_amp += amp;
    amp     *= p.gain;
    freq    *= p.lacunarity;
  }

  return sum / max_amp;
}

f32 nya_noise_fbm3(NYA_Noise* noise, f32 x, f32 y, f32 z, NYA_NoiseParams p) {
  nya_assert(noise != nullptr);

  f32 sum     = 0.0F;
  f32 amp     = 1.0F;
  f32 freq    = 1.0F;
  f32 max_amp = 0.0F;

  for (u32 i = 0; i < p.octaves; i++) {
    sum     += nya_noise_perlin3(noise, x * freq, y * freq, z * freq) * amp;
    max_amp += amp;
    amp     *= p.gain;
    freq    *= p.lacunarity;
  }

  return sum / max_amp;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION — UTILITIES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

f32 nya_noise_ridged2(NYA_Noise* noise, f32 x, f32 y, NYA_NoiseParams p) {
  nya_assert(noise != nullptr);

  f32 sum     = 0.0F;
  f32 amp     = 1.0F;
  f32 freq    = 1.0F;
  f32 max_amp = 0.0F;

  for (u32 i = 0; i < p.octaves; i++) {
    f32 n    = 1.0F - fabsf(nya_noise_perlin2(noise, x * freq, y * freq));
    sum     += n * n * amp;
    max_amp += amp;
    amp     *= p.gain;
    freq    *= p.lacunarity;
  }

  return sum / max_amp;
}

f32 nya_noise_turbulence2(NYA_Noise* noise, f32 x, f32 y, NYA_NoiseParams p) {
  nya_assert(noise != nullptr);

  f32 sum     = 0.0F;
  f32 amp     = 1.0F;
  f32 freq    = 1.0F;
  f32 max_amp = 0.0F;

  for (u32 i = 0; i < p.octaves; i++) {
    sum     += fabsf(nya_noise_perlin2(noise, x * freq, y * freq)) * amp;
    max_amp += amp;
    amp     *= p.gain;
    freq    *= p.lacunarity;
  }

  return sum / max_amp;
}

f32 nya_noise_remap01(f32 value) {
  return value * 0.5F + 0.5F;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL f32 _nya_noise_fade(f32 t) {
  return t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
}

NYA_INTERNAL f32 _nya_noise_lerp(f32 a, f32 b, f32 t) {
  return a + t * (b - a);
}

NYA_INTERNAL f32 _nya_noise_grad1(s32 hash, f32 x) {
  return (hash & 1) ? x : -x;
}

NYA_INTERNAL f32 _nya_noise_grad2(s32 hash, f32 x, f32 y) {
  s32 h = hash & 3;
  return ((h & 1) ? -x : x) + ((h & 2) ? -y : y);
}

NYA_INTERNAL f32 _nya_noise_grad3(s32 hash, f32 x, f32 y, f32 z) {
  s32 h = hash & 15;
  f32 u = h < 8 ? x : y;
  f32 v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
  return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}
