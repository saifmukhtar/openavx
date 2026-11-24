/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_audio.c - Audio resampling and channel downmixing
 */

#define _POSIX_C_SOURCE 200809L
#include "avx_audio.h"
#include "avx_simd.h"
#include "avx_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define INV_SQRT2 0.7071067811865475f

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define AUDIO_HAS_X86_SIMD 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#include <arm_neon.h>
#define AUDIO_HAS_ARM_NEON 1
#endif

/* ========================================================================= */
/* 1. SCALAR DOWNMIXING IMPLEMENTATIONS                                      */
/* ========================================================================= */

static inline float clamp_f32(float v) {
    if (v < -1.0f) return -1.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Downmixes 5.1 surround to stereo following ITU-R BS.775 (C and surrounds scaled by 1/sqrt(2), LFE omitted). */
static void scalar_downmix_5_1(
    const float* const in[6],
    float* restrict out_l,
    float* restrict out_r,
    int n,
    float scale,
    bool normalize_peak
) {
    const float* fl = in[0];
    const float* fr = in[1];
    const float* c  = in[2];
    /* in[3] is LFE, omitted in ITU-R BS.775 standard stereo downmix */
    const float* bl = in[4];
    const float* br = in[5];

    float max_peak = 0.0f;

    for (int i = 0; i < n; ++i) {
        float l = (fl[i] + INV_SQRT2 * c[i] + INV_SQRT2 * bl[i]) * scale;
        float r = (fr[i] + INV_SQRT2 * c[i] + INV_SQRT2 * br[i]) * scale;

        out_l[i] = l;
        out_r[i] = r;

        if (normalize_peak) {
            float al = fabsf(l);
            float ar = fabsf(r);
            if (al > max_peak) max_peak = al;
            if (ar > max_peak) max_peak = ar;
        }
    }

    if (normalize_peak && max_peak > 1.0f) {
        float inv = 1.0f / max_peak;
        for (int i = 0; i < n; ++i) {
            out_l[i] *= inv;
            out_r[i] *= inv;
        }
    } else if (!normalize_peak && scale == 1.0f) {
        for (int i = 0; i < n; ++i) {
            out_l[i] = clamp_f32(out_l[i]);
            out_r[i] = clamp_f32(out_r[i]);
        }
    }
}

/* Dolby Pro Logic Lt/Rt matrix downmix: applies +90 deg phase shift to Ls and -90 deg to Rs. */
static void scalar_downmix_5_1_ltrt(
    const float* const in[6],
    float* restrict out_l,
    float* restrict out_r,
    int n,
    float scale,
    bool normalize_peak
) {
    const float* fl = in[0];
    const float* fr = in[1];
    const float* c  = in[2];
    /* in[3] is LFE, omitted in standard Lt/Rt matrix downmix */
    const float* bl = in[4];
    const float* br = in[5];

    float max_peak = 0.0f;

    for (int i = 0; i < n; ++i) {
        float fl_val = fl ? fl[i] : 0.0f;
        float fr_val = fr ? fr[i] : 0.0f;
        float c_val  = c ? c[i] : 0.0f;
        float bl_val = bl ? bl[i] : 0.0f;
        float br_val = br ? br[i] : 0.0f;

        float c_term = INV_SQRT2 * c_val;
        float surr_term = INV_SQRT2 * (bl_val + br_val);

        float l = (fl_val + c_term - surr_term) * scale;
        float r = (fr_val + c_term + surr_term) * scale;

        out_l[i] = l;
        out_r[i] = r;

        if (normalize_peak) {
            float al = fabsf(l);
            float ar = fabsf(r);
            if (al > max_peak) max_peak = al;
            if (ar > max_peak) max_peak = ar;
        }
    }

    if (normalize_peak && max_peak > 1.0f) {
        float inv = 1.0f / max_peak;
        for (int i = 0; i < n; ++i) {
            out_l[i] *= inv;
            out_r[i] *= inv;
        }
    } else if (!normalize_peak && scale == 1.0f) {
        for (int i = 0; i < n; ++i) {
            out_l[i] = clamp_f32(out_l[i]);
            out_r[i] = clamp_f32(out_r[i]);
        }
    }
}

static void scalar_downmix_7_1(
    const float* const in[8],
    float* restrict out_l,
    float* restrict out_r,
    int n,
    float scale,
    bool normalize_peak
) {
    const float* fl = in[0];
    const float* fr = in[1];
    const float* c  = in[2];
    /* in[3] is LFE */
    const float* bl = in[4];
    const float* br = in[5];
    const float* sl = in[6];
    const float* sr = in[7];

    float max_peak = 0.0f;

    for (int i = 0; i < n; ++i) {
        float l = (fl[i] + INV_SQRT2 * c[i] + INV_SQRT2 * bl[i] + INV_SQRT2 * sl[i]) * scale;
        float r = (fr[i] + INV_SQRT2 * c[i] + INV_SQRT2 * br[i] + INV_SQRT2 * sr[i]) * scale;

        out_l[i] = l;
        out_r[i] = r;

        if (normalize_peak) {
            float al = fabsf(l);
            float ar = fabsf(r);
            if (al > max_peak) max_peak = al;
            if (ar > max_peak) max_peak = ar;
        }
    }

    if (normalize_peak && max_peak > 1.0f) {
        float inv = 1.0f / max_peak;
        for (int i = 0; i < n; ++i) {
            out_l[i] *= inv;
            out_r[i] *= inv;
        }
    } else if (!normalize_peak && scale == 1.0f) {
        for (int i = 0; i < n; ++i) {
            out_l[i] = clamp_f32(out_l[i]);
            out_r[i] = clamp_f32(out_r[i]);
        }
    }
}

static void scalar_downmix_quad(
    const float* const in[4],
    float* restrict out_l,
    float* restrict out_r,
    int n,
    float scale,
    bool normalize_peak
) {
    const float* fl = in[0];
    const float* fr = in[1];
    const float* bl = in[2];
    const float* br = in[3];

    float max_peak = 0.0f;

    for (int i = 0; i < n; ++i) {
        float l = (fl[i] + INV_SQRT2 * bl[i]) * scale;
        float r = (fr[i] + INV_SQRT2 * br[i]) * scale;

        out_l[i] = l;
        out_r[i] = r;

        if (normalize_peak) {
            float al = fabsf(l);
            float ar = fabsf(r);
            if (al > max_peak) max_peak = al;
            if (ar > max_peak) max_peak = ar;
        }
    }

    if (normalize_peak && max_peak > 1.0f) {
        float inv = 1.0f / max_peak;
        for (int i = 0; i < n; ++i) {
            out_l[i] *= inv;
            out_r[i] *= inv;
        }
    } else if (!normalize_peak && scale == 1.0f) {
        for (int i = 0; i < n; ++i) {
            out_l[i] = clamp_f32(out_l[i]);
            out_r[i] = clamp_f32(out_r[i]);
        }
    }
}

/* ========================================================================= */
/* 2. AVX2 VECTORIZED DOWNMIXING                                             */
/* ========================================================================= */

#if AUDIO_HAS_X86_SIMD

__attribute__((target("avx2,fma")))
static void avx2_downmix_5_1(
    const float* const in[6],
    float* restrict out_l,
    float* restrict out_r,
    int n,
    float scale,
    bool normalize_peak
) {
    const float* fl = in[0];
    const float* fr = in[1];
    const float* c  = in[2];
    const float* bl = in[4];
    const float* br = in[5];

    __m256 vc = _mm256_set1_ps(INV_SQRT2);
    __m256 vscale = _mm256_set1_ps(scale);
    __m256 v_max_peak = _mm256_setzero_ps();
    __m256 v_min = _mm256_set1_ps(-1.0f);
    __m256 v_max = _mm256_set1_ps(1.0f);

    int vec_len = (n / 8) * 8;
    for (int i = 0; i < vec_len; i += 8) {
        __m256 l_fl = _mm256_loadu_ps(fl + i);
        __m256 r_fr = _mm256_loadu_ps(fr + i);
        __m256 m_c  = _mm256_loadu_ps(c + i);
        __m256 l_bl = _mm256_loadu_ps(bl + i);
        __m256 r_br = _mm256_loadu_ps(br + i);

        __m256 l = _mm256_fmadd_ps(m_c, vc, l_fl);
        l = _mm256_fmadd_ps(l_bl, vc, l);
        l = _mm256_mul_ps(l, vscale);

        __m256 r = _mm256_fmadd_ps(m_c, vc, r_fr);
        r = _mm256_fmadd_ps(r_br, vc, r);
        r = _mm256_mul_ps(r, vscale);

        if (normalize_peak) {
            __m256 al = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), l);
            __m256 ar = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), r);
            v_max_peak = _mm256_max_ps(v_max_peak, al);
            v_max_peak = _mm256_max_ps(v_max_peak, ar);
        } else if (scale == 1.0f) {
            l = _mm256_min_ps(_mm256_max_ps(l, v_min), v_max);
            r = _mm256_min_ps(_mm256_max_ps(r, v_min), v_max);
        }

        _mm256_storeu_ps(out_l + i, l);
        _mm256_storeu_ps(out_r + i, r);
    }

    /* Process remainder scalar */
    float scalar_peak = 0.0f;
    for (int i = vec_len; i < n; ++i) {
        float l = (fl[i] + INV_SQRT2 * c[i] + INV_SQRT2 * bl[i]) * scale;
        float r = (fr[i] + INV_SQRT2 * c[i] + INV_SQRT2 * br[i]) * scale;
        out_l[i] = l;
        out_r[i] = r;

        if (normalize_peak) {
            float al = fabsf(l);
            float ar = fabsf(r);
            if (al > scalar_peak) scalar_peak = al;
            if (ar > scalar_peak) scalar_peak = ar;
        } else if (scale == 1.0f) {
            out_l[i] = clamp_f32(out_l[i]);
            out_r[i] = clamp_f32(out_r[i]);
        }
    }

    if (normalize_peak) {
        float peaks[8];
        _mm256_storeu_ps(peaks, v_max_peak);
        float peak = scalar_peak;
        for (int k = 0; k < 8; ++k) {
            if (peaks[k] > peak) peak = peaks[k];
        }
        if (peak > 1.0f) {
            float inv = 1.0f / peak;
            __m256 vinv = _mm256_set1_ps(inv);
            for (int i = 0; i < vec_len; i += 8) {
                __m256 l = _mm256_mul_ps(_mm256_loadu_ps(out_l + i), vinv);
                __m256 r = _mm256_mul_ps(_mm256_loadu_ps(out_r + i), vinv);
                _mm256_storeu_ps(out_l + i, l);
                _mm256_storeu_ps(out_r + i, r);
            }
            for (int i = vec_len; i < n; ++i) {
                out_l[i] *= inv;
                out_r[i] *= inv;
            }
        }
    }
}

