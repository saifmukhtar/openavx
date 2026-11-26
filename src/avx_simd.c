/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_simd.c - SIMD kernels (AVX2, ARM NEON, and scalar fallbacks)
 */

#include "avx_simd.h"
#include "avx_platform.h"
#include <string.h>

#if !defined(AVX_OS_WINDOWS) || defined(__MINGW32__) || defined(__MINGW64__)
#include <pthread.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define HAS_X86_SIMD 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#include <arm_neon.h>
#define HAS_ARM_NEON 1
#endif

/* ========================================================================= */
/* 1. SCALAR REFERENCE IMPLEMENTATIONS (Portable C99 Fallbacks)              */
/* ========================================================================= */

static inline uint8_t clamp_u8_scalar(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

typedef struct {
    /* YUV -> RGB (256-scaled, limited range Y-16, U-128, V-128) */
    int cy;
    int crv;
    int cgu;
    int cgv;
    int cbu;
    /* RGB -> YUV (256-scaled, limited range +16 Y, +128 UV) */
    int yr, yg, yb;
    int ur, ug, ub;
    int vr, vg, vb;
} avx_yuv_matrix_t;

static const avx_yuv_matrix_t g_yuv_bt601 = {
    298, 409, 100, 208, 516,
    66, 129, 25,
    38, 74, 112,
    112, 94, 18
};

static const avx_yuv_matrix_t g_yuv_bt709 = {
    298, 459, 55, 136, 541,
    47, 157, 16,
    26, 86, 112,
    112, 102, 10
};

static inline const avx_yuv_matrix_t* select_yuv_matrix(int width, int height) {
    return (width >= 1280 || height >= 720) ? &g_yuv_bt709 : &g_yuv_bt601;
}

static void scalar_uyvy_to_rgbx_matrix(const uint8_t *__restrict src, uint8_t *__restrict dst,
                                       int width, int height,
                                       int r_off, int b_off,
                                       const avx_yuv_matrix_t *m) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    size_t total_pixels = (size_t)width * height;
    size_t num_pairs = total_pixels / 2;
    const uint8_t *__restrict s = src;
    uint8_t *__restrict d = dst;
    const int cy = m->cy, crv = m->crv, cgu = m->cgu, cgv = m->cgv, cbu = m->cbu;

    for (size_t i = 0; i < num_pairs; ++i) {
        int u  = s[0];
        int y0 = s[1];
        int v  = s[2];
        int y1 = s[3];
        s += 4;

        int c0 = y0 - 16;
        int c1 = y1 - 16;
        int d_u = u - 128;
        int e_v = v - 128;

        int r0 = (cy * c0 + crv * e_v + 128) >> 8;
        int g0 = (cy * c0 - cgu * d_u - cgv * e_v + 128) >> 8;
        int b0 = (cy * c0 + cbu * d_u + 128) >> 8;

        int r1 = (cy * c1 + crv * e_v + 128) >> 8;
        int g1 = (cy * c1 - cgu * d_u - cgv * e_v + 128) >> 8;
        int b1 = (cy * c1 + cbu * d_u + 128) >> 8;

        d[r_off] = clamp_u8_scalar(r0);
        d[1]     = clamp_u8_scalar(g0);
        d[b_off] = clamp_u8_scalar(b0);
        d[3]     = 255;

        d[4 + r_off] = clamp_u8_scalar(r1);
        d[5]         = clamp_u8_scalar(g1);
        d[4 + b_off] = clamp_u8_scalar(b1);
        d[7]         = 255;

        d += 8;
    }

    if (total_pixels % 2 != 0) {
        int u  = s[0];
        int y0 = s[1];
        /* A w*h*2 UYVY buffer holds only [U, Y0] for the trailing odd pixel; avoid reading s[2] OOB. */
        int v  = 128;

        int c0 = y0 - 16;
        int d_u = u - 128;
        int e_v = v - 128;

        int r0 = (cy * c0 + crv * e_v + 128) >> 8;
        int g0 = (cy * c0 - cgu * d_u - cgv * e_v + 128) >> 8;
        int b0 = (cy * c0 + cbu * d_u + 128) >> 8;

        d[r_off] = clamp_u8_scalar(r0);
        d[1]     = clamp_u8_scalar(g0);
        d[b_off] = clamp_u8_scalar(b0);
        d[3]     = 255;
    }
}

void avx_scalar_uyvy_to_rgba(const uint8_t *src, uint8_t *dst, int width, int height) {
    scalar_uyvy_to_rgbx_matrix(src, dst, width, height, 0, 2, select_yuv_matrix(width, height));
}

void avx_scalar_uyvy_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height) {
    scalar_uyvy_to_rgbx_matrix(src, dst, width, height, 2, 0, select_yuv_matrix(width, height));
}

static void scalar_rgbx_to_uyvy_matrix(const uint8_t *__restrict src, uint8_t *__restrict dst,
                                       int width, int height,
                                       int r_off, int b_off,
                                       const avx_yuv_matrix_t *m) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    size_t total_pixels = (size_t)width * height;
    size_t num_pairs = total_pixels / 2;
    const uint8_t *__restrict s = src;
    uint8_t *__restrict d = dst;
    const int yr = m->yr, yg = m->yg, yb = m->yb;
    const int ur = m->ur, ug = m->ug, ub = m->ub;
    const int vr = m->vr, vg = m->vg, vb = m->vb;

    for (size_t i = 0; i < num_pairs; ++i) {
        int r0 = s[r_off];
        int g0 = s[1];
        int b0 = s[b_off];

        int r1 = s[4 + r_off];
        int g1 = s[5];
        int b1 = s[4 + b_off];
        s += 8;

        int y0 = ((yr * r0 + yg * g0 + yb * b0 + 128) >> 8) + 16;
        int y1 = ((yr * r1 + yg * g1 + yb * b1 + 128) >> 8) + 16;

        int r_avg = (r0 + r1) >> 1;
        int g_avg = (g0 + g1) >> 1;
        int b_avg = (b0 + b1) >> 1;

        int u = ((-ur * r_avg - ug * g_avg + ub * b_avg + 128) >> 8) + 128;
        int v = ((vr * r_avg - vg * g_avg - vb * b_avg + 128) >> 8) + 128;

        d[0] = clamp_u8_scalar(u);
        d[1] = clamp_u8_scalar(y0);
        d[2] = clamp_u8_scalar(v);
        d[3] = clamp_u8_scalar(y1);
        d += 4;
    }

    if (total_pixels % 2 != 0) {
        int r0 = s[r_off];
        int g0 = s[1];
        int b0 = s[b_off];

        int y0 = ((yr * r0 + yg * g0 + yb * b0 + 128) >> 8) + 16;
        int u  = ((-ur * r0 - ug * g0 + ub * b0 + 128) >> 8) + 128;

        /* Write only [U, Y0] within the caller's w*h*2 byte contract. */
        d[0] = clamp_u8_scalar(u);
        d[1] = clamp_u8_scalar(y0);
    }
}

void avx_scalar_rgba_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height) {
    scalar_rgbx_to_uyvy_matrix(src, dst, width, height, 0, 2, select_yuv_matrix(width, height));
}

void avx_scalar_bgra_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height) {
    scalar_rgbx_to_uyvy_matrix(src, dst, width, height, 2, 0, select_yuv_matrix(width, height));
}

void avx_scalar_rgba_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    size_t count = (size_t)width * height;

    for (size_t i = 0; i < count; ++i) {
        uint8_t r = src[i * 4 + 0];
        uint8_t g = src[i * 4 + 1];
        uint8_t b = src[i * 4 + 2];
        uint8_t a = src[i * 4 + 3];

        dst[i * 4 + 0] = b;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = r;
        dst[i * 4 + 3] = a;
    }
}

void avx_scalar_alpha_blend_rgba(const uint8_t *fg, const uint8_t *bg, uint8_t *out, int width, int height) {
    if (!fg || !bg || !out || width <= 0 || height <= 0) return;
    size_t count = (size_t)width * height;

    for (size_t i = 0; i < count; ++i) {
        size_t idx = i * 4;
        uint32_t a = fg[idx + 3];

        if (a == 255) {
            out[idx + 0] = fg[idx + 0];
            out[idx + 1] = fg[idx + 1];
            out[idx + 2] = fg[idx + 2];
            out[idx + 3] = 255;
        } else if (a == 0) {
            out[idx + 0] = bg[idx + 0];
            out[idx + 1] = bg[idx + 1];
            out[idx + 2] = bg[idx + 2];
            out[idx + 3] = bg[idx + 3];
        } else {
            uint32_t inv_a = 255 - a;
            out[idx + 0] = (uint8_t)((fg[idx + 0] * a + bg[idx + 0] * inv_a) / 255);
            out[idx + 1] = (uint8_t)((fg[idx + 1] * a + bg[idx + 1] * inv_a) / 255);
            out[idx + 2] = (uint8_t)((fg[idx + 2] * a + bg[idx + 2] * inv_a) / 255);
            out[idx + 3] = 255;
        }
    }
}

void avx_scalar_fdct_8x8(const int16_t in[64], int16_t out[64]) {
    int32_t tmp[64];
    for (int r = 0; r < 8; ++r) {
        const int16_t *row = &in[r * 8];
        int32_t *t = &tmp[r * 8];

        int32_t a0 = row[0] + row[7];
        int32_t a1 = row[1] + row[6];
        int32_t a2 = row[2] + row[5];
        int32_t a3 = row[3] + row[4];

        int32_t s0 = row[0] - row[7];
        int32_t s1 = row[1] - row[6];
        int32_t s2 = row[2] - row[5];
        int32_t s3 = row[3] - row[4];

        int32_t b0 = a0 + a3;
        int32_t b1 = a1 + a2;
        int32_t b2 = a1 - a2;
        int32_t b3 = a0 - a3;

        t[0] = (int32_t)((uint32_t)(b0 + b1) << 3);
        t[4] = (int32_t)((uint32_t)(b0 - b1) << 3);
        t[2] = (b3 * 181 + b2 * 181) >> 5;
        t[6] = (b3 * 181 - b2 * 181) >> 5;

        t[1] = (s0 * 251 + s1 * 213 + s2 * 142 + s3 * 49) >> 5;
        t[3] = (s0 * 213 - s1 * 49  - s2 * 251 - s3 * 142) >> 5;
        t[5] = (s0 * 142 - s1 * 251 + s2 * 49  + s3 * 213) >> 5;
        t[7] = (s0 * 49  - s1 * 142 + s2 * 213 - s3 * 251) >> 5;
    }

    for (int c = 0; c < 8; ++c) {
        int32_t a0 = tmp[0 * 8 + c] + tmp[7 * 8 + c];
        int32_t a1 = tmp[1 * 8 + c] + tmp[6 * 8 + c];
        int32_t a2 = tmp[2 * 8 + c] + tmp[5 * 8 + c];
        int32_t a3 = tmp[3 * 8 + c] + tmp[4 * 8 + c];

        int32_t s0 = tmp[0 * 8 + c] - tmp[7 * 8 + c];
        int32_t s1 = tmp[1 * 8 + c] - tmp[6 * 8 + c];
        int32_t s2 = tmp[2 * 8 + c] - tmp[5 * 8 + c];
        int32_t s3 = tmp[3 * 8 + c] - tmp[4 * 8 + c];

        int32_t b0 = a0 + a3;
        int32_t b1 = a1 + a2;
        int32_t b2 = a1 - a2;
        int32_t b3 = a0 - a3;

        out[0 * 8 + c] = (int16_t)((b0 + b1 + 16) >> 5);
        out[4 * 8 + c] = (int16_t)((b0 - b1 + 16) >> 5);
        out[2 * 8 + c] = (int16_t)((((b3 * 181 + b2 * 181) >> 8) + 16) >> 5);
        out[6 * 8 + c] = (int16_t)((((b3 * 181 - b2 * 181) >> 8) + 16) >> 5);

        out[1 * 8 + c] = (int16_t)((((s0 * 251 + s1 * 213 + s2 * 142 + s3 * 49)  >> 8) + 16) >> 5);
        out[3 * 8 + c] = (int16_t)((((s0 * 213 - s1 * 49  - s2 * 251 - s3 * 142) >> 8) + 16) >> 5);
        out[5 * 8 + c] = (int16_t)((((s0 * 142 - s1 * 251 + s2 * 49  + s3 * 213) >> 8) + 16) >> 5);
        out[7 * 8 + c] = (int16_t)((((s0 * 49  - s1 * 142 + s2 * 213 - s3 * 251) >> 8) + 16) >> 5);
    }
}

