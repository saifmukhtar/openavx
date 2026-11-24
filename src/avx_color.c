/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_color.c - Color format conversion and alpha blending
 */

#include "avx_color.h"
#include "avx_simd.h"

void avx_color_uyvy_to_rgba(const uint8_t *src, uint8_t *dst, int width, int height) {
    avx_simd_uyvy_to_rgba(src, dst, width, height);
}

void avx_color_uyvy_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height) {
    avx_simd_uyvy_to_bgra(src, dst, width, height);
}

void avx_color_rgba_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height) {
    avx_simd_rgba_to_uyvy(src, dst, width, height);
}

void avx_color_bgra_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height) {
    avx_simd_bgra_to_uyvy(src, dst, width, height);
}

void avx_color_rgba_to_bgra(const uint8_t *src, uint8_t *dst, int width, int height) {
    avx_simd_rgba_to_bgra(src, dst, width, height);
}

void avx_color_alpha_blend_rgba(const uint8_t *fg, const uint8_t *bg, uint8_t *out, int width, int height) {
    avx_simd_alpha_blend_rgba(fg, bg, out, width, height);
}

/* Unpacks 16-bit planar P216 to 8-bit packed UYVY, discarding lower 8 bits of precision. */
void avx_color_p216_to_uyvy(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    const uint16_t *src_y = (const uint16_t*)src;
    const uint16_t *src_uv = (const uint16_t*)(src + (size_t)width * height * 2);
    size_t num_pairs = ((size_t)width * height) / 2;

    for (size_t i = 0; i < num_pairs; ++i) {
        uint8_t y0 = (uint8_t)(src_y[2 * i + 0] >> 8);
        uint8_t y1 = (uint8_t)(src_y[2 * i + 1] >> 8);
        uint8_t u  = (uint8_t)(src_uv[2 * i + 0] >> 8);
        uint8_t v  = (uint8_t)(src_uv[2 * i + 1] >> 8);

        dst[4 * i + 0] = u;
        dst[4 * i + 1] = y0;
        dst[4 * i + 2] = v;
        dst[4 * i + 3] = y1;
    }
}

/* Expands 8-bit packed UYVY to 16-bit planar P216, replicating 8-bit values into both bytes. */
void avx_color_uyvy_to_p216(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    uint16_t *dst_y = (uint16_t*)dst;
    uint16_t *dst_uv = (uint16_t*)(dst + (size_t)width * height * 2);
    size_t num_pairs = ((size_t)width * height) / 2;

    for (size_t i = 0; i < num_pairs; ++i) {
        uint8_t u  = src[4 * i + 0];
        uint8_t y0 = src[4 * i + 1];
        uint8_t v  = src[4 * i + 2];
        uint8_t y1 = src[4 * i + 3];

        dst_y[2 * i + 0]  = (uint16_t)((y0 << 8) | y0);
        dst_y[2 * i + 1]  = (uint16_t)((y1 << 8) | y1);
        dst_uv[2 * i + 0] = (uint16_t)((u << 8) | u);
        dst_uv[2 * i + 1] = (uint16_t)((v << 8) | v);
    }
}

