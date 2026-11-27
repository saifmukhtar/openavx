/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_utilities.c - Interleaved audio helpers and SDI format conversion
 */

#include "openavx.h"
#include "avx_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

/* Helper to clamp 32-bit integer to 16-bit signed range */
static inline int16_t clamp_s16(int val) {
    if (val < -32768) return -32768;
    if (val > 32767) return 32767;
    return (int16_t)val;
}

/* Helper to clamp float to [-1.0, 1.0] */
static inline float clamp_f32(float val) {
    if (val < -1.0f) return -1.0f;
    if (val > 1.0f) return 1.0f;
    return val;
}

/* Converts planar float audio channels to interleaved signed 16-bit PCM. */
void avx_util_audio_to_interleaved_16s(const oavx_audio_frame_t *src, oavx_audio_frame_interleaved_16s_t *dst) {
    if (!src || !dst || !src->planes[0] || !dst->data) return;

    dst->sample_rate = src->sample_rate;
    dst->channels = src->channels;
    dst->frames = src->frames;
    dst->pts_ns = src->pts_ns;

    uint16_t channels = src->channels;
    uint32_t samples = src->frames;
    size_t stride = (src->stride_bytes > 0) ? (src->stride_bytes / sizeof(float)) : (size_t)samples;

    float gain = 1.0f;

    for (uint16_t c = 0; c < channels; ++c) {
        const float *src_ch =
            src->planes[c] ? (const float *)src->planes[c] : ((const float *)src->planes[0] + (size_t)c * stride);
        int16_t *dst_ptr = dst->data + c;

        for (uint32_t s = 0; s < samples; ++s) {
            float f = src_ch[s] * gain;
            if (isnan(f)) {
                dst_ptr[(size_t)s * channels] = 0;
            } else if (f >= 1.0f) {
                dst_ptr[(size_t)s * channels] = INT16_MAX;
            } else if (f <= -1.0f) {
                dst_ptr[(size_t)s * channels] = INT16_MIN;
            } else {
                dst_ptr[(size_t)s * channels] = clamp_s16((int)roundf(f * 32767.0f));
            }
        }
    }
}

void avx_util_audio_from_interleaved_16s(const oavx_audio_frame_interleaved_16s_t *src, oavx_audio_frame_t *dst) {
    if (!src || !dst || !src->data || !dst->planes[0]) return;

    dst->sample_rate = src->sample_rate;
    dst->channels = src->channels;
    dst->frames = src->frames;
    dst->format = OAVX_AUDIO_FMT_FLT_PLANAR;
    dst->pts_ns = src->pts_ns;
    dst->stride_bytes = (size_t)src->frames * sizeof(float);

    uint16_t channels = src->channels;
    uint32_t samples = src->frames;
    size_t stride = (size_t)samples;

    float gain = 1.0f / 32767.0f;

    for (uint16_t c = 0; c < channels; ++c) {
        if (!dst->planes[c]) {
            dst->planes[c] = (float *)dst->planes[0] + (size_t)c * stride;
        }
        float *dst_ch = (float *)dst->planes[c];
        const int16_t *src_ptr = src->data + c;

        for (uint32_t s = 0; s < samples; ++s) {
            dst_ch[s] = clamp_f32((float)src_ptr[(size_t)s * channels] * gain);
        }
    }
}

void avx_util_audio_to_interleaved_32f(const oavx_audio_frame_t *src, oavx_audio_frame_interleaved_32f_t *dst) {
    if (!src || !dst || !src->planes[0] || !dst->data) return;

    dst->sample_rate = src->sample_rate;
    dst->channels = src->channels;
    dst->frames = src->frames;
    dst->pts_ns = src->pts_ns;

    uint16_t channels = src->channels;
    uint32_t samples = src->frames;
    size_t stride = (src->stride_bytes > 0) ? (src->stride_bytes / sizeof(float)) : (size_t)samples;

    for (uint16_t c = 0; c < channels; ++c) {
        const float *src_ch =
            src->planes[c] ? (const float *)src->planes[c] : ((const float *)src->planes[0] + (size_t)c * stride);
        float *dst_ptr = dst->data + c;

        for (uint32_t s = 0; s < samples; ++s) {
            dst_ptr[(size_t)s * channels] = src_ch[s];
        }
    }
}