void avx_scalar_idct_8x8_put(const int16_t in[64], uint8_t *dest, int stride) {
    int32_t tmp[64];
    for (int c = 0; c < 8; ++c) {
        int32_t x0 = in[0 * 8 + c];
        int32_t x1 = in[1 * 8 + c];
        int32_t x2 = in[2 * 8 + c];
        int32_t x3 = in[3 * 8 + c];
        int32_t x4 = in[4 * 8 + c];
        int32_t x5 = in[5 * 8 + c];
        int32_t x6 = in[6 * 8 + c];
        int32_t x7 = in[7 * 8 + c];

        int32_t a0 = (int32_t)((uint32_t)(x0 + x4) << 3);
        int32_t a1 = (int32_t)((uint32_t)(x0 - x4) << 3);
        int32_t a2 = (x2 * 181 - x6 * 181) >> 5;
        int32_t a3 = (x2 * 181 + x6 * 181) >> 5;

        int32_t b0 = a0 + a3;
        int32_t b1 = a1 + a2;
        int32_t b2 = a1 - a2;
        int32_t b3 = a0 - a3;

        int32_t s0 = (x1 * 251 + x3 * 213 + x5 * 142 + x7 * 49)  >> 5;
        int32_t s1 = (x1 * 213 - x3 * 49  - x5 * 251 - x7 * 142) >> 5;
        int32_t s2 = (x1 * 142 - x3 * 251 + x5 * 49  + x7 * 213) >> 5;
        int32_t s3 = (x1 * 49  - x3 * 142 + x5 * 213 - x7 * 251) >> 5;

        tmp[0 * 8 + c] = b0 + s0;
        tmp[7 * 8 + c] = b0 - s0;
        tmp[1 * 8 + c] = b1 + s1;
        tmp[6 * 8 + c] = b1 - s1;
        tmp[2 * 8 + c] = b2 + s2;
        tmp[5 * 8 + c] = b2 - s2;
        tmp[3 * 8 + c] = b3 + s3;
        tmp[4 * 8 + c] = b3 - s3;
    }

    for (int r = 0; r < 8; ++r) {
        const int32_t *t = &tmp[r * 8];
        uint8_t *d = &dest[r * stride];

        int32_t a0 = (t[0] + t[4]);
        int32_t a1 = (t[0] - t[4]);
        int32_t a2 = (t[2] * 181 - t[6] * 181) >> 8;
        int32_t a3 = (t[2] * 181 + t[6] * 181) >> 8;

        int32_t b0 = a0 + a3;
        int32_t b1 = a1 + a2;
        int32_t b2 = a1 - a2;
        int32_t b3 = a0 - a3;

        int32_t s0 = (t[1] * 251 + t[3] * 213 + t[5] * 142 + t[7] * 49)  >> 8;
        int32_t s1 = (t[1] * 213 - t[3] * 49  - t[5] * 251 - t[7] * 142) >> 8;
        int32_t s2 = (t[1] * 142 - t[3] * 251 + t[5] * 49  + t[7] * 213) >> 8;
        int32_t s3 = (t[1] * 49  - t[3] * 142 + t[5] * 213 - t[7] * 251) >> 8;

        d[0] = clamp_u8_scalar((b0 + s0 + 64) >> 7);
        d[7] = clamp_u8_scalar((b0 - s0 + 64) >> 7);
        d[1] = clamp_u8_scalar((b1 + s1 + 64) >> 7);
        d[6] = clamp_u8_scalar((b1 - s1 + 64) >> 7);
        d[2] = clamp_u8_scalar((b2 + s2 + 64) >> 7);
        d[5] = clamp_u8_scalar((b2 - s2 + 64) >> 7);
        d[3] = clamp_u8_scalar((b3 + s3 + 64) >> 7);
        d[4] = clamp_u8_scalar((b3 - s3 + 64) >> 7);
    }
}

/* ========================================================================= */
/* 2. AVX2 VECTORIZED IMPLEMENTATIONS (Target: x86_64 with AVX2 & FMA)       */
/* ========================================================================= */

#if HAS_X86_SIMD

AVX_TARGET_AVX2
static inline void avx2_uyvy_to_rgbx_impl(const uint8_t *__restrict src, uint8_t *__restrict dst,
                                          size_t num_pixels, bool is_bgra,
                                          int cy, int crv, int cgu, int cgv, int cbu) {
    const __m128i mask_y = _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i mask_u = _mm_setr_epi8(0, 0, 4, 4, 8, 8, 12, 12, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i mask_v = _mm_setr_epi8(2, 2, 6, 6, 10, 10, 14, 14, -1, -1, -1, -1, -1, -1, -1, -1);

    const __m256i v16  = _mm256_set1_epi32(16);
    const __m256i v128 = _mm256_set1_epi32(128);
    const __m256i vcy  = _mm256_set1_epi32(cy);
    const __m256i vcrv = _mm256_set1_epi32(crv);
    const __m256i vcgu = _mm256_set1_epi32(cgu);
    const __m256i vcgv = _mm256_set1_epi32(cgv);
    const __m256i vcbu = _mm256_set1_epi32(cbu);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i max255 = _mm256_set1_epi32(255);
    const __m256i alpha = _mm256_set1_epi32((int)0xFF000000);

    size_t count8 = num_pixels / 8;
    for (size_t i = 0; i < count8; ++i) {
        __m128i raw = _mm_loadu_si128((const __m128i*)(src + i * 16));
        __m256i y = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(raw, mask_y));
        __m256i u = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(raw, mask_u));
        __m256i v = _mm256_cvtepu8_epi32(_mm_shuffle_epi8(raw, mask_v));

        __m256i c  = _mm256_sub_epi32(y, v16);
        __m256i du = _mm256_sub_epi32(u, v128);
        __m256i ev = _mm256_sub_epi32(v, v128);

        __m256i cyc = _mm256_mullo_epi32(c, vcy);
        __m256i r = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(cyc, _mm256_mullo_epi32(ev, vcrv)), v128), 8);
        __m256i g = _mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(_mm256_sub_epi32(cyc, _mm256_mullo_epi32(du, vcgu)), _mm256_mullo_epi32(ev, vcgv)), v128), 8);
        __m256i b = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(cyc, _mm256_mullo_epi32(du, vcbu)), v128), 8);

        r = _mm256_min_epi32(_mm256_max_epi32(r, zero), max255);
        g = _mm256_min_epi32(_mm256_max_epi32(g, zero), max255);
        b = _mm256_min_epi32(_mm256_max_epi32(b, zero), max255);

        __m256i out = is_bgra
            ? _mm256_or_si256(_mm256_or_si256(b, _mm256_slli_epi32(g, 8)),
                              _mm256_or_si256(_mm256_slli_epi32(r, 16), alpha))
            : _mm256_or_si256(_mm256_or_si256(r, _mm256_slli_epi32(g, 8)),
                              _mm256_or_si256(_mm256_slli_epi32(b, 16), alpha));

        _mm256_storeu_si256((__m256i*)(dst + i * 32), out);
    }
}

AVX_TARGET_AVX2
static void avx2_uyvy_to_rgba_block(const uint8_t *src, uint8_t *dst, size_t num_pixels,
                                    const avx_yuv_matrix_t *m) {
    if (m == &g_yuv_bt709) {
        avx2_uyvy_to_rgbx_impl(src, dst, num_pixels, false, 298, 459, 55, 136, 541);
    } else {
        avx2_uyvy_to_rgbx_impl(src, dst, num_pixels, false, 298, 409, 100, 208, 516);
    }
}

AVX_TARGET_AVX2
static void avx2_uyvy_to_bgra_block(const uint8_t *src, uint8_t *dst, size_t num_pixels,
                                    const avx_yuv_matrix_t *m) {
    if (m == &g_yuv_bt709) {
        avx2_uyvy_to_rgbx_impl(src, dst, num_pixels, true, 298, 459, 55, 136, 541);
    } else {
        avx2_uyvy_to_rgbx_impl(src, dst, num_pixels, true, 298, 409, 100, 208, 516);
    }
}

/*
 * AVX2-targeted RGBX -> UYVY conversion block.
 *
 * ARCHITECTURAL DESIGN & VECTORIZATION NOTE:
 * Unlike UYVY -> RGBA (which performs 1:1 pixel expansion), RGBX -> UYVY involves:
 *   1. 4:2:2 horizontal subsampling with integer truncation: (c0 + c1) >> 1
 *   2. BT.709 (HD) or BT.601 (SD) matrix arithmetic across 3 color components
 *   3. 4-channel deinterleaving from [R, G, B, A] and reinterleaving to [U, Y0, V, Y1]
 *
 * Compile-time constant matrix specialization preserves compiler constant folding
 * and vectorization throughput with bitwise parity against the scalar reference.
 */
AVX_TARGET_AVX2
static inline void avx2_rgbx_to_uyvy_impl(const uint8_t *__restrict src, uint8_t *__restrict dst,
                                          size_t num_pixels, bool is_bgra,
                                          int yr, int yg, int yb,
                                          int ur, int ug, int ub,
                                          int vr, int vg, int vb) {
    size_t num_pairs = num_pixels / 2;
    const uint8_t *__restrict s = src;
    uint8_t *__restrict d = dst;

#if defined(__clang__)
    #pragma clang loop vectorize(enable)
    #pragma clang loop unroll_count(8)
#elif defined(__GNUC__)
    #pragma GCC ivdep
    #pragma GCC unroll 8
#endif
    for (size_t i = 0; i < num_pairs; ++i) {
        int r0 = s[is_bgra ? 2 : 0], g0 = s[1], b0 = s[is_bgra ? 0 : 2];
        int r1 = s[is_bgra ? 6 : 4], g1 = s[5], b1 = s[is_bgra ? 4 : 6];
        s += 8;

        int y0 = ((yr * r0 + yg * g0 + yb * b0 + 128) >> 8) + 16;
        int y1 = ((yr * r1 + yg * g1 + yb * b1 + 128) >> 8) + 16;

        int r_avg = (r0 + r1) >> 1;
        int g_avg = (g0 + g1) >> 1;
        int b_avg = (b0 + b1) >> 1;

        int u = ((-ur * r_avg - ug * g_avg + ub * b_avg + 128) >> 8) + 128;
        int v = ((vr * r_avg - vg * g_avg - vb * b_avg + 128) >> 8) + 128;

        d[0] = clamp_u8_scalar(u);
        d[1] = clamp_u8_scalar(y0);
        d[2] = clamp_u8_scalar(v);
        d[3] = clamp_u8_scalar(y1);
        d += 4;
    }
}

AVX_TARGET_AVX2
static void avx2_rgba_to_uyvy_block(const uint8_t *src, uint8_t *dst, size_t num_pixels,
                                    const avx_yuv_matrix_t *m) {
    if (m == &g_yuv_bt709) {
        avx2_rgbx_to_uyvy_impl(src, dst, num_pixels, false, 47, 157, 16, 26, 86, 112, 112, 102, 10);
    } else {
        avx2_rgbx_to_uyvy_impl(src, dst, num_pixels, false, 66, 129, 25, 38, 74, 112, 112, 94, 18);
    }
}

