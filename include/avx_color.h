/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_color.h - Color format conversion and alpha blending
 */

#ifndef AVX_COLOR_H
#define AVX_COLOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Converts packed 4:2:2 UYVY to 32-bit packed RGBA (Alpha = 255).
 */
void avx_color_uyvy_to_rgba(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Converts packed 4:2:2 UYVY to 32-bit packed BGRA (Alpha = 255).
 */
void avx_color_uyvy_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Converts 32-bit packed RGBA to packed 4:2:2 UYVY.
 */
void avx_color_rgba_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Converts 32-bit packed BGRA to packed 4:2:2 UYVY.
 */
void avx_color_bgra_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * In-place or copy conversion swapping Red and Blue channels (RGBA <-> BGRA).
 */
void avx_color_rgba_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Composites a transparent foreground RGBA image over a background RGBA image.
 */
void avx_color_alpha_blend_rgba(const uint8_t *fg, const uint8_t *bg, uint8_t *out, int width, int height);

/**
 * Downconverts 16-bit P216 (semi-planar Y + interleaved UV) to packed 8-bit UYVY.
 */
void avx_color_p216_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Upconverts packed 8-bit UYVY to 16-bit P216 (semi-planar Y + interleaved UV).
 */
void avx_color_uyvy_to_p216(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Converts 16-bit P216 (semi-planar Y + interleaved UV) to packed 32-bit RGBA.
 */
void avx_color_p216_to_rgba(const uint8_t *src, uint8_t *dst, int width, int height);

/**
 * Converts packed 4:2:2 UYVY + separate 8-bit Alpha plane (UYVA) to 32-bit packed RGBA.
 */
void avx_color_uyva_to_rgba(const uint8_t *uyvy, const uint8_t *alpha, uint8_t *dst, int width, int height);

/**
 * Converts packed 4:2:2 UYVY + separate 8-bit Alpha plane (UYVA) to 32-bit packed BGRA.
 */
void avx_color_uyva_to_bgra(const uint8_t *uyvy, const uint8_t *alpha, uint8_t *dst, int width, int height);

/**
 * Converts 32-bit packed RGBA to packed 4:2:2 UYVY + separate 8-bit Alpha plane (UYVA).
 */
void avx_color_rgba_to_uyva(const uint8_t *src, uint8_t *uyvy, uint8_t *alpha, int width, int height);

/**
 * Converts NV12 (semi-planar Y + interleaved UV 4:2:0) to packed 4:2:2 UYVY.
 */
void avx_color_nv12_to_uyvy(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *uv_src, size_t uv_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height);

/**
 * Converts NV12 (semi-planar Y + interleaved UV 4:2:0) to 32-bit packed RGBA.
 */
void avx_color_nv12_to_rgba(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *uv_src, size_t uv_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height);

/**
 * Converts NV12 (semi-planar Y + interleaved UV 4:2:0) to 32-bit packed BGRA.
 */
void avx_color_nv12_to_bgra(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *uv_src, size_t uv_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height);

/**
 * Converts I420 (planar Y + U + V 4:2:0) to packed 4:2:2 UYVY.
 */
void avx_color_i420_to_uyvy(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *u_src, size_t u_stride,
                            const uint8_t *v_src, size_t v_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height);

/**
 * Converts I420 (planar Y + U + V 4:2:0) to 32-bit packed RGBA.
 */
void avx_color_i420_to_rgba(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *u_src, size_t u_stride,
                            const uint8_t *v_src, size_t v_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height);

/**
 * Converts packed 4:2:2 UYVY to NV12 (semi-planar Y + interleaved UV 4:2:0).
 */
void avx_color_uyvy_to_nv12(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *uv_dst, size_t uv_stride,
                            int width, int height);

/**
 * Converts packed 4:2:2 UYVY to I420 (planar Y + U + V 4:2:0).
 */
void avx_color_uyvy_to_i420(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *u_dst, size_t u_stride,
                            uint8_t *v_dst, size_t v_stride,
                            int width, int height);

/**
 * Converts packed 4:2:2 UYVY to YV12 (planar Y + V + U 4:2:0).
 */
void avx_color_uyvy_to_yv12(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *v_dst, size_t v_stride,
                            uint8_t *u_dst, size_t u_stride,
                            int width, int height);

/**
 * Converts YV12 (planar Y + V + U 4:2:0) to packed 4:2:2 UYVY.
 */
void avx_color_yv12_to_uyvy(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *v_src, size_t v_stride,
                            const uint8_t *u_src, size_t u_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height);

/**
 * Converts YV12 (planar Y + V + U 4:2:0) to 32-bit packed RGBA.
 */
void avx_color_yv12_to_rgba(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *v_src, size_t v_stride,
                            const uint8_t *u_src, size_t u_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height);

/**
 * Converts 32-bit packed RGBA directly to NV12 (semi-planar Y + interleaved UV 4:2:0).
 */
void avx_color_rgba_to_nv12(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *uv_dst, size_t uv_stride,
                            int width, int height);

/**
 * Converts 32-bit packed RGBA directly to I420 (planar Y + U + V 4:2:0).
 */
void avx_color_rgba_to_i420(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *u_dst, size_t u_stride,
                            uint8_t *v_dst, size_t v_stride,
                            int width, int height);

/**
 * Converts 32-bit packed RGBA directly to YV12 (planar Y + V + U 4:2:0).
 */
void avx_color_rgba_to_yv12(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *v_dst, size_t v_stride,
                            uint8_t *u_dst, size_t u_stride,
                            int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* AVX_COLOR_H */