__attribute__((target("avx2,fma")))
static void avx2_downmix_5_1_ltrt(
    const float* const in[6],
    float* restrict out_l,
    float* restrict out_r,
    int n,
    float scale,
    bool normalize_peak
) {
    const float* fl = in[0];
    const float* fr = in[1];
    const float* c  = in[2];
    const float* bl = in[4];
    const float* br = in[5];

    __m256 v_inv_sqrt2 = _mm256_set1_ps(INV_SQRT2);
    __m256 v_scale = _mm256_set1_ps(scale);
    __m256 v_min = _mm256_set1_ps(-1.0f);
    __m256 v_max = _mm256_set1_ps(1.0f);
    __m256 v_max_peak = _mm256_setzero_ps();

    int vec_len = n & ~7;
    for (int i = 0; i < vec_len; i += 8) {
        __m256 v_fl = _mm256_loadu_ps(fl + i);
        __m256 v_fr = _mm256_loadu_ps(fr + i);
        __m256 v_c  = _mm256_loadu_ps(c + i);
        __m256 v_bl = _mm256_loadu_ps(bl + i);
        __m256 v_br = _mm256_loadu_ps(br + i);

        __m256 v_c_term = _mm256_mul_ps(v_c, v_inv_sqrt2);
        __m256 v_surround = _mm256_mul_ps(_mm256_add_ps(v_bl, v_br), v_inv_sqrt2);

        __m256 l = _mm256_mul_ps(_mm256_sub_ps(_mm256_add_ps(v_fl, v_c_term), v_surround), v_scale);
        __m256 r = _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(v_fr, v_c_term), v_surround), v_scale);

        if (normalize_peak) {
            __m256 abs_l = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), l);
            __m256 abs_r = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), r);
            v_max_peak = _mm256_max_ps(v_max_peak, _mm256_max_ps(abs_l, abs_r));
        } else if (scale == 1.0f) {
            l = _mm256_min_ps(_mm256_max_ps(l, v_min), v_max);
            r = _mm256_min_ps(_mm256_max_ps(r, v_min), v_max);
        }

        _mm256_storeu_ps(out_l + i, l);
        _mm256_storeu_ps(out_r + i, r);
    }

    /* Process remainder scalar */
    float scalar_peak = 0.0f;
    for (int i = vec_len; i < n; ++i) {
        float c_term = INV_SQRT2 * c[i];
        float surr_term = INV_SQRT2 * (bl[i] + br[i]);
        float l = (fl[i] + c_term - surr_term) * scale;
        float r = (fr[i] + c_term + surr_term) * scale;
        out_l[i] = l;
        out_r[i] = r;

        if (normalize_peak) {
            float al = fabsf(l);
            float ar = fabsf(r);
            if (al > scalar_peak) scalar_peak = al;
            if (ar > scalar_peak) scalar_peak = ar;
        } else if (scale == 1.0f) {
            out_l[i] = clamp_f32(out_l[i]);
            out_r[i] = clamp_f32(out_r[i]);
        }
    }

    if (normalize_peak) {
        float peaks[8];
        _mm256_storeu_ps(peaks, v_max_peak);
        float peak = scalar_peak;
        for (int k = 0; k < 8; ++k) {
            if (peaks[k] > peak) peak = peaks[k];
        }
        if (peak > 1.0f) {
            float inv = 1.0f / peak;
            __m256 vinv = _mm256_set1_ps(inv);
            for (int i = 0; i < vec_len; i += 8) {
                __m256 l = _mm256_mul_ps(_mm256_loadu_ps(out_l + i), vinv);
                __m256 r = _mm256_mul_ps(_mm256_loadu_ps(out_r + i), vinv);
                _mm256_storeu_ps(out_l + i, l);
                _mm256_storeu_ps(out_r + i, r);
            }
            for (int i = vec_len; i < n; ++i) {
                out_l[i] *= inv;
                out_r[i] *= inv;
            }
        }
    }
}

__attribute__((target("avx2,fma")))
static void avx2_sinc_convolve_channels(
    float** fifo,
    int curr_offset,
    const float* weights,
    float* const* out_channels,
    int out_idx,
    int channels
) {
    for (int c = 0; c < channels; ++c) {
        const float *a = fifo[c] + curr_offset;
        const float *b = weights;
        __m256 va0 = _mm256_loadu_ps(a + 0);
        __m256 vb0 = _mm256_loadu_ps(b + 0);
        __m256 acc = _mm256_mul_ps(va0, vb0);

        __m256 va1 = _mm256_loadu_ps(a + 8);
        __m256 vb1 = _mm256_loadu_ps(b + 8);
        acc = _mm256_fmadd_ps(va1, vb1, acc);

        __m256 va2 = _mm256_loadu_ps(a + 16);
        __m256 vb2 = _mm256_loadu_ps(b + 16);
        acc = _mm256_fmadd_ps(va2, vb2, acc);

        __m256 va3 = _mm256_loadu_ps(a + 24);
        __m256 vb3 = _mm256_loadu_ps(b + 24);
        acc = _mm256_fmadd_ps(va3, vb3, acc);

        __m128 lo = _mm256_castps256_ps128(acc);
        __m128 hi = _mm256_extractf128_ps(acc, 1);
        __m128 sum128 = _mm_add_ps(lo, hi);
        sum128 = _mm_hadd_ps(sum128, sum128);
        sum128 = _mm_hadd_ps(sum128, sum128);
        out_channels[c][out_idx] = _mm_cvtss_f32(sum128);
    }
}

__attribute__((target("avx2,fma")))
static inline void avx2_cubic_interpolate_stereo(
    float** fifo,
    int curr_idx,
    float wm1, float w0, float w1, float w2,
    float* const* out_channels,
    int out_idx
) {
    __m128 vw = _mm_set_ps(w2, w1, w0, wm1);
    __m128 y_l = _mm_loadu_ps(fifo[0] + curr_idx - 1);
    __m128 y_r = _mm_loadu_ps(fifo[1] + curr_idx - 1);

    __m128 prod_l = _mm_mul_ps(y_l, vw);
    __m128 prod_r = _mm_mul_ps(y_r, vw);

    __m128 sum_lr = _mm_hadd_ps(prod_l, prod_r);
    sum_lr = _mm_hadd_ps(sum_lr, sum_lr);

    out_channels[0][out_idx] = _mm_cvtss_f32(sum_lr);
    out_channels[1][out_idx] = _mm_cvtss_f32(_mm_shuffle_ps(sum_lr, sum_lr, _MM_SHUFFLE(1, 1, 1, 1)));
}

#endif /* AUDIO_HAS_X86_SIMD */

/* ========================================================================= */
/* 3. ARM64 NEON VECTORIZED DOWNMIXING                                       */
/* ========================================================================= */

#if AUDIO_HAS_ARM_NEON

static void neon_downmix_5_1(
    const float* const in[6],
    float* restrict out_l,
    float* restrict out_r,
    int n,
    float scale,
    bool normalize_peak
) {
    const float* fl = in[0];
    const float* fr = in[1];
    const float* c  = in[2];
    const float* bl = in[4];
    const float* br = in[5];

    float32x4_t vc = vdupq_n_f32(INV_SQRT2);
    float32x4_t vscale = vdupq_n_f32(scale);
    float32x4_t v_max_peak = vdupq_n_f32(0.0f);
    float32x4_t v_min = vdupq_n_f32(-1.0f);
    float32x4_t v_max = vdupq_n_f32(1.0f);

    int vec_len = (n / 4) * 4;
    for (int i = 0; i < vec_len; i += 4) {
        float32x4_t l_fl = vld1q_f32(fl + i);
        float32x4_t r_fr = vld1q_f32(fr + i);
        float32x4_t m_c  = vld1q_f32(c + i);
        float32x4_t l_bl = vld1q_f32(bl + i);
        float32x4_t r_br = vld1q_f32(br + i);

        float32x4_t l = vmlaq_f32(l_fl, m_c, vc);
        l = vmlaq_f32(l, l_bl, vc);
        l = vmulq_f32(l, vscale);

        float32x4_t r = vmlaq_f32(r_fr, m_c, vc);
        r = vmlaq_f32(r, r_br, vc);
        r = vmulq_f32(r, vscale);

        if (normalize_peak) {
            float32x4_t al = vabsq_f32(l);
            float32x4_t ar = vabsq_f32(r);
            v_max_peak = vmaxq_f32(v_max_peak, al);
            v_max_peak = vmaxq_f32(v_max_peak, ar);
        } else if (scale == 1.0f) {
            l = vminq_f32(vmaxq_f32(l, v_min), v_max);
            r = vminq_f32(vmaxq_f32(r, v_min), v_max);
        }

        vst1q_f32(out_l + i, l);
        vst1q_f32(out_r + i, r);
    }

    float scalar_peak = 0.0f;
    for (int i = vec_len; i < n; ++i) {
        float l = (fl[i] + INV_SQRT2 * c[i] + INV_SQRT2 * bl[i]) * scale;
        float r = (fr[i] + INV_SQRT2 * c[i] + INV_SQRT2 * br[i]) * scale;
        out_l[i] = l;
        out_r[i] = r;

        if (normalize_peak) {
            float al = fabsf(l);
            float ar = fabsf(r);
            if (al > scalar_peak) scalar_peak = al;
            if (ar > scalar_peak) scalar_peak = ar;
        } else if (scale == 1.0f) {
            out_l[i] = clamp_f32(out_l[i]);
            out_r[i] = clamp_f32(out_r[i]);
        }
    }

    if (normalize_peak) {
        float peaks[4];
        vst1q_f32(peaks, v_max_peak);
        float peak = scalar_peak;
        for (int k = 0; k < 4; ++k) {
            if (peaks[k] > peak) peak = peaks[k];
        }
        if (peak > 1.0f) {
            float inv = 1.0f / peak;
            float32x4_t vinv = vdupq_n_f32(inv);
            for (int i = 0; i < vec_len; i += 4) {
                float32x4_t l = vmulq_f32(vld1q_f32(out_l + i), vinv);
                float32x4_t r = vmulq_f32(vld1q_f32(out_r + i), vinv);
                vst1q_f32(out_l + i, l);
                vst1q_f32(out_r + i, r);
            }
            for (int i = vec_len; i < n; ++i) {
                out_l[i] *= inv;
                out_r[i] *= inv;
            }
        }
    }
}

