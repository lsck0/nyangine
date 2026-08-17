/**
 * @file nn_simd.h
 *
 * The handful of float32 kernels every tensor op is built out of, vectorized.
 *
 * ## Why these and not a library
 *
 * Every op in nn_tensor.c reduces to one of five shapes: an elementwise pass over two arrays, a
 * scaled accumulate (`y += a·x`), a dot product, a horizontal sum, and a masked accumulate for
 * ReLU's backward. Writing those five once here is what lets matmul, bias, add, sub, mul, scale,
 * sum, mean, MSE and Huber all get wider without any of them containing an intrinsic.
 *
 * ## Why AVX2 rather than SDL's GPU compute
 *
 * nn is compiled into the build tool, which builds with -DNYA_NO_SDL and therefore has no SDL to
 * call — nyangine.c includes nn.c outside the `#ifndef NYA_NO_SDL` guard, which is the same reason
 * nn_draw.c and nn_neat_draw.c live with the renderer instead of here. Beyond the layering, the
 * shapes involved are small: a DQN batch is 64 rows through 96-wide layers, and the result is needed
 * by the very next act() call. Per-dispatch latency and readback would cost more than the arithmetic
 * saved.
 *
 * ## Accumulation order
 *
 * The reductions here sum four or eight partial lanes and combine them at the end, so they do not
 * produce the same bits as a sequential scalar loop — floating point addition is not associative.
 * The result is not *less* accurate (pairwise summation over lanes is typically better than a single
 * running total), but it is different, so a test asserting on exact equality with a scalar reference
 * will need a tolerance.
 *
 * The scalar fallback below is not dead code: CFLAGS names -mavx2, which is x86 only, so an ARM or
 * WASM target compiles the plain loops and still gets correct answers.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

#if ARCH_X86_64 || ARCH_X86
#define _NYA_NN_SIMD 1
#else
#define _NYA_NN_SIMD 0
#endif

/*
 * Fused multiply-add, or the two instructions it replaces.
 *
 * AVX2 and FMA3 are separate feature bits and -mavx2 does not imply -mfma, even though every part
 * that shipped with one has the other. CFLAGS names both, so the fused form is what actually gets
 * compiled here; the fallback exists so this header stays correct under any flag set rather than
 * failing to build, which is what it did before the flag was added.
 *
 * Not merely a speed difference: the fused form rounds once instead of twice, so the two paths can
 * disagree in the last bit. Nothing here depends on which one it got.
 */
#if _NYA_NN_SIMD && defined(__FMA__)
#define _nya_nn_fmadd(a, b, c) _mm256_fmadd_ps(a, b, c)
#elif _NYA_NN_SIMD
#define _nya_nn_fmadd(a, b, c) _mm256_add_ps(_mm256_mul_ps(a, b), c)
#endif

#if _NYA_NN_SIMD
/** The eight lanes of an AVX register summed to one float, in three shuffle-and-add steps. */
__attr_allow_unused NYA_INTERNAL inline f32 _nya_nn_simd_horizontal_sum(__m256 v) {
    __m128 low  = _mm256_castps256_ps128(v);
    __m128 high = _mm256_extractf128_ps(v, 1);
    __m128 sum  = _mm_add_ps(low, high);

    sum = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
    sum = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 0x55));

    return _mm_cvtss_f32(sum);
}
#endif

/** `sum(a[i] * b[i])`. The inner loop of matmul's dA and of every dot product in the graph. */
__attr_allow_unused NYA_INTERNAL inline f32 nya_nn_simd_dot(const f32* a, const f32* b, u32 count) {
    u32 i = 0;
    f32 total = 0.0F;

#if _NYA_NN_SIMD
    // Four accumulators rather than one: an FMA has a latency of four or five cycles and a
    // throughput of two per cycle, so a single dependent chain runs at a fraction of the issue rate.
    // Four independent chains keep the units fed and cost nothing but registers.
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    __m256 acc2 = _mm256_setzero_ps();
    __m256 acc3 = _mm256_setzero_ps();

    for (; i + 32 <= count; i += 32) {
        acc0 = _nya_nn_fmadd(_mm256_loadu_ps(a + i + 0), _mm256_loadu_ps(b + i + 0), acc0);
        acc1 = _nya_nn_fmadd(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), acc1);
        acc2 = _nya_nn_fmadd(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), acc2);
        acc3 = _nya_nn_fmadd(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), acc3);
    }

    for (; i + 8 <= count; i += 8) {
        acc0 = _nya_nn_fmadd(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), acc0);
    }

    acc0  = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
    total = _nya_nn_simd_horizontal_sum(acc0);
#endif

    for (; i < count; i++) total += a[i] * b[i];

    return total;
}

/** `y[i] += alpha * x[i]`. Matmul's forward inner loop, and its dB in backward. */
__attr_allow_unused NYA_INTERNAL inline void nya_nn_simd_axpy(f32* y, f32 alpha, const f32* x, u32 count) {
    u32 i = 0;

#if _NYA_NN_SIMD
    __m256 scale = _mm256_set1_ps(alpha);

    for (; i + 16 <= count; i += 16) {
        _mm256_storeu_ps(y + i + 0, _nya_nn_fmadd(scale, _mm256_loadu_ps(x + i + 0), _mm256_loadu_ps(y + i + 0)));
        _mm256_storeu_ps(y + i + 8, _nya_nn_fmadd(scale, _mm256_loadu_ps(x + i + 8), _mm256_loadu_ps(y + i + 8)));
    }

    for (; i + 8 <= count; i += 8) {
        _mm256_storeu_ps(y + i, _nya_nn_fmadd(scale, _mm256_loadu_ps(x + i), _mm256_loadu_ps(y + i)));
    }
#endif

    for (; i < count; i++) y[i] += alpha * x[i];
}