void avx_color_p216_to_rgba(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    const uint16_t *src_y = (const uint16_t*)src;
    const uint16_t *src_uv = (const uint16_t*)(src + (size_t)width * height * 2);
    size_t num_pairs = ((size_t)width * height) / 2;

    for (size_t i = 0; i < num_pairs; ++i) {
        int u  = src_uv[2 * i + 0] >> 8;
        int y0 = src_y[2 * i + 0] >> 8;
        int v  = src_uv[2 * i + 1] >> 8;
        int y1 = src_y[2 * i + 1] >> 8;

        int c0 = y0 - 16;
        int c1 = y1 - 16;
        int d_u = u - 128;
        int e_v = v - 128;

        int r0 = (298 * c0 + 409 * e_v + 128) >> 8;
        int g0 = (298 * c0 - 100 * d_u - 208 * e_v + 128) >> 8;
        int b0 = (298 * c0 + 516 * d_u + 128) >> 8;

        int r1 = (298 * c1 + 409 * e_v + 128) >> 8;
        int g1 = (298 * c1 - 100 * d_u - 208 * e_v + 128) >> 8;
        int b1 = (298 * c1 + 516 * d_u + 128) >> 8;

        dst[8 * i + 0] = (uint8_t)(r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
        dst[8 * i + 1] = (uint8_t)(g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
        dst[8 * i + 2] = (uint8_t)(b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));
        dst[8 * i + 3] = 255;

        dst[8 * i + 4] = (uint8_t)(r1 < 0 ? 0 : (r1 > 255 ? 255 : r1));
        dst[8 * i + 5] = (uint8_t)(g1 < 0 ? 0 : (g1 > 255 ? 255 : g1));
        dst[8 * i + 6] = (uint8_t)(b1 < 0 ? 0 : (b1 > 255 ? 255 : b1));
        dst[8 * i + 7] = 255;
    }
}

/*
 * Converts 4:2:2:4 UYVA to 32-bit RGBA, converting UYVY colorspace and copying
 * independent 8-bit alpha plane directly into destination alpha channel.
 */
void avx_color_uyva_to_rgba(const uint8_t *uyvy, const uint8_t *alpha, uint8_t *dst, int width, int height) {
    if (!uyvy || !dst || width <= 0 || height <= 0) return;
    avx_color_uyvy_to_rgba(uyvy, dst, width, height);
    if (alpha) {
        size_t total = (size_t)width * height;
        for (size_t i = 0; i < total; ++i) {
            dst[i * 4 + 3] = alpha[i];
        }
    }
}

void avx_color_uyva_to_bgra(const uint8_t *uyvy, const uint8_t *alpha, uint8_t *dst, int width, int height) {
    if (!uyvy || !dst || width <= 0 || height <= 0) return;
    avx_color_uyvy_to_bgra(uyvy, dst, width, height);
    if (alpha) {
        size_t total = (size_t)width * height;
        for (size_t i = 0; i < total; ++i) {
            dst[i * 4 + 3] = alpha[i];
        }
    }
}

void avx_color_rgba_to_uyva(const uint8_t *src, uint8_t *uyvy, uint8_t *alpha, int width, int height) {
    if (!src || !uyvy || width <= 0 || height <= 0) return;
    avx_color_rgba_to_uyvy(src, uyvy, width, height);
    if (alpha) {
        size_t total = (size_t)width * height;
        for (size_t i = 0; i < total; ++i) {
            alpha[i] = src[i * 4 + 3];
        }
    }
}

/*
 * Upsamples bi-planar NV12 (4:2:0) to packed UYVY (4:2:2), vertically duplicating
 * interleaved UV chroma samples across adjacent even/odd scanlines.
 */
void avx_color_nv12_to_uyvy(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *uv_src, size_t uv_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height)
{
    if (!y_src || !uv_src || !dst || width <= 0 || height <= 0) return;
    if (y_stride == 0) y_stride = (size_t)width;
    if (uv_stride == 0) uv_stride = (size_t)width;
    if (dst_stride == 0) dst_stride = (size_t)width * 2;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *y_row0 = y_src + (size_t)y0 * y_stride;
        const uint8_t *y_row1 = (y1 < height) ? (y_src + (size_t)y1 * y_stride) : y_row0;
        const uint8_t *uv_row = uv_src + (size_t)y2 * uv_stride;
        uint8_t *dst_row0 = dst + (size_t)y0 * dst_stride;
        uint8_t *dst_row1 = (y1 < height) ? (dst + (size_t)y1 * dst_stride) : NULL;

        for (int x2 = 0; x2 < half_w; ++x2) {
            uint8_t u = uv_row[2 * x2 + 0];
            uint8_t v = uv_row[2 * x2 + 1];
            uint8_t y00 = y_row0[2 * x2 + 0];
            uint8_t y01 = (2 * x2 + 1 < width) ? y_row0[2 * x2 + 1] : y00;

            dst_row0[4 * x2 + 0] = u;
            dst_row0[4 * x2 + 1] = y00;
            dst_row0[4 * x2 + 2] = v;
            dst_row0[4 * x2 + 3] = y01;

            if (dst_row1) {
                uint8_t y10 = y_row1[2 * x2 + 0];
                uint8_t y11 = (2 * x2 + 1 < width) ? y_row1[2 * x2 + 1] : y10;
                dst_row1[4 * x2 + 0] = u;
                dst_row1[4 * x2 + 1] = y10;
                dst_row1[4 * x2 + 2] = v;
                dst_row1[4 * x2 + 3] = y11;
            }
        }
    }
}