/*
 * AVX2-targeted BGRA -> UYVY conversion block.
 *
 * Uses compiler-guided 256-bit AVX2 vectorization (see architectural note above).
 */
AVX_TARGET_AVX2
static void avx2_bgra_to_uyvy_block(const uint8_t *src, uint8_t *dst, size_t num_pixels,
                                    const avx_yuv_matrix_t *m) {
    if (m == &g_yuv_bt709) {
        avx2_rgbx_to_uyvy_impl(src, dst, num_pixels, true, 47, 157, 16, 26, 86, 112, 112, 102, 10);
    } else {
        avx2_rgbx_to_uyvy_impl(src, dst, num_pixels, true, 66, 129, 25, 38, 74, 112, 112, 94, 18);
    }
}

AVX_TARGET_AVX2
static void avx2_rgba_to_bgra_block(const uint8_t *src, uint8_t *dst, size_t num_pixels) {
    const __m256i mask = _mm256_setr_epi8(
        2, 1, 0, 3,  6, 5, 4, 7,  10, 9, 8, 11,  14, 13, 12, 15,
        2, 1, 0, 3,  6, 5, 4, 7,  10, 9, 8, 11,  14, 13, 12, 15
    );
    size_t count8 = num_pixels / 8;
    for (size_t i = 0; i < count8; ++i) {
        __m256i raw = _mm256_loadu_si256((const __m256i*)(src + i * 32));
        __m256i swapped = _mm256_shuffle_epi8(raw, mask);
        _mm256_storeu_si256((__m256i*)(dst + i * 32), swapped);
    }
}

AVX_TARGET_AVX2
static void avx2_alpha_blend_rgba_block(const uint8_t *fg, const uint8_t *bg, uint8_t *out, size_t num_pixels) {
    const __m256i alpha_broadcast_mask = _mm256_setr_epi8(
        6, 7, 6, 7, 6, 7, 6, 7,  14, 15, 14, 15, 14, 15, 14, 15,
        6, 7, 6, 7, 6, 7, 6, 7,  14, 15, 14, 15, 14, 15, 14, 15
    );
    const __m256i v255 = _mm256_set1_epi16(255);
    const __m256i v1   = _mm256_set1_epi16(1);
    const __m256i vzero = _mm256_setzero_si256();
    const __m256i alpha_channel_mask = _mm256_setr_epi16(
        0, 0, 0, (short)0xFFFF, 0, 0, 0, (short)0xFFFF,
        0, 0, 0, (short)0xFFFF, 0, 0, 0, (short)0xFFFF
    );

    size_t count8 = num_pixels / 8;
    for (size_t i = 0; i < count8; ++i) {
        __m256i f = _mm256_loadu_si256((const __m256i*)(fg + i * 32));
        __m256i b = _mm256_loadu_si256((const __m256i*)(bg + i * 32));

        __m128i f_lo = _mm256_castsi256_si128(f);
        __m128i f_hi = _mm256_extracti128_si256(f, 1);
        __m128i b_lo = _mm256_castsi256_si128(b);
        __m128i b_hi = _mm256_extracti128_si256(b, 1);

        __m256i f_lo16 = _mm256_cvtepu8_epi16(f_lo);
        __m256i f_hi16 = _mm256_cvtepu8_epi16(f_hi);
        __m256i b_lo16 = _mm256_cvtepu8_epi16(b_lo);
        __m256i b_hi16 = _mm256_cvtepu8_epi16(b_hi);

        /* Process Low 4 pixels */
        __m256i a_lo = _mm256_shuffle_epi8(f_lo16, alpha_broadcast_mask);
        __m256i inv_a_lo = _mm256_sub_epi16(v255, a_lo);

        __m256i prod_f_lo = _mm256_mullo_epi16(f_lo16, a_lo);
        __m256i prod_b_lo = _mm256_mullo_epi16(b_lo16, inv_a_lo);
        __m256i sum_lo = _mm256_add_epi16(prod_f_lo, prod_b_lo);

        /* Fast divide by 255: (sum + 1 + (sum >> 8)) >> 8 */
        __m256i div_lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(sum_lo, v1), _mm256_srli_epi16(sum_lo, 8)), 8);

        /* Alpha replacement: if a > 0, set alpha channel to 255 */
        __m256i a_pos_lo = _mm256_cmpgt_epi16(a_lo, vzero);
        __m256i repl_lo  = _mm256_and_si256(a_pos_lo, alpha_channel_mask);
        __m256i res_lo16 = _mm256_blendv_epi8(div_lo, v255, repl_lo);

        /* Process High 4 pixels */
        __m256i a_hi = _mm256_shuffle_epi8(f_hi16, alpha_broadcast_mask);
        __m256i inv_a_hi = _mm256_sub_epi16(v255, a_hi);

        __m256i prod_f_hi = _mm256_mullo_epi16(f_hi16, a_hi);
        __m256i prod_b_hi = _mm256_mullo_epi16(b_hi16, inv_a_hi);
        __m256i sum_hi = _mm256_add_epi16(prod_f_hi, prod_b_hi);

        __m256i div_hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(sum_hi, v1), _mm256_srli_epi16(sum_hi, 8)), 8);

        __m256i a_pos_hi = _mm256_cmpgt_epi16(a_hi, vzero);
        __m256i repl_hi  = _mm256_and_si256(a_pos_hi, alpha_channel_mask);
        __m256i res_hi16 = _mm256_blendv_epi8(div_hi, v255, repl_hi);

        /* Pack 16-bit words back to 8-bit bytes */
        __m128i p_lo0 = _mm256_castsi256_si128(res_lo16);
        __m128i p_lo1 = _mm256_extracti128_si256(res_lo16, 1);
        __m128i packed_lo = _mm_packus_epi16(p_lo0, p_lo1);

        __m128i p_hi0 = _mm256_castsi256_si128(res_hi16);
        __m128i p_hi1 = _mm256_extracti128_si256(res_hi16, 1);
        __m128i packed_hi = _mm_packus_epi16(p_hi0, p_hi1);

        _mm_storeu_si128((__m128i*)(out + i * 32), packed_lo);
        _mm_storeu_si128((__m128i*)(out + i * 32 + 16), packed_hi);
    }
}

AVX_TARGET_AVX2
static inline void transpose8x8_32(__m256i *r0, __m256i *r1, __m256i *r2, __m256i *r3,
                                  __m256i *r4, __m256i *r5, __m256i *r6, __m256i *r7) {
    __m256i t0 = _mm256_unpacklo_epi32(*r0, *r1);
    __m256i t1 = _mm256_unpackhi_epi32(*r0, *r1);
    __m256i t2 = _mm256_unpacklo_epi32(*r2, *r3);
    __m256i t3 = _mm256_unpackhi_epi32(*r2, *r3);
    __m256i t4 = _mm256_unpacklo_epi32(*r4, *r5);
    __m256i t5 = _mm256_unpackhi_epi32(*r4, *r5);
    __m256i t6 = _mm256_unpacklo_epi32(*r6, *r7);
    __m256i t7 = _mm256_unpackhi_epi32(*r6, *r7);

    __m256i u0 = _mm256_unpacklo_epi64(t0, t2);
    __m256i u1 = _mm256_unpackhi_epi64(t0, t2);
    __m256i u2 = _mm256_unpacklo_epi64(t1, t3);
    __m256i u3 = _mm256_unpackhi_epi64(t1, t3);
    __m256i u4 = _mm256_unpacklo_epi64(t4, t6);
    __m256i u5 = _mm256_unpackhi_epi64(t4, t6);
    __m256i u6 = _mm256_unpacklo_epi64(t5, t7);
    __m256i u7 = _mm256_unpackhi_epi64(t5, t7);

    *r0 = _mm256_permute2f128_si256(u0, u4, 0x20);
    *r1 = _mm256_permute2f128_si256(u1, u5, 0x20);
    *r2 = _mm256_permute2f128_si256(u2, u6, 0x20);
    *r3 = _mm256_permute2f128_si256(u3, u7, 0x20);
    *r4 = _mm256_permute2f128_si256(u0, u4, 0x31);
    *r5 = _mm256_permute2f128_si256(u1, u5, 0x31);
    *r6 = _mm256_permute2f128_si256(u2, u6, 0x31);
    *r7 = _mm256_permute2f128_si256(u3, u7, 0x31);
}

