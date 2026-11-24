/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (c) 2017 Steinar H. Gunderson
 * Copyright (c) 2017-2026 FFmpeg contributors
 * Copyright (c) 2026 Saif Mukhtar (OpenAVX integration)
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_speedhq.h - Standalone SpeedHQ intra-frame codec
 *
 * Derived from FFmpeg libavcodec (speedhq.c).
 * Licensed under the GNU Lesser General Public License version 2.1 or later.
 */

#ifndef AVX_SPEEDHQ_H
#define AVX_SPEEDHQ_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fast FourCC check for SpeedHQ compressed stream types (SHQ0-SHQ9).
 */
static inline bool avx_is_speedhq_fourcc(uint32_t fourcc) {
    switch (fourcc) {
        case OAVX_FOURCC_VIDEO_TYPE_SHQ0:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ1:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ2:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ3:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ4:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ5:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ7:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ9:
            return true;
        default:
            return false;
    }
}

/**
 * Global initialization of SpeedHQ codec engine.
 * Initializes static VLC tables and native SIMD vectorization.
 */
OPENAVX_API bool avx_speedhq_init(void);

/**
 * Dynamic Bitrate & Quantization Quality Control:
 * Sets the default SpeedHQ quantization quality (20-98, default: 96).
 * Lower quality drastically reduces wire bitrate for congested/WAN links.
 */
OPENAVX_API void avx_speedhq_set_quality(uint8_t quality);
OPENAVX_API uint8_t avx_speedhq_get_quality(void);

/**
 * Returns true if native SIMD hardware acceleration (AVX2/NEON) is active.
 */
OPENAVX_API bool avx_speedhq_has_acceleration(void);

/**
 * High-performance standalone encoder:
 * Compresses packed UYVY 4:2:2 frame to SpeedHQ-2 (SHQ2) bitstream.
 * Uses native SIMD (AVX2/NEON) vectorization.
 */
OPENAVX_API int avx_speedhq_encode(int width, int height,
                                  const uint8_t *uyvy_data,
                                  uint8_t *out_buf, size_t out_max,
                                  size_t *out_size);

/**
 * High-performance standalone decoder:
 * Decompresses SpeedHQ-2 (SHQ2) bitstream to packed UYVY 4:2:2.
 * Uses native SIMD (AVX2/NEON) vectorization.
 */
OPENAVX_API int avx_speedhq_decode(int width, int height,
                                  const uint8_t *shq_data, size_t shq_size,
                                  uint8_t *uyvy_out);

/**
 * Pure C99 standalone fallback encoder (zero dependencies).
 */
OPENAVX_API int avx_speedhq_encode_c99(int width, int height,
                                      const uint8_t *uyvy_data,
                                      uint8_t *out_buf, size_t out_max,
                                      size_t *out_size);

/**
 * Pure C99 standalone fallback decoder (zero dependencies).
 */
OPENAVX_API int avx_speedhq_decode_c99(int width, int height,
                                      const uint8_t *shq_data, size_t shq_size,
                                      uint8_t *uyvy_out);

/**
 * Extended high-performance standalone encoder:
 * Compresses packed UYVY 4:2:2 frame and optional alpha plane to SpeedHQ (SHQ0-SHQ9) bitstream.
 */
OPENAVX_API int avx_speedhq_encode_ex(uint32_t fourcc, int width, int height,
                                     const uint8_t *uyvy_data, const uint8_t *alpha_data,
                                     uint8_t *out_buf, size_t out_max,
                                     size_t *out_size);

/**
 * Strided and per-instance quality SpeedHQ encoder:
 * `uyvy_stride == 0` defaults to width * 2; `alpha_stride == 0` defaults to width;
 * `quality == 0` inherits the global default from avx_speedhq_get_quality().
 */
OPENAVX_API int avx_speedhq_encode_strided_ex(uint32_t fourcc, int width, int height,
                                             const uint8_t *uyvy_data, size_t uyvy_stride,
                                             const uint8_t *alpha_data, size_t alpha_stride,
                                             uint8_t quality,
                                             uint8_t *out_buf, size_t out_max,
                                             size_t *out_size);

/**
 * Extended high-performance standalone decoder:
 * Decompresses SpeedHQ bitstream to packed UYVY 4:2:2 and optional alpha plane.
 */
OPENAVX_API int avx_speedhq_decode_ex(uint32_t fourcc, int width, int height,
                                     const uint8_t *shq_data, size_t shq_size,
                                     uint8_t *uyvy_out, uint8_t *alpha_out,
                                     bool *p_has_alpha);

/**
 * Extended pure C99 standalone fallback encoder.
 */
OPENAVX_API int avx_speedhq_encode_c99_ex(uint32_t fourcc, int width, int height,
                                         const uint8_t *uyvy_data, const uint8_t *alpha_data,
                                         uint8_t *out_buf, size_t out_max,
                                         size_t *out_size);

/**
 * Extended pure C99 standalone fallback decoder.
 */
OPENAVX_API int avx_speedhq_decode_c99_ex(uint32_t fourcc, int width, int height,
                                         const uint8_t *shq_data, size_t shq_size,
                                         uint8_t *uyvy_out, uint8_t *alpha_out,
                                         bool *p_has_alpha);

/**
 * Free any global codec resources.
 */
OPENAVX_API void avx_speedhq_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* AVX_SPEEDHQ_H */