static inline uint8_t avx_clamp_u8(int val) {
    return (uint8_t)(val < 0 ? 0 : (val > 255 ? 255 : val));
}

static void avx_color_nv12_to_rgb32(const uint8_t *y_src, size_t y_stride,
                                    const uint8_t *uv_src, size_t uv_stride,
                                    uint8_t *dst, size_t dst_stride,
                                    int width, int height,
                                    bool is_bgra)
{
    if (!y_src || !uv_src || !dst || width <= 0 || height <= 0) return;
    if (y_stride == 0) y_stride = (size_t)width;
    if (uv_stride == 0) uv_stride = (size_t)width;
    if (dst_stride == 0) dst_stride = (size_t)width * 4;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;
    const int r_off = is_bgra ? 2 : 0;
    const int b_off = is_bgra ? 0 : 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *y_row0 = y_src + (size_t)y0 * y_stride;
        const uint8_t *y_row1 = (y1 < height) ? (y_src + (size_t)y1 * y_stride) : y_row0;
        const uint8_t *uv_row = uv_src + (size_t)y2 * uv_stride;
        uint8_t *dst_row0 = dst + (size_t)y0 * dst_stride;
        uint8_t *dst_row1 = (y1 < height) ? (dst + (size_t)y1 * dst_stride) : NULL;

        for (int x2 = 0; x2 < half_w; ++x2) {
            int u = uv_row[2 * x2 + 0];
            int v = uv_row[2 * x2 + 1];
            int d_u = u - 128;
            int e_v = v - 128;

            /* Row 0, pixel 0 */
            int c00 = y_row0[2 * x2 + 0] - 16;
            dst_row0[8 * x2 + r_off] = avx_clamp_u8((298 * c00 + 409 * e_v + 128) >> 8);
            dst_row0[8 * x2 + 1]     = avx_clamp_u8((298 * c00 - 100 * d_u - 208 * e_v + 128) >> 8);
            dst_row0[8 * x2 + b_off] = avx_clamp_u8((298 * c00 + 516 * d_u + 128) >> 8);
            dst_row0[8 * x2 + 3]     = 255;

            /* Row 0, pixel 1 */
            if (2 * x2 + 1 < width) {
                int c01 = y_row0[2 * x2 + 1] - 16;
                dst_row0[8 * x2 + 4 + r_off] = avx_clamp_u8((298 * c01 + 409 * e_v + 128) >> 8);
                dst_row0[8 * x2 + 5]         = avx_clamp_u8((298 * c01 - 100 * d_u - 208 * e_v + 128) >> 8);
                dst_row0[8 * x2 + 4 + b_off] = avx_clamp_u8((298 * c01 + 516 * d_u + 128) >> 8);
                dst_row0[8 * x2 + 7]         = 255;
            }

            /* Row 1, pixels 0 and 1 */
            if (dst_row1) {
                int c10 = y_row1[2 * x2 + 0] - 16;
                dst_row1[8 * x2 + r_off] = avx_clamp_u8((298 * c10 + 409 * e_v + 128) >> 8);
                dst_row1[8 * x2 + 1]     = avx_clamp_u8((298 * c10 - 100 * d_u - 208 * e_v + 128) >> 8);
                dst_row1[8 * x2 + b_off] = avx_clamp_u8((298 * c10 + 516 * d_u + 128) >> 8);
                dst_row1[8 * x2 + 3]     = 255;

                if (2 * x2 + 1 < width) {
                    int c11 = y_row1[2 * x2 + 1] - 16;
                    dst_row1[8 * x2 + 4 + r_off] = avx_clamp_u8((298 * c11 + 409 * e_v + 128) >> 8);
                    dst_row1[8 * x2 + 5]         = avx_clamp_u8((298 * c11 - 100 * d_u - 208 * e_v + 128) >> 8);
                    dst_row1[8 * x2 + 4 + b_off] = avx_clamp_u8((298 * c11 + 516 * d_u + 128) >> 8);
                    dst_row1[8 * x2 + 7]         = 255;
                }
            }
        }
    }
}

