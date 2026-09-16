/**
 * @file nn_simd.h
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
