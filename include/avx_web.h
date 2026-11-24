/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_web.h - Embedded HTTP and fragmented MP4 preview server
 */

#ifndef AVX_WEB_H
#define AVX_WEB_H

#include "openavx.h"
#include "avx_rtsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum concurrent browser preview clients */
#define AVX_WEB_MAX_CLIENTS 32

/* Magic number for binary web frame envelopes ('OAVX' = 0x5856414F) */
#define AVX_WEB_FRAME_MAGIC 0x5856414FU

/* Binary frame header transmitted by browser WebSocket clients */
typedef struct avx_web_frame_header_t {
    uint32_t magic;      /* AVX_WEB_FRAME_MAGIC */
    uint32_t fourcc;     /* OAVX_FOURCC_VIDEO_TYPE_H264 or 'MJPG' (0x47504A4D) */
    uint16_t width;      /* Video frame width */
    uint16_t height;     /* Video frame height */
    uint32_t flags;      /* bit 0: 1 = keyframe / IDR */
} avx_web_frame_header_t;

/* Callback invoked when a browser streams a video frame in (phone camera ingress) */
typedef void (*avx_web_ingress_frame_cb)(
    const uint8_t *frame_data,
    size_t frame_size,
    uint32_t fourcc,
    uint32_t width,
    uint32_t height,
    bool is_keyframe,
    void *user_ctx
);

/* Opaque server handle */
typedef struct avx_web_server_t avx_web_server_t;

/* Server configuration */
typedef struct avx_web_server_config_t {
    uint16_t port;                /* HTTP port (e.g. 8080, or 0 for ephemeral/auto-assign) */
    const char *stream_name;      /* Stream title displayed on the web monitor HUD */
    avx_rtsp_codec_e codec;         /* Video codec: AVX_RTSP_CODEC_H264, HEVC, AV1 */
    void (*on_keyframe_request)(void *user_ctx); /* Called when a browser connects to request instant IDR */
    avx_web_ingress_frame_cb on_ingress_frame;   /* Ingress callback for smartphone camera streaming */
    void (*on_tally_change)(void *user_ctx, const oavx_tally_t *tally); /* Ingress callback for REST tally set */
    void *user_ctx;               /* User context passed to callbacks */
} avx_web_server_config_t;

/* ========================================================================= */
/* 1. EMBEDDED HTTP / fMP4 SERVER ENGINE                                     */
/* ========================================================================= */

/**
 * Creates and starts the embedded HTTP/fMP4 web preview server on a background thread.
 * Returns server handle, or NULL on failure.
 */
OPENAVX_API avx_web_server_t* avx_web_server_create(const avx_web_server_config_t *config);

/**
 * Destroys the web server, gracefully closing all active HTTP client connections.
 */
OPENAVX_API void avx_web_server_destroy(avx_web_server_t *server);

/**
 * Returns the active TCP listening port (useful if port 0 was passed to auto-assign).
 */
OPENAVX_API uint16_t avx_web_server_get_port(const avx_web_server_t *server);

/**
 * Returns the number of currently connected browser clients.
 */
OPENAVX_API int avx_web_server_get_client_count(const avx_web_server_t *server);

/**
 * Feeds an encoded video frame (H.264/HEVC/AV1) into the web server.
 * The server packages the frame into fMP4 fragments (moof + mdat) and transmits them
 * to all connected browser clients.
 * Returns true if frame was processed and enqueued to clients.
 */
OPENAVX_API bool avx_web_server_feed_frame(
    avx_web_server_t *server,
    const uint8_t *frame_data,
    size_t frame_size,
    uint64_t pts_ns,
    bool is_keyframe
);

/**
 * Registers or updates the camera ingress frame callback at runtime.
 */
OPENAVX_API void avx_web_server_set_ingress_callback(
    avx_web_server_t *server,
    avx_web_ingress_frame_cb cb,
    void *user_ctx
);

/**
 * Broadcasts updated tally status (Program / Preview) to all active WebSocket clients.
 */
OPENAVX_API void avx_web_server_broadcast_tally(avx_web_server_t *server, const oavx_tally_t *tally);

/**
 * Computes standard FIPS 180-1 SHA-1 hash (20 bytes output).
 */
OPENAVX_API void avx_sha1_hash(const uint8_t *data, size_t len, uint8_t digest[20]);

/**
 * Computes RFC 6455 Sec-WebSocket-Accept token from a client's Sec-WebSocket-Key.
 * out_accept must be at least 32 bytes. Returns length of token.
 */
OPENAVX_API size_t avx_websocket_compute_accept(const char *client_key, char *out_accept, size_t out_max);

/* ========================================================================= */
/* 2. PURE C99 ISOBMFF / fMP4 PACKAGING ENGINE                               */
/* ========================================================================= */

/**
 * Pure C99 fMP4 Initialization Segment Generator (ftyp + moov).
 * Produces the ISO BMFF init segment for MSE SourceBuffer initialization.
 * Returns bytes written, or 0 on error/insufficient buffer.
 */
OPENAVX_API size_t avx_fmp4_generate_init_segment(
    avx_rtsp_codec_e codec,
    uint32_t width,
    uint32_t height,
    const uint8_t *sps,
    size_t sps_len,
    const uint8_t *pps,
    size_t pps_len,
    const uint8_t *vps,
    size_t vps_len,
    uint8_t *out_buf,
    size_t out_max
);

/**
 * Pure C99 fMP4 Media Fragment Generator (moof + mdat).
 * Produces the movie fragment for a single video sample.
 * Returns bytes written, or 0 on error/insufficient buffer.
 */
OPENAVX_API size_t avx_fmp4_generate_media_fragment(
    uint32_t sequence_number,
    uint64_t base_decode_time_90khz,
    uint32_t sample_duration_90khz,
    bool is_keyframe,
    const uint8_t *sample_avcc_data,
    size_t sample_avcc_len,
    uint8_t *out_buf,
    size_t out_max
);

#ifdef __cplusplus
}
#endif

#endif /* AVX_WEB_H */