void avx_color_nv12_to_rgba(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *uv_src, size_t uv_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height)
{
    avx_color_nv12_to_rgb32(y_src, y_stride, uv_src, uv_stride, dst, dst_stride, width, height, false);
}

void avx_color_nv12_to_bgra(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *uv_src, size_t uv_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height)
{
    avx_color_nv12_to_rgb32(y_src, y_stride, uv_src, uv_stride, dst, dst_stride, width, height, true);
}

/*
 * Upsamples tri-planar I420 (4:2:0) to packed UYVY (4:2:2), vertically duplicating
 * planar U and V chroma samples across adjacent even/odd scanlines.
 */
void avx_color_i420_to_uyvy(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *u_src, size_t u_stride,
                            const uint8_t *v_src, size_t v_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height)
{
    if (!y_src || !u_src || !v_src || !dst || width <= 0 || height <= 0) return;
    if (y_stride == 0) y_stride = (size_t)width;
    if (u_stride == 0) u_stride = (size_t)(width + 1) / 2;
    if (v_stride == 0) v_stride = (size_t)(width + 1) / 2;
    if (dst_stride == 0) dst_stride = (size_t)width * 2;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *y_row0 = y_src + (size_t)y0 * y_stride;
        const uint8_t *y_row1 = (y1 < height) ? (y_src + (size_t)y1 * y_stride) : y_row0;
        const uint8_t *u_row = u_src + (size_t)y2 * u_stride;
        const uint8_t *v_row = v_src + (size_t)y2 * v_stride;
        uint8_t *dst_row0 = dst + (size_t)y0 * dst_stride;
        uint8_t *dst_row1 = (y1 < height) ? (dst + (size_t)y1 * dst_stride) : NULL;

        for (int x2 = 0; x2 < half_w; ++x2) {
            uint8_t u = u_row[x2];
            uint8_t v = v_row[x2];
            uint8_t y00 = y_row0[2 * x2 + 0];
            uint8_t y01 = (2 * x2 + 1 < width) ? y_row0[2 * x2 + 1] : y00;

            dst_row0[4 * x2 + 0] = u;
            dst_row0[4 * x2 + 1] = y00;
            dst_row0[4 * x2 + 2] = v;
            dst_row0[4 * x2 + 3] = y01;

            if (dst_row1) {
                uint8_t y10 = y_row1[2 * x2 + 0];
                uint8_t y11 = (2 * x2 + 1 < width) ? y_row1[2 * x2 + 1] : y10;
                dst_row1[4 * x2 + 0] = u;
                dst_row1[4 * x2 + 1] = y10;
                dst_row1[4 * x2 + 2] = v;
                dst_row1[4 * x2 + 3] = y11;
            }
        }
    }
}

