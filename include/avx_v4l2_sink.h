/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_v4l2_sink.h - Linux V4L2 Virtual Camera Loopback Sink
 */

#ifndef AVX_V4L2_SINK_H
#define AVX_V4L2_SINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "avx_platform.h"
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct avx_v4l2_sink_t avx_v4l2_sink_t;

typedef struct {
    const char *device_path;    /* Target device, e.g. "/dev/video10", or NULL for auto-probe */
    uint32_t    width;          /* Frame width (e.g. 1920) */
    uint32_t    height;         /* Frame height (e.g. 1080) */
    uint32_t    pixelformat;    /* V4L2 FourCC (e.g. 'YUYV', 'UYVY', 'BGR3') - 0 for default YUYV */
    uint32_t    fps_num;        /* Target FPS numerator (e.g. 30, 60, 0 = default 30) */
    uint32_t    fps_den;        /* Target FPS denominator (e.g. 1, 0 = default 1) */
} avx_v4l2_sink_config_t;

/**
 * Probes the system for available V4L2 loopback devices (e.g. created by v4l2loopback).
 * Returns true if a valid loopback device was found and writes path to out_path.
 */
OPENAVX_API bool avx_v4l2_sink_probe_loopback(char *out_path, size_t out_path_len);

/**
 * Creates and initializes a V4L2 Virtual Camera loopback sink.
 * If cfg->device_path is NULL or empty, automatically probes for an available loopback device.
 * Returns NULL on failure (e.g. device does not exist or permission denied).
 */
OPENAVX_API avx_v4l2_sink_t* avx_v4l2_sink_create(const avx_v4l2_sink_config_t *cfg);

/**
 * Dynamically reconfigures the output resolution or pixel format.
 */
OPENAVX_API int avx_v4l2_sink_set_format(avx_v4l2_sink_t *sink, uint32_t width, uint32_t height, uint32_t pixelformat);

/**
 * Writes an uncompressed frame buffer directly to the V4L2 device.
 */
OPENAVX_API int avx_v4l2_sink_write_raw(avx_v4l2_sink_t *sink, const void *data, size_t size);

/**
 * Writes an OpenAVX video frame, performing high-speed SIMD pixel format conversion
 * (e.g. UYVY -> YUYV or RGBA -> YUYV) if the frame format differs from the sink format.
 */
OPENAVX_API int avx_v4l2_sink_write_frame(avx_v4l2_sink_t *sink, const oavx_video_frame_t *frame);

/**
 * Returns the currently active V4L2 device path (e.g. "/dev/video10").
 */
OPENAVX_API const char* avx_v4l2_sink_get_device_path(const avx_v4l2_sink_t *sink);

/**
 * Closes the V4L2 device and frees all allocated buffers.
 */
OPENAVX_API void avx_v4l2_sink_destroy(avx_v4l2_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif /* AVX_V4L2_SINK_H */