/** `out[i] = a[i] + b[i]`. */
__attr_allow_unused NYA_INTERNAL inline void nya_nn_simd_add(f32* out, const f32* a, const f32* b, u32 count) {
    u32 i = 0;

#if _NYA_NN_SIMD
    for (; i + 8 <= count; i += 8) _mm256_storeu_ps(out + i, _mm256_add_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
#endif

    for (; i < count; i++) out[i] = a[i] + b[i];
}

/** `out[i] = a[i] - b[i]`. */
__attr_allow_unused NYA_INTERNAL inline void nya_nn_simd_sub(f32* out, const f32* a, const f32* b, u32 count) {
    u32 i = 0;

#if _NYA_NN_SIMD
    for (; i + 8 <= count; i += 8) _mm256_storeu_ps(out + i, _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
#endif

    for (; i < count; i++) out[i] = a[i] - b[i];
}

/** `out[i] = a[i] * b[i]`. */
__attr_allow_unused NYA_INTERNAL inline void nya_nn_simd_mul(f32* out, const f32* a, const f32* b, u32 count) {
    u32 i = 0;

#if _NYA_NN_SIMD
    for (; i + 8 <= count; i += 8) _mm256_storeu_ps(out + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));
#endif

    for (; i < count; i++) out[i] = a[i] * b[i];
}

/** `out[i] = a[i] * scalar`. */
__attr_allow_unused NYA_INTERNAL inline void nya_nn_simd_scale(f32* out, const f32* a, f32 scalar, u32 count) {
    u32 i = 0;

#if _NYA_NN_SIMD
    __m256 scale = _mm256_set1_ps(scalar);
    for (; i + 8 <= count; i += 8) _mm256_storeu_ps(out + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), scale));
#endif

    for (; i < count; i++) out[i] = a[i] * scalar;
}

/** `out[i] = max(a[i], 0)`. */
__attr_allow_unused NYA_INTERNAL inline void nya_nn_simd_relu(f32* out, const f32* a, u32 count) {
    u32 i = 0;

#if _NYA_NN_SIMD
    __m256 zero = _mm256_setzero_ps();
    for (; i + 8 <= count; i += 8) _mm256_storeu_ps(out + i, _mm256_max_ps(_mm256_loadu_ps(a + i), zero));
#endif

    for (; i < count; i++) out[i] = a[i] > 0.0F ? a[i] : 0.0F;
}

/**
 * `grad[i] += upstream[i]` wherever `gate[i] > 0`. ReLU's backward.
 *
 * Branchless: the comparison produces a lane mask of all ones or all zeros, and ANDing the upstream
 * gradient with it adds zero where the unit was dead. A per element branch here would mispredict on
 * roughly half the lanes of a typical activation.
 * */
__attr_allow_unused NYA_INTERNAL inline void nya_nn_simd_relu_backward(f32* grad, const f32* gate, const f32* upstream, u32 count) {
    u32 i = 0;

#if _NYA_NN_SIMD
    __m256 zero = _mm256_setzero_ps();

    for (; i + 8 <= count; i += 8) {
        __m256 mask = _mm256_cmp_ps(_mm256_loadu_ps(gate + i), zero, _CMP_GT_OQ);
        __m256 add  = _mm256_and_ps(_mm256_loadu_ps(upstream + i), mask);

        _mm256_storeu_ps(grad + i, _mm256_add_ps(_mm256_loadu_ps(grad + i), add));
    }
#endif

    for (; i < count; i++) {
        if (gate[i] > 0.0F) grad[i] += upstream[i];
    }
}

/** `sum(a[i])`. */
__attr_allow_unused NYA_INTERNAL inline f32 nya_nn_simd_sum(const f32* a, u32 count) {
    u32 i = 0;
    f32 total = 0.0F;

#if _NYA_NN_SIMD
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();

    for (; i + 16 <= count; i += 16) {
        acc0 = _mm256_add_ps(acc0, _mm256_loadu_ps(a + i + 0));
        acc1 = _mm256_add_ps(acc1, _mm256_loadu_ps(a + i + 8));
    }

    for (; i + 8 <= count; i += 8) acc0 = _mm256_add_ps(acc0, _mm256_loadu_ps(a + i));

    total = _nya_nn_simd_horizontal_sum(_mm256_add_ps(acc0, acc1));
#endif

    for (; i < count; i++) total += a[i];

    return total;
}

/** `sum((a[i] - b[i])^2)`. The MSE forward, before the division by count. */
__attr_allow_unused NYA_INTERNAL inline f32 nya_nn_simd_sum_squared_difference(const f32* a, const f32* b, u32 count) {
    u32 i = 0;
    f32 total = 0.0F;

#if _NYA_NN_SIMD
    __m256 acc = _mm256_setzero_ps();

    for (; i + 8 <= count; i += 8) {
        __m256 difference = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        acc               = _nya_nn_fmadd(difference, difference, acc);
    }

    total = _nya_nn_simd_horizontal_sum(acc);
#endif

    for (; i < count; i++) {
        f32 difference  = a[i] - b[i];
        total          += difference * difference;
    }

    return total;
}