void avx_util_audio_from_interleaved_32f(const oavx_audio_frame_interleaved_32f_t *src, oavx_audio_frame_t *dst) {
    if (!src || !dst || !src->data || !dst->planes[0]) return;

    dst->sample_rate = src->sample_rate;
    dst->channels = src->channels;
    dst->frames = src->frames;
    dst->format = OAVX_AUDIO_FMT_FLT_PLANAR;
    dst->pts_ns = src->pts_ns;
    dst->stride_bytes = (size_t)src->frames * sizeof(float);

    uint16_t channels = src->channels;
    uint32_t samples = src->frames;
    size_t stride = (size_t)samples;

    for (uint16_t c = 0; c < channels; ++c) {
        if (!dst->planes[c]) {
            dst->planes[c] = (float *)dst->planes[0] + (size_t)c * stride;
        }
        float *dst_ch = (float *)dst->planes[c];
        const float *src_ptr = src->data + c;

        for (uint32_t s = 0; s < samples; ++s) {
            dst_ch[s] = src_ptr[(size_t)s * channels];
        }
    }
}

void avx_util_audio_to_interleaved_32s(const oavx_audio_frame_t *src, oavx_audio_frame_interleaved_32s_t *dst) {
    if (!src || !dst || !src->planes[0] || !dst->data) return;

    dst->sample_rate = src->sample_rate;
    dst->channels = src->channels;
    dst->frames = src->frames;
    dst->pts_ns = src->pts_ns;

    uint16_t channels = src->channels;
    uint32_t samples = src->frames;
    size_t stride = (src->stride_bytes > 0) ? (src->stride_bytes / sizeof(float)) : (size_t)samples;

    for (uint16_t c = 0; c < channels; ++c) {
        const float *src_ch =
            src->planes[c] ? (const float *)src->planes[c] : ((const float *)src->planes[0] + (size_t)c * stride);
        int32_t *dst_ptr = dst->data + c;

        for (uint32_t s = 0; s < samples; ++s) {
            double d = (double)src_ch[s];
            if (isnan(d)) {
                dst_ptr[(size_t)s * channels] = 0;
            } else if (d >= 1.0) {
                dst_ptr[(size_t)s * channels] = INT32_MAX;
            } else if (d <= -1.0) {
                dst_ptr[(size_t)s * channels] = INT32_MIN;
            } else {
                dst_ptr[(size_t)s * channels] = (int32_t)round(d * 2147483647.0);
            }
        }
    }
}

void avx_util_audio_from_interleaved_32s(const oavx_audio_frame_interleaved_32s_t *src, oavx_audio_frame_t *dst) {
    if (!src || !dst || !src->data || !dst->planes[0]) return;

    dst->sample_rate = src->sample_rate;
    dst->channels = src->channels;
    dst->frames = src->frames;
    dst->format = OAVX_AUDIO_FMT_FLT_PLANAR;
    dst->pts_ns = src->pts_ns;
    dst->stride_bytes = (size_t)src->frames * sizeof(float);

    uint16_t channels = src->channels;
    uint32_t samples = src->frames;
    size_t stride = (size_t)samples;

    for (uint16_t c = 0; c < channels; ++c) {
        if (!dst->planes[c]) {
            dst->planes[c] = (float *)dst->planes[0] + (size_t)c * stride;
        }
        float *dst_ch = (float *)dst->planes[c];
        const int32_t *src_ptr = src->data + c;

        for (uint32_t s = 0; s < samples; ++s) {
            dst_ch[s] = (float)((double)src_ptr[(size_t)s * channels] / 2147483647.0);
        }
    }
}