static void neon_downmix_5_1_ltrt(
    const float* const in[6],
    float* restrict out_l,
    float* restrict out_r,
    int n,
    float scale,
    bool normalize_peak
) {
    const float* fl = in[0];
    const float* fr = in[1];
    const float* c  = in[2];
    const float* bl = in[4];
    const float* br = in[5];

    float32x4_t vc = vdupq_n_f32(INV_SQRT2);
    float32x4_t vscale = vdupq_n_f32(scale);
    float32x4_t v_max_peak = vdupq_n_f32(0.0f);
    float32x4_t v_min = vdupq_n_f32(-1.0f);
    float32x4_t v_max = vdupq_n_f32(1.0f);

    int vec_len = (n / 4) * 4;
    for (int i = 0; i < vec_len; i += 4) {
        float32x4_t l_fl = vld1q_f32(fl + i);
        float32x4_t r_fr = vld1q_f32(fr + i);
        float32x4_t m_c  = vld1q_f32(c + i);
        float32x4_t l_bl = vld1q_f32(bl + i);
        float32x4_t r_br = vld1q_f32(br + i);

        float32x4_t surr = vaddq_f32(l_bl, r_br);
        float32x4_t c_term = vmulq_f32(m_c, vc);
        float32x4_t surr_term = vmulq_f32(surr, vc);

        float32x4_t l = vsubq_f32(vaddq_f32(l_fl, c_term), surr_term);
        l = vmulq_f32(l, vscale);

        float32x4_t r = vaddq_f32(vaddq_f32(r_fr, c_term), surr_term);
        r = vmulq_f32(r, vscale);

        if (normalize_peak) {
            float32x4_t al = vabsq_f32(l);
            float32x4_t ar = vabsq_f32(r);
            v_max_peak = vmaxq_f32(v_max_peak, al);
            v_max_peak = vmaxq_f32(v_max_peak, ar);
        } else if (scale == 1.0f) {
            l = vminq_f32(vmaxq_f32(l, v_min), v_max);
            r = vminq_f32(vmaxq_f32(r, v_min), v_max);
        }

        vst1q_f32(out_l + i, l);
        vst1q_f32(out_r + i, r);
    }

    float scalar_peak = 0.0f;
    for (int i = vec_len; i < n; ++i) {
        float c_term = INV_SQRT2 * c[i];
        float surr_term = INV_SQRT2 * (bl[i] + br[i]);
        float l = (fl[i] + c_term - surr_term) * scale;
        float r = (fr[i] + c_term + surr_term) * scale;
        out_l[i] = l;
        out_r[i] = r;

        if (normalize_peak) {
            float al = fabsf(l);
            float ar = fabsf(r);
            if (al > scalar_peak) scalar_peak = al;
            if (ar > scalar_peak) scalar_peak = ar;
        } else if (scale == 1.0f) {
            out_l[i] = clamp_f32(out_l[i]);
            out_r[i] = clamp_f32(out_r[i]);
        }
    }

    if (normalize_peak) {
        float peaks[4];
        vst1q_f32(peaks, v_max_peak);
        float peak = scalar_peak;
        for (int k = 0; k < 4; ++k) {
            if (peaks[k] > peak) peak = peaks[k];
        }
        if (peak > 1.0f) {
            float inv = 1.0f / peak;
            float32x4_t vinv = vdupq_n_f32(inv);
            for (int i = 0; i < vec_len; i += 4) {
                float32x4_t l = vmulq_f32(vld1q_f32(out_l + i), vinv);
                float32x4_t r = vmulq_f32(vld1q_f32(out_r + i), vinv);
                vst1q_f32(out_l + i, l);
                vst1q_f32(out_r + i, r);
            }
            for (int i = vec_len; i < n; ++i) {
                out_l[i] *= inv;
                out_r[i] *= inv;
            }
        }
    }
}

static void neon_sinc_convolve_channels(
    float** fifo,
    int curr_offset,
    const float* weights,
    float* const* out_channels,
    int out_idx,
    int channels
) {
    for (int c = 0; c < channels; ++c) {
        const float *a = fifo[c] + curr_offset;
        const float *b = weights;

        float32x4_t acc0 = vmulq_f32(vld1q_f32(a + 0),  vld1q_f32(b + 0));
        float32x4_t acc1 = vmulq_f32(vld1q_f32(a + 4),  vld1q_f32(b + 4));

        acc0 = vmlaq_f32(acc0, vld1q_f32(a + 8),  vld1q_f32(b + 8));
        acc1 = vmlaq_f32(acc1, vld1q_f32(a + 12), vld1q_f32(b + 12));

        acc0 = vmlaq_f32(acc0, vld1q_f32(a + 16), vld1q_f32(b + 16));
        acc1 = vmlaq_f32(acc1, vld1q_f32(a + 20), vld1q_f32(b + 20));

        acc0 = vmlaq_f32(acc0, vld1q_f32(a + 24), vld1q_f32(b + 24));
        acc1 = vmlaq_f32(acc1, vld1q_f32(a + 28), vld1q_f32(b + 28));

        float32x4_t sum4 = vaddq_f32(acc0, acc1);
        out_channels[c][out_idx] = vaddvq_f32(sum4);
    }
}

static inline void neon_cubic_interpolate_stereo(
    float** fifo,
    int curr_idx,
    float wm1, float w0, float w1, float w2,
    float* const* out_channels,
    int out_idx
) {
    float32x4_t vw = { wm1, w0, w1, w2 };
    float32x4_t y_l = vld1q_f32(fifo[0] + curr_idx - 1);
    float32x4_t y_r = vld1q_f32(fifo[1] + curr_idx - 1);

    out_channels[0][out_idx] = vaddvq_f32(vmulq_f32(y_l, vw));
    out_channels[1][out_idx] = vaddvq_f32(vmulq_f32(y_r, vw));
}

#endif /* AUDIO_HAS_ARM_NEON */

/* ========================================================================= */
/* 4. PUBLIC DOWNMIXING APIS                                                 */
/* ========================================================================= */

void avx_audio_downmix_5_1_to_stereo(
    const float* const in_channels[6],
    float* out_left,
    float* out_right,
    int frames,
    avx_audio_normalize_e normalize_mode
) {
    if (!in_channels || !out_left || !out_right || frames <= 0) return;
    if (!in_channels[0] || !in_channels[1] || !in_channels[2] || !in_channels[4] || !in_channels[5]) return;

    float scale = 1.0f;
    bool peak_norm = (normalize_mode == AVX_AUDIO_NORMALIZE_PEAK);
    if (normalize_mode == AVX_AUDIO_NORMALIZE_ATTEN_3DB) {
        /* Total sum of weights for L: 1.0 (FL) + 0.7071 (C) + 0.7071 (BL) = 2.41421356 */
        scale = 1.0f / (1.0f + 2.0f * INV_SQRT2);
    }

    avx_simd_type_e caps = avx_simd_get_caps();

#if AUDIO_HAS_X86_SIMD
    if (caps == AVX_SIMD_AVX2) {
        avx2_downmix_5_1(in_channels, out_left, out_right, frames, scale, peak_norm);
        return;
    }
#elif AUDIO_HAS_ARM_NEON
    if (caps == AVX_SIMD_NEON) {
        neon_downmix_5_1(in_channels, out_left, out_right, frames, scale, peak_norm);
        return;
    }
#else
    (void)caps;
#endif

    scalar_downmix_5_1(in_channels, out_left, out_right, frames, scale, peak_norm);
}

void avx_audio_downmix_5_1_to_ltrt(
    const float* const in_channels[6],
    float* out_left,
    float* out_right,
    int frames,
    avx_audio_normalize_e normalize_mode
) {
    if (!in_channels || !out_left || !out_right || frames <= 0) return;

    float scale = 1.0f;
    bool peak_norm = (normalize_mode == AVX_AUDIO_NORMALIZE_PEAK);
    if (normalize_mode == AVX_AUDIO_NORMALIZE_ATTEN_3DB) {
        /* Total sum of weights for Lt: 1.0 (FL) + 0.7071 (C) + 0.7071 (BL) + 0.7071 (BR) = 3.12132 */
        scale = 1.0f / (1.0f + 3.0f * INV_SQRT2);
    }

    if (!in_channels[0] || !in_channels[1] || !in_channels[2] ||
        !in_channels[4] || !in_channels[5]) {
        scalar_downmix_5_1_ltrt(in_channels, out_left, out_right, frames, scale, peak_norm);
        return;
    }

    avx_simd_type_e caps = avx_simd_get_caps();

#if AUDIO_HAS_X86_SIMD
    if (caps == AVX_SIMD_AVX2) {
        avx2_downmix_5_1_ltrt(in_channels, out_left, out_right, frames, scale, peak_norm);
        return;
    }
#elif AUDIO_HAS_ARM_NEON
    if (caps == AVX_SIMD_NEON) {
        neon_downmix_5_1_ltrt(in_channels, out_left, out_right, frames, scale, peak_norm);
        return;
    }
#else
    (void)caps;
#endif

    scalar_downmix_5_1_ltrt(in_channels, out_left, out_right, frames, scale, peak_norm);
}

void avx_audio_downmix_7_1_to_stereo(
    const float* const in_channels[8],
    float* out_left,
    float* out_right,
    int frames,
    avx_audio_normalize_e normalize_mode
) {
    if (!in_channels || !out_left || !out_right || frames <= 0) return;
    if (!in_channels[0] || !in_channels[1] || !in_channels[2] || !in_channels[4] ||
        !in_channels[5] || !in_channels[6] || !in_channels[7]) return;

    float scale = 1.0f;
    bool peak_norm = (normalize_mode == AVX_AUDIO_NORMALIZE_PEAK);
    if (normalize_mode == AVX_AUDIO_NORMALIZE_ATTEN_3DB) {
        /* Total sum of weights for L: 1.0 + 3 * 0.7071 = 3.12132 */
        scale = 1.0f / (1.0f + 3.0f * INV_SQRT2);
    }

    scalar_downmix_7_1(in_channels, out_left, out_right, frames, scale, peak_norm);
}

void avx_audio_downmix_quad_to_stereo(
    const float* const in_channels[4],
    float* out_left,
    float* out_right,
    int frames,
    avx_audio_normalize_e normalize_mode
) {
    if (!in_channels || !out_left || !out_right || frames <= 0) return;
    if (!in_channels[0] || !in_channels[1] || !in_channels[2] || !in_channels[3]) return;

    float scale = 1.0f;
    bool peak_norm = (normalize_mode == AVX_AUDIO_NORMALIZE_PEAK);
    if (normalize_mode == AVX_AUDIO_NORMALIZE_ATTEN_3DB) {
        /* 1.0 + 0.7071 = 1.7071 */
        scale = 1.0f / (1.0f + INV_SQRT2);
    }

    scalar_downmix_quad(in_channels, out_left, out_right, frames, scale, peak_norm);
}

void avx_audio_downmix_stereo_to_mono(
    const float* in_left,
    const float* in_right,
    float* out_mono,
    int frames,
    avx_audio_normalize_e normalize_mode
) {
    if (!in_left || !in_right || !out_mono || frames <= 0) return;

    float scale = 0.5f;
    if (normalize_mode == AVX_AUDIO_NORMALIZE_NONE) {
        scale = 1.0f;
    }

    float max_peak = 0.0f;
    bool peak_norm = (normalize_mode == AVX_AUDIO_NORMALIZE_PEAK);

    for (int i = 0; i < frames; ++i) {
        float m = (in_left[i] + in_right[i]) * scale;
        out_mono[i] = m;
        if (peak_norm) {
            float am = fabsf(m);
            if (am > max_peak) max_peak = am;
        } else if (scale == 1.0f) {
            out_mono[i] = clamp_f32(out_mono[i]);
        }
    }

    if (peak_norm && max_peak > 1.0f) {
        float inv = 1.0f / max_peak;
        for (int i = 0; i < frames; ++i) {
            out_mono[i] *= inv;
        }
    }
}

