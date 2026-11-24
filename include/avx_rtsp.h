/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_rtsp.h - Native RFC 2326 RTSP 1.0 & RFC 3550/6184/7798/9584 RTP Engine
 */

#ifndef AVX_RTSP_H
#define AVX_RTSP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Codec Identifiers for Elementary Streams */
typedef enum avx_rtsp_codec_e {
    AVX_RTSP_CODEC_UNKNOWN = 0,
    AVX_RTSP_CODEC_H264    = 1, /* Advanced Video Coding (AVC / H.264) */
    AVX_RTSP_CODEC_HEVC    = 2, /* High Efficiency Video Coding (H.265 / HEVC) */
    AVX_RTSP_CODEC_AV1     = 3  /* AOMedia Video 1 (AV1) */
} avx_rtsp_codec_e;

/* Reassembled Frame Type */
typedef enum avx_rtsp_frame_type_e {
    AVX_RTSP_FRAME_TYPE_UNKNOWN   = 0,
    AVX_RTSP_FRAME_TYPE_KEYFRAME  = 1, /* IDR / Keyframe (Instant Random Access) */
    AVX_RTSP_FRAME_TYPE_DELTA     = 2, /* P-Frame / B-Frame predicted slice */
    AVX_RTSP_FRAME_TYPE_CONFIG    = 3  /* Out-of-band parameter set update (SPS/PPS/VPS) */
} avx_rtsp_frame_type_e;

/* Reassembled Elementary Stream Frame */
typedef struct avx_rtsp_frame_t {
    avx_rtsp_codec_e      codec;
    avx_rtsp_frame_type_e frame_type;
    uint32_t              fourcc;         /* OAVX_FOURCC_VIDEO_TYPE_H264, HEVC, AV01 */
    int                   width;          /* Display resolution extracted from SPS */
    int                   height;
    int                   frame_rate_n;   /* Frame rate numerator (e.g. 60000) */
    int                   frame_rate_d;   /* Frame rate denominator (e.g. 1000) */
    uint32_t              rtp_timestamp;
    int64_t               timecode;
    const uint8_t*        data;           /* Complete Annex-B byte stream with start codes */
    size_t                size;
} avx_rtsp_frame_t;

/* Opaque Demuxer Context */
typedef struct avx_rtsp_demuxer_t avx_rtsp_demuxer_t;

/* Opaque RTSP 1.0 Client Context */
typedef struct avx_rtsp_t avx_rtsp_t;

/* ========================================================================= */
/* 1. DEMUXER LIFECYCLE & INGESTION APIS                                     */
/* ========================================================================= */

/**
 * Creates an elementary stream demuxer instance.
 * default_codec: Initial expected codec (AVX_RTSP_CODEC_H264 or AVX_RTSP_CODEC_HEVC).
 */
OPENAVX_API avx_rtsp_demuxer_t* avx_rtsp_demuxer_create(avx_rtsp_codec_e default_codec);

/**
 * Destroys a demuxer instance and frees all internal reassembly buffers.
 */
OPENAVX_API void avx_rtsp_demuxer_destroy(avx_rtsp_demuxer_t* demux);

/**
 * Resets demuxer reassembly buffers and sequence counters (retains cached parameter sets).
 */
OPENAVX_API void avx_rtsp_demuxer_reset(avx_rtsp_demuxer_t* demux);

/**
 * Ingests an incoming RTP packet (RFC 3550, RFC 6184, RFC 7798, RFC 9584).
 * Returns true if an entire Access Unit (video frame) was completed by this packet.
 */
OPENAVX_API bool avx_rtsp_demuxer_feed_rtp(
    avx_rtsp_demuxer_t* demux,
    const uint8_t* rtp_packet,
    size_t packet_size
);

/**
 * Ingests a raw NAL unit (e.g. from an OpenAVX stream socket or length-prefixed AVCC source).
 * is_last_slice: true if this NAL completes the current video frame.
 */
OPENAVX_API bool avx_rtsp_demuxer_feed_nalu(
    avx_rtsp_demuxer_t* demux,
    const uint8_t* nalu_data,
    size_t nalu_size,
    bool is_last_slice,
    uint32_t timestamp
);

/**
 * Retrieves the most recently completed reassembled Annex-B frame.
 * The buffer in out_frame->data remains valid until the next feed_* or reset call.
 */
OPENAVX_API bool avx_rtsp_demuxer_get_frame(
    avx_rtsp_demuxer_t* demux,
    avx_rtsp_frame_t* out_frame
);

/**
 * Manually injects out-of-band parameter sets (e.g. from RTSP SDP sprop-parameter-sets).
 * Cached parameter sets are automatically prepended to incoming orphan IDR keyframes.
 */
OPENAVX_API bool avx_rtsp_demuxer_set_parameter_sets(
    avx_rtsp_demuxer_t* demux,
    avx_rtsp_codec_e codec,
    const uint8_t* sps, size_t sps_len,
    const uint8_t* pps, size_t pps_len,
    const uint8_t* vps, size_t vps_len
);

/* ========================================================================= */
/* 2. STANDALONE BITSTREAM & PARAMETER PARSING                               */
/* ========================================================================= */

/**
 * Parses an H.264 Sequence Parameter Set (SPS) in pure C99 (Exp-Golomb decoder).
 * Extracts display width, height, profile, and level with cropping applied.
 */
OPENAVX_API bool avx_rtsp_parse_sps_h264(
    const uint8_t* sps,
    size_t size,
    int* out_width,
    int* out_height,
    int* out_profile,
    int* out_level
);

/**
 * Parses an HEVC (H.265) Sequence Parameter Set (SPS) in pure C99.
 * Extracts display width and height with conformance window offsets applied.
 */