/* Packs 16-bit semi-planar P216 (10-bit msb-aligned) into SMPTE 292M v210 128-bit blocks. */
void avx_util_P216_to_V210(const oavx_video_frame_t *src_p216, oavx_video_frame_t *dst_v210) {
    if (!src_p216 || !dst_v210 || !src_p216->planes[0] || !dst_v210->planes[0]) return;

    uint32_t width = src_p216->width;
    uint32_t height = src_p216->height;
    if (width == 0 || height == 0) return;

    dst_v210->width = width;
    dst_v210->height = height;
    dst_v210->fourcc = (oavx_fourcc_video_type_e)0x30313276; /* "v210" */
    dst_v210->framerate = src_p216->framerate;
    dst_v210->pts_ns = src_p216->pts_ns;
    dst_v210->dts_ns = src_p216->dts_ns;
    dst_v210->strides[0] = ((width + 47) / 48) * 128;

    size_t src_stride = (src_p216->strides[0] > 0) ? (src_p216->strides[0] / 2) : (size_t)width;
    size_t src_uv_stride = (src_p216->strides[1] > 0) ? (src_p216->strides[1] / 2) : src_stride;
    const uint16_t *src_y = (const uint16_t *)src_p216->planes[0];
    const uint16_t *src_uv = src_p216->planes[1] ? (const uint16_t *)src_p216->planes[1]
                                                 : (const uint16_t *)(src_p216->planes[0] + src_stride * height * 2);

    uint32_t *dst_words = (uint32_t *)dst_v210->planes[0];
    size_t dst_stride_words = dst_v210->strides[0] / sizeof(uint32_t);

    for (uint32_t y = 0; y < height; ++y) {
        const uint16_t *y_line = src_y + (size_t)y * src_stride;
        const uint16_t *uv_line = src_uv + (size_t)y * src_uv_stride;
        uint32_t *dst_line = dst_words + (size_t)y * dst_stride_words;

        uint32_t x = 0;
        for (; x + 6 <= width; x += 6) {
            /* Extract 10-bit values (P216 is 16-bit left-justified, so shift right 6) */
            uint32_t u0 = (uint32_t)(uv_line[x + 0] >> 6) & 0x3FF;
            uint32_t y0 = (uint32_t)(y_line[x + 0] >> 6) & 0x3FF;
            uint32_t v0 = (uint32_t)(uv_line[x + 1] >> 6) & 0x3FF;

            uint32_t y1 = (uint32_t)(y_line[x + 1] >> 6) & 0x3FF;
            uint32_t u1 = (uint32_t)(uv_line[x + 2] >> 6) & 0x3FF;
            uint32_t y2 = (uint32_t)(y_line[x + 2] >> 6) & 0x3FF;

            uint32_t v1 = (uint32_t)(uv_line[x + 3] >> 6) & 0x3FF;
            uint32_t y3 = (uint32_t)(y_line[x + 3] >> 6) & 0x3FF;
            uint32_t u2 = (uint32_t)(uv_line[x + 4] >> 6) & 0x3FF;

            uint32_t y4 = (uint32_t)(y_line[x + 4] >> 6) & 0x3FF;
            uint32_t v2 = (uint32_t)(uv_line[x + 5] >> 6) & 0x3FF;
            uint32_t y5 = (uint32_t)(y_line[x + 5] >> 6) & 0x3FF;

            /* Pack into SMPTE 292M 128-bit blocks (4 x 32-bit words) */
            size_t w_idx = (size_t)(x / 6) * 4;
            dst_line[w_idx + 0] = (u0) | (y0 << 10) | (v0 << 20);
            dst_line[w_idx + 1] = (y1) | (u1 << 10) | (y2 << 20);
            dst_line[w_idx + 2] = (v1) | (y3 << 10) | (u2 << 20);
            dst_line[w_idx + 3] = (y4) | (v2 << 10) | (y5 << 20);
        }

        if (x < width) {
            uint32_t rem = width - x;
            uint32_t ys[6] = {0}, uvs[6] = {0};
            for (uint32_t i = 0; i < rem; ++i) {
                ys[i] = (uint32_t)(y_line[x + i] >> 6) & 0x3FF;
                uvs[i] = (uint32_t)(uv_line[x + i] >> 6) & 0x3FF;
            }
            size_t w_idx = (size_t)(x / 6) * 4;
            dst_line[w_idx + 0] = (uvs[0]) | (ys[0] << 10) | (uvs[1] << 20);
            dst_line[w_idx + 1] = (ys[1]) | (uvs[2] << 10) | (ys[2] << 20);
            dst_line[w_idx + 2] = (uvs[3]) | (ys[3] << 10) | (uvs[4] << 20);
            dst_line[w_idx + 3] = (ys[4]) | (uvs[5] << 10) | (ys[5] << 20);
        }
    }
}