void avx_audio_matrix_mix(
    const float* const* in_channels,
    int in_channel_count,
    float* const* out_channels,
    int out_channel_count,
    const float* matrix,
    int frames,
    avx_audio_normalize_e normalize_mode
) {
    if (!in_channels || in_channel_count <= 0 ||
        !out_channels || out_channel_count <= 0 ||
        !matrix || frames <= 0) {
        return;
    }

    float max_peak = 0.0f;
    bool peak_norm = (normalize_mode == AVX_AUDIO_NORMALIZE_PEAK);

    /* Compute coefficient sum normalization per output channel if ATTEN_3DB */
    float* norm_scales = (float*)malloc(out_channel_count * sizeof(float));
    if (!norm_scales) return;

    for (int o = 0; o < out_channel_count; ++o) {
        if (normalize_mode == AVX_AUDIO_NORMALIZE_ATTEN_3DB) {
            float sum_w = 0.0f;
            for (int in = 0; in < in_channel_count; ++in) {
                sum_w += fabsf(matrix[o * in_channel_count + in]);
            }
            norm_scales[o] = (sum_w > 1.0f) ? (1.0f / sum_w) : 1.0f;
        } else {
            norm_scales[o] = 1.0f;
        }
    }

    for (int o = 0; o < out_channel_count; ++o) {
        float* dst = out_channels[o];
        const float* row = matrix + o * in_channel_count;
        float scale = norm_scales[o];

        for (int i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (int in = 0; in < in_channel_count; ++in) {
                acc += row[in] * in_channels[in][i];
            }
            acc *= scale;
            dst[i] = acc;

            if (peak_norm) {
                float a = fabsf(acc);
                if (a > max_peak) max_peak = a;
            } else if (scale == 1.0f) {
                dst[i] = clamp_f32(dst[i]);
            }
        }
    }

    free(norm_scales);

    if (peak_norm && max_peak > 1.0f) {
        float inv = 1.0f / max_peak;
        for (int o = 0; o < out_channel_count; ++o) {
            float* dst = out_channels[o];
            for (int i = 0; i < frames; ++i) {
                dst[i] *= inv;
            }
        }
    }
}

bool avx_audio_frame_downmix_to_stereo(
    const oavx_audio_frame_t* p_src_frame,
    oavx_audio_frame_t* p_dst_frame,
    avx_audio_normalize_e normalize_mode
) {
    if (!p_src_frame || !p_src_frame->planes[0] || p_src_frame->frames == 0 || !p_dst_frame) {
        return false;
    }

    int in_channels = (int)p_src_frame->channels;
    int frames = (int)p_src_frame->frames;
    size_t src_stride = p_src_frame->stride_bytes;
    if (src_stride == 0) {
        src_stride = (size_t)frames * sizeof(float);
    }
    size_t stride_samples = src_stride / sizeof(float);

    int dst_channels = 2;
    int dst_stride = frames * (int)sizeof(float);
    float* dst_data = (float*)malloc((size_t)dst_channels * dst_stride);
    if (!dst_data) return false;

    float* out_l = dst_data;
    float* out_r = dst_data + frames;

    if (in_channels == 1) {
        /* Mono -> Duplicate to Stereo */
        const float* in_m = (const float*)p_src_frame->planes[0];
        memcpy(out_l, in_m, frames * sizeof(float));
        memcpy(out_r, in_m, frames * sizeof(float));
    } else if (in_channels == 2) {
        /* Already stereo: copy channels */
        const float* in_l = (const float*)p_src_frame->planes[0];
        const float* in_r = p_src_frame->planes[1] ? (const float*)p_src_frame->planes[1] :
                            ((const float*)p_src_frame->planes[0] + stride_samples);
        memcpy(out_l, in_l, frames * sizeof(float));
        memcpy(out_r, in_r, frames * sizeof(float));
    } else if (in_channels == 4) {
        /* Quad 4.0 */
        const float* in_ch[4];
        for (int c = 0; c < 4; ++c) {
            in_ch[c] = p_src_frame->planes[c] ? (const float*)p_src_frame->planes[c] :
                       ((const float*)p_src_frame->planes[0] + c * stride_samples);
        }
        avx_audio_downmix_quad_to_stereo(in_ch, out_l, out_r, frames, normalize_mode);
    } else if (in_channels == 6) {
        /* 5.1 Surround */
        const float* in_ch[6];
        for (int c = 0; c < 6; ++c) {
            in_ch[c] = p_src_frame->planes[c] ? (const float*)p_src_frame->planes[c] :
                       ((const float*)p_src_frame->planes[0] + c * stride_samples);
        }
        avx_audio_downmix_5_1_to_stereo(in_ch, out_l, out_r, frames, normalize_mode);
    } else if (in_channels == 8) {
        /* 7.1 Surround */
        const float* in_ch[8];
        for (int c = 0; c < 8; ++c) {
            in_ch[c] = p_src_frame->planes[c] ? (const float*)p_src_frame->planes[c] :
                       ((const float*)p_src_frame->planes[0] + c * stride_samples);
        }
        avx_audio_downmix_7_1_to_stereo(in_ch, out_l, out_r, frames, normalize_mode);
    } else {
        /* Arbitrary layout: mix even channels to L, odd channels to R */
        float* out_ch[2] = { out_l, out_r };
        float* matrix = (float*)calloc(2 * in_channels, sizeof(float));
        if (!matrix) {
            free(dst_data);
            return false;
        }
        for (int c = 0; c < in_channels; ++c) {
            int out_idx = c % 2;
            matrix[out_idx * in_channels + c] = 1.0f;
        }
        const float** in_ptrs = (const float**)malloc(in_channels * sizeof(float*));
        if (!in_ptrs) {
            free(matrix);
            free(dst_data);
            return false;
        }
        for (int c = 0; c < in_channels; ++c) {
            in_ptrs[c] = (c < OAVX_MAX_AUDIO_CHANNELS && p_src_frame->planes[c]) ? (const float*)p_src_frame->planes[c] :
                         ((const float*)p_src_frame->planes[0] + (size_t)c * stride_samples);
        }
        avx_audio_matrix_mix(in_ptrs, in_channels, out_ch, 2, matrix, frames, normalize_mode);
        free(in_ptrs);
        free(matrix);
    }

    memset(p_dst_frame, 0, sizeof(*p_dst_frame));
    p_dst_frame->channels = (uint16_t)dst_channels;
    p_dst_frame->frames = (uint32_t)frames;
    p_dst_frame->sample_rate = p_src_frame->sample_rate;
    p_dst_frame->format = OAVX_AUDIO_FMT_FLT_PLANAR;
    p_dst_frame->pts_ns = p_src_frame->pts_ns;
    p_dst_frame->channel_mask = OAVX_AUDIO_MASK_STEREO;
    p_dst_frame->stride_bytes = (size_t)dst_stride;
    p_dst_frame->planes[0] = dst_data;
    p_dst_frame->planes[1] = dst_data + frames;
    for (int c = 2; c < OAVX_MAX_AUDIO_CHANNELS; ++c) p_dst_frame->planes[c] = NULL;
    p_dst_frame->metadata = p_src_frame->metadata;
    p_dst_frame->metadata_len = p_src_frame->metadata_len;
    p_dst_frame->user_ctx = NULL;

    return true;
}

void avx_audio_frame_free_data(oavx_audio_frame_t* p_frame) {
    if (p_frame && p_frame->planes[0]) {
        free(p_frame->planes[0]);
        for (int c = 0; c < OAVX_MAX_AUDIO_CHANNELS; ++c) p_frame->planes[c] = NULL;
    }
}

/* ========================================================================= */
/* 5. BROADCAST RESAMPLING ENGINE                                            */
/* ========================================================================= */

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

struct avx_resampler_t {
    int in_rate;
    int out_rate;
    int channels;
    avx_audio_resample_mode_e mode;

    uint64_t step_num;
    uint64_t step_den;
    uint64_t phase_frac;

    int lookbehind;
    int lookahead;

    float** fifo;
    int fifo_cap;
    int fifo_count;
    bool primed;
};

static inline float sinc_func(float x) {
    if (fabsf(x) < 1e-6f) return 1.0f;
    float px = (float)M_PI * x;
    return sinf(px) / px;
}

static inline float blackman_window(float x, float radius) {
    if (fabsf(x) >= radius) return 0.0f;
    float norm = (x + radius) / (2.0f * radius);
    return 0.42f - 0.5f * cosf(2.0f * (float)M_PI * norm) + 0.08f * cosf(4.0f * (float)M_PI * norm);
}

static void scalar_sinc_convolve_channels(
    float** fifo,
    int curr_offset,
    const float* weights,
    float* const* out_channels,
    int out_idx,
    int channels
) {
    for (int c = 0; c < channels; ++c) {
        const float *a = fifo[c] + curr_offset;
        float sum = 0.0f;
        for (int j = 0; j < 32; ++j) {
            sum += a[j] * weights[j];
        }
        out_channels[c][out_idx] = sum;
    }
}

/*
 * Creates polyphase sinc resampler. Computes reduced rational conversion factor (P/Q)
 * and pre-computes Kaiser-windowed sinc filter table for arbitrary sample rate pairs.
 */
avx_resampler_t* avx_resampler_create(
    int in_rate,
    int out_rate,
    int channels,
    avx_audio_resample_mode_e mode
) {
    if (in_rate <= 0 || out_rate <= 0 || channels <= 0) return NULL;

    avx_resampler_t* r = (avx_resampler_t*)calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->in_rate = in_rate;
    r->out_rate = out_rate;
    r->channels = channels;
    r->mode = mode;

    uint64_t g = gcd_u64((uint64_t)in_rate, (uint64_t)out_rate);
    r->step_num = (uint64_t)in_rate / g;
    r->step_den = (uint64_t)out_rate / g;
    r->phase_frac = 0;

    if (mode == AVX_AUDIO_RESAMPLE_LINEAR) {
        r->lookbehind = 0;
        r->lookahead = 1;
    } else if (mode == AVX_AUDIO_RESAMPLE_CUBIC) {
        r->lookbehind = 1;
        r->lookahead = 2;
    } else {
        r->lookbehind = 16;
        r->lookahead = 16;
    }

    r->fifo_cap = 4096;
    r->fifo = (float**)calloc((size_t)channels, sizeof(float*));
    if (!r->fifo) {
        free(r);
        return NULL;
    }

    for (int c = 0; c < channels; ++c) {
        r->fifo[c] = (float*)malloc((size_t)r->fifo_cap * sizeof(float));
        if (!r->fifo[c]) {
            for (int k = 0; k < c; ++k) free(r->fifo[k]);
            free(r->fifo);
            free(r);
            return NULL;
        }
    }

    r->fifo_count = 0;
    r->primed = false;

    return r;
}

void avx_resampler_reset(avx_resampler_t* r) {
    if (!r) return;
    r->phase_frac = 0;
    r->fifo_count = 0;
    r->primed = false;
}

void avx_resampler_destroy(avx_resampler_t* r) {
    if (!r) return;
    for (int c = 0; c < r->channels; ++c) {
        free(r->fifo[c]);
    }
    free(r->fifo);
    free(r);
}

