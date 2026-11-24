/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_pipewire_sink.h - Native user-space PipeWire virtual camera sink
 */

#ifndef AVX_PIPEWIRE_SINK_H
#define AVX_PIPEWIRE_SINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque PipeWire virtual camera sink handle */
typedef struct avx_pipewire_sink_t avx_pipewire_sink_t;

/* PipeWire sink configuration */
typedef struct avx_pipewire_sink_config_t {
    const char *node_name;        /* Node name, e.g. "OpenAVX Virtual Camera" */
    const char *node_description; /* Node human description, e.g. "OpenAVX Virtual Camera Feed" */
    uint32_t width;               /* Initial frame width (default: 1920) */
    uint32_t height;              /* Initial frame height (default: 1080) */
    uint32_t fps_num;             /* Framerate numerator (default: 30) */
    uint32_t fps_den;             /* Framerate denominator (default: 1) */
    uint32_t preferred_fourcc;     /* Target pixel format: OAVX_FOURCC_VIDEO_TYPE_UYVY or YUYV */
} avx_pipewire_sink_config_t;

/**
 * Checks if the PipeWire daemon is currently available on the system.
 */
OPENAVX_API bool avx_pipewire_is_available(void);

/**
 * Creates and starts a native PipeWire virtual camera video source (media.class = "Video/Source").
 * The PipeWire event loop runs in a dedicated background worker thread.
 * Returns sink handle on success, NULL on failure.
 */
OPENAVX_API avx_pipewire_sink_t *avx_pipewire_sink_create(const avx_pipewire_sink_config_t *config);

/**
 * Stops and destroys the PipeWire virtual camera sink, unregistering the camera node from PipeWire.
 */
OPENAVX_API void avx_pipewire_sink_destroy(avx_pipewire_sink_t *sink);

/**
 * Pushes a video frame into the PipeWire video stream.
 * Automatically performs format conversion (e.g. UYVY -> YUY2 / RGBA) if necessary.
 * Thread-safe: can be called from any worker thread.
 */
OPENAVX_API bool avx_pipewire_sink_push_frame(
    avx_pipewire_sink_t *sink,
    const uint8_t *frame_data,
    size_t frame_size,
    uint32_t width,
    uint32_t height,
    uint32_t fourcc
);

/**
 * Returns true if at least one client (e.g. Chrome, Firefox, Discord, Zoom) is currently consuming the stream.
 */
OPENAVX_API bool avx_pipewire_sink_is_active(const avx_pipewire_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif /* AVX_PIPEWIRE_SINK_H */