/* Unpacks SMPTE 292M v210 10-bit packed words into 16-bit semi-planar P216. */
void avx_util_V210_to_P216(const oavx_video_frame_t *src_v210, oavx_video_frame_t *dst_p216) {
    if (!src_v210 || !dst_p216 || !src_v210->planes[0] || !dst_p216->planes[0]) return;

    uint32_t width = src_v210->width;
    uint32_t height = src_v210->height;
    if (width == 0 || height == 0) return;

    dst_p216->width = width;
    dst_p216->height = height;
    dst_p216->fourcc = OAVX_FOURCC_VIDEO_TYPE_P216;
    dst_p216->framerate = src_v210->framerate;
    dst_p216->pts_ns = src_v210->pts_ns;
    dst_p216->dts_ns = src_v210->dts_ns;
    dst_p216->strides[0] = width * 2;
    dst_p216->strides[1] = width * 2;

    const uint32_t *src_words = (const uint32_t *)src_v210->planes[0];
    size_t src_stride_words = src_v210->strides[0] / sizeof(uint32_t);

    uint16_t *dst_y = (uint16_t *)dst_p216->planes[0];
    uint16_t *dst_uv = dst_p216->planes[1] ? (uint16_t *)dst_p216->planes[1]
                                           : (uint16_t *)(dst_p216->planes[0] + (size_t)width * height * 2);
    size_t dst_stride = (size_t)width;

    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t *src_line = src_words + (size_t)y * src_stride_words;
        uint16_t *y_line = dst_y + (size_t)y * dst_stride;
        uint16_t *uv_line = dst_uv + (size_t)y * dst_stride;

        uint32_t x = 0;
        for (; x + 6 <= width; x += 6) {
            size_t w_idx = (size_t)(x / 6) * 4;
            uint32_t w0 = src_line[w_idx + 0];
            uint32_t w1 = src_line[w_idx + 1];
            uint32_t w2 = src_line[w_idx + 2];
            uint32_t w3 = src_line[w_idx + 3];

            /* Unpack 10-bit values */
            uint32_t u0 = (w0) & 0x3FF;
            uint32_t y0 = (w0 >> 10) & 0x3FF;
            uint32_t v0 = (w0 >> 20) & 0x3FF;

            uint32_t y1 = (w1) & 0x3FF;
            uint32_t u1 = (w1 >> 10) & 0x3FF;
            uint32_t y2 = (w1 >> 20) & 0x3FF;

            uint32_t v1 = (w2) & 0x3FF;
            uint32_t y3 = (w2 >> 10) & 0x3FF;
            uint32_t u2 = (w2 >> 20) & 0x3FF;

            uint32_t y4 = (w3) & 0x3FF;
            uint32_t v2 = (w3 >> 10) & 0x3FF;
            uint32_t y5 = (w3 >> 20) & 0x3FF;

            /* Store in P216 (16-bit left-justified) */
            y_line[x + 0] = (uint16_t)(y0 << 6);
            y_line[x + 1] = (uint16_t)(y1 << 6);
            y_line[x + 2] = (uint16_t)(y2 << 6);
            y_line[x + 3] = (uint16_t)(y3 << 6);
            y_line[x + 4] = (uint16_t)(y4 << 6);
            y_line[x + 5] = (uint16_t)(y5 << 6);

            uv_line[x + 0] = (uint16_t)(u0 << 6);
            uv_line[x + 1] = (uint16_t)(v0 << 6);
            uv_line[x + 2] = (uint16_t)(u1 << 6);
            uv_line[x + 3] = (uint16_t)(v1 << 6);
            uv_line[x + 4] = (uint16_t)(u2 << 6);
            uv_line[x + 5] = (uint16_t)(v2 << 6);
        }

        if (x < width) {
            uint32_t rem = width - x;
            size_t w_idx = (size_t)(x / 6) * 4;
            uint32_t w0 = src_line[w_idx + 0];
            uint32_t w1 = src_line[w_idx + 1];
            uint32_t w2 = src_line[w_idx + 2];
            uint32_t w3 = src_line[w_idx + 3];

            uint16_t ys[6] = {(uint16_t)(((w0 >> 10) & 0x3FF) << 6), (uint16_t)(((w1) & 0x3FF) << 6),
                              (uint16_t)(((w1 >> 20) & 0x3FF) << 6), (uint16_t)(((w2 >> 10) & 0x3FF) << 6),
                              (uint16_t)(((w3) & 0x3FF) << 6),       (uint16_t)(((w3 >> 20) & 0x3FF) << 6)};
            uint16_t uvs[6] = {(uint16_t)(((w0) & 0x3FF) << 6),       (uint16_t)(((w0 >> 20) & 0x3FF) << 6),
                               (uint16_t)(((w1 >> 10) & 0x3FF) << 6), (uint16_t)(((w2) & 0x3FF) << 6),
                               (uint16_t)(((w2 >> 20) & 0x3FF) << 6), (uint16_t)(((w3 >> 10) & 0x3FF) << 6)};
            for (uint32_t i = 0; i < rem; ++i) {
                y_line[x + i] = ys[i];
                uv_line[x + i] = uvs[i];
            }
        }
    }
}