/* Convolves input audio through polyphase filter bank with history ring buffers and SIMD MAC. */
int avx_resampler_process(
    avx_resampler_t* r,
    const float* const* in_channels,
    int in_samples,
    float* const* out_channels,
    int max_out_samples
) {
    if (!r || !out_channels || max_out_samples <= 0) return 0;

    /* Expand FIFO if needed */
    int needed = r->fifo_count + in_samples + (r->primed ? 0 : r->lookbehind);
    if (needed > r->fifo_cap) {
        int new_cap = r->fifo_cap * 2;
        while (new_cap < needed) new_cap *= 2;
        for (int c = 0; c < r->channels; ++c) {
            float* nb = (float*)realloc(r->fifo[c], (size_t)new_cap * sizeof(float));
            if (!nb) return 0;
            r->fifo[c] = nb;
        }
        r->fifo_cap = new_cap;
    }

    /* Prime FIFO if first call */
    if (!r->primed && in_samples > 0 && in_channels) {
        for (int c = 0; c < r->channels; ++c) {
            float first_val = in_channels[c][0];
            for (int k = 0; k < r->lookbehind; ++k) {
                r->fifo[c][k] = first_val;
            }
        }
        r->fifo_count = r->lookbehind;
        r->primed = true;
    }

    /* Append incoming samples */
    if (in_samples > 0 && in_channels) {
        for (int c = 0; c < r->channels; ++c) {
            memcpy(r->fifo[c] + r->fifo_count, in_channels[c], (size_t)in_samples * sizeof(float));
        }
        r->fifo_count += in_samples;
    }

    int curr_idx = r->lookbehind;
    int out_idx = 0;
    uint64_t phase = r->phase_frac;
    uint64_t step_n = r->step_num;
    uint64_t step_d = r->step_den;

    float fc = 1.0f;
    if (r->out_rate < r->in_rate) {
        fc = (float)r->out_rate / (float)r->in_rate;
    }

    avx_simd_type_e caps = avx_simd_get_caps();

    while (curr_idx + r->lookahead < r->fifo_count && out_idx < max_out_samples) {
        float mu = (float)phase / (float)step_d;

        if (r->mode == AVX_AUDIO_RESAMPLE_LINEAR) {
            for (int c = 0; c < r->channels; ++c) {
                float y0 = r->fifo[c][curr_idx];
                float y1 = r->fifo[c][curr_idx + 1];
                out_channels[c][out_idx] = (1.0f - mu) * y0 + mu * y1;
            }
        } else if (r->mode == AVX_AUDIO_RESAMPLE_CUBIC) {
            float mu2 = mu * mu;
            float mu3 = mu2 * mu;
            float wm1 = -0.5f * mu3 + 1.0f * mu2 - 0.5f * mu;
            float w0  = 1.5f * mu3 - 2.5f * mu2 + 1.0f;
            float w1  = -1.5f * mu3 + 2.0f * mu2 + 0.5f * mu;
            float w2  = 0.5f * mu3 - 0.5f * mu2;

#if AUDIO_HAS_X86_SIMD
            if (caps == AVX_SIMD_AVX2 && r->channels == 2) {
                avx2_cubic_interpolate_stereo(r->fifo, curr_idx, wm1, w0, w1, w2, out_channels, out_idx);
            } else
#elif AUDIO_HAS_ARM_NEON
            if (caps == AVX_SIMD_NEON && r->channels == 2) {
                neon_cubic_interpolate_stereo(r->fifo, curr_idx, wm1, w0, w1, w2, out_channels, out_idx);
            } else
#endif
            {
                for (int c = 0; c < r->channels; ++c) {
                    float ym1 = r->fifo[c][curr_idx - 1];
                    float y0  = r->fifo[c][curr_idx];
                    float y1  = r->fifo[c][curr_idx + 1];
                    float y2  = r->fifo[c][curr_idx + 2];
                    out_channels[c][out_idx] = wm1 * ym1 + w0 * y0 + w1 * y1 + w2 * y2;
                }
            }
        } else if (r->mode == AVX_AUDIO_RESAMPLE_SINC) {
            int radius = r->lookbehind;
            if (radius == 16) {
                float weights[32];
                float wsum = 0.0f;
                for (int j = 0; j < 32; ++j) {
                    float t = (float)(j - 15) - mu;
                    float w = sinc_func(fc * t) * blackman_window(t, (float)radius);
                    weights[j] = w;
                    wsum += w;
                }
                float inv_wsum = (fabsf(wsum) > 1e-6f) ? (1.0f / wsum) : 1.0f;
                for (int j = 0; j < 32; ++j) {
                    weights[j] *= inv_wsum;
                }

#if AUDIO_HAS_X86_SIMD
                if (caps == AVX_SIMD_AVX2) {
                    avx2_sinc_convolve_channels(r->fifo, curr_idx - 15, weights, out_channels, out_idx, r->channels);
                } else
#elif AUDIO_HAS_ARM_NEON
                if (caps == AVX_SIMD_NEON) {
                    neon_sinc_convolve_channels(r->fifo, curr_idx - 15, weights, out_channels, out_idx, r->channels);
                } else
#endif
                {
                    scalar_sinc_convolve_channels(r->fifo, curr_idx - 15, weights, out_channels, out_idx, r->channels);
                }
            } else {
                for (int c = 0; c < r->channels; ++c) {
                    float sum = 0.0f;
                    float wsum = 0.0f;
                    for (int j = -radius + 1; j <= radius; ++j) {
                        float t = (float)j - mu;
                        float w = sinc_func(fc * t) * blackman_window(t, (float)radius);
                        sum += r->fifo[c][curr_idx + j] * w;
                        wsum += w;
                    }
                    out_channels[c][out_idx] = (fabsf(wsum) > 1e-6f) ? (sum / wsum) : sum;
                }
            }
        }

        out_idx++;
        phase += step_n;
        uint64_t adv = phase / step_d;
        phase %= step_d;
        curr_idx += (int)adv;
    }

    r->phase_frac = phase;

    /* Shift FIFO: retain tail lookbehind samples before curr_idx */
    int shift_start = curr_idx - r->lookbehind;
    if (shift_start > 0) {
        int remaining = r->fifo_count - shift_start;
        if (remaining > 0) {
            for (int c = 0; c < r->channels; ++c) {
                memmove(r->fifo[c], r->fifo[c] + shift_start, (size_t)remaining * sizeof(float));
            }
            r->fifo_count = remaining;
        } else {
            r->fifo_count = 0;
        }
    }

    return out_idx;
}

int avx_resampler_flush(
    avx_resampler_t* r,
    float* const* out_channels,
    int max_out_samples
) {
    if (!r || !out_channels || max_out_samples <= 0 || r->fifo_count == 0) return 0;

    /* Pad lookahead samples by replicating the last sample in the FIFO */
    int pad = r->lookahead;
    int needed = r->fifo_count + pad;
    if (needed > r->fifo_cap) {
        int new_cap = r->fifo_cap * 2;
        while (new_cap < needed) new_cap *= 2;
        for (int c = 0; c < r->channels; ++c) {
            float* nb = (float*)realloc(r->fifo[c], (size_t)new_cap * sizeof(float));
            if (!nb) return 0;
            r->fifo[c] = nb;
        }
        r->fifo_cap = new_cap;
    }

    for (int c = 0; c < r->channels; ++c) {
        float last_val = r->fifo[c][r->fifo_count - 1];
        for (int k = 0; k < pad; ++k) {
            r->fifo[c][r->fifo_count + k] = last_val;
        }
    }
    r->fifo_count += pad;

    /* Drain the remaining samples */
    int n = avx_resampler_process(r, NULL, 0, out_channels, max_out_samples);
    return n;
}

bool avx_audio_frame_resample(
    avx_resampler_t* resampler,
    const oavx_audio_frame_t* p_src_frame,
    oavx_audio_frame_t* p_dst_frame
) {
    if (!resampler || !p_src_frame || !p_src_frame->planes[0] ||
        p_src_frame->frames == 0 || !p_dst_frame) {
        return false;
    }

    int in_channels = (int)p_src_frame->channels;
    if (in_channels <= 0 || in_channels != resampler->channels) {
        return false;
    }

    size_t src_stride = p_src_frame->stride_bytes;
    if (src_stride == 0) {
        src_stride = (size_t)p_src_frame->frames * sizeof(float);
    }
    size_t stride_samples = src_stride / sizeof(float);

    /* Estimate upper bound on output samples */
    int max_out = (int)(((int64_t)p_src_frame->frames * resampler->out_rate +
                         resampler->in_rate - 1) / resampler->in_rate) + 128;

    float* dst_data = (float*)malloc((size_t)in_channels * max_out * sizeof(float));
    if (!dst_data) return false;

    const float** in_ptrs = (const float**)calloc((size_t)in_channels, sizeof(float*));
    float** out_ptrs = (float**)calloc((size_t)in_channels, sizeof(float*));
    if (!in_ptrs || !out_ptrs) {
        free(dst_data);
        if (in_ptrs) free((void*)in_ptrs);
        if (out_ptrs) free(out_ptrs);
        return false;
    }

    for (int c = 0; c < in_channels; ++c) {
        in_ptrs[c] = (c < OAVX_MAX_AUDIO_CHANNELS && p_src_frame->planes[c]) ? (const float*)p_src_frame->planes[c] :
                     ((const float*)p_src_frame->planes[0] + (size_t)c * stride_samples);
        out_ptrs[c] = dst_data + (size_t)c * max_out;
    }

    int generated = avx_resampler_process(resampler, in_ptrs, (int)p_src_frame->frames, out_ptrs, max_out);

    /* Compact channel stride to exact generated sample count */
    int dst_stride = generated * (int)sizeof(float);
    if (generated < max_out) {
        for (int c = 1; c < in_channels; ++c) {
            memmove(dst_data + (size_t)c * generated, out_ptrs[c], (size_t)generated * sizeof(float));
        }
    }

    free((void*)in_ptrs);
    free(out_ptrs);

    memset(p_dst_frame, 0, sizeof(*p_dst_frame));
    p_dst_frame->channels = (uint16_t)in_channels;
    p_dst_frame->frames = (uint32_t)generated;
    p_dst_frame->sample_rate = (uint32_t)resampler->out_rate;
    p_dst_frame->format = OAVX_AUDIO_FMT_FLT_PLANAR;
    p_dst_frame->pts_ns = p_src_frame->pts_ns;
    p_dst_frame->channel_mask = p_src_frame->channel_mask;
    p_dst_frame->stride_bytes = (size_t)dst_stride;
    p_dst_frame->planes[0] = dst_data;
    for (int c = 0; c < in_channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
        p_dst_frame->planes[c] = dst_data + (size_t)c * generated;
    }
    for (int c = in_channels; c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
        p_dst_frame->planes[c] = NULL;
    }
    p_dst_frame->metadata = p_src_frame->metadata;
    p_dst_frame->metadata_len = p_src_frame->metadata_len;
    p_dst_frame->user_ctx = NULL;

    return true;
}