AVX_TARGET_AVX2
static void avx2_fdct_8x8_impl(const int16_t in[64], int16_t out[64]) {
    /* Step 1: Load 8 rows and sign-extend to 32-bit */
    __m256i r0 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[0 * 8]));
    __m256i r1 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[1 * 8]));
    __m256i r2 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[2 * 8]));
    __m256i r3 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[3 * 8]));
    __m256i r4 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[4 * 8]));
    __m256i r5 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[5 * 8]));
    __m256i r6 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[6 * 8]));
    __m256i r7 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[7 * 8]));

    /* Transpose so each register holds one column across all 8 rows */
    transpose8x8_32(&r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7);

    /* Pass 1: 1D FDCT on Rows (calculated vertically across all 8 rows) */
    __m256i a0 = _mm256_add_epi32(r0, r7);
    __m256i a1 = _mm256_add_epi32(r1, r6);
    __m256i a2 = _mm256_add_epi32(r2, r5);
    __m256i a3 = _mm256_add_epi32(r3, r4);

    __m256i s0 = _mm256_sub_epi32(r0, r7);
    __m256i s1 = _mm256_sub_epi32(r1, r6);
    __m256i s2 = _mm256_sub_epi32(r2, r5);
    __m256i s3 = _mm256_sub_epi32(r3, r4);

    __m256i b0 = _mm256_add_epi32(a0, a3);
    __m256i b1 = _mm256_add_epi32(a1, a2);
    __m256i b2 = _mm256_sub_epi32(a1, a2);
    __m256i b3 = _mm256_sub_epi32(a0, a3);

    const __m256i v181 = _mm256_set1_epi32(181);
    const __m256i v251 = _mm256_set1_epi32(251);
    const __m256i v213 = _mm256_set1_epi32(213);
    const __m256i v142 = _mm256_set1_epi32(142);
    const __m256i v49  = _mm256_set1_epi32(49);

    __m256i t0 = _mm256_slli_epi32(_mm256_add_epi32(b0, b1), 3);
    __m256i t4 = _mm256_slli_epi32(_mm256_sub_epi32(b0, b1), 3);
    __m256i t2 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_mullo_epi32(b3, v181), _mm256_mullo_epi32(b2, v181)), 5);
    __m256i t6 = _mm256_srai_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(b3, v181), _mm256_mullo_epi32(b2, v181)), 5);

    __m256i t1 = _mm256_srai_epi32(_mm256_add_epi32(
        _mm256_add_epi32(_mm256_mullo_epi32(s0, v251), _mm256_mullo_epi32(s1, v213)),
        _mm256_add_epi32(_mm256_mullo_epi32(s2, v142), _mm256_mullo_epi32(s3, v49))), 5);

    __m256i t3 = _mm256_srai_epi32(_mm256_sub_epi32(
        _mm256_sub_epi32(_mm256_mullo_epi32(s0, v213), _mm256_mullo_epi32(s1, v49)),
        _mm256_add_epi32(_mm256_mullo_epi32(s2, v251), _mm256_mullo_epi32(s3, v142))), 5);

    __m256i t5 = _mm256_srai_epi32(_mm256_add_epi32(
        _mm256_sub_epi32(_mm256_mullo_epi32(s0, v142), _mm256_mullo_epi32(s1, v251)),
        _mm256_add_epi32(_mm256_mullo_epi32(s2, v49), _mm256_mullo_epi32(s3, v213))), 5);

    __m256i t7 = _mm256_srai_epi32(_mm256_sub_epi32(
        _mm256_add_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(s0, v49), _mm256_mullo_epi32(s1, v142)),
        _mm256_mullo_epi32(s2, v213)), _mm256_mullo_epi32(s3, v251)), 5);

    /* Transpose intermediate matrix so columns are in registers for Pass 2 */
    transpose8x8_32(&t0, &t1, &t2, &t3, &t4, &t5, &t6, &t7);

    /* Pass 2: 1D FDCT on Columns */
    a0 = _mm256_add_epi32(t0, t7);
    a1 = _mm256_add_epi32(t1, t6);
    a2 = _mm256_add_epi32(t2, t5);
    a3 = _mm256_add_epi32(t3, t4);

    s0 = _mm256_sub_epi32(t0, t7);
    s1 = _mm256_sub_epi32(t1, t6);
    s2 = _mm256_sub_epi32(t2, t5);
    s3 = _mm256_sub_epi32(t3, t4);

    b0 = _mm256_add_epi32(a0, a3);
    b1 = _mm256_add_epi32(a1, a2);
    b2 = _mm256_sub_epi32(a1, a2);
    b3 = _mm256_sub_epi32(a0, a3);

    const __m256i v16 = _mm256_set1_epi32(16);

    __m256i o0 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(b0, b1), v16), 5);
    __m256i o4 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(b0, b1), v16), 5);

    __m256i o2 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_srai_epi32(_mm256_add_epi32(_mm256_mullo_epi32(b3, v181), _mm256_mullo_epi32(b2, v181)), 8), v16), 5);
    __m256i o6 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_srai_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(b3, v181), _mm256_mullo_epi32(b2, v181)), 8), v16), 5);

    __m256i o1 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_srai_epi32(
        _mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(s0, v251), _mm256_mullo_epi32(s1, v213)),
        _mm256_add_epi32(_mm256_mullo_epi32(s2, v142), _mm256_mullo_epi32(s3, v49))), 8), v16), 5);

    __m256i o3 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_srai_epi32(
        _mm256_sub_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(s0, v213), _mm256_mullo_epi32(s1, v49)),
        _mm256_add_epi32(_mm256_mullo_epi32(s2, v251), _mm256_mullo_epi32(s3, v142))), 8), v16), 5);

    __m256i o5 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_srai_epi32(
        _mm256_add_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(s0, v142), _mm256_mullo_epi32(s1, v251)),
        _mm256_add_epi32(_mm256_mullo_epi32(s2, v49), _mm256_mullo_epi32(s3, v213))), 8), v16), 5);

    __m256i o7 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_srai_epi32(
        _mm256_sub_epi32(_mm256_add_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(s0, v49), _mm256_mullo_epi32(s1, v142)),
        _mm256_mullo_epi32(s2, v213)), _mm256_mullo_epi32(s3, v251)), 8), v16), 5);

    /* Pack 32-bit integers to 16-bit (with truncation matching scalar (int16_t)) and store into out */
    const __m128i shuf_trunc = _mm_setr_epi8(0, 1, 4, 5, 8, 9, 12, 13, -1, -1, -1, -1, -1, -1, -1, -1);
    #define PACK_STORE_FDCT(row_idx, vec) do { \
        __m128i lo = _mm256_castsi256_si128(vec); \
        __m128i hi = _mm256_extracti128_si256(vec, 1); \
        __m128i lo_t = _mm_shuffle_epi8(lo, shuf_trunc); \
        __m128i hi_t = _mm_shuffle_epi8(hi, shuf_trunc); \
        __m128i p = _mm_unpacklo_epi64(lo_t, hi_t); \
        _mm_storeu_si128((__m128i*)&out[row_idx * 8], p); \
    } while(0)

    PACK_STORE_FDCT(0, o0);
    PACK_STORE_FDCT(1, o1);
    PACK_STORE_FDCT(2, o2);
    PACK_STORE_FDCT(3, o3);
    PACK_STORE_FDCT(4, o4);
    PACK_STORE_FDCT(5, o5);
    PACK_STORE_FDCT(6, o6);
    PACK_STORE_FDCT(7, o7);
    #undef PACK_STORE_FDCT
}

AVX_TARGET_AVX2
static void avx2_idct_8x8_put_impl(const int16_t in[64], uint8_t *dest, int stride) {
    __m256i x0 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[0 * 8]));
    __m256i x1 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[1 * 8]));
    __m256i x2 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[2 * 8]));
    __m256i x3 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[3 * 8]));
    __m256i x4 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[4 * 8]));
    __m256i x5 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[5 * 8]));
    __m256i x6 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[6 * 8]));
    __m256i x7 = _mm256_cvtepi16_epi32(_mm_loadu_si128((const __m128i*)&in[7 * 8]));

    /* Pass 1: 1D IDCT on Columns */
    __m256i a0 = _mm256_slli_epi32(_mm256_add_epi32(x0, x4), 3);
    __m256i a1 = _mm256_slli_epi32(_mm256_sub_epi32(x0, x4), 3);

    const __m256i v181 = _mm256_set1_epi32(181);
    const __m256i v251 = _mm256_set1_epi32(251);
    const __m256i v213 = _mm256_set1_epi32(213);
    const __m256i v142 = _mm256_set1_epi32(142);
    const __m256i v49  = _mm256_set1_epi32(49);

    __m256i a2 = _mm256_srai_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(x2, v181), _mm256_mullo_epi32(x6, v181)), 5);
    __m256i a3 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_mullo_epi32(x2, v181), _mm256_mullo_epi32(x6, v181)), 5);

    __m256i b0 = _mm256_add_epi32(a0, a3);
    __m256i b1 = _mm256_add_epi32(a1, a2);
    __m256i b2 = _mm256_sub_epi32(a1, a2);
    __m256i b3 = _mm256_sub_epi32(a0, a3);

    __m256i s0 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(x1, v251), _mm256_mullo_epi32(x3, v213)),
                                   _mm256_add_epi32(_mm256_mullo_epi32(x5, v142), _mm256_mullo_epi32(x7, v49))), 5);
    __m256i s1 = _mm256_srai_epi32(_mm256_sub_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(x1, v213), _mm256_mullo_epi32(x3, v49)),
                                   _mm256_add_epi32(_mm256_mullo_epi32(x5, v251), _mm256_mullo_epi32(x7, v142))), 5);
    __m256i s2 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(x1, v142), _mm256_mullo_epi32(x3, v251)),
                                   _mm256_add_epi32(_mm256_mullo_epi32(x5, v49), _mm256_mullo_epi32(x7, v213))), 5);
    __m256i s3 = _mm256_srai_epi32(_mm256_sub_epi32(_mm256_add_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(x1, v49), _mm256_mullo_epi32(x3, v142)),
                                   _mm256_mullo_epi32(x5, v213)), _mm256_mullo_epi32(x7, v251)), 5);

    /* The 8 row vectors of the intermediate matrix */
    __m256i t0 = _mm256_add_epi32(b0, s0);
    __m256i t7 = _mm256_sub_epi32(b0, s0);
    __m256i t1 = _mm256_add_epi32(b1, s1);
    __m256i t6 = _mm256_sub_epi32(b1, s1);
    __m256i t2 = _mm256_add_epi32(b2, s2);
    __m256i t5 = _mm256_sub_epi32(b2, s2);
    __m256i t3 = _mm256_add_epi32(b3, s3);
    __m256i t4 = _mm256_sub_epi32(b3, s3);

    /* Transpose intermediate matrix so columns are in registers for Pass 2 */
    transpose8x8_32(&t0, &t1, &t2, &t3, &t4, &t5, &t6, &t7);

    /* Pass 2: 1D IDCT on Rows (calculated vertically across all 8 rows) */
    __m256i ra0 = _mm256_add_epi32(t0, t4);
    __m256i ra1 = _mm256_sub_epi32(t0, t4);
    __m256i ra2 = _mm256_srai_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(t2, v181), _mm256_mullo_epi32(t6, v181)), 8);
    __m256i ra3 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_mullo_epi32(t2, v181), _mm256_mullo_epi32(t6, v181)), 8);

    __m256i rb0 = _mm256_add_epi32(ra0, ra3);
    __m256i rb1 = _mm256_add_epi32(ra1, ra2);
    __m256i rb2 = _mm256_sub_epi32(ra1, ra2);
    __m256i rb3 = _mm256_sub_epi32(ra0, ra3);

    __m256i rs0 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(_mm256_mullo_epi32(t1, v251), _mm256_mullo_epi32(t3, v213)),
                                    _mm256_add_epi32(_mm256_mullo_epi32(t5, v142), _mm256_mullo_epi32(t7, v49))), 8);
    __m256i rs1 = _mm256_srai_epi32(_mm256_sub_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(t1, v213), _mm256_mullo_epi32(t3, v49)),
                                    _mm256_add_epi32(_mm256_mullo_epi32(t5, v251), _mm256_mullo_epi32(t7, v142))), 8);
    __m256i rs2 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(t1, v142), _mm256_mullo_epi32(t3, v251)),
                                    _mm256_add_epi32(_mm256_mullo_epi32(t5, v49), _mm256_mullo_epi32(t7, v213))), 8);
    __m256i rs3 = _mm256_srai_epi32(_mm256_sub_epi32(_mm256_add_epi32(_mm256_sub_epi32(_mm256_mullo_epi32(t1, v49), _mm256_mullo_epi32(t3, v142)),
                                    _mm256_mullo_epi32(t5, v213)), _mm256_mullo_epi32(t7, v251)), 8);

    const __m256i v64 = _mm256_set1_epi32(64);
    __m256i o0 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(rb0, rs0), v64), 7);
    __m256i o7 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(rb0, rs0), v64), 7);
    __m256i o1 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(rb1, rs1), v64), 7);
    __m256i o6 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(rb1, rs1), v64), 7);
    __m256i o2 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(rb2, rs2), v64), 7);
    __m256i o5 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(rb2, rs2), v64), 7);
    __m256i o3 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(rb3, rs3), v64), 7);
    __m256i o4 = _mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(rb3, rs3), v64), 7);

    /* Transpose o0..o7 back so each register contains row 0..7 */
    transpose8x8_32(&o0, &o1, &o2, &o3, &o4, &o5, &o6, &o7);

    /* Pack to uint8_t with clamp and store 8 bytes into dest[r * stride] */
    #define PACK_STORE_U8_IDCT(r_idx, vec) do { \
        __m128i lo = _mm256_castsi256_si128(vec); \
        __m128i hi = _mm256_extracti128_si256(vec, 1); \
        __m128i p16 = _mm_packs_epi32(lo, hi); \
        __m128i p8  = _mm_packus_epi16(p16, p16); \
        uint64_t val = (uint64_t)_mm_cvtsi128_si64(p8); \
        memcpy(&dest[r_idx * stride], &val, 8); \
    } while(0)

    PACK_STORE_U8_IDCT(0, o0);
    PACK_STORE_U8_IDCT(1, o1);
    PACK_STORE_U8_IDCT(2, o2);
    PACK_STORE_U8_IDCT(3, o3);
    PACK_STORE_U8_IDCT(4, o4);
    PACK_STORE_U8_IDCT(5, o5);
    PACK_STORE_U8_IDCT(6, o6);
    PACK_STORE_U8_IDCT(7, o7);
    #undef PACK_STORE_U8_IDCT
}

#endif /* HAS_X86_SIMD */

/* ========================================================================= */
/* 3. ARM64 NEON VECTORIZED IMPLEMENTATIONS (Target: aarch64 / ARMv8-A)      */
/* ========================================================================= */

#if HAS_ARM_NEON