void avx_color_i420_to_rgba(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *u_src, size_t u_stride,
                            const uint8_t *v_src, size_t v_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height)
{
    if (!y_src || !u_src || !v_src || !dst || width <= 0 || height <= 0) return;
    if (y_stride == 0) y_stride = (size_t)width;
    if (u_stride == 0) u_stride = (size_t)(width + 1) / 2;
    if (v_stride == 0) v_stride = (size_t)(width + 1) / 2;
    if (dst_stride == 0) dst_stride = (size_t)width * 4;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *y_row0 = y_src + (size_t)y0 * y_stride;
        const uint8_t *y_row1 = (y1 < height) ? (y_src + (size_t)y1 * y_stride) : y_row0;
        const uint8_t *u_row = u_src + (size_t)y2 * u_stride;
        const uint8_t *v_row = v_src + (size_t)y2 * v_stride;
        uint8_t *dst_row0 = dst + (size_t)y0 * dst_stride;
        uint8_t *dst_row1 = (y1 < height) ? (dst + (size_t)y1 * dst_stride) : NULL;

        for (int x2 = 0; x2 < half_w; ++x2) {
            int u = u_row[x2];
            int v = v_row[x2];
            int d_u = u - 128;
            int e_v = v - 128;

            /* Row 0, pixel 0 */
            int c00 = y_row0[2 * x2 + 0] - 16;
            int r00 = (298 * c00 + 409 * e_v + 128) >> 8;
            int g00 = (298 * c00 - 100 * d_u - 208 * e_v + 128) >> 8;
            int b00 = (298 * c00 + 516 * d_u + 128) >> 8;
            dst_row0[8 * x2 + 0] = (uint8_t)(r00 < 0 ? 0 : (r00 > 255 ? 255 : r00));
            dst_row0[8 * x2 + 1] = (uint8_t)(g00 < 0 ? 0 : (g00 > 255 ? 255 : g00));
            dst_row0[8 * x2 + 2] = (uint8_t)(b00 < 0 ? 0 : (b00 > 255 ? 255 : b00));
            dst_row0[8 * x2 + 3] = 255;

            /* Row 0, pixel 1 */
            if (2 * x2 + 1 < width) {
                int c01 = y_row0[2 * x2 + 1] - 16;
                int r01 = (298 * c01 + 409 * e_v + 128) >> 8;
                int g01 = (298 * c01 - 100 * d_u - 208 * e_v + 128) >> 8;
                int b01 = (298 * c01 + 516 * d_u + 128) >> 8;
                dst_row0[8 * x2 + 4] = (uint8_t)(r01 < 0 ? 0 : (r01 > 255 ? 255 : r01));
                dst_row0[8 * x2 + 5] = (uint8_t)(g01 < 0 ? 0 : (g01 > 255 ? 255 : g01));
                dst_row0[8 * x2 + 6] = (uint8_t)(b01 < 0 ? 0 : (b01 > 255 ? 255 : b01));
                dst_row0[8 * x2 + 7] = 255;
            }

            /* Row 1, pixels 0 and 1 */
            if (dst_row1) {
                int c10 = y_row1[2 * x2 + 0] - 16;
                int r10 = (298 * c10 + 409 * e_v + 128) >> 8;
                int g10 = (298 * c10 - 100 * d_u - 208 * e_v + 128) >> 8;
                int b10 = (298 * c10 + 516 * d_u + 128) >> 8;
                dst_row1[8 * x2 + 0] = (uint8_t)(r10 < 0 ? 0 : (r10 > 255 ? 255 : r10));
                dst_row1[8 * x2 + 1] = (uint8_t)(g10 < 0 ? 0 : (g10 > 255 ? 255 : g10));
                dst_row1[8 * x2 + 2] = (uint8_t)(b10 < 0 ? 0 : (b10 > 255 ? 255 : b10));
                dst_row1[8 * x2 + 3] = 255;

                if (2 * x2 + 1 < width) {
                    int c11 = y_row1[2 * x2 + 1] - 16;
                    int r11 = (298 * c11 + 409 * e_v + 128) >> 8;
                    int g11 = (298 * c11 - 100 * d_u - 208 * e_v + 128) >> 8;
                    int b11 = (298 * c11 + 516 * d_u + 128) >> 8;
                    dst_row1[8 * x2 + 4] = (uint8_t)(r11 < 0 ? 0 : (r11 > 255 ? 255 : r11));
                    dst_row1[8 * x2 + 5] = (uint8_t)(g11 < 0 ? 0 : (g11 > 255 ? 255 : g11));
                    dst_row1[8 * x2 + 6] = (uint8_t)(b11 < 0 ? 0 : (b11 > 255 ? 255 : b11));
                    dst_row1[8 * x2 + 7] = 255;
                }
            }
        }
    }
}

/*
 * Downsamples packed UYVY (4:2:2) to bi-planar NV12 (4:2:0), vertically averaging
 * chroma pairs across two scanlines into interleaved UV planes.
 */