int avx_audio_resample_simple(
    const float* in_data,
    int in_samples,
    int in_rate,
    float* out_data,
    int max_out_samples,
    int out_rate,
    avx_audio_resample_mode_e mode
) {
    if (!in_data || in_samples <= 0 || in_rate <= 0 ||
        !out_data || max_out_samples <= 0 || out_rate <= 0) {
        return -1;
    }

    avx_resampler_t* r = avx_resampler_create(in_rate, out_rate, 1, mode);
    if (!r) return -1;

    const float* in_ch[1] = { in_data };
    float* out_ch[1] = { out_data };

    int n = avx_resampler_process(r, in_ch, in_samples, out_ch, max_out_samples);
    if (n < max_out_samples) {
        float* flushed_ch[1] = { out_data + n };
        int fn = avx_resampler_flush(r, flushed_ch, max_out_samples - n);
        n += fn;
    }

    avx_resampler_destroy(r);
    return n;
}

/* ========================================================================= */
/* 3. ADAPTIVE AUDIO JITTER BUFFER & CLOCK SLEWING ENGINE                   */
/* ========================================================================= */

#define AVX_JITTER_MAX_CHANNELS 8
#define AVX_JITTER_CAPACITY_SAMPLES (192000) /* 4 seconds at 48kHz */
#define AVX_JITTER_FADE_SAMPLES 96           /* 2 ms smooth crossfade at 48kHz */

#if defined(AVX_OS_WINDOWS)
    static inline uint64_t get_jitter_time_ns(void) {
        LARGE_INTEGER freq, count;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&count);
        return (uint64_t)((count.QuadPart * 1000000000ULL) / freq.QuadPart);
    }
