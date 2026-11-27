/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_video_codec.h - Decoupled internal video encoder & decoder contracts
 */

#ifndef AVX_VIDEO_CODEC_H
#define AVX_VIDEO_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard Video Codec Return Codes */
#define AVX_VIDEO_CODEC_OK 0
#define AVX_VIDEO_CODEC_NEED_MORE_DATA 1 /* Decoder buffered input (priming/B-frame); no frame yet */
#define AVX_VIDEO_CODEC_ERROR (-1)

/* ========================================================================= */
/* Opaque Per-Stream Video Encoder & Decoder Handles                         */
/* ========================================================================= */

typedef struct avx_video_encoder_t avx_video_encoder_t;
typedef struct avx_video_decoder_t avx_video_decoder_t;

/* ========================================================================= */
/* Video Encoder Configuration & Backend Contract                            */
/* ========================================================================= */

typedef struct avx_video_enc_cfg_t {
    uint32_t fourcc;     /* Target compressed FourCC (e.g. SHQ2) */
    uint32_t src_fourcc; /* Source pixel FourCC (e.g. P216 for 10-bit encode) */
    uint32_t width;
    uint32_t height;
    uint8_t quality;       /* 0 = backend default, 20..98 for SpeedHQ */
    uint32_t bitrate_kbps; /* Target bitrate (0 = backend/quality default) */
    int32_t fps_num;
    int32_t fps_den;
    uint32_t keyframe_interval; /* 0 = backend default / intra-frame */
} avx_video_enc_cfg_t;

typedef struct avx_video_encoder_ops_t {
    const char *name;
    int priority; /* Higher priority backend wins when multiple support FourCC */
    bool is_hardware;

    bool (*supports_fourcc)(uint32_t target_fourcc);
    int (*create)(const avx_video_enc_cfg_t *cfg, void **backend_ctx);
    int (*encode)(void *backend_ctx, const oavx_video_frame_t *in_frame, uint8_t *out_buf, size_t out_cap,
                  size_t *out_size, bool *out_is_keyframe);
    int (*set_quality)(void *backend_ctx, uint8_t quality);
    void (*request_keyframe)(void *backend_ctx);
    const char *(*get_name)(void *backend_ctx);
    void (*destroy)(void *backend_ctx);
} avx_video_encoder_ops_t;

/* ========================================================================= */
/* Video Decoder Configuration & Backend Contract                            */
/* ========================================================================= */

typedef struct avx_video_dec_cfg_t {
    uint32_t fourcc;           /* Compressed input FourCC (SHQ2, H264, HEVC, AV01, ...) */
    uint32_t width;            /* Initial frame width (0 if determined from bitstream) */
    uint32_t height;           /* Initial frame height (0 if determined from bitstream) */
    uint32_t preferred_fourcc; /* Hint for output pixel FourCC (0 = backend's natural layout) */
} avx_video_dec_cfg_t;

typedef struct avx_video_decoder_ops_t {
    const char *name;
    int priority; /* Higher priority backend wins when multiple support FourCC */
    bool is_hardware;

    bool (*supports_fourcc)(uint32_t src_fourcc);
    int (*create)(const avx_video_dec_cfg_t *cfg, void **backend_ctx);

    /**
     * Decodes one compressed packet into `out_frame`.
     * Backend populates `out_frame->fourcc`, `width`, `height`, `planes[4]`, `strides[4]`
     * pointing to backend-owned memory valid until next decode/reset/destroy call.
     * Returns:
     *   AVX_VIDEO_CODEC_OK (0)             when `out_frame` contains a decoded frame,
     *   AVX_VIDEO_CODEC_NEED_MORE_DATA (1) when input was consumed/buffered with no frame yet,
     *   < 0                                on fatal decode error.
     */
    int (*decode)(void *backend_ctx, const uint8_t *pkt_data, size_t pkt_size, oavx_video_frame_t *out_frame);
    void (*reset)(void *backend_ctx);
    void (*destroy)(void *backend_ctx);
} avx_video_decoder_ops_t;

/* ========================================================================= */
/* Codec Subsystem Lifecycle & Backend Registration                          */
/* ========================================================================= */

/**
 * Initializes the video codec subsystem and registers built-in backends (SpeedHQ).
 * Safe to call repeatedly; idempotent under internal locking.
 */
OPENAVX_API bool avx_video_codec_init(void);

/**
 * Releases registered backend table references and shuts down built-in video codecs.
 */
OPENAVX_API void avx_video_codec_destroy(void);

/**
 * Registers an encoder backend. Backends remain independent from decoder backends.
 */
OPENAVX_API bool avx_video_encoder_register(const avx_video_encoder_ops_t *ops);

/**
 * Registers a decoder backend. Backends remain independent from encoder backends.
 */
OPENAVX_API bool avx_video_decoder_register(const avx_video_decoder_ops_t *ops);

/**
 * Returns true if at least one registered encoder backend accepts `target_fourcc`.
 */
OPENAVX_API bool avx_video_encoder_has_backend(uint32_t target_fourcc);

/**
 * Returns true if at least one registered decoder backend accepts `src_fourcc`.
 */
OPENAVX_API bool avx_video_decoder_has_backend(uint32_t src_fourcc);

/* ========================================================================= */
/* Per-Stream Video Encoder Operations                                       */
/* ========================================================================= */

OPENAVX_API avx_video_encoder_t *avx_video_encoder_create(const avx_video_enc_cfg_t *cfg);
OPENAVX_API void avx_video_encoder_destroy(avx_video_encoder_t *enc);
OPENAVX_API int avx_video_encoder_encode(avx_video_encoder_t *enc, const oavx_video_frame_t *in_frame, uint8_t *out_buf,
                                         size_t out_cap, size_t *out_size, bool *out_is_keyframe);
OPENAVX_API int avx_video_encoder_set_quality(avx_video_encoder_t *enc, uint8_t quality);
OPENAVX_API void avx_video_encoder_request_keyframe(avx_video_encoder_t *enc);
OPENAVX_API uint32_t avx_video_encoder_get_fourcc(const avx_video_encoder_t *enc);
OPENAVX_API const char *avx_video_encoder_get_backend_name(const avx_video_encoder_t *enc);

/* ========================================================================= */
/* Per-Stream Video Decoder Operations                                       */
/* ========================================================================= */

OPENAVX_API avx_video_decoder_t *avx_video_decoder_create(const avx_video_dec_cfg_t *cfg);
OPENAVX_API void avx_video_decoder_destroy(avx_video_decoder_t *dec);
OPENAVX_API int avx_video_decoder_decode(avx_video_decoder_t *dec, const uint8_t *pkt_data, size_t pkt_size,
                                         oavx_video_frame_t *out_frame);
OPENAVX_API void avx_video_decoder_reset(avx_video_decoder_t *dec);
OPENAVX_API uint32_t avx_video_decoder_get_fourcc(const avx_video_decoder_t *dec);
OPENAVX_API const char *avx_video_decoder_get_backend_name(const avx_video_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif /* AVX_VIDEO_CODEC_H */