OPENAVX_API bool avx_rtsp_parse_sps_hevc(
    const uint8_t* sps,
    size_t size,
    int* out_width,
    int* out_height,
    int* out_profile,
    int* out_level
);

/**
 * Parses an AV1 Sequence Header OBU in pure C99.
 * Extracts display width, height, profile, and level.
 */
OPENAVX_API bool avx_rtsp_parse_sequence_header_av1(
    const uint8_t* obu,
    size_t size,
    int* out_width,
    int* out_height,
    int* out_profile,
    int* out_level
);

/**
 * Inspects a NAL unit to determine whether it is an IDR / Keyframe.
 */
OPENAVX_API bool avx_rtsp_is_keyframe(avx_rtsp_codec_e codec, const uint8_t* nalu, size_t size);

/* ========================================================================= */
/* 3. RTSP 1.0 CLIENT ENGINE (RFC 2326)                                      */
/* ========================================================================= */

/**
 * Connects to an RTSP server (e.g. rtsp://192.168.1.100:554/live/ch0).
 * Performs OPTIONS, DESCRIBE, parses SDP for tracks/codecs, and negotiates SETUP.
 * Uses RTP-over-TCP interleaved mode by default for zero firewall issues.
 */
OPENAVX_API avx_rtsp_t* avx_rtsp_connect(const char* url, int timeout_ms);

/**
 * Closes an RTSP session and issues TEARDOWN.
 */
OPENAVX_API void avx_rtsp_close(avx_rtsp_t* rtsp);

/**
 * Sends an RTCP Picture Loss Indication (PLI / RFC 4585) or Full Intra Request (FIR)
 * to request an instant IDR refresh from the camera.
 */
OPENAVX_API bool avx_rtsp_send_keyframe_request(avx_rtsp_t* rtsp);

/**
 * Reads the next interleaved RTP packet or demuxed frame from the RTSP session.
 * Automatically feeds demuxer and returns true when a complete frame is ready.
 */
OPENAVX_API int avx_rtsp_read_frame(
    avx_rtsp_t* rtsp,
    avx_rtsp_demuxer_t* demux,
    avx_rtsp_frame_t* out_frame,
    int timeout_ms
);

/* ========================================================================= */
/* 4. RTP PACKETIZATION ENGINE (RFC 6184, RFC 7798, RFC 9584)               */
/* ========================================================================= */

typedef void (*avx_rtsp_rtp_packet_cb)(const uint8_t *rtp_pkt, size_t rtp_len, void *user_data);

/**
 * Pure C99 Base64 Encoder (RFC 4648).
 * dst must have capacity of at least ((src_len + 2) / 3) * 4 + 1.
 * Returns length of generated base64 string excluding null terminator.
 */
OPENAVX_API size_t avx_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_max);

/**
 * Packetizes a NAL unit into standard RTP packets.
 * For H.264: Single NAL (nalu_len <= 1400) or FU-A (RFC 6184 Section 5.8).
 * For HEVC:  Single NAL (nalu_len <= 1400) or FU (RFC 7798 Section 4.4.3).
 * For AV1:   Single OBU or fragmented chunks (RFC 9584).
 * Calls cb(...) for each generated RTP packet.
 * Returns the number of RTP packets generated.
 */
OPENAVX_API size_t avx_rtsp_rtp_packetize_nalu(
    avx_rtsp_codec_e codec,
    const uint8_t *nalu,
    size_t nalu_len,
    uint32_t rtp_timestamp,
    uint16_t *seq_num,
    uint32_t ssrc,
    bool is_last_nalu_in_frame,
    avx_rtsp_rtp_packet_cb cb,
    void *user_data
);

/* ========================================================================= */
/* 5. NATIVE RTSP 1.0 SERVER ENGINE (RFC 2326, RFC 4566)                     */
/* ========================================================================= */

typedef struct avx_rtsp_server_t avx_rtsp_server_t;

typedef struct avx_rtsp_server_config_t {
    uint16_t port;               /* RTSP listener port (0 defaults to 8554, or ephemeral if specified) */
    const char *path;            /* RTSP mount path (e.g. "/live", NULL defaults to "/live") */
    avx_rtsp_codec_e codec;      /* Video codec: AVX_RTSP_CODEC_H264, HEVC, AV1 */
    void (*on_keyframe_request)(void *user_ctx); /* Called when a client issues PLAY or sends RTCP PLI */
    void *user_ctx;              /* User context passed to callbacks */
} avx_rtsp_server_config_t;

/**
 * Creates and starts an RTSP 1.0 Server instance on a background thread.
 * Returns server handle, or NULL on failure.
 */
OPENAVX_API avx_rtsp_server_t* avx_rtsp_server_create(const avx_rtsp_server_config_t *config);

/**
 * Stops and destroys an RTSP 1.0 Server instance, disconnecting all clients.
 */
OPENAVX_API void avx_rtsp_server_destroy(avx_rtsp_server_t *srv);

/**
 * Returns the number of actively playing RTSP clients.
 */
OPENAVX_API int avx_rtsp_server_get_client_count(avx_rtsp_server_t *srv);

/**
 * Returns the active port number the RTSP server is listening on.
 */
OPENAVX_API uint16_t avx_rtsp_server_get_port(avx_rtsp_server_t *srv);

/**
 * Ingests an Annex-B elementary stream frame, parses parameter sets,
 * packetizes NALUs into RTP, and streams interleaved packets to all active RTSP clients.
 */
OPENAVX_API bool avx_rtsp_server_feed_frame(
    avx_rtsp_server_t *srv,
    const uint8_t *data,
    size_t size,
    int64_t pts_ns,
    bool is_keyframe
);

#ifdef __cplusplus
}
#endif

#endif /* AVX_RTSP_H */