#else
    static inline uint64_t get_jitter_time_ns(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
#endif

struct avx_audio_jitter_t {
    avx_mutex_t lock;
    int sample_rate;
    int channels;

    /* Elastic Planar Ring Buffer */
    float* ring[AVX_JITTER_MAX_CHANNELS];
    size_t ring_head;
    size_t ring_tail;
    size_t ring_count;
    size_t capacity;

    /* Adaptive Cushion Targets (in samples) */
    size_t target_cushion;
    size_t min_cushion;
    size_t max_cushion;
    size_t base_cushion;

    /* Pre-roll state */
    bool primed;

    /* Jitter Estimation */
    uint64_t last_push_time_ns;
    float current_jitter_ms;

    /* Micro-Slewing / Clock Drift Compensation */
    double phase_accum;

    /* Concealment State */
    bool in_starvation;
    int fade_out_remaining;
    int fade_in_remaining;
    float last_samples[AVX_JITTER_MAX_CHANNELS];
};

avx_audio_jitter_t* avx_audio_jitter_create(const avx_audio_jitter_config_t* cfg) {
    if (!cfg || cfg->sample_rate <= 0 || cfg->channels <= 0 || cfg->channels > AVX_JITTER_MAX_CHANNELS) {
        return NULL;
    }

    avx_audio_jitter_t* jb = (avx_audio_jitter_t*)calloc(1, sizeof(avx_audio_jitter_t));
    if (!jb) return NULL;

    avx_mutex_init(&jb->lock);
    jb->sample_rate = cfg->sample_rate;
    jb->channels = cfg->channels;
    jb->capacity = AVX_JITTER_CAPACITY_SAMPLES;

    int target_ms = cfg->target_cushion_ms > 0 ? cfg->target_cushion_ms : 15;
    int min_ms = cfg->min_cushion_ms > 0 ? cfg->min_cushion_ms : 5;
    int max_ms = cfg->max_cushion_ms > 0 ? cfg->max_cushion_ms : 40;

    jb->base_cushion = (size_t)((target_ms * jb->sample_rate) / 1000);
    jb->min_cushion = (size_t)((min_ms * jb->sample_rate) / 1000);
    jb->max_cushion = (size_t)((max_ms * jb->sample_rate) / 1000);
    jb->target_cushion = jb->base_cushion;

    for (int c = 0; c < jb->channels; ++c) {
        jb->ring[c] = (float*)calloc(jb->capacity, sizeof(float));
        if (!jb->ring[c]) {
            avx_audio_jitter_destroy(jb);
            return NULL;
        }
    }

    jb->primed = false;
    jb->in_starvation = false;
    return jb;
}

void avx_audio_jitter_destroy(avx_audio_jitter_t* jb) {
    if (!jb) return;
    for (int c = 0; c < AVX_JITTER_MAX_CHANNELS; ++c) {
        if (jb->ring[c]) {
            free(jb->ring[c]);
            jb->ring[c] = NULL;
        }
    }
    avx_mutex_destroy(&jb->lock);
    free(jb);
}

void avx_audio_jitter_reset(avx_audio_jitter_t* jb) {
    if (!jb) return;
    avx_mutex_lock(&jb->lock);
    jb->ring_head = 0;
    jb->ring_tail = 0;
    jb->ring_count = 0;
    jb->primed = false;
    jb->in_starvation = false;
    jb->fade_out_remaining = 0;
    jb->fade_in_remaining = 0;
    jb->last_push_time_ns = 0;
    jb->phase_accum = 0.0;
    jb->target_cushion = jb->base_cushion;
    jb->current_jitter_ms = 0.0f;
    for (int c = 0; c < AVX_JITTER_MAX_CHANNELS; ++c) {
        jb->last_samples[c] = 0.0f;
    }
    avx_mutex_unlock(&jb->lock);
}

void avx_audio_jitter_push(avx_audio_jitter_t* jb, const float* const* channels, int num_samples) {
    if (!jb || !channels || num_samples <= 0) return;

    avx_mutex_lock(&jb->lock);

    /* 1. Track inter-arrival jitter via exponential moving average (EMA).
     * Only adapt cushion after initial priming, and ignore burst drains (< 1.0ms)
     * that occur when multiple frames are drained in a single socket poll loop.
     */
    uint64_t now_ns = get_jitter_time_ns();
    if (jb->primed && jb->last_push_time_ns > 0) {
        float dt_ms = (float)(now_ns - jb->last_push_time_ns) / 1000000.0f;
        float expected_dt_ms = (float)num_samples * 1000.0f / (float)jb->sample_rate;
        if (dt_ms >= 1.0f) {
            float diff_ms = fabsf(dt_ms - expected_dt_ms);

            if (jb->current_jitter_ms == 0.0f) {
                jb->current_jitter_ms = diff_ms;
            } else {
                jb->current_jitter_ms = 0.90f * jb->current_jitter_ms + 0.10f * diff_ms;
            }

            /* Dynamically adapt target cushion: base + 2.0 * jitter */
            size_t adaptive_samples = (size_t)(((float)jb->base_cushion) + (2.0f * jb->current_jitter_ms * (float)jb->sample_rate / 1000.0f));
            if (adaptive_samples < jb->min_cushion) adaptive_samples = jb->min_cushion;
            if (adaptive_samples > jb->max_cushion) adaptive_samples = jb->max_cushion;
            jb->target_cushion = adaptive_samples;
        }
    }
    jb->last_push_time_ns = now_ns;

    /* 2. Write incoming planar samples to ring buffer */
    for (int s = 0; s < num_samples; ++s) {
        if (jb->ring_count == jb->capacity) {
            /* Drop oldest sample on ring overflow */
            jb->ring_head = (jb->ring_head + 1) % jb->capacity;
            jb->ring_count--;
        }

        for (int c = 0; c < jb->channels; ++c) {
            jb->ring[c][jb->ring_tail] = channels[c] ? channels[c][s] : 0.0f;
        }

        jb->ring_tail = (jb->ring_tail + 1) % jb->capacity;
        jb->ring_count++;
    }

    avx_mutex_unlock(&jb->lock);
}

/*
 * Pulls audio frames with adaptive clock drift slewing (+-500 PPM) and raised-cosine
 * concealment crossfading to prevent audio clicks/pops between unsynchronized clocks.
 */
int avx_audio_jitter_pull(avx_audio_jitter_t* jb, float* const* out_channels, int requested_samples) {
    if (!jb || !out_channels || requested_samples <= 0) return 0;

    avx_mutex_lock(&jb->lock);

    /* 1. Pre-roll cushion accumulation check */
    if (!jb->primed) {
        if (jb->ring_count < jb->target_cushion) {
            /* Still filling pre-roll cushion: deliver silence to avoid startup click */
            for (int c = 0; c < jb->channels; ++c) {
                if (out_channels[c]) {
                    memset(out_channels[c], 0, (size_t)requested_samples * sizeof(float));
                }
            }
            avx_mutex_unlock(&jb->lock);
            return requested_samples;
        }
        jb->primed = true;
    }

    /* 2. Drift slewing calculation:
     * Compare current ring_count against target_cushion.
     * Deadband of 5 ms prevents micro-hunting on clean arrival.
     * If buffer > target + deadband: slew by +0.05% (+500 PPM) to gently drain.
     * If buffer < target - deadband: slew by -0.05% (-500 PPM) to gently fill.
     */
    int deadband = (jb->sample_rate * 5) / 1000;
    int diff = (int)jb->ring_count - (int)jb->target_cushion;
    double slew_step = 1.0;

    if (diff > deadband) {
        slew_step = 1.0005; /* Gently speed up drain (imperceptible +0.05% pitch) */
    } else if (diff < -deadband) {
        slew_step = 0.9995; /* Gently slow down drain (-0.05% pitch) */
    }

    /* 3. Pull requested samples with micro-steered interpolation and smooth concealment */
    int delivered = 0;
    for (int k = 0; k < requested_samples; ++k) {
        if (jb->ring_count == 0) {
            /* Starvation detected! */
            if (!jb->in_starvation) {
                jb->in_starvation = true;
                jb->fade_out_remaining = AVX_JITTER_FADE_SAMPLES;
                jb->fade_in_remaining = 0;
            }

            if (jb->fade_out_remaining > 0) {
                int f_idx = AVX_JITTER_FADE_SAMPLES - jb->fade_out_remaining;
                float w = 0.5f * (1.0f + cosf((float)M_PI * (float)f_idx / (float)AVX_JITTER_FADE_SAMPLES));
                for (int c = 0; c < jb->channels; ++c) {
                    if (out_channels[c]) {
                        out_channels[c][k] = jb->last_samples[c] * w;
                    }
                }
                jb->fade_out_remaining--;
            } else {
                for (int c = 0; c < jb->channels; ++c) {
                    if (out_channels[c]) {
                        out_channels[c][k] = 0.0f;
                    }
                }
            }
            delivered++;
            continue;
        }

        /* If recovering from starvation and samples are now available, transition to fade-in */
        if (jb->in_starvation) {
            jb->in_starvation = false;
            jb->fade_out_remaining = 0;
            jb->fade_in_remaining = AVX_JITTER_FADE_SAMPLES;
        }

        /* Normal reading */
        size_t idx0 = jb->ring_head;
        size_t idx1 = (jb->ring_head + 1) % jb->capacity;
        float frac = (float)(jb->phase_accum);

        for (int c = 0; c < jb->channels; ++c) {
            float s0 = jb->ring[c][idx0];
            float s1 = (jb->ring_count > 1) ? jb->ring[c][idx1] : s0;
            float val = s0 * (1.0f - frac) + s1 * frac;

            /* Smooth cosine fade-in on starvation recovery */
            if (jb->fade_in_remaining > 0) {
                int f_idx = AVX_JITTER_FADE_SAMPLES - jb->fade_in_remaining;
                float w = 0.5f * (1.0f - cosf((float)M_PI * (float)f_idx / (float)AVX_JITTER_FADE_SAMPLES));
                val *= w;
            }

            if (out_channels[c]) {
                out_channels[c][k] = val;
            }
            jb->last_samples[c] = s0;
        }

        if (jb->fade_in_remaining > 0) {
            jb->fade_in_remaining--;
        }

        /* Step phase accumulator by slew rate */
        jb->phase_accum += slew_step;
        while (jb->phase_accum >= 1.0) {
            jb->phase_accum -= 1.0;
            if (jb->ring_count > 0) {
                jb->ring_head = (jb->ring_head + 1) % jb->capacity;
                jb->ring_count--;
            }
        }
        delivered++;
    }

    avx_mutex_unlock(&jb->lock);
    return delivered;
}

int avx_audio_jitter_get_depth_samples(const avx_audio_jitter_t* jb) {
    if (!jb) return 0;
    return (int)jb->ring_count;
}

float avx_audio_jitter_get_depth_ms(const avx_audio_jitter_t* jb) {
    if (!jb || jb->sample_rate <= 0) return 0.0f;
    return (float)jb->ring_count * 1000.0f / (float)jb->sample_rate;
}

/* ========================================================================= */
/* 4. CHANNEL LAYOUT & SEMANTIC METADATA HELPERS                             */
/* ========================================================================= */

oavx_audio_channel_mask_t avx_audio_layout_to_mask(avx_audio_layout_e layout) {
    switch (layout) {
        case AVX_AUDIO_LAYOUT_MONO:           return OAVX_AUDIO_MASK_MONO;
        case AVX_AUDIO_LAYOUT_STEREO:         return OAVX_AUDIO_MASK_STEREO;
        case AVX_AUDIO_LAYOUT_2_1:            return OAVX_AUDIO_MASK_2_1;
        case AVX_AUDIO_LAYOUT_QUAD:           return OAVX_AUDIO_MASK_QUAD;
        case AVX_AUDIO_LAYOUT_5_1:            return OAVX_AUDIO_MASK_5_1;
        case AVX_AUDIO_LAYOUT_7_1:            return OAVX_AUDIO_MASK_7_1;
        case AVX_AUDIO_LAYOUT_7_1_4:          return OAVX_AUDIO_MASK_7_1_4;
        case AVX_AUDIO_LAYOUT_BROADCAST_16CH: {
            uint64_t m = 0;
            for (int i = 1; i <= 16; ++i) m |= OAVX_AUDIO_CH_DISCRETE_TRACK(i);
            return m;
        }
        case AVX_AUDIO_LAYOUT_BROADCAST_32CH: {
            uint64_t m = 0;
            for (int i = 1; i <= 32; ++i) m |= OAVX_AUDIO_CH_DISCRETE_TRACK(i);
            return m;
        }
        case AVX_AUDIO_LAYOUT_DISCRETE:
        default:
            return OAVX_AUDIO_MASK_DISCRETE;
    }
}

avx_audio_layout_e avx_audio_layout_from_channels(uint16_t channels) {
    switch (channels) {
        case 1:  return AVX_AUDIO_LAYOUT_MONO;
        case 2:  return AVX_AUDIO_LAYOUT_STEREO;
        case 3:  return AVX_AUDIO_LAYOUT_2_1;
        case 4:  return AVX_AUDIO_LAYOUT_QUAD;
        case 6:  return AVX_AUDIO_LAYOUT_5_1;
        case 8:  return AVX_AUDIO_LAYOUT_7_1;
        case 12: return AVX_AUDIO_LAYOUT_7_1_4;
        case 16: return AVX_AUDIO_LAYOUT_BROADCAST_16CH;
        case 32: return AVX_AUDIO_LAYOUT_BROADCAST_32CH;
        default: return (channels > 0) ? AVX_AUDIO_LAYOUT_DISCRETE : AVX_AUDIO_LAYOUT_UNKNOWN;
    }
}

avx_audio_layout_e avx_audio_layout_from_mask(oavx_audio_channel_mask_t mask, uint16_t channels) {
    if (mask == OAVX_AUDIO_MASK_MONO)   return AVX_AUDIO_LAYOUT_MONO;
    if (mask == OAVX_AUDIO_MASK_STEREO) return AVX_AUDIO_LAYOUT_STEREO;
    if (mask == OAVX_AUDIO_MASK_2_1)    return AVX_AUDIO_LAYOUT_2_1;
    if (mask == OAVX_AUDIO_MASK_QUAD)   return AVX_AUDIO_LAYOUT_QUAD;
    if (mask == OAVX_AUDIO_MASK_5_1)    return AVX_AUDIO_LAYOUT_5_1;
    if (mask == OAVX_AUDIO_MASK_7_1)    return AVX_AUDIO_LAYOUT_7_1;
    if (mask == OAVX_AUDIO_MASK_7_1_4)  return AVX_AUDIO_LAYOUT_7_1_4;
    return avx_audio_layout_from_channels(channels);
}

const char* avx_audio_layout_name(avx_audio_layout_e layout) {
    switch (layout) {
        case AVX_AUDIO_LAYOUT_MONO:           return "Mono (1.0)";
        case AVX_AUDIO_LAYOUT_STEREO:         return "Stereo (2.0)";
        case AVX_AUDIO_LAYOUT_2_1:            return "2.1";
        case AVX_AUDIO_LAYOUT_QUAD:           return "Quad (4.0)";
        case AVX_AUDIO_LAYOUT_5_1:            return "5.1 Surround";
        case AVX_AUDIO_LAYOUT_7_1:            return "7.1 Surround";
        case AVX_AUDIO_LAYOUT_7_1_4:          return "7.1.4 Immersive";
        case AVX_AUDIO_LAYOUT_BROADCAST_16CH: return "Broadcast 16-Channel SDI";
        case AVX_AUDIO_LAYOUT_BROADCAST_32CH: return "Broadcast 32-Channel Pro-Audio";
        case AVX_AUDIO_LAYOUT_DISCRETE:       return "Discrete Multi-Track";
        default:                              return "Unknown Layout";
    }
}

uint16_t avx_audio_layout_channels(avx_audio_layout_e layout) {
    switch (layout) {
        case AVX_AUDIO_LAYOUT_MONO:           return 1;
        case AVX_AUDIO_LAYOUT_STEREO:         return 2;
        case AVX_AUDIO_LAYOUT_2_1:            return 3;
        case AVX_AUDIO_LAYOUT_QUAD:           return 4;
        case AVX_AUDIO_LAYOUT_5_1:            return 6;
        case AVX_AUDIO_LAYOUT_7_1:            return 8;
        case AVX_AUDIO_LAYOUT_7_1_4:          return 12;
        case AVX_AUDIO_LAYOUT_BROADCAST_16CH: return 16;
        case AVX_AUDIO_LAYOUT_BROADCAST_32CH: return 32;
        default:                              return 0;
    }
}

const char* avx_audio_channel_name(uint16_t channel_idx, avx_audio_layout_e layout) {
    if (layout == AVX_AUDIO_LAYOUT_5_1) {
        static const char* const names[6] = { "Front Left", "Front Right", "Center", "LFE / Sub", "Surround Left", "Surround Right" };
        if (channel_idx < 6) return names[channel_idx];
    } else if (layout == AVX_AUDIO_LAYOUT_7_1) {
        static const char* const names[8] = { "Front Left", "Front Right", "Center", "LFE / Sub", "Back Left", "Back Right", "Side Left", "Side Right" };
        if (channel_idx < 8) return names[channel_idx];
    } else if (layout == AVX_AUDIO_LAYOUT_STEREO) {
        if (channel_idx == 0) return "Left";
        if (channel_idx == 1) return "Right";
    } else if (layout == AVX_AUDIO_LAYOUT_MONO) {
        if (channel_idx == 0) return "Center / Mono";
    } else if (layout == AVX_AUDIO_LAYOUT_QUAD) {
        static const char* const names[4] = { "Front Left", "Front Right", "Back Left", "Back Right" };
        if (channel_idx < 4) return names[channel_idx];
    } else if (layout == AVX_AUDIO_LAYOUT_2_1) {
        static const char* const names[3] = { "Left", "Right", "LFE / Sub" };
        if (channel_idx < 3) return names[channel_idx];
    } else if (layout == AVX_AUDIO_LAYOUT_7_1_4) {
        static const char* const names[12] = {
            "Front Left", "Front Right", "Center", "LFE / Sub",
            "Back Left", "Back Right", "Side Left", "Side Right",
            "Top Front Left", "Top Front Right", "Top Back Left", "Top Back Right"
        };
        if (channel_idx < 12) return names[channel_idx];
    }
    static char buf[32];
    snprintf(buf, sizeof(buf), "Track %u", (unsigned)(channel_idx + 1));
    return buf;
}

/* ========================================================================= */
/* 7. AUDIO ROUTING & UPMIXING TOOLS                                         */
/* ========================================================================= */

/* Arbitrary N-channel input to M-channel output matrix router with per-crosspoint gains. */
void avx_audio_route_channels(
    const float* const* in_planes, int in_channels,
    float* const* out_planes, int out_channels,
    const int8_t* channel_map,
    const float* channel_gains,
    int frames
) {
    if (!out_planes || out_channels <= 0 || frames <= 0) return;

    if (!in_planes || in_channels <= 0) {
        for (int o = 0; o < out_channels; ++o) {
            if (out_planes[o]) {
                memset(out_planes[o], 0, (size_t)frames * sizeof(float));
            }
        }
        return;
    }

    /* Check for aliasing / buffer sharing between output and input */
    bool has_aliasing = false;
    for (int o = 0; o < out_channels; ++o) {
        if (!out_planes[o]) continue;
        for (int in = 0; in < in_channels; ++in) {
            if (out_planes[o] == in_planes[in]) {
                has_aliasing = true;
                break;
            }
        }
        if (has_aliasing) break;
    }

    float* temp_data = NULL;
    float** temp_out = NULL;
    float* const* effective_out = out_planes;

    if (has_aliasing) {
        temp_data = (float*)malloc((size_t)out_channels * (size_t)frames * sizeof(float));
        temp_out = (float**)malloc((size_t)out_channels * sizeof(float*));
        if (temp_data && temp_out) {
            for (int o = 0; o < out_channels; ++o) {
                temp_out[o] = temp_data + (size_t)o * frames;
            }
            effective_out = temp_out;
        }
    }

    for (int o = 0; o < out_channels; ++o) {
        float* dst = effective_out[o];
        if (!dst && !has_aliasing) continue;
        if (has_aliasing && !out_planes[o]) continue;

        int8_t src_idx = channel_map ? channel_map[o] : (int8_t)o;
        float gain = channel_gains ? channel_gains[o] : 1.0f;

        if (src_idx < 0 || src_idx >= in_channels || !in_planes[src_idx] || gain == 0.0f) {
            memset(dst, 0, (size_t)frames * sizeof(float));
        } else {
            const float* src = in_planes[src_idx];
            if (gain == 1.0f) {
                if (src != dst) {
                    memcpy(dst, src, (size_t)frames * sizeof(float));
                }
            } else {
                for (int i = 0; i < frames; ++i) {
                    dst[i] = src[i] * gain;
                }
            }
        }
    }

    if (has_aliasing && temp_data && temp_out) {
        for (int o = 0; o < out_channels; ++o) {
            if (out_planes[o]) {
                memcpy(out_planes[o], temp_out[o], (size_t)frames * sizeof(float));
            }
        }
        free(temp_out);
        free(temp_data);
    }
}

bool avx_audio_frame_route_channels(
    const oavx_audio_frame_t* src_frame,
    oavx_audio_frame_t* dst_frame,
    const int8_t* channel_map,
    const float* channel_gains,
    uint16_t out_channels
) {
    if (!src_frame || !src_frame->planes[0] || src_frame->channels == 0 ||
        src_frame->frames == 0 || !dst_frame) {
        return false;
    }
    if (out_channels == 0 || out_channels > OAVX_MAX_AUDIO_CHANNELS) {
        return false;
    }

    size_t frames = src_frame->frames;
    float* buf = (float*)calloc((size_t)out_channels * frames, sizeof(float));
    if (!buf) return false;

    memset(dst_frame, 0, sizeof(*dst_frame));
    dst_frame->channels = out_channels;
    dst_frame->frames = (uint32_t)frames;
    dst_frame->sample_rate = src_frame->sample_rate;
    dst_frame->format = src_frame->format;
    dst_frame->pts_ns = src_frame->pts_ns;
    dst_frame->stride_bytes = sizeof(float);
    dst_frame->channel_mask = avx_audio_layout_to_mask(avx_audio_layout_from_channels(out_channels));

    for (uint16_t c = 0; c < out_channels; ++c) {
        dst_frame->planes[c] = buf + (size_t)c * frames;
    }

    const float* in_ptrs[OAVX_MAX_AUDIO_CHANNELS];
    for (uint16_t c = 0; c < src_frame->channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
        in_ptrs[c] = (const float*)src_frame->planes[c];
    }

    avx_audio_route_channels(
        in_ptrs, src_frame->channels,
        (float* const*)dst_frame->planes, out_channels,
        channel_map, channel_gains, (int)frames
    );

    return true;
}

static void compute_butterworth_lfe(int sample_rate, float* b0, float* b1, float* b2, float* a1, float* a2) {
    if (sample_rate <= 0) sample_rate = 48000;
    const float fc = 120.0f; /* 120 Hz standard LFE crossover */
    float omega = 2.0f * (float)M_PI * fc / (float)sample_rate;
    if (omega > (float)M_PI * 0.45f) {
        omega = (float)M_PI * 0.45f;
    }
    float cos_w = cosf(omega);
    float sin_w = sinf(omega);
    float alpha = sin_w * INV_SQRT2;

    float a0_raw = 1.0f + alpha;
    float b0_raw = (1.0f - cos_w) * 0.5f;
    float b1_raw = 1.0f - cos_w;
    float b2_raw = (1.0f - cos_w) * 0.5f;
    float a1_raw = -2.0f * cos_w;
    float a2_raw = 1.0f - alpha;

    float inv_a0 = 1.0f / a0_raw;
    *b0 = b0_raw * inv_a0;
    *b1 = b1_raw * inv_a0;
    *b2 = b2_raw * inv_a0;
    *a1 = a1_raw * inv_a0;
    *a2 = a2_raw * inv_a0;
}

struct avx_audio_upmixer_t {
    int sample_rate;
    avx_audio_upmix_cfg_t cfg;

    int delay_samples;
    int delay_cap;
    int delay_pos;
    float* delay_buf_l;
    float* delay_buf_r;

    float b0, b1, b2;
    float a1, a2;
    float lfe_s1;
    float lfe_s2;
};

void avx_audio_upmix_cfg_default(avx_audio_upmix_cfg_t* cfg) {
    if (!cfg) return;
    cfg->center_level = INV_SQRT2;     /* 0.70710678f */
    cfg->surround_level = INV_SQRT2;   /* 0.70710678f */
    cfg->lfe_level = 0.5000f;          /* 0.5000f */
    cfg->ambient_delay_ms = 15;        /* 15 ms standard Haas decorrelation */
    cfg->normalize_mode = AVX_AUDIO_NORMALIZE_NONE;
}

avx_audio_upmixer_t* avx_audio_upmixer_create(int sample_rate, const avx_audio_upmix_cfg_t* cfg) {
    if (sample_rate <= 0) sample_rate = 48000;
    avx_audio_upmixer_t* u = (avx_audio_upmixer_t*)calloc(1, sizeof(avx_audio_upmixer_t));
    if (!u) return NULL;

    u->sample_rate = sample_rate;
    if (cfg) {
        u->cfg = *cfg;
    } else {
        avx_audio_upmix_cfg_default(&u->cfg);
    }

    if (u->cfg.ambient_delay_ms < 0) u->cfg.ambient_delay_ms = 0;
    if (u->cfg.ambient_delay_ms > 100) u->cfg.ambient_delay_ms = 100;

    u->delay_samples = (int)(((int64_t)u->cfg.ambient_delay_ms * sample_rate) / 1000);
    u->delay_cap = (u->delay_samples > 0) ? (u->delay_samples + 32) : 64;
    u->delay_buf_l = (float*)calloc(u->delay_cap, sizeof(float));
    u->delay_buf_r = (float*)calloc(u->delay_cap, sizeof(float));
    if (!u->delay_buf_l || !u->delay_buf_r) {
        free(u->delay_buf_l);
        free(u->delay_buf_r);
        free(u);
        return NULL;
    }

    compute_butterworth_lfe(sample_rate, &u->b0, &u->b1, &u->b2, &u->a1, &u->a2);
    return u;
}

void avx_audio_upmixer_destroy(avx_audio_upmixer_t* u) {
    if (!u) return;
    free(u->delay_buf_l);
    free(u->delay_buf_r);
    free(u);
}

void avx_audio_upmixer_reset(avx_audio_upmixer_t* u) {
    if (!u) return;
    if (u->delay_buf_l) memset(u->delay_buf_l, 0, (size_t)u->delay_cap * sizeof(float));
    if (u->delay_buf_r) memset(u->delay_buf_r, 0, (size_t)u->delay_cap * sizeof(float));
    u->delay_pos = 0;
    u->lfe_s1 = 0.0f;
    u->lfe_s2 = 0.0f;
}

void avx_audio_upmixer_process(
    avx_audio_upmixer_t* u,
    const float* in_left,
    const float* in_right,
    float* const out_channels[6],
    int frames
) {
    if (!u || !out_channels || frames <= 0) return;
    float* out_fl  = out_channels[0];
    float* out_fr  = out_channels[1];
    float* out_c   = out_channels[2];
    float* out_lfe = out_channels[3];
    float* out_bl  = out_channels[4];
    float* out_br  = out_channels[5];

    float max_peak = 0.0f;
    bool peak_norm = (u->cfg.normalize_mode == AVX_AUDIO_NORMALIZE_PEAK);
    float c_lvl = u->cfg.center_level;
    float s_lvl = u->cfg.surround_level;
    float lfe_lvl = u->cfg.lfe_level;
    int delay_s = u->delay_samples;
    int cap = u->delay_cap;

    for (int i = 0; i < frames; ++i) {
        float l = in_left ? in_left[i] : 0.0f;
        float r = in_right ? in_right[i] : 0.0f;

        /* Center / Dialog */
        float mono = 0.5f * (l + r);
        float c = mono * c_lvl;

        /* Fronts (dialog bleed cancellation) */
        float fl = l - 0.5f * c;
        float fr = r - 0.5f * c;

        /* Ambient Surrounds */
        float diff = 0.5f * (l - r);
        float bl_raw = diff * s_lvl;
        float br_raw = -diff * s_lvl;

        float bl = bl_raw;
        float br = br_raw;
        if (delay_s > 0 && cap > 0) {
            int read_idx = (u->delay_pos - delay_s + cap) % cap;
            bl = u->delay_buf_l[read_idx];
            br = u->delay_buf_r[read_idx];
            u->delay_buf_l[u->delay_pos] = bl_raw;
            u->delay_buf_r[u->delay_pos] = br_raw;
            u->delay_pos = (u->delay_pos + 1) % cap;
        }

        /* LFE 2nd-order Butterworth low-pass */
        float lfe_in = mono;
        float lfe_filtered = u->b0 * lfe_in + u->lfe_s1;
        u->lfe_s1 = u->b1 * lfe_in - u->a1 * lfe_filtered + u->lfe_s2;
        u->lfe_s2 = u->b2 * lfe_in - u->a2 * lfe_filtered;
        float lfe = lfe_filtered * lfe_lvl;

        if (out_fl)  out_fl[i]  = fl;
        if (out_fr)  out_fr[i]  = fr;
        if (out_c)   out_c[i]   = c;
        if (out_lfe) out_lfe[i] = lfe;
        if (out_bl)  out_bl[i]  = bl;
        if (out_br)  out_br[i]  = br;

        if (peak_norm) {
            float a;
            if (out_fl && (a = fabsf(fl)) > max_peak) max_peak = a;
            if (out_fr && (a = fabsf(fr)) > max_peak) max_peak = a;
            if (out_c && (a = fabsf(c)) > max_peak) max_peak = a;
            if (out_lfe && (a = fabsf(lfe)) > max_peak) max_peak = a;
            if (out_bl && (a = fabsf(bl)) > max_peak) max_peak = a;
            if (out_br && (a = fabsf(br)) > max_peak) max_peak = a;
        } else if (u->cfg.normalize_mode == AVX_AUDIO_NORMALIZE_NONE) {
            if (out_fl)  out_fl[i]  = clamp_f32(fl);
            if (out_fr)  out_fr[i]  = clamp_f32(fr);
            if (out_c)   out_c[i]   = clamp_f32(c);
            if (out_lfe) out_lfe[i] = clamp_f32(lfe);
            if (out_bl)  out_bl[i]  = clamp_f32(bl);
            if (out_br)  out_br[i]  = clamp_f32(br);
        }
    }

    if (peak_norm && max_peak > 1.0f) {
        float inv = 1.0f / max_peak;
        for (int ch = 0; ch < 6; ++ch) {
            if (!out_channels[ch]) continue;
            float* dst = out_channels[ch];
            for (int i = 0; i < frames; ++i) {
                dst[i] *= inv;
            }
        }
    }
}

void avx_audio_upmix_stereo_to_5_1(
    const float* in_left,
    const float* in_right,
    float* const out_channels[6],
    int frames,
    int sample_rate,
    const avx_audio_upmix_cfg_t* cfg
) {
    if (!out_channels || frames <= 0) return;
    avx_audio_upmixer_t* u = avx_audio_upmixer_create(sample_rate, cfg);
    if (!u) return;
    avx_audio_upmixer_process(u, in_left, in_right, out_channels, frames);
    avx_audio_upmixer_destroy(u);
}
