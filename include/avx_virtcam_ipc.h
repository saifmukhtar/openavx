/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_virtcam_ipc.h - Cross-platform shared memory IPC for Virtual Camera Plugins
 */

#ifndef AVX_VIRTCAM_IPC_H
#define AVX_VIRTCAM_IPC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVX_VIRTCAM_MAGIC          0x5856434DU /* 'MVXC' */
#define AVX_VIRTCAM_VERSION        1
#define AVX_VIRTCAM_POSIX_SHM_NAME "/openavx_virtcam_shm"
#define AVX_VIRTCAM_WIN_SHM_NAME   "Local\\OpenAVX_VirtualCamera_Shm"
#define AVX_VIRTCAM_MAX_FRAME_SIZE (3840 * 2160 * 4) /* Up to 4K BGRA */

/* Memory-mapped shared frame header */
typedef struct avx_virtcam_shm_header_t {
    uint32_t magic;           /* AVX_VIRTCAM_MAGIC */
    uint32_t version;         /* AVX_VIRTCAM_VERSION */
    uint32_t width;           /* Video width (e.g. 1920) */
    uint32_t height;          /* Video height (e.g. 1080) */
    uint32_t fourcc;          /* OAVX_FOURCC_VIDEO_TYPE_UYVY, YUYV, BGRA */
    uint32_t fps_num;         /* Framerate numerator (30) */
    uint32_t fps_den;         /* Framerate denominator (1) */
    uint32_t stride;          /* Row stride in bytes */
    uint32_t data_size;       /* Total frame bytes */
    uint64_t sequence;        /* Monotonically increasing frame index */
    int64_t  timestamp_ns;    /* Monotonic timestamp in nanoseconds */
    uint32_t clients_active;  /* Counter of active consuming applications (Zoom/Teams/Meet) */
    uint32_t reserved[7];
} avx_virtcam_shm_header_t;

/* Total mapped buffer layout: Header + Pixel Buffer */
typedef struct avx_virtcam_shm_layout_t {
    avx_virtcam_shm_header_t header;
    uint8_t                  pixels[AVX_VIRTCAM_MAX_FRAME_SIZE];
} avx_virtcam_shm_layout_t;

/* Opaque IPC Producer Handle (used by openavx-virtcam) */
typedef struct avx_virtcam_producer_t avx_virtcam_producer_t;

/* Opaque IPC Consumer Handle (used by DirectShow DLL & CoreMediaIO DAL) */
typedef struct avx_virtcam_consumer_t avx_virtcam_consumer_t;

/**
 * Creates and initializes the shared memory region as a video producer.
 */
OPENAVX_API avx_virtcam_producer_t *avx_virtcam_producer_create(void);

/**
 * Destroys the shared memory producer and unmaps memory.
 */
OPENAVX_API void avx_virtcam_producer_destroy(avx_virtcam_producer_t *prod);

/**
 * Pushes a video frame into shared memory for consuming camera plugins (DirectShow / DAL).
 */
OPENAVX_API bool avx_virtcam_producer_write_frame(
    avx_virtcam_producer_t *prod,
    const uint8_t *data,
    size_t size,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t fourcc,
    uint32_t fps_num,
    uint32_t fps_den
);

/**
 * Checks if at least one consumer (Zoom, Teams, Meet) is actively reading the virtual camera.
 */
OPENAVX_API bool avx_virtcam_producer_has_clients(const avx_virtcam_producer_t *prod);

/**
 * Connects to the shared memory region as a video consumer (plugin side).
 */
OPENAVX_API avx_virtcam_consumer_t *avx_virtcam_consumer_create(void);

/**
 * Disconnects the consumer from the shared memory region.
 */
OPENAVX_API void avx_virtcam_consumer_destroy(avx_virtcam_consumer_t *cons);

/**
 * Reads the latest available frame from the shared memory region.
 * Returns true if a new frame was available and copied.
 */
OPENAVX_API bool avx_virtcam_consumer_read_frame(
    avx_virtcam_consumer_t *cons,
    avx_virtcam_shm_header_t *out_header,
    uint8_t *out_buffer,
    size_t buffer_max,
    uint64_t *inout_last_seq
);

#ifdef __cplusplus
}
#endif

#endif /* AVX_VIRTCAM_IPC_H */