static void neon_uyvy_to_rgba_block(const uint8_t *src, uint8_t *dst, size_t num_pixels,
                                    const avx_yuv_matrix_t *m) {
    static const uint8_t mask_y_data[16] = {1, 3, 5, 7, 9, 11, 13, 15, 0,0,0,0,0,0,0,0};
    static const uint8_t mask_u_data[16] = {0, 0, 4, 4, 8, 8, 12, 12, 0,0,0,0,0,0,0,0};
    static const uint8_t mask_v_data[16] = {2, 2, 6, 6, 10, 10, 14, 14, 0,0,0,0,0,0,0,0};
    uint8x16_t mask_y = vld1q_u8(mask_y_data);
    uint8x16_t mask_u = vld1q_u8(mask_u_data);
    uint8x16_t mask_v = vld1q_u8(mask_v_data);
    const int32x4_t vcy  = vdupq_n_s32(m->cy);
    const int32x4_t vcrv = vdupq_n_s32(m->crv);
    const int32x4_t vcgu = vdupq_n_s32(m->cgu);
    const int32x4_t vcgv = vdupq_n_s32(m->cgv);
    const int32x4_t vcbu = vdupq_n_s32(m->cbu);

    size_t count8 = num_pixels / 8;
    for (size_t i = 0; i < count8; ++i) {
        uint8x16_t in = vld1q_u8(src + i * 16);
        uint8x8_t y_8 = vget_low_u8(vqtbl1q_u8(in, mask_y));
        uint8x8_t u_8 = vget_low_u8(vqtbl1q_u8(in, mask_u));
        uint8x8_t v_8 = vget_low_u8(vqtbl1q_u8(in, mask_v));

        int16x8_t y_16 = vreinterpretq_s16_u16(vmovl_u8(y_8));
        int16x8_t u_16 = vreinterpretq_s16_u16(vmovl_u8(u_8));
        int16x8_t v_16 = vreinterpretq_s16_u16(vmovl_u8(v_8));

        int16x8_t c = vsubq_s16(y_16, vdupq_n_s16(16));
        int16x8_t du = vsubq_s16(u_16, vdupq_n_s16(128));
        int16x8_t ev = vsubq_s16(v_16, vdupq_n_s16(128));

        int32x4_t c_low = vmovl_s16(vget_low_s16(c));
        int32x4_t du_low = vmovl_s16(vget_low_s16(du));
        int32x4_t ev_low = vmovl_s16(vget_low_s16(ev));
        int32x4_t cyc_low = vmulq_s32(c_low, vcy);
        int32x4_t r_low = vshrq_n_s32(vaddq_s32(vaddq_s32(cyc_low, vmulq_s32(ev_low, vcrv)), vdupq_n_s32(128)), 8);
        int32x4_t g_low = vshrq_n_s32(vaddq_s32(vsubq_s32(vsubq_s32(cyc_low, vmulq_s32(du_low, vcgu)), vmulq_s32(ev_low, vcgv)), vdupq_n_s32(128)), 8);
        int32x4_t b_low = vshrq_n_s32(vaddq_s32(vaddq_s32(cyc_low, vmulq_s32(du_low, vcbu)), vdupq_n_s32(128)), 8);

        int32x4_t c_high = vmovl_s16(vget_high_s16(c));
        int32x4_t du_high = vmovl_s16(vget_high_s16(du));
        int32x4_t ev_high = vmovl_s16(vget_high_s16(ev));
        int32x4_t cyc_high = vmulq_s32(c_high, vcy);
        int32x4_t r_high = vshrq_n_s32(vaddq_s32(vaddq_s32(cyc_high, vmulq_s32(ev_high, vcrv)), vdupq_n_s32(128)), 8);
        int32x4_t g_high = vshrq_n_s32(vaddq_s32(vsubq_s32(vsubq_s32(cyc_high, vmulq_s32(du_high, vcgu)), vmulq_s32(ev_high, vcgv)), vdupq_n_s32(128)), 8);
        int32x4_t b_high = vshrq_n_s32(vaddq_s32(vaddq_s32(cyc_high, vmulq_s32(du_high, vcbu)), vdupq_n_s32(128)), 8);

        int16x8_t r_16 = vcombine_s16(vqmovn_s32(r_low), vqmovn_s32(r_high));
        int16x8_t g_16 = vcombine_s16(vqmovn_s32(g_low), vqmovn_s32(g_high));
        int16x8_t b_16 = vcombine_s16(vqmovn_s32(b_low), vqmovn_s32(b_high));

        uint8x8x4_t rgba;
        rgba.val[0] = vqmovun_s16(r_16);
        rgba.val[1] = vqmovun_s16(g_16);
        rgba.val[2] = vqmovun_s16(b_16);
        rgba.val[3] = vdup_n_u8(255);
        vst4_u8(dst + i * 32, rgba);
    }
}

static void neon_uyvy_to_bgra_block(const uint8_t *src, uint8_t *dst, size_t num_pixels,
                                    const avx_yuv_matrix_t *m) {
    static const uint8_t mask_y_data[16] = {1, 3, 5, 7, 9, 11, 13, 15, 0,0,0,0,0,0,0,0};
    static const uint8_t mask_u_data[16] = {0, 0, 4, 4, 8, 8, 12, 12, 0,0,0,0,0,0,0,0};
    static const uint8_t mask_v_data[16] = {2, 2, 6, 6, 10, 10, 14, 14, 0,0,0,0,0,0,0,0};
    uint8x16_t mask_y = vld1q_u8(mask_y_data);
    uint8x16_t mask_u = vld1q_u8(mask_u_data);
    uint8x16_t mask_v = vld1q_u8(mask_v_data);
    const int32x4_t vcy  = vdupq_n_s32(m->cy);
    const int32x4_t vcrv = vdupq_n_s32(m->crv);
    const int32x4_t vcgu = vdupq_n_s32(m->cgu);
    const int32x4_t vcgv = vdupq_n_s32(m->cgv);
    const int32x4_t vcbu = vdupq_n_s32(m->cbu);

    size_t count8 = num_pixels / 8;
    for (size_t i = 0; i < count8; ++i) {
        uint8x16_t in = vld1q_u8(src + i * 16);
        uint8x8_t y_8 = vget_low_u8(vqtbl1q_u8(in, mask_y));
        uint8x8_t u_8 = vget_low_u8(vqtbl1q_u8(in, mask_u));
        uint8x8_t v_8 = vget_low_u8(vqtbl1q_u8(in, mask_v));

        int16x8_t y_16 = vreinterpretq_s16_u16(vmovl_u8(y_8));
        int16x8_t u_16 = vreinterpretq_s16_u16(vmovl_u8(u_8));
        int16x8_t v_16 = vreinterpretq_s16_u16(vmovl_u8(v_8));

        int16x8_t c = vsubq_s16(y_16, vdupq_n_s16(16));
        int16x8_t du = vsubq_s16(u_16, vdupq_n_s16(128));
        int16x8_t ev = vsubq_s16(v_16, vdupq_n_s16(128));

        int32x4_t c_low = vmovl_s16(vget_low_s16(c));
        int32x4_t du_low = vmovl_s16(vget_low_s16(du));
        int32x4_t ev_low = vmovl_s16(vget_low_s16(ev));
        int32x4_t cyc_low = vmulq_s32(c_low, vcy);
        int32x4_t r_low = vshrq_n_s32(vaddq_s32(vaddq_s32(cyc_low, vmulq_s32(ev_low, vcrv)), vdupq_n_s32(128)), 8);
        int32x4_t g_low = vshrq_n_s32(vaddq_s32(vsubq_s32(vsubq_s32(cyc_low, vmulq_s32(du_low, vcgu)), vmulq_s32(ev_low, vcgv)), vdupq_n_s32(128)), 8);
        int32x4_t b_low = vshrq_n_s32(vaddq_s32(vaddq_s32(cyc_low, vmulq_s32(du_low, vcbu)), vdupq_n_s32(128)), 8);

        int32x4_t c_high = vmovl_s16(vget_high_s16(c));
        int32x4_t du_high = vmovl_s16(vget_high_s16(du));
        int32x4_t ev_high = vmovl_s16(vget_high_s16(ev));
        int32x4_t cyc_high = vmulq_s32(c_high, vcy);
        int32x4_t r_high = vshrq_n_s32(vaddq_s32(vaddq_s32(cyc_high, vmulq_s32(ev_high, vcrv)), vdupq_n_s32(128)), 8);
        int32x4_t g_high = vshrq_n_s32(vaddq_s32(vsubq_s32(vsubq_s32(cyc_high, vmulq_s32(du_high, vcgu)), vmulq_s32(ev_high, vcgv)), vdupq_n_s32(128)), 8);
        int32x4_t b_high = vshrq_n_s32(vaddq_s32(vaddq_s32(cyc_high, vmulq_s32(du_high, vcbu)), vdupq_n_s32(128)), 8);

        int16x8_t r_16 = vcombine_s16(vqmovn_s32(r_low), vqmovn_s32(r_high));
        int16x8_t g_16 = vcombine_s16(vqmovn_s32(g_low), vqmovn_s32(g_high));
        int16x8_t b_16 = vcombine_s16(vqmovn_s32(b_low), vqmovn_s32(b_high));

        uint8x8x4_t bgra;
        bgra.val[0] = vqmovun_s16(b_16);
        bgra.val[1] = vqmovun_s16(g_16);
        bgra.val[2] = vqmovun_s16(r_16);
        bgra.val[3] = vdup_n_u8(255);
        vst4_u8(dst + i * 32, bgra);
    }
}

static void neon_rgba_to_uyvy_block(const uint8_t *src, uint8_t *dst, size_t num_pixels,
                                    const avx_yuv_matrix_t *m) {
    size_t count8 = num_pixels / 8;
    const uint16x8_t c128 = vdupq_n_u16(128);
    const uint8x8_t c16 = vdup_n_u8(16);
    const uint8x8_t vyr = vdup_n_u8((uint8_t)m->yr);
    const uint8x8_t vyg = vdup_n_u8((uint8_t)m->yg);
    const uint8x8_t vyb = vdup_n_u8((uint8_t)m->yb);
    const int16x4_t vur = vdup_n_s16((int16_t)m->ur);
    const int16x4_t vug = vdup_n_s16((int16_t)m->ug);
    const int16x4_t vub = vdup_n_s16((int16_t)m->ub);
    const int16x4_t vvr = vdup_n_s16((int16_t)m->vr);
    const int16x4_t vvg = vdup_n_s16((int16_t)m->vg);
    const int16x4_t vvb = vdup_n_s16((int16_t)m->vb);

    for (size_t i = 0; i < count8; ++i) {
        uint8x8x4_t rgba = vld4_u8(src + i * 32);
        uint8x8_t r = rgba.val[0];
        uint8x8_t g = rgba.val[1];
        uint8x8_t b = rgba.val[2];

        uint16x8_t y_acc = c128;
        y_acc = vmlal_u8(y_acc, r, vyr);
        y_acc = vmlal_u8(y_acc, g, vyg);
        y_acc = vmlal_u8(y_acc, b, vyb);
        uint8x8_t y = vadd_u8(vshrn_n_u16(y_acc, 8), c16);

        uint8x8x2_t y_uzp = vuzp_u8(y, y);
        uint8x8_t y_even = y_uzp.val[0];
        uint8x8_t y_odd  = y_uzp.val[1];

        uint8x8x2_t r_uzp = vuzp_u8(r, r);
        uint8x8x2_t g_uzp = vuzp_u8(g, g);
        uint8x8x2_t b_uzp = vuzp_u8(b, b);

        uint8x8_t r_avg8 = vhadd_u8(r_uzp.val[0], r_uzp.val[1]);
        uint8x8_t g_avg8 = vhadd_u8(g_uzp.val[0], g_uzp.val[1]);
        uint8x8_t b_avg8 = vhadd_u8(b_uzp.val[0], b_uzp.val[1]);

        int16x4_t r_avg = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(r_avg8)));
        int16x4_t g_avg = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(g_avg8)));
        int16x4_t b_avg = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(b_avg8)));

        int16x4_t u_acc = vdup_n_s16(128);
        u_acc = vmls_s16(u_acc, r_avg, vur);
        u_acc = vmls_s16(u_acc, g_avg, vug);
        u_acc = vmla_s16(u_acc, b_avg, vub);
        int16x4_t u_val = vadd_s16(vshr_n_s16(u_acc, 8), vdup_n_s16(128));
        uint8x8_t u_sat = vqmovun_s16(vcombine_s16(u_val, u_val));

        int16x4_t v_acc = vdup_n_s16(128);
        v_acc = vmla_s16(v_acc, r_avg, vvr);
        v_acc = vmls_s16(v_acc, g_avg, vvg);
        v_acc = vmls_s16(v_acc, b_avg, vvb);
        int16x4_t v_val = vadd_s16(vshr_n_s16(v_acc, 8), vdup_n_s16(128));
        uint8x8_t v_sat = vqmovun_s16(vcombine_s16(v_val, v_val));

        uint8x8x2_t p0 = vzip_u8(u_sat, y_even);
        uint8x8x2_t p1 = vzip_u8(v_sat, y_odd);
        uint16x4x2_t res = vzip_u16(vreinterpret_u16_u8(p0.val[0]), vreinterpret_u16_u8(p1.val[0]));
        uint8x16_t out_vec = vcombine_u8(vreinterpret_u8_u16(res.val[0]), vreinterpret_u8_u16(res.val[1]));
        vst1q_u8(dst + i * 16, out_vec);
    }
}

