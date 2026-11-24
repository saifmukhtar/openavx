/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_simd.h - SIMD color conversion, DCT, and audio kernels
 */

#ifndef AVX_SIMD_H
#define AVX_SIMD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AVX_SIMD_NONE = 0,
    AVX_SIMD_AVX2 = 1,
    AVX_SIMD_NEON = 2
} avx_simd_type_e;

/**
 * Returns the currently active SIMD backend (detected dynamically at runtime).
 */
avx_simd_type_e avx_simd_get_caps(void);

/**
 * Returns human-readable name of active SIMD backend ("AVX2+FMA", "ARM NEON", "Portable C99").
 */
const char* avx_simd_get_name(void);

/* SIMD Accelerated Color Conversion Routines */
void avx_simd_uyvy_to_rgba(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_simd_uyvy_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_simd_rgba_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_simd_bgra_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_simd_rgba_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_simd_alpha_blend_rgba(const uint8_t *fg, const uint8_t *bg, uint8_t *out, int width, int height);

/* SIMD Accelerated SpeedHQ-2 8x8 DCT Transforms */
void avx_simd_fdct_8x8(const int16_t in[64], int16_t out[64]);
void avx_simd_idct_8x8_put(const int16_t in[64], uint8_t *dest, int stride);

/* Scalar Reference Implementations (for benchmark comparison & verification) */
void avx_scalar_uyvy_to_rgba(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_scalar_uyvy_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_scalar_rgba_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_scalar_bgra_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_scalar_rgba_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height);
void avx_scalar_alpha_blend_rgba(const uint8_t *fg, const uint8_t *bg, uint8_t *out, int width, int height);
void avx_scalar_fdct_8x8(const int16_t in[64], int16_t out[64]);
void avx_scalar_idct_8x8_put(const int16_t in[64], uint8_t *dest, int stride);

#ifdef __cplusplus
}
#endif

#endif /* AVX_SIMD_H */