void avx_color_uyvy_to_nv12(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *uv_dst, size_t uv_stride,
                            int width, int height)
{
    if (!src || !y_dst || !uv_dst || width <= 0 || height <= 0) return;
    if (src_stride == 0) src_stride = (size_t)width * 2;
    if (y_stride == 0) y_stride = (size_t)width;
    if (uv_stride == 0) uv_stride = (size_t)width;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *src_row0 = src + (size_t)y0 * src_stride;
        const uint8_t *src_row1 = (y1 < height) ? (src + (size_t)y1 * src_stride) : src_row0;
        uint8_t *y_out0 = y_dst + (size_t)y0 * y_stride;
        uint8_t *y_out1 = (y1 < height) ? (y_dst + (size_t)y1 * y_stride) : NULL;
        uint8_t *uv_out = uv_dst + (size_t)y2 * uv_stride;

        for (int x2 = 0; x2 < half_w; ++x2) {
            uint8_t u0 = src_row0[4 * x2 + 0];
            uint8_t y00 = src_row0[4 * x2 + 1];
            uint8_t v0 = src_row0[4 * x2 + 2];
            uint8_t y01 = src_row0[4 * x2 + 3];

            y_out0[2 * x2 + 0] = y00;
            if (2 * x2 + 1 < width) y_out0[2 * x2 + 1] = y01;

            uint8_t u1 = u0;
            uint8_t v1 = v0;
            if (y_out1) {
                u1 = src_row1[4 * x2 + 0];
                uint8_t y10 = src_row1[4 * x2 + 1];
                v1 = src_row1[4 * x2 + 2];
                uint8_t y11 = src_row1[4 * x2 + 3];

                y_out1[2 * x2 + 0] = y10;
                if (2 * x2 + 1 < width) y_out1[2 * x2 + 1] = y11;
            }

            uv_out[2 * x2 + 0] = (uint8_t)(((int)u0 + (int)u1) / 2);
            uv_out[2 * x2 + 1] = (uint8_t)(((int)v0 + (int)v1) / 2);
        }
    }
}

void avx_color_uyvy_to_i420(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *u_dst, size_t u_stride,
                            uint8_t *v_dst, size_t v_stride,
                            int width, int height)
{
    if (!src || !y_dst || !u_dst || !v_dst || width <= 0 || height <= 0) return;
    if (src_stride == 0) src_stride = (size_t)width * 2;
    if (y_stride == 0) y_stride = (size_t)width;
    if (u_stride == 0) u_stride = (size_t)(width + 1) / 2;
    if (v_stride == 0) v_stride = (size_t)(width + 1) / 2;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *src_row0 = src + (size_t)y0 * src_stride;
        const uint8_t *src_row1 = (y1 < height) ? (src + (size_t)y1 * src_stride) : src_row0;
        uint8_t *y_out0 = y_dst + (size_t)y0 * y_stride;
        uint8_t *y_out1 = (y1 < height) ? (y_dst + (size_t)y1 * y_stride) : NULL;
        uint8_t *u_out = u_dst + (size_t)y2 * u_stride;
        uint8_t *v_out = v_dst + (size_t)y2 * v_stride;

        for (int x2 = 0; x2 < half_w; ++x2) {
            uint8_t u0 = src_row0[4 * x2 + 0];
            uint8_t y00 = src_row0[4 * x2 + 1];
            uint8_t v0 = src_row0[4 * x2 + 2];
            uint8_t y01 = src_row0[4 * x2 + 3];

            y_out0[2 * x2 + 0] = y00;
            if (2 * x2 + 1 < width) y_out0[2 * x2 + 1] = y01;

            uint8_t u1 = u0;
            uint8_t v1 = v0;
            if (y_out1) {
                u1 = src_row1[4 * x2 + 0];
                uint8_t y10 = src_row1[4 * x2 + 1];
                v1 = src_row1[4 * x2 + 2];
                uint8_t y11 = src_row1[4 * x2 + 3];

                y_out1[2 * x2 + 0] = y10;
                if (2 * x2 + 1 < width) y_out1[2 * x2 + 1] = y11;
            }

            u_out[x2] = (uint8_t)(((int)u0 + (int)u1) / 2);
            v_out[x2] = (uint8_t)(((int)v0 + (int)v1) / 2);
        }
    }
}

void avx_color_uyvy_to_yv12(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *v_dst, size_t v_stride,
                            uint8_t *u_dst, size_t u_stride,
                            int width, int height)
{
    /* YV12 is identical to I420 with U and V destinations swapped */
    avx_color_uyvy_to_i420(src, src_stride, y_dst, y_stride, u_dst, u_stride, v_dst, v_stride, width, height);
}