static void neon_bgra_to_uyvy_block(const uint8_t *src, uint8_t *dst, size_t num_pixels,
                                    const avx_yuv_matrix_t *m) {
    size_t count8 = num_pixels / 8;
    const uint16x8_t c128 = vdupq_n_u16(128);
    const uint8x8_t c16 = vdup_n_u8(16);
    const uint8x8_t vyr = vdup_n_u8((uint8_t)m->yr);
    const uint8x8_t vyg = vdup_n_u8((uint8_t)m->yg);
    const uint8x8_t vyb = vdup_n_u8((uint8_t)m->yb);
    const int16x4_t vur = vdup_n_s16((int16_t)m->ur);
    const int16x4_t vug = vdup_n_s16((int16_t)m->ug);
    const int16x4_t vub = vdup_n_s16((int16_t)m->ub);
    const int16x4_t vvr = vdup_n_s16((int16_t)m->vr);
    const int16x4_t vvg = vdup_n_s16((int16_t)m->vg);
    const int16x4_t vvb = vdup_n_s16((int16_t)m->vb);

    for (size_t i = 0; i < count8; ++i) {
        uint8x8x4_t bgra = vld4_u8(src + i * 32);
        uint8x8_t b = bgra.val[0];
        uint8x8_t g = bgra.val[1];
        uint8x8_t r = bgra.val[2];

        uint16x8_t y_acc = c128;
        y_acc = vmlal_u8(y_acc, r, vyr);
        y_acc = vmlal_u8(y_acc, g, vyg);
        y_acc = vmlal_u8(y_acc, b, vyb);
        uint8x8_t y = vadd_u8(vshrn_n_u16(y_acc, 8), c16);

        uint8x8x2_t y_uzp = vuzp_u8(y, y);
        uint8x8_t y_even = y_uzp.val[0];
        uint8x8_t y_odd  = y_uzp.val[1];

        uint8x8x2_t r_uzp = vuzp_u8(r, r);
        uint8x8x2_t g_uzp = vuzp_u8(g, g);
        uint8x8x2_t b_uzp = vuzp_u8(b, b);

        uint8x8_t r_avg8 = vhadd_u8(r_uzp.val[0], r_uzp.val[1]);
        uint8x8_t g_avg8 = vhadd_u8(g_uzp.val[0], g_uzp.val[1]);
        uint8x8_t b_avg8 = vhadd_u8(b_uzp.val[0], b_uzp.val[1]);

        int16x4_t r_avg = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(r_avg8)));
        int16x4_t g_avg = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(g_avg8)));
        int16x4_t b_avg = vreinterpret_s16_u16(vget_low_u16(vmovl_u8(b_avg8)));

        int16x4_t u_acc = vdup_n_s16(128);
        u_acc = vmls_s16(u_acc, r_avg, vur);
        u_acc = vmls_s16(u_acc, g_avg, vug);
        u_acc = vmla_s16(u_acc, b_avg, vub);
        int16x4_t u_val = vadd_s16(vshr_n_s16(u_acc, 8), vdup_n_s16(128));
        uint8x8_t u_sat = vqmovun_s16(vcombine_s16(u_val, u_val));

        int16x4_t v_acc = vdup_n_s16(128);
        v_acc = vmla_s16(v_acc, r_avg, vvr);
        v_acc = vmls_s16(v_acc, g_avg, vvg);
        v_acc = vmls_s16(v_acc, b_avg, vvb);
        int16x4_t v_val = vadd_s16(vshr_n_s16(v_acc, 8), vdup_n_s16(128));
        uint8x8_t v_sat = vqmovun_s16(vcombine_s16(v_val, v_val));

        uint8x8x2_t p0 = vzip_u8(u_sat, y_even);
        uint8x8x2_t p1 = vzip_u8(v_sat, y_odd);
        uint16x4x2_t res = vzip_u16(vreinterpret_u16_u8(p0.val[0]), vreinterpret_u16_u8(p1.val[0]));
        uint8x16_t out_vec = vcombine_u8(vreinterpret_u8_u16(res.val[0]), vreinterpret_u8_u16(res.val[1]));
        vst1q_u8(dst + i * 16, out_vec);
    }
}

static void neon_rgba_to_bgra_block(const uint8_t *src, uint8_t *dst, size_t num_pixels) {
    size_t count16 = num_pixels / 16;
    for (size_t i = 0; i < count16; ++i) {
        uint8x16x4_t rgba = vld4q_u8(src + i * 64);
        uint8x16_t r = rgba.val[0];
        rgba.val[0] = rgba.val[2];
        rgba.val[2] = r;
        vst4q_u8(dst + i * 64, rgba);
    }
}

static void neon_alpha_blend_rgba_block(const uint8_t *fg, const uint8_t *bg, uint8_t *out, size_t num_pixels) {
    size_t count8 = num_pixels / 8;
    const uint16x8_t vone = vdupq_n_u16(1);
    const uint8x8_t v255_u8 = vdup_n_u8(255);

    for (size_t i = 0; i < count8; ++i) {
        uint8x8x4_t f_px = vld4_u8(fg + i * 32);
        uint8x8x4_t b_px = vld4_u8(bg + i * 32);

        uint8x8_t a = f_px.val[3];
        uint8x8_t inv_a = vsub_u8(v255_u8, a);
        uint8x8_t a_zero = vceq_u8(a, vdup_n_u8(0));

        #define NEON_BLEND_CHANNEL(idx) do { \
            uint16x8_t prod = vmull_u8(f_px.val[idx], a); \
            prod = vmlal_u8(prod, b_px.val[idx], inv_a); \
            uint16x8_t prod_s = vshrq_n_u16(prod, 8); \
            uint16x8_t div = vshrq_n_u16(vaddq_u16(vaddq_u16(prod, prod_s), vone), 8); \
            uint8x8_t blended = vmovn_u16(div); \
            out_px.val[idx] = vbsl_u8(a_zero, b_px.val[idx], blended); \
        } while(0)

        uint8x8x4_t out_px;
        NEON_BLEND_CHANNEL(0);
        NEON_BLEND_CHANNEL(1);
        NEON_BLEND_CHANNEL(2);
        #undef NEON_BLEND_CHANNEL

        out_px.val[3] = vbsl_u8(a_zero, b_px.val[3], v255_u8);
        vst4_u8(out + i * 32, out_px);
    }
}

static inline void neon_transpose4x4_32(int32x4_t *q0, int32x4_t *q1, int32x4_t *q2, int32x4_t *q3) {
    int32x4x2_t t0 = vtrnq_s32(*q0, *q1);
    int32x4x2_t t1 = vtrnq_s32(*q2, *q3);
    *q0 = vcombine_s32(vget_low_s32(t0.val[0]), vget_low_s32(t1.val[0]));
    *q1 = vcombine_s32(vget_low_s32(t0.val[1]), vget_low_s32(t1.val[1]));
    *q2 = vcombine_s32(vget_high_s32(t0.val[0]), vget_high_s32(t1.val[0]));
    *q3 = vcombine_s32(vget_high_s32(t0.val[1]), vget_high_s32(t1.val[1]));
}

static inline void neon_transpose8x8_32(int32x4_t r_lo[8], int32x4_t r_hi[8]) {
    neon_transpose4x4_32(&r_lo[0], &r_lo[1], &r_lo[2], &r_lo[3]);
    neon_transpose4x4_32(&r_hi[0], &r_hi[1], &r_hi[2], &r_hi[3]);
    neon_transpose4x4_32(&r_lo[4], &r_lo[5], &r_lo[6], &r_lo[7]);
    neon_transpose4x4_32(&r_hi[4], &r_hi[5], &r_hi[6], &r_hi[7]);

    for (int i = 0; i < 4; ++i) {
        int32x4_t tmp = r_hi[i];
        r_hi[i] = r_lo[4 + i];
        r_lo[4 + i] = tmp;
    }
}

