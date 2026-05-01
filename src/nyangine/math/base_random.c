#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// Nothing up my sleeve: those are the hex digits of Φ,
// the least approximable irrational number.
// $ echo 'scale=310;obase=16;(sqrt(5)-1)/2' | bc
NYA_INTERNAL const u64 PHI[16] = {
  0x9E3779B97F4A7C15, 0xF39CC0605CEDC834, 0x1082276BF3A27251, 0xF86C6A11D0C18E95, 0x2767F0B153D27B7F, 0x0347045B5BF1827F,
  0x01886F0928403002, 0xC1D64BA40F335E36, 0xF06AD7AE9717877E, 0x85839D6EFFBD7DC6, 0x64D325D1C5371682, 0xCADD0CCCFDFFBBE1,
  0x626E33B8D04B4331, 0xBBF73C790D94F79D, 0x471C4AB3ED3D82A5, 0xFEC507705E4AE6E5,
};

NYA_INTERNAL void _nya_rng_fill_buffer(NYA_RNG* rng);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_rng_fill_buffer(NYA_RNG* rng) {
  nya_assert(rng != nullptr);

  __m256i o0 = rng->output[0];
  __m256i o1 = rng->output[1];
  __m256i o2 = rng->output[2];
  __m256i o3 = rng->output[3];
  __m256i s0 = rng->state[0];
  __m256i s1 = rng->state[1];
  __m256i s2 = rng->state[2];
  __m256i s3 = rng->state[3];
  __m256i t0;
  __m256i t1;
  __m256i t2;
  __m256i t3;
  __m256i u0;
  __m256i u1;
  __m256i u2;
  __m256i u3;
  __m256i counter = rng->counter;

  // The following shuffles move weak (low-diffusion) 32-bit parts of 64-bit
  // additions to strong positions for enrichment. The low 32-bit part of a
  // 64-bit chunk never moves to the same 64-bit chunk as its high part.
  // They do not remain in the same chunk. Each part eventually reaches all
  // positions ringwise: A to B, B to C, …, H to A.
  // You may notice that they are simply 256-bit rotations (96 and 160).
  __m256i shu0 = _mm256_set_epi32(4, 3, 2, 1, 0, 7, 6, 5);
  __m256i shu1 = _mm256_set_epi32(2, 1, 0, 7, 6, 5, 4, 3);

  // The counter is not necessary to beat PractRand.
  // It sets a lower bound of 2^71 bytes = 2 ZiB to the period,
  // or about 7 millennia at 10 GiB/s.
  // The increments are picked as odd numbers,
  // since only coprimes of the base cover the full cycle,
  // and all odd numbers are coprime of 2.
  // I use different odd numbers for each 64-bit chunk
  // for a tiny amount of variation stirring.
  // I used the smallest odd numbers to avoid having a magic number.
  __m256i increment = _mm256_set_epi64x(1, 3, 5, 7);

  for (u64 index = 0; index < _NYA_RNG_BUFFER_SIZE; index += 128) {
    _mm256_storeu_si256((__m256i*)&rng->buffer[index + 0], o0);
    _mm256_storeu_si256((__m256i*)&rng->buffer[index + 32], o1);
    _mm256_storeu_si256((__m256i*)&rng->buffer[index + 64], o2);
    _mm256_storeu_si256((__m256i*)&rng->buffer[index + 96], o3);

    // I apply the counter to s1, since it is the one whose shift loses most entropy.
    s1      = _mm256_add_epi64(s1, counter);
    s3      = _mm256_add_epi64(s3, counter);
    counter = _mm256_add_epi64(counter, increment);

    // SIMD does not support rotations. Shift is the next best thing to entangle
    // bits with other 64-bit positions. We must shift by an odd number so that
    // each bit reaches all 64-bit positions, not just half. We must lose bits
    // of information, so we minimize it: 1 and 3. We use different shift values
    // to increase divergence between the two sides. We use rightward shift
    // because the rightmost bits have the least diffusion in addition (the low
    // bit is just a XOR of the low bits).
    u0 = _mm256_srli_epi64(s0, 1);
    u1 = _mm256_srli_epi64(s1, 3);
    u2 = _mm256_srli_epi64(s2, 1);
    u3 = _mm256_srli_epi64(s3, 3);
    t0 = _mm256_permutevar8x32_epi32(s0, shu0);
    t1 = _mm256_permutevar8x32_epi32(s1, shu1);
    t2 = _mm256_permutevar8x32_epi32(s2, shu0);
    t3 = _mm256_permutevar8x32_epi32(s3, shu1);

    // Addition is the main source of diffusion.
    // Storing the output in the state keeps that diffusion permanently.
    s0 = _mm256_add_epi64(t0, u0);
    s1 = _mm256_add_epi64(t1, u1);
    s2 = _mm256_add_epi64(t2, u2);
    s3 = _mm256_add_epi64(t3, u3);

    // Two orthogonally grown pieces evolving independently, XORed.
    o0 = _mm256_xor_si256(u0, t1);
    o1 = _mm256_xor_si256(u2, t3);
    o2 = _mm256_xor_si256(s0, s3);
    o3 = _mm256_xor_si256(s2, s1);
  }

  rng->output[0] = o0;
  rng->output[1] = o1;
  rng->output[2] = o2;
  rng->output[3] = o3;
  rng->state[0]  = s0;
  rng->state[1]  = s1;
  rng->state[2]  = s2;
  rng->state[3]  = s3;
  rng->counter   = counter;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_RNG nya_rng_create_with_options(NYA_RNGOptions options) {
  NYA_RNG rng     = { 0 };
  u64     seed[4] = { 0 };

  if (options.seed == nullptr || options.seed[0] == '\0') {
    // generate seed values from timestamp
    u64 now = nya_clock_get_timestamp_ms();
    seed[0] = now;
    seed[1] = now ^ 0xBEEFBEEFBEEFBEEF;
    seed[2] = now ^ 0x123456789ABCDEF0;
    seed[3] = now ^ 0x0FEDCBA987654321;
  } else {
    // validate and decode hex string to seed values, left-padding with '0' if shorter than 64 chars
    u64 cursor = 0;
    while (options.seed[cursor] != '\0' && cursor <= 64) {
      char ch = options.seed[cursor];
      if (!(('0' <= ch && ch <= '9') || ('A' <= ch && ch <= 'F'))) {
        nya_panic("Invalid seed: Must be an uppercase hex string, got '%c' at position " FMTu64 ".", ch, cursor);
      }
      cursor++;
    }
    if (cursor > 64) nya_panic("Invalid seed: Must be at most 64 characters, got " FMTu64 " characters.", cursor);
    u64 padding = 64 - cursor;

    for (u64 s = 0; s < 4; s++) {
      seed[s] = 0;
      for (u64 i = 0; i < 16; i++) {
        u64 idx = s * 16 + i;
        u64 v   = 0;
        if (idx >= padding) {
          char ch = options.seed[idx - padding];
          v       = ('0' <= ch && ch <= '9') ? (u64)(ch - '0') : (u64)(ch - 'A' + 10);
        }
        seed[s] = (seed[s] << 4) | v;
      }
    }
  }

  // encode seed as hex string
  const char hex[] = "0123456789ABCDEF";
  for (u64 s = 0; s < 4; s++) {
    for (u64 i = 0; i < 16; i++) rng.seed[s * 16 + i] = hex[(seed[s] >> (60 - i * 4)) & 0xF];
  }
  rng.seed[64] = '\0';

  // Diffuse first two seed elements in s0, then the last two. Same for s1.
  // We must keep half of the state unchanged so users cannot set a bad state.
  rng.state[0] = _mm256_set_epi64x((s64)PHI[3], (s64)(PHI[2] ^ seed[1]), (s64)PHI[1], (s64)(PHI[0] ^ seed[0]));
  rng.state[1] = _mm256_set_epi64x((s64)PHI[7], (s64)(PHI[6] ^ seed[3]), (s64)PHI[5], (s64)(PHI[4] ^ seed[2]));
  rng.state[2] = _mm256_set_epi64x((s64)PHI[11], (s64)(PHI[10] ^ seed[3]), (s64)PHI[9], (s64)(PHI[8] ^ seed[2]));
  rng.state[3] = _mm256_set_epi64x((s64)PHI[15], (s64)(PHI[14] ^ seed[1]), (s64)PHI[13], (s64)(PHI[12] ^ seed[0]));

  for (u64 round = 0; round < _NYA_RNG_INIT_ROUNDS; round++) {
    _nya_rng_fill_buffer(&rng);

    rng.state[0] = rng.output[3];
    rng.state[1] = rng.output[2];
    rng.state[2] = rng.output[1];
    rng.state[3] = rng.output[0];
  }

  return rng;
}

void nya_rng_gen_bytes(NYA_RNG* rng, u8 buffer[], u64 size) {
  nya_assert(rng != nullptr);

  u64 remaining_bytes   = size;
  u64 rng_buffer_offset = 0;

  while (remaining_bytes > 0) {
    // did we exhaust the buffer?
    if (rng->cursor >= _NYA_RNG_BUFFER_SIZE) {
      _nya_rng_fill_buffer(rng);
      rng->cursor = 0;
    }

    u64 available_bytes = nya_min(remaining_bytes, _NYA_RNG_BUFFER_SIZE - rng->cursor);
    nya_memcpy(&buffer[rng_buffer_offset], &rng->buffer[rng->cursor], available_bytes);

    rng->cursor       += available_bytes;
    rng_buffer_offset += available_bytes;
    remaining_bytes   -= available_bytes;
  }
}

u8 nya_rng_sample_u8(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = round(nya_rng_sample_f64(rng, distribution));
  u8  result = (u8)nya_clamp(raw, (f64)U8_MIN, (f64)U8_MAX);
  return result;
}

u16 nya_rng_sample_u16(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = round(nya_rng_sample_f64(rng, distribution));
  u16 result = (u16)nya_clamp(raw, (f64)U16_MIN, (f64)U16_MAX);
  return result;
}

u32 nya_rng_sample_u32(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = round(nya_rng_sample_f64(rng, distribution));
  u32 result = (u32)nya_clamp(raw, (f64)U32_MIN, (f64)U32_MAX);
  return result;
}

u64 nya_rng_sample_u64(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = round(nya_rng_sample_f64(rng, distribution));
  u64 result = (u64)nya_clamp(raw, (f64)U64_MIN, (f64)U64_MAX);
  return result;
}

s8 nya_rng_sample_s8(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = round(nya_rng_sample_f64(rng, distribution));
  s8  result = (s8)nya_clamp(raw, (f64)S8_MIN, (f64)S8_MAX);
  return result;
}

s16 nya_rng_sample_s16(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = round(nya_rng_sample_f64(rng, distribution));
  s16 result = (s16)nya_clamp(raw, (f64)S16_MIN, (f64)S16_MAX);
  return result;
}

s32 nya_rng_sample_s32(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = round(nya_rng_sample_f64(rng, distribution));
  s32 result = (s32)nya_clamp(raw, (f64)S32_MIN, (f64)S32_MAX);
  return result;
}

s64 nya_rng_sample_s64(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = round(nya_rng_sample_f64(rng, distribution));
  s64 result = (s64)nya_clamp(raw, (f64)S64_MIN, (f64)S64_MAX);
  return result;
}

f16 nya_rng_sample_f16(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = nya_rng_sample_f64(rng, distribution);
  f16 result = (f16)nya_clamp(raw, (f64)F16_MIN, (f64)F16_MAX);
  return result;
}

f32 nya_rng_sample_f32(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  f64 raw    = nya_rng_sample_f64(rng, distribution);
  f32 result = (f32)nya_clamp(raw, (f64)F32_MIN, (f64)F32_MAX);
  return result;
}

f64 nya_rng_sample_f64(NYA_RNG* rng, NYA_RNGDistribution distribution) {
  nya_assert(rng != nullptr);

  switch (distribution.type) {
    // clamping and moving
    case NYA_RNG_DISTRIBUTION_UNIFORM: {
      f64 min = distribution.uniform.min;
      f64 max = distribution.uniform.max;
      nya_assert(min <= max, "Malformed uniform distribution: min (" FMTf64 ") must be <= max (" FMTf64 ").", min, max);

      u64 r;
      nya_rng_gen_bytes(rng, (u8*)&r, sizeof(u64));
      f64 u = (f64)r / (f64)U64_MAX;

      return min + u * (max - min);
    } break;

    // using Box-Muller transform
    case NYA_RNG_DISTRIBUTION_NORMAL: {
      f64 mean   = distribution.normal.mean;
      f64 stddev = distribution.normal.stddev;
      nya_assert(stddev > 0.0, "Malformed normal distribution: stddev (" FMTf64 ") must be > 0.", stddev);

      u64 r1;
      u64 r2;
      nya_rng_gen_bytes(rng, (u8*)&r1, sizeof(u64));
      nya_rng_gen_bytes(rng, (u8*)&r2, sizeof(u64));
      f64 u1 = ((f64)r1 + 0.5) / ((f64)U64_MAX + 1.0);
      f64 u2 = (f64)r2 / (f64)U64_MAX;

      f64 z0     = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
      f64 result = z0 * stddev + mean;

      return result;
    } break;

    // Inverse transform sampling: X = -ln(1-u) / lambda
    case NYA_RNG_DISTRIBUTION_EXPONENTIAL: {
      f64 lambda = distribution.exponential.lambda;
      nya_assert(lambda > 0.0, "Malformed exponential distribution: lambda (" FMTf64 ") must be > 0.", lambda);

      u64 r;
      nya_rng_gen_bytes(rng, (u8*)&r, sizeof(u64));
      f64 u = ((f64)r + 0.5) / ((f64)U64_MAX + 1.0);

      f64 result = -log(1.0 - u) / lambda;
      return result;
    } break;

    // Knuth algorithm for small lambda
    case NYA_RNG_DISTRIBUTION_POISSON: {
      f64 lambda = distribution.poisson.lambda;
      nya_assert(lambda > 0.0, "Malformed poisson distribution: lambda (" FMTf64 ") must be > 0.", lambda);

      f64 L = exp(-lambda);
      f64 p = 1.0;
      u64 k = 0;

      do {
        k++;
        u64 r;
        nya_rng_gen_bytes(rng, (u8*)&r, sizeof(u64));
        f64 u  = (f64)r / (f64)U64_MAX;
        p     *= u;
      } while (p > L);

      return (f64)(k - 1);
    } break;

    // Count successes in n trials
    case NYA_RNG_DISTRIBUTION_BINOMIAL: {
      u64 n = distribution.binomial.n;
      f64 p = distribution.binomial.p;
      nya_assert(0.0 <= p && p <= 1.0, "Malformed binomial distribution: p (" FMTf64 ") must be in [0, 1].", p);

      u64 successes = 0;
      for (u64 i = 0; i < n; i++) {
        u64 r;
        nya_rng_gen_bytes(rng, (u8*)&r, sizeof(u64));
        f64 u = (f64)r / (f64)U64_MAX;
        if (u < p) successes++;
      }

      return (f64)successes;
    } break;

    // Inverse transform: number of trials until first success
    case NYA_RNG_DISTRIBUTION_GEOMETRIC: {
      f64 p = distribution.geometric.p;
      nya_assert(p > 0.0 && p <= 1.0, "Malformed geometric distribution: p (" FMTf64 ") must be in (0, 1].", p);

      u64 r;
      nya_rng_gen_bytes(rng, (u8*)&r, sizeof(u64));
      f64 u = ((f64)r + 0.5) / ((f64)U64_MAX + 1.0);

      return floor(log(1.0 - u) / log(1.0 - p)) + 1.0;
    } break;

    default: nya_unreachable();
  }
  static_assert(NYA_RNG_DISTRIBUTION_COUNT == 6, "Unhandled NYA_RNGDistributionType enum value.");
}

b8 nya_rng_gen_bool(NYA_RNG* rng, f32 true_chance) {
  nya_assert(rng != nullptr);
  nya_assert(true_chance >= 0.0F && true_chance <= 1.0F, "true_chance (" FMTf32 ") must be in [0.0, 1.0].", true_chance);

  u64 r;
  nya_rng_gen_bytes(rng, (u8*)&r, sizeof(u64));
  f64 u = (f64)r / (f64)U64_MAX;

  return u < (f64)true_chance;
}