void avx_color_yv12_to_uyvy(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *v_src, size_t v_stride,
                            const uint8_t *u_src, size_t u_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height)
{
    /* YV12 has V as plane 1 and U as plane 2 */
    avx_color_i420_to_uyvy(y_src, y_stride, u_src, u_stride, v_src, v_stride, dst, dst_stride, width, height);
}

void avx_color_yv12_to_rgba(const uint8_t *y_src, size_t y_stride,
                            const uint8_t *v_src, size_t v_stride,
                            const uint8_t *u_src, size_t u_stride,
                            uint8_t *dst, size_t dst_stride,
                            int width, int height)
{
    /* YV12 has V as plane 1 and U as plane 2 */
    avx_color_i420_to_rgba(y_src, y_stride, u_src, u_stride, v_src, v_stride, dst, dst_stride, width, height);
}

/*
 * Downsamples 32-bit RGBA to bi-planar NV12 (4:2:0) with 2x2 box filtering for chroma
 * and BT.601 studio swing clamping (Y in [16..235], UV in [16..240]).
 */
void avx_color_rgba_to_nv12(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *uv_dst, size_t uv_stride,
                            int width, int height)
{
    if (!src || !y_dst || !uv_dst || width <= 0 || height <= 0) return;
    if (src_stride == 0) src_stride = (size_t)width * 4;
    if (y_stride == 0) y_stride = (size_t)width;
    if (uv_stride == 0) uv_stride = (size_t)width;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *src_row0 = src + (size_t)y0 * src_stride;
        const uint8_t *src_row1 = (y1 < height) ? (src + (size_t)y1 * src_stride) : src_row0;
        uint8_t *y_out0 = y_dst + (size_t)y0 * y_stride;
        uint8_t *y_out1 = (y1 < height) ? (y_dst + (size_t)y1 * y_stride) : NULL;
        uint8_t *uv_out = uv_dst + (size_t)y2 * uv_stride;

        for (int x2 = 0; x2 < half_w; ++x2) {
            int x0 = x2 * 2;
            int x1 = x0 + 1;

            int r00 = src_row0[4 * x0 + 0];
            int g00 = src_row0[4 * x0 + 1];
            int b00 = src_row0[4 * x0 + 2];

            int r01 = r00, g01 = g00, b01 = b00;
            if (x1 < width) {
                r01 = src_row0[4 * x1 + 0];
                g01 = src_row0[4 * x1 + 1];
                b01 = src_row0[4 * x1 + 2];
            }

            int r10 = r00, g10 = g00, b10 = b00;
            int r11 = r01, g11 = g01, b11 = b01;
            if (y_out1) {
                r10 = src_row1[4 * x0 + 0];
                g10 = src_row1[4 * x0 + 1];
                b10 = src_row1[4 * x0 + 2];
                if (x1 < width) {
                    r11 = src_row1[4 * x1 + 0];
                    g11 = src_row1[4 * x1 + 1];
                    b11 = src_row1[4 * x1 + 2];
                } else {
                    r11 = r10; g11 = g10; b11 = b10;
                }
            }

            int y_val00 = ((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16;
            y_out0[x0] = (uint8_t)(y_val00 < 0 ? 0 : (y_val00 > 255 ? 255 : y_val00));
            if (x1 < width) {
                int y_val01 = ((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16;
                y_out0[x1] = (uint8_t)(y_val01 < 0 ? 0 : (y_val01 > 255 ? 255 : y_val01));
            }

            if (y_out1) {
                int y_val10 = ((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16;
                y_out1[x0] = (uint8_t)(y_val10 < 0 ? 0 : (y_val10 > 255 ? 255 : y_val10));
                if (x1 < width) {
                    int y_val11 = ((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16;
                    y_out1[x1] = (uint8_t)(y_val11 < 0 ? 0 : (y_val11 > 255 ? 255 : y_val11));
                }
            }

            int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;
            int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;

            int u_val = ((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8) + 128;
            int v_val = ((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8) + 128;

            uv_out[2 * x2 + 0] = (uint8_t)(u_val < 0 ? 0 : (u_val > 255 ? 255 : u_val));
            uv_out[2 * x2 + 1] = (uint8_t)(v_val < 0 ? 0 : (v_val > 255 ? 255 : v_val));
        }
    }
}

/*
 * Downsamples 32-bit RGBA to tri-planar I420 (4:2:0) with 2x2 box filtering for chroma
 * and BT.601 studio swing clamping.
 */
void avx_color_rgba_to_i420(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *u_dst, size_t u_stride,
                            uint8_t *v_dst, size_t v_stride,
                            int width, int height)
{
    if (!src || !y_dst || !u_dst || !v_dst || width <= 0 || height <= 0) return;
    if (src_stride == 0) src_stride = (size_t)width * 4;
    if (y_stride == 0) y_stride = (size_t)width;
    if (u_stride == 0) u_stride = (size_t)(width + 1) / 2;
    if (v_stride == 0) v_stride = (size_t)(width + 1) / 2;

    int half_h = (height + 1) / 2;
    int half_w = (width + 1) / 2;

    for (int y2 = 0; y2 < half_h; ++y2) {
        int y0 = y2 * 2;
        int y1 = y0 + 1;
        const uint8_t *src_row0 = src + (size_t)y0 * src_stride;
        const uint8_t *src_row1 = (y1 < height) ? (src + (size_t)y1 * src_stride) : src_row0;
        uint8_t *y_out0 = y_dst + (size_t)y0 * y_stride;
        uint8_t *y_out1 = (y1 < height) ? (y_dst + (size_t)y1 * y_stride) : NULL;
        uint8_t *u_out = u_dst + (size_t)y2 * u_stride;
        uint8_t *v_out = v_dst + (size_t)y2 * v_stride;

        for (int x2 = 0; x2 < half_w; ++x2) {
            int x0 = x2 * 2;
            int x1 = x0 + 1;

            int r00 = src_row0[4 * x0 + 0];
            int g00 = src_row0[4 * x0 + 1];
            int b00 = src_row0[4 * x0 + 2];

            int r01 = r00, g01 = g00, b01 = b00;
            if (x1 < width) {
                r01 = src_row0[4 * x1 + 0];
                g01 = src_row0[4 * x1 + 1];
                b01 = src_row0[4 * x1 + 2];
            }

            int r10 = r00, g10 = g00, b10 = b00;
            int r11 = r01, g11 = g01, b11 = b01;
            if (y_out1) {
                r10 = src_row1[4 * x0 + 0];
                g10 = src_row1[4 * x0 + 1];
                b10 = src_row1[4 * x0 + 2];
                if (x1 < width) {
                    r11 = src_row1[4 * x1 + 0];
                    g11 = src_row1[4 * x1 + 1];
                    b11 = src_row1[4 * x1 + 2];
                } else {
                    r11 = r10; g11 = g10; b11 = b10;
                }
            }

            int y_val00 = ((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16;
            y_out0[x0] = (uint8_t)(y_val00 < 0 ? 0 : (y_val00 > 255 ? 255 : y_val00));
            if (x1 < width) {
                int y_val01 = ((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16;
                y_out0[x1] = (uint8_t)(y_val01 < 0 ? 0 : (y_val01 > 255 ? 255 : y_val01));
            }

            if (y_out1) {
                int y_val10 = ((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16;
                y_out1[x0] = (uint8_t)(y_val10 < 0 ? 0 : (y_val10 > 255 ? 255 : y_val10));
                if (x1 < width) {
                    int y_val11 = ((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16;
                    y_out1[x1] = (uint8_t)(y_val11 < 0 ? 0 : (y_val11 > 255 ? 255 : y_val11));
                }
            }

            int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;
            int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;

            int u_val = ((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8) + 128;
            int v_val = ((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8) + 128;

            u_out[x2] = (uint8_t)(u_val < 0 ? 0 : (u_val > 255 ? 255 : u_val));
            v_out[x2] = (uint8_t)(v_val < 0 ? 0 : (v_val > 255 ? 255 : v_val));
        }
    }
}

void avx_color_rgba_to_yv12(const uint8_t *src, size_t src_stride,
                            uint8_t *y_dst, size_t y_stride,
                            uint8_t *v_dst, size_t v_stride,
                            uint8_t *u_dst, size_t u_stride,
                            int width, int height)
{
    /* YV12 has V as plane 1 and U as plane 2 */
    avx_color_rgba_to_i420(src, src_stride, y_dst, y_stride, u_dst, u_stride, v_dst, v_stride, width, height);
}