static void neon_fdct_8x8_impl(const int16_t in[64], int16_t out[64]) {
    int32x4_t r_lo[8], r_hi[8];
    for (int r = 0; r < 8; ++r) {
        int16x8_t q = vld1q_s16(&in[r * 8]);
        r_lo[r] = vmovl_s16(vget_low_s16(q));
        r_hi[r] = vmovl_s16(vget_high_s16(q));
    }

    /* Transpose so r[0..7] hold columns across rows */
    neon_transpose8x8_32(r_lo, r_hi);

    /* Pass 1: 1D FDCT on rows (executed vertically across both column halves) */
    int32x4_t t_lo[8], t_hi[8];
    #define NEON_FDCT_PASS1_HALF(in_r, out_t) do { \
        int32x4_t a0 = vaddq_s32(in_r[0], in_r[7]); \
        int32x4_t a1 = vaddq_s32(in_r[1], in_r[6]); \
        int32x4_t a2 = vaddq_s32(in_r[2], in_r[5]); \
        int32x4_t a3 = vaddq_s32(in_r[3], in_r[4]); \
        int32x4_t s0 = vsubq_s32(in_r[0], in_r[7]); \
        int32x4_t s1 = vsubq_s32(in_r[1], in_r[6]); \
        int32x4_t s2 = vsubq_s32(in_r[2], in_r[5]); \
        int32x4_t s3 = vsubq_s32(in_r[3], in_r[4]); \
        int32x4_t b0 = vaddq_s32(a0, a3); \
        int32x4_t b1 = vaddq_s32(a1, a2); \
        int32x4_t b2 = vsubq_s32(a1, a2); \
        int32x4_t b3 = vsubq_s32(a0, a3); \
        out_t[0] = vshlq_n_s32(vaddq_s32(b0, b1), 3); \
        out_t[4] = vshlq_n_s32(vsubq_s32(b0, b1), 3); \
        out_t[2] = vshrq_n_s32(vaddq_s32(vmulq_n_s32(b3, 181), vmulq_n_s32(b2, 181)), 5); \
        out_t[6] = vshrq_n_s32(vsubq_s32(vmulq_n_s32(b3, 181), vmulq_n_s32(b2, 181)), 5); \
        out_t[1] = vshrq_n_s32(vaddq_s32(vaddq_s32(vmulq_n_s32(s0, 251), vmulq_n_s32(s1, 213)), vaddq_s32(vmulq_n_s32(s2, 142), vmulq_n_s32(s3, 49))), 5); \
        out_t[3] = vshrq_n_s32(vsubq_s32(vsubq_s32(vmulq_n_s32(s0, 213), vmulq_n_s32(s1, 49)), vaddq_s32(vmulq_n_s32(s2, 251), vmulq_n_s32(s3, 142))), 5); \
        out_t[5] = vshrq_n_s32(vaddq_s32(vsubq_s32(vmulq_n_s32(s0, 142), vmulq_n_s32(s1, 251)), vaddq_s32(vmulq_n_s32(s2, 49), vmulq_n_s32(s3, 213))), 5); \
        out_t[7] = vshrq_n_s32(vsubq_s32(vaddq_s32(vsubq_s32(vmulq_n_s32(s0, 49), vmulq_n_s32(s1, 142)), vmulq_n_s32(s2, 213)), vmulq_n_s32(s3, 251)), 5); \
    } while(0)

    NEON_FDCT_PASS1_HALF(r_lo, t_lo);
    NEON_FDCT_PASS1_HALF(r_hi, t_hi);
    #undef NEON_FDCT_PASS1_HALF

    /* Transpose intermediate so t[0..7] hold rows across columns */
    neon_transpose8x8_32(t_lo, t_hi);

    /* Pass 2: 1D FDCT on columns */
    #define NEON_FDCT_PASS2_HALF(in_t, offset) do { \
        int32x4_t a0 = vaddq_s32(in_t[0], in_t[7]); \
        int32x4_t a1 = vaddq_s32(in_t[1], in_t[6]); \
        int32x4_t a2 = vaddq_s32(in_t[2], in_t[5]); \
        int32x4_t a3 = vaddq_s32(in_t[3], in_t[4]); \
        int32x4_t s0 = vsubq_s32(in_t[0], in_t[7]); \
        int32x4_t s1 = vsubq_s32(in_t[1], in_t[6]); \
        int32x4_t s2 = vsubq_s32(in_t[2], in_t[5]); \
        int32x4_t s3 = vsubq_s32(in_t[3], in_t[4]); \
        int32x4_t b0 = vaddq_s32(a0, a3); \
        int32x4_t b1 = vaddq_s32(a1, a2); \
        int32x4_t b2 = vsubq_s32(a1, a2); \
        int32x4_t b3 = vsubq_s32(a0, a3); \
        int32x4_t v16 = vdupq_n_s32(16); \
        int32x4_t o0 = vshrq_n_s32(vaddq_s32(vaddq_s32(b0, b1), v16), 5); \
        int32x4_t o4 = vshrq_n_s32(vaddq_s32(vsubq_s32(b0, b1), v16), 5); \
        int32x4_t o2 = vshrq_n_s32(vaddq_s32(vshrq_n_s32(vaddq_s32(vmulq_n_s32(b3, 181), vmulq_n_s32(b2, 181)), 8), v16), 5); \
        int32x4_t o6 = vshrq_n_s32(vaddq_s32(vshrq_n_s32(vsubq_s32(vmulq_n_s32(b3, 181), vmulq_n_s32(b2, 181)), 8), v16), 5); \
        int32x4_t o1 = vshrq_n_s32(vaddq_s32(vshrq_n_s32(vaddq_s32(vaddq_s32(vmulq_n_s32(s0, 251), vmulq_n_s32(s1, 213)), vaddq_s32(vmulq_n_s32(s2, 142), vmulq_n_s32(s3, 49))), 8), v16), 5); \
        int32x4_t o3 = vshrq_n_s32(vaddq_s32(vshrq_n_s32(vsubq_s32(vsubq_s32(vmulq_n_s32(s0, 213), vmulq_n_s32(s1, 49)), vaddq_s32(vmulq_n_s32(s2, 251), vmulq_n_s32(s3, 142))), 8), v16), 5); \
        int32x4_t o5 = vshrq_n_s32(vaddq_s32(vshrq_n_s32(vaddq_s32(vsubq_s32(vmulq_n_s32(s0, 142), vmulq_n_s32(s1, 251)), vaddq_s32(vmulq_n_s32(s2, 49), vmulq_n_s32(s3, 213))), 8), v16), 5); \
        int32x4_t o7 = vshrq_n_s32(vaddq_s32(vshrq_n_s32(vsubq_s32(vaddq_s32(vsubq_s32(vmulq_n_s32(s0, 49), vmulq_n_s32(s1, 142)), vmulq_n_s32(s2, 213)), vmulq_n_s32(s3, 251)), 8), v16), 5); \
        /* Truncate (not saturate) to match scalar (int16_t) */ \
        int16x4_t s_o0 = vmovn_s32(o0); \
        int16x4_t s_o1 = vmovn_s32(o1); \
        int16x4_t s_o2 = vmovn_s32(o2); \
        int16x4_t s_o3 = vmovn_s32(o3); \
        int16x4_t s_o4 = vmovn_s32(o4); \
        int16x4_t s_o5 = vmovn_s32(o5); \
        int16x4_t s_o6 = vmovn_s32(o6); \
        int16x4_t s_o7 = vmovn_s32(o7); \
        vst1_s16(&out[0 * 8 + offset], s_o0); \
        vst1_s16(&out[1 * 8 + offset], s_o1); \
        vst1_s16(&out[2 * 8 + offset], s_o2); \
        vst1_s16(&out[3 * 8 + offset], s_o3); \
        vst1_s16(&out[4 * 8 + offset], s_o4); \
        vst1_s16(&out[5 * 8 + offset], s_o5); \
        vst1_s16(&out[6 * 8 + offset], s_o6); \
        vst1_s16(&out[7 * 8 + offset], s_o7); \
    } while(0)

    NEON_FDCT_PASS2_HALF(t_lo, 0);
    NEON_FDCT_PASS2_HALF(t_hi, 4);
    #undef NEON_FDCT_PASS2_HALF
}

static void neon_idct_8x8_put_impl(const int16_t in[64], uint8_t *dest, int stride) {
    int32x4_t x_lo[8], x_hi[8];
    for (int r = 0; r < 8; ++r) {
        int16x8_t q = vld1q_s16(&in[r * 8]);
        x_lo[r] = vmovl_s16(vget_low_s16(q));
        x_hi[r] = vmovl_s16(vget_high_s16(q));
    }

    /* Pass 1: 1D IDCT on columns */
    int32x4_t t_lo[8], t_hi[8];
    #define NEON_IDCT_PASS1_HALF(x, t) do { \
        int32x4_t a0 = vshlq_n_s32(vaddq_s32(x[0], x[4]), 3); \
        int32x4_t a1 = vshlq_n_s32(vsubq_s32(x[0], x[4]), 3); \
        int32x4_t a2 = vshrq_n_s32(vsubq_s32(vmulq_n_s32(x[2], 181), vmulq_n_s32(x[6], 181)), 5); \
        int32x4_t a3 = vshrq_n_s32(vaddq_s32(vmulq_n_s32(x[2], 181), vmulq_n_s32(x[6], 181)), 5); \
        int32x4_t b0 = vaddq_s32(a0, a3); \
        int32x4_t b1 = vaddq_s32(a1, a2); \
        int32x4_t b2 = vsubq_s32(a1, a2); \
        int32x4_t b3 = vsubq_s32(a0, a3); \
        int32x4_t s0 = vshrq_n_s32(vaddq_s32(vaddq_s32(vmulq_n_s32(x[1], 251), vmulq_n_s32(x[3], 213)), vaddq_s32(vmulq_n_s32(x[5], 142), vmulq_n_s32(x[7], 49))), 5); \
        int32x4_t s1 = vshrq_n_s32(vsubq_s32(vsubq_s32(vmulq_n_s32(x[1], 213), vmulq_n_s32(x[3], 49)), vaddq_s32(vmulq_n_s32(x[5], 251), vmulq_n_s32(x[7], 142))), 5); \
        int32x4_t s2 = vshrq_n_s32(vaddq_s32(vsubq_s32(vmulq_n_s32(x[1], 142), vmulq_n_s32(x[3], 251)), vaddq_s32(vmulq_n_s32(x[5], 49), vmulq_n_s32(x[7], 213))), 5); \
        int32x4_t s3 = vshrq_n_s32(vsubq_s32(vaddq_s32(vsubq_s32(vmulq_n_s32(x[1], 49), vmulq_n_s32(x[3], 142)), vmulq_n_s32(x[5], 213)), vmulq_n_s32(x[7], 251)), 5); \
        t[0] = vaddq_s32(b0, s0); \
        t[7] = vsubq_s32(b0, s0); \
        t[1] = vaddq_s32(b1, s1); \
        t[6] = vsubq_s32(b1, s1); \
        t[2] = vaddq_s32(b2, s2); \
        t[5] = vsubq_s32(b2, s2); \
        t[3] = vaddq_s32(b3, s3); \
        t[4] = vsubq_s32(b3, s3); \
    } while(0)

    NEON_IDCT_PASS1_HALF(x_lo, t_lo);
    NEON_IDCT_PASS1_HALF(x_hi, t_hi);
    #undef NEON_IDCT_PASS1_HALF

    /* Transpose so t[0..7] hold columns across rows for Pass 2 */
    neon_transpose8x8_32(t_lo, t_hi);

    /* Pass 2: 1D IDCT on rows */
    int32x4_t o_lo[8], o_hi[8];
    #define NEON_IDCT_PASS2_HALF(t, o) do { \
        int32x4_t ra0 = vaddq_s32(t[0], t[4]); \
        int32x4_t ra1 = vsubq_s32(t[0], t[4]); \
        int32x4_t ra2 = vshrq_n_s32(vsubq_s32(vmulq_n_s32(t[2], 181), vmulq_n_s32(t[6], 181)), 8); \
        int32x4_t ra3 = vshrq_n_s32(vaddq_s32(vmulq_n_s32(t[2], 181), vmulq_n_s32(t[6], 181)), 8); \
        int32x4_t rb0 = vaddq_s32(ra0, ra3); \
        int32x4_t rb1 = vaddq_s32(ra1, ra2); \
        int32x4_t rb2 = vsubq_s32(ra1, ra2); \
        int32x4_t rb3 = vsubq_s32(ra0, ra3); \
        int32x4_t rs0 = vshrq_n_s32(vaddq_s32(vaddq_s32(vmulq_n_s32(t[1], 251), vmulq_n_s32(t[3], 213)), vaddq_s32(vmulq_n_s32(t[5], 142), vmulq_n_s32(t[7], 49))), 8); \
        int32x4_t rs1 = vshrq_n_s32(vsubq_s32(vsubq_s32(vmulq_n_s32(t[1], 213), vmulq_n_s32(t[3], 49)), vaddq_s32(vmulq_n_s32(t[5], 251), vmulq_n_s32(t[7], 142))), 8); \
        int32x4_t rs2 = vshrq_n_s32(vaddq_s32(vsubq_s32(vmulq_n_s32(t[1], 142), vmulq_n_s32(t[3], 251)), vaddq_s32(vmulq_n_s32(t[5], 49), vmulq_n_s32(t[7], 213))), 8); \
        int32x4_t rs3 = vshrq_n_s32(vsubq_s32(vaddq_s32(vsubq_s32(vmulq_n_s32(t[1], 49), vmulq_n_s32(t[3], 142)), vmulq_n_s32(t[5], 213)), vmulq_n_s32(t[7], 251)), 8); \
        int32x4_t v64 = vdupq_n_s32(64); \
        o[0] = vshrq_n_s32(vaddq_s32(vaddq_s32(rb0, rs0), v64), 7); \
        o[7] = vshrq_n_s32(vaddq_s32(vsubq_s32(rb0, rs0), v64), 7); \
        o[1] = vshrq_n_s32(vaddq_s32(vaddq_s32(rb1, rs1), v64), 7); \
        o[6] = vshrq_n_s32(vaddq_s32(vsubq_s32(rb1, rs1), v64), 7); \
        o[2] = vshrq_n_s32(vaddq_s32(vaddq_s32(rb2, rs2), v64), 7); \
        o[5] = vshrq_n_s32(vaddq_s32(vsubq_s32(rb2, rs2), v64), 7); \
        o[3] = vshrq_n_s32(vaddq_s32(vaddq_s32(rb3, rs3), v64), 7); \
        o[4] = vshrq_n_s32(vaddq_s32(vsubq_s32(rb3, rs3), v64), 7); \
    } while(0)

    NEON_IDCT_PASS2_HALF(t_lo, o_lo);
    NEON_IDCT_PASS2_HALF(t_hi, o_hi);
    #undef NEON_IDCT_PASS2_HALF

    /* Transpose o_lo, o_hi so register r contains row r */
    neon_transpose8x8_32(o_lo, o_hi);

    /* Clamp and store */
    for (int r = 0; r < 8; ++r) {
        int16x4_t p16_lo = vqmovn_s32(o_lo[r]);
        int16x4_t p16_hi = vqmovn_s32(o_hi[r]);
        int16x8_t p16 = vcombine_s16(p16_lo, p16_hi);
        uint8x8_t p8 = vqmovun_s16(p16);
        vst1_u8(&dest[r * stride], p8);
    }
}

#endif /* HAS_ARM_NEON */

/* ========================================================================= */
/* 4. DYNAMIC CPU DETECTION & DISPATCH LOGIC                                 */
/* ========================================================================= */

#if !defined(AVX_OS_WINDOWS) || defined(__MINGW32__) || defined(__MINGW64__)
static pthread_once_t g_simd_once = PTHREAD_ONCE_INIT;
#else
static INIT_ONCE g_simd_once = INIT_ONCE_STATIC_INIT;
#endif
static avx_simd_type_e g_simd_type = AVX_SIMD_NONE;

/*
 * Thread-safe one-time CPU capability detector. Queries CPUID for AVX2 and FMA
 * on x86, or enables NEON on ARMv8-A. Cached globally for zero-overhead hot loops.
 */
static void detect_cpu_caps_init(void) {
#if HAS_X86_SIMD
    #if defined(__GNUC__) || defined(__clang__)
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        g_simd_type = AVX_SIMD_AVX2;
    }
    #elif defined(_MSC_VER)
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 0);
    int nIds = cpuInfo[0];
    if (nIds >= 7) {
        int cpuInfo7[4] = {0};
        __cpuidex(cpuInfo7, 7, 0);
        bool has_avx2 = (cpuInfo7[1] & (1 << 5)) != 0;
        int cpuInfo1[4] = {0};
        __cpuid(cpuInfo1, 1);
        bool has_fma = (cpuInfo1[2] & (1 << 12)) != 0;
        bool has_osxsave = (cpuInfo1[2] & (1 << 27)) != 0;
        if (has_avx2 && has_fma && has_osxsave) {
            g_simd_type = AVX_SIMD_AVX2;
        }
    }
    #endif
#elif HAS_ARM_NEON
    g_simd_type = AVX_SIMD_NEON;
#endif
}

#if defined(AVX_OS_WINDOWS) && !defined(__MINGW32__) && !defined(__MINGW64__)
static BOOL CALLBACK simd_once_cb(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce; (void)Parameter; (void)Context;
    detect_cpu_caps_init();
    return TRUE;
}
#endif

static inline void detect_cpu_caps(void) {
#if !defined(AVX_OS_WINDOWS) || defined(__MINGW32__) || defined(__MINGW64__)
    pthread_once(&g_simd_once, detect_cpu_caps_init);
#else
    InitOnceExecuteOnce(&g_simd_once, simd_once_cb, NULL, NULL);
#endif
}

avx_simd_type_e avx_simd_get_caps(void) {
    detect_cpu_caps();
    return g_simd_type;
}

const char* avx_simd_get_name(void) {
    detect_cpu_caps();
    switch (g_simd_type) {
        case AVX_SIMD_AVX2: return "AVX2+FMA (Hand-Vectorized)";
        case AVX_SIMD_NEON: return "ARM NEON (Hand-Vectorized)";
        default:            return "Portable C99 (Scalar Fallback)";
    }
}

/* ========================================================================= */
/* 5. DISPATCHED HIGH-PERFORMANCE ENTRY POINTS                               */
/* ========================================================================= */

/*
 * Dispatches UYVY 4:2:2 to RGBA conversion using AVX2 or NEON in 8-pixel blocks.
 * Non-multiple-of-8 scanline tails are processed via scalar fallback.
 */
void avx_simd_uyvy_to_rgba(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    detect_cpu_caps();
    const avx_yuv_matrix_t *m = select_yuv_matrix(width, height);

#if HAS_X86_SIMD
    if (g_simd_type == AVX_SIMD_AVX2) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        avx2_uyvy_to_rgba_block(src, dst, vector_pixels, m);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            scalar_uyvy_to_rgbx_matrix(src + vector_pixels * 2, dst + vector_pixels * 4, (int)rem, 1, 0, 2, m);
        }
        return;
    }
#elif HAS_ARM_NEON
    if (g_simd_type == AVX_SIMD_NEON) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        neon_uyvy_to_rgba_block(src, dst, vector_pixels, m);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            scalar_uyvy_to_rgbx_matrix(src + vector_pixels * 2, dst + vector_pixels * 4, (int)rem, 1, 0, 2, m);
        }
        return;
    }
#endif

    scalar_uyvy_to_rgbx_matrix(src, dst, width, height, 0, 2, m);
}

void avx_simd_uyvy_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    detect_cpu_caps();
    const avx_yuv_matrix_t *m = select_yuv_matrix(width, height);

#if HAS_X86_SIMD
    if (g_simd_type == AVX_SIMD_AVX2) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        avx2_uyvy_to_bgra_block(src, dst, vector_pixels, m);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            scalar_uyvy_to_rgbx_matrix(src + vector_pixels * 2, dst + vector_pixels * 4, (int)rem, 1, 2, 0, m);
        }
        return;
    }
#elif HAS_ARM_NEON
    if (g_simd_type == AVX_SIMD_NEON) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        neon_uyvy_to_bgra_block(src, dst, vector_pixels, m);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            scalar_uyvy_to_rgbx_matrix(src + vector_pixels * 2, dst + vector_pixels * 4, (int)rem, 1, 2, 0, m);
        }
        return;
    }
#endif

    scalar_uyvy_to_rgbx_matrix(src, dst, width, height, 2, 0, m);
}

/*
 * Dispatches RGBA to UYVY 4:2:2 conversion. Uses SIMD matrix integer math for
 * BT.709 (HD) or BT.601 (SD) color conversion with horizontal chroma sub-sampling.
 */
void avx_simd_rgba_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    detect_cpu_caps();
    const avx_yuv_matrix_t *m = select_yuv_matrix(width, height);

#if HAS_X86_SIMD
    if (g_simd_type == AVX_SIMD_AVX2) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        avx2_rgba_to_uyvy_block(src, dst, vector_pixels, m);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            scalar_rgbx_to_uyvy_matrix(src + vector_pixels * 4, dst + vector_pixels * 2, (int)rem, 1, 0, 2, m);
        }
        return;
    }
#elif HAS_ARM_NEON
    if (g_simd_type == AVX_SIMD_NEON) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        neon_rgba_to_uyvy_block(src, dst, vector_pixels, m);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            scalar_rgbx_to_uyvy_matrix(src + vector_pixels * 4, dst + vector_pixels * 2, (int)rem, 1, 0, 2, m);
        }
        return;
    }
#endif

    scalar_rgbx_to_uyvy_matrix(src, dst, width, height, 0, 2, m);
}

void avx_simd_bgra_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    detect_cpu_caps();
    const avx_yuv_matrix_t *m = select_yuv_matrix(width, height);

#if HAS_X86_SIMD
    if (g_simd_type == AVX_SIMD_AVX2) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        avx2_bgra_to_uyvy_block(src, dst, vector_pixels, m);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            scalar_rgbx_to_uyvy_matrix(src + vector_pixels * 4, dst + vector_pixels * 2, (int)rem, 1, 2, 0, m);
        }
        return;
    }
#elif HAS_ARM_NEON
    if (g_simd_type == AVX_SIMD_NEON) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        neon_bgra_to_uyvy_block(src, dst, vector_pixels, m);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            scalar_rgbx_to_uyvy_matrix(src + vector_pixels * 4, dst + vector_pixels * 2, (int)rem, 1, 2, 0, m);
        }
        return;
    }
#endif

    scalar_rgbx_to_uyvy_matrix(src, dst, width, height, 2, 0, m);
}

void avx_simd_rgba_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    detect_cpu_caps();

#if HAS_X86_SIMD
    if (g_simd_type == AVX_SIMD_AVX2) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        avx2_rgba_to_bgra_block(src, dst, vector_pixels);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            avx_scalar_rgba_to_bgra(src + vector_pixels * 4, dst + vector_pixels * 4, (int)rem, 1);
        }
        return;
    }
#elif HAS_ARM_NEON
    if (g_simd_type == AVX_SIMD_NEON) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 16) * 16;
        neon_rgba_to_bgra_block(src, dst, vector_pixels);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            avx_scalar_rgba_to_bgra(src + vector_pixels * 4, dst + vector_pixels * 4, (int)rem, 1);
        }
        return;
    }
#endif

    avx_scalar_rgba_to_bgra(src, dst, width, height);
}

/*
 * Dispatches Porter-Duff Over alpha compositing of two RGBA images using
 * vectorized 16-bit integer multiplication and rounding.
 */
void avx_simd_alpha_blend_rgba(const uint8_t *fg, const uint8_t *bg, uint8_t *out, int width, int height) {
    if (!fg || !bg || !out || width <= 0 || height <= 0) return;
    detect_cpu_caps();

#if HAS_X86_SIMD
    if (g_simd_type == AVX_SIMD_AVX2) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        avx2_alpha_blend_rgba_block(fg, bg, out, vector_pixels);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            avx_scalar_alpha_blend_rgba(fg + vector_pixels * 4, bg + vector_pixels * 4, out + vector_pixels * 4, (int)rem, 1);
        }
        return;
    }
#elif HAS_ARM_NEON
    if (g_simd_type == AVX_SIMD_NEON) {
        size_t total_pixels = (size_t)width * height;
        size_t vector_pixels = (total_pixels / 8) * 8;
        neon_alpha_blend_rgba_block(fg, bg, out, vector_pixels);

        size_t rem = total_pixels - vector_pixels;
        if (rem > 0) {
            avx_scalar_alpha_blend_rgba(fg + vector_pixels * 4, bg + vector_pixels * 4, out + vector_pixels * 4, (int)rem, 1);
        }
        return;
    }
#endif

    avx_scalar_alpha_blend_rgba(fg, bg, out, width, height);
}

/* Dispatches 2D 8x8 forward DCT using vectorized AAN algorithm with matrix transpose. */
void avx_simd_fdct_8x8(const int16_t in[64], int16_t out[64]) {
    detect_cpu_caps();

#if HAS_X86_SIMD
    if (g_simd_type == AVX_SIMD_AVX2) {
        avx2_fdct_8x8_impl(in, out);
        return;
    }
#elif HAS_ARM_NEON
    if (g_simd_type == AVX_SIMD_NEON) {
        neon_fdct_8x8_impl(in, out);
        return;
    }
#endif

    avx_scalar_fdct_8x8(in, out);
}

/* Dispatches 2D 8x8 inverse DCT with IDCT output clamped and written directly to destination. */
void avx_simd_idct_8x8_put(const int16_t in[64], uint8_t *dest, int stride) {
    detect_cpu_caps();

#if HAS_X86_SIMD
    if (g_simd_type == AVX_SIMD_AVX2) {
        avx2_idct_8x8_put_impl(in, dest, stride);
        return;
    }
#elif HAS_ARM_NEON
    if (g_simd_type == AVX_SIMD_NEON) {
        neon_idct_8x8_put_impl(in, dest, stride);
        return;
    }
#endif

    avx_scalar_idct_8x8_put(in, dest, stride);
}
