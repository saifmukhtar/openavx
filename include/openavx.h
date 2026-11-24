/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/openavx.h - Primary public C API
 */

#ifndef OPENAVX_H
#define OPENAVX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(_WIN32)
    #ifdef OPENAVX_EXPORTS
        #define OPENAVX_API __declspec(dllexport)
    #else
        #define OPENAVX_API __declspec(dllimport)
    #endif
#else
    #define OPENAVX_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Rational Number Representation */
typedef struct oavx_rational {
    uint32_t num;
    uint32_t den;
} oavx_rational_t;

/* FourCC Definitions for Video */
typedef enum oavx_fourcc_video_type_e {
    OAVX_FOURCC_VIDEO_TYPE_UYVY = 0x59565955, /* "UYVY" */
    OAVX_FOURCC_VIDEO_TYPE_BGRA = 0x41524742, /* "BGRA" */
    OAVX_FOURCC_VIDEO_TYPE_BGRX = 0x58524742, /* "BGRX" */
    OAVX_FOURCC_VIDEO_TYPE_RGBA = 0x41424752, /* "RGBA" */
    OAVX_FOURCC_VIDEO_TYPE_RGBX = 0x58424752, /* "RGBX" */
    OAVX_FOURCC_VIDEO_TYPE_UYVA = 0x41565955, /* "UYVA" */
    OAVX_FOURCC_VIDEO_TYPE_P216 = 0x36313250, /* "P216" 16-bit 4:2:2 */
    OAVX_FOURCC_VIDEO_TYPE_PA16 = 0x36314150, /* "PA16" 16-bit 4:2:2:4 */
    OAVX_FOURCC_VIDEO_TYPE_YV12 = 0x32315659, /* "YV12" */
    OAVX_FOURCC_VIDEO_TYPE_I420 = 0x30323449, /* "I420" */
    OAVX_FOURCC_VIDEO_TYPE_NV12 = 0x3231564E, /* "NV12" */

    /* SpeedHQ Intra-DCT Compressed FourCCs */
    OAVX_FOURCC_VIDEO_TYPE_SHQ0 = 0x30514853, /* "SHQ0" 4:2:0 */
    OAVX_FOURCC_VIDEO_TYPE_SHQ1 = 0x31514853, /* "SHQ1" 4:2:0 + Alpha */
    OAVX_FOURCC_VIDEO_TYPE_SHQ2 = 0x32514853, /* "SHQ2" 4:2:2 Standard */
    OAVX_FOURCC_VIDEO_TYPE_SHQ3 = 0x33514853, /* "SHQ3" 4:2:2 + Alpha */
    OAVX_FOURCC_VIDEO_TYPE_SHQ4 = 0x34514853, /* "SHQ4" 4:4:4 */
    OAVX_FOURCC_VIDEO_TYPE_SHQ5 = 0x35514853, /* "SHQ5" 4:4:4 + Alpha */
    OAVX_FOURCC_VIDEO_TYPE_SHQ7 = 0x37514853, /* "SHQ7" 4:2:2 Alpha luma */
    OAVX_FOURCC_VIDEO_TYPE_SHQ9 = 0x39514853, /* "SHQ9" 4:4:4 Alpha luma */

    /* Compressed FourCCs (H.264 / HEVC / AV1 / MJPEG) */
    OAVX_FOURCC_VIDEO_TYPE_H264 = 0x34363248, /* "H264" */
    OAVX_FOURCC_VIDEO_TYPE_HEVC = 0x43564548, /* "HEVC" */
    OAVX_FOURCC_VIDEO_TYPE_AV01 = 0x31305641, /* "AV01" */
    OAVX_FOURCC_VIDEO_TYPE_MJPEG = 0x47504A4D, /* "MJPG" */
    OAVX_FOURCC_VIDEO_TYPE_COUNT = 21
} oavx_fourcc_video_type_e;

typedef oavx_fourcc_video_type_e oavx_fourcc_t;

/* FourCC Definitions for Audio */
typedef enum oavx_fourcc_audio_type_e {
    OAVX_FOURCC_AUDIO_TYPE_FLTP  = 0x50544C46, /* "FLTP" 32-bit floating point */
    OAVX_FOURCC_AUDIO_TYPE_OPUS  = 0x5355504F, /* "OPUS" Opus compressed audio */
    OAVX_FOURCC_AUDIO_TYPE_ADPC  = 0x43504441, /* "ADPC" IMA ADPCM 4:1 audio */
    OAVX_FOURCC_AUDIO_TYPE_COUNT = 3
} oavx_fourcc_audio_type_e;

/* Audio Compression Operating Modes */
typedef enum oavx_audio_compression_mode_e {
    OAVX_AUDIO_COMPRESSION_OFF      = 0, /* Always uncompressed 32-bit float PCM */
    OAVX_AUDIO_COMPRESSION_MANUAL   = 1, /* Always compressed (Opus if available, ADPCM fallback) */
    OAVX_AUDIO_COMPRESSION_ADAPTIVE = 2  /* Uncompressed on clean LAN; auto-compresses under congestion */
} oavx_audio_compression_mode_e;

typedef oavx_audio_compression_mode_e oavx_audio_compression_mode_t;

/* Standard SMPTE Scan / Interlacing Types */
typedef enum oavx_scan_type_e {
    OAVX_SCAN_PROGRESSIVE        = 0,
    OAVX_SCAN_TOP_FIELD_FIRST    = 1,
    OAVX_SCAN_BOTTOM_FIELD_FIRST = 2,
    OAVX_SCAN_COUNT              = 3
} oavx_scan_type_e;

typedef oavx_scan_type_e oavx_scan_type_t;

/* Audio Format Types */
typedef enum oavx_audio_format_e {
    OAVX_AUDIO_FMT_FLT_PLANAR      = 0,
    OAVX_AUDIO_FMT_FLT_INTERLEAVED = 1,
    OAVX_AUDIO_FMT_S16_INTERLEAVED = 2,
    OAVX_AUDIO_FMT_S32_INTERLEAVED = 3,
    OAVX_AUDIO_FMT_COUNT           = 4
} oavx_audio_format_e;

typedef oavx_audio_format_e oavx_audio_format_t;

/* Frame Types returned by Receiver / Sender */
typedef enum oavx_frame_type_e {
    OAVX_FRAME_TYPE_NONE          = 0,
    OAVX_FRAME_TYPE_VIDEO         = 1,
    OAVX_FRAME_TYPE_AUDIO         = 2,
    OAVX_FRAME_TYPE_METADATA      = 3,
    OAVX_FRAME_TYPE_ERROR         = 4,
    OAVX_FRAME_TYPE_STATUS_CHANGE = 5,
    OAVX_FRAME_TYPE_COUNT         = 6
} oavx_frame_type_e;

typedef oavx_frame_type_e oavx_frame_type_t;

/* Stream Source / Endpoint Descriptor */
typedef struct oavx_endpoint {
    const char *name;
    const char *address;       /* IP or hostname */
    uint16_t    port;
    const char *group;
} oavx_endpoint_t;

/* Native Video Frame Descriptor (Planar-aware, modern media layout) */
typedef struct oavx_video_frame {
    /* Dimensions & Pixel Format */
    uint32_t                    width;
    uint32_t                    height;
    oavx_fourcc_video_type_e    fourcc;
    oavx_scan_type_t            scan_type;

    /* Buffers (Planar-aware, supports packed & multi-plane formats) */
    uint8_t                    *planes[4];
    size_t                      strides[4];

    /* Timing & Sync */
    oavx_rational_t             framerate;
    oavx_rational_t             aspect_ratio;
    int64_t                     pts_ns;          /* Presentation timestamp in nanoseconds */
    int64_t                     dts_ns;          /* Decode timestamp in nanoseconds */

    /* Extended Metadata (opaque string or key-value) */
    const char                 *metadata;
    size_t                      metadata_len;

    /* User / Memory Context */
    void                       *user_ctx;        /* For custom buffer release callbacks */
} oavx_video_frame_t;

/* Maximum supported audio channels across OpenAVX (Pro-Audio / Dante / SDI 16-ch / 32-ch standard) */
#define OAVX_MAX_AUDIO_CHANNELS 32

/* Standard Channel Position Bitmasks (compatible with SMPTE, Microsoft WAVEFORMATEXTENSIBLE, ALSA, CoreAudio) */
typedef enum oavx_audio_channel_mask_e {
    OAVX_AUDIO_CH_NONE                     = 0ULL,
    OAVX_AUDIO_CH_FRONT_LEFT               = (1ULL << 0),  /* FL */
    OAVX_AUDIO_CH_FRONT_RIGHT              = (1ULL << 1),  /* FR */
    OAVX_AUDIO_CH_FRONT_CENTER             = (1ULL << 2),  /* FC / C */
    OAVX_AUDIO_CH_LOW_FREQUENCY            = (1ULL << 3),  /* LFE / Subwoofer */
    OAVX_AUDIO_CH_BACK_LEFT                = (1ULL << 4),  /* BL */
    OAVX_AUDIO_CH_BACK_RIGHT               = (1ULL << 5),  /* BR */
    OAVX_AUDIO_CH_FRONT_LEFT_OF_CENTER     = (1ULL << 6),  /* FLC */
    OAVX_AUDIO_CH_FRONT_RIGHT_OF_CENTER    = (1ULL << 7),  /* FRC */
    OAVX_AUDIO_CH_BACK_CENTER              = (1ULL << 8),  /* BC */
    OAVX_AUDIO_CH_SIDE_LEFT                = (1ULL << 9),  /* SL */
    OAVX_AUDIO_CH_SIDE_RIGHT               = (1ULL << 10), /* SR */
    OAVX_AUDIO_CH_TOP_CENTER               = (1ULL << 11), /* TC */
    OAVX_AUDIO_CH_TOP_FRONT_LEFT           = (1ULL << 12), /* TFL */
    OAVX_AUDIO_CH_TOP_FRONT_CENTER         = (1ULL << 13), /* TFC */
    OAVX_AUDIO_CH_TOP_FRONT_RIGHT          = (1ULL << 14), /* TFR */
    OAVX_AUDIO_CH_TOP_BACK_LEFT            = (1ULL << 15), /* TBL */
    OAVX_AUDIO_CH_TOP_BACK_CENTER          = (1ULL << 16), /* TBC */
    OAVX_AUDIO_CH_TOP_BACK_RIGHT           = (1ULL << 17), /* TBR */

    /* Spatial Layout Standard Masks */
    OAVX_AUDIO_MASK_MONO   = (OAVX_AUDIO_CH_FRONT_CENTER),
    OAVX_AUDIO_MASK_STEREO = (OAVX_AUDIO_CH_FRONT_LEFT | OAVX_AUDIO_CH_FRONT_RIGHT),
    OAVX_AUDIO_MASK_2_1    = (OAVX_AUDIO_CH_FRONT_LEFT | OAVX_AUDIO_CH_FRONT_RIGHT | OAVX_AUDIO_CH_LOW_FREQUENCY),
    OAVX_AUDIO_MASK_QUAD   = (OAVX_AUDIO_CH_FRONT_LEFT | OAVX_AUDIO_CH_FRONT_RIGHT |
                              OAVX_AUDIO_CH_BACK_LEFT  | OAVX_AUDIO_CH_BACK_RIGHT),
    OAVX_AUDIO_MASK_5_1    = (OAVX_AUDIO_CH_FRONT_LEFT | OAVX_AUDIO_CH_FRONT_RIGHT |
                              OAVX_AUDIO_CH_FRONT_CENTER | OAVX_AUDIO_CH_LOW_FREQUENCY |
                              OAVX_AUDIO_CH_BACK_LEFT  | OAVX_AUDIO_CH_BACK_RIGHT),
    OAVX_AUDIO_MASK_7_1    = (OAVX_AUDIO_CH_FRONT_LEFT | OAVX_AUDIO_CH_FRONT_RIGHT |
                              OAVX_AUDIO_CH_FRONT_CENTER | OAVX_AUDIO_CH_LOW_FREQUENCY |
                              OAVX_AUDIO_CH_BACK_LEFT  | OAVX_AUDIO_CH_BACK_RIGHT |
                              OAVX_AUDIO_CH_SIDE_LEFT  | OAVX_AUDIO_CH_SIDE_RIGHT),
    OAVX_AUDIO_MASK_7_1_4  = (OAVX_AUDIO_MASK_7_1 |
                              OAVX_AUDIO_CH_TOP_FRONT_LEFT | OAVX_AUDIO_CH_TOP_FRONT_RIGHT |
                              OAVX_AUDIO_CH_TOP_BACK_LEFT  | OAVX_AUDIO_CH_TOP_BACK_RIGHT)
} oavx_audio_channel_mask_e;

typedef uint64_t oavx_audio_channel_mask_t;

/* Discrete Broadcast Tracks (bit 32..63: generic independent tracks 1..32) */
#define OAVX_AUDIO_MASK_DISCRETE (1ULL << 32)
#define OAVX_AUDIO_CH_DISCRETE_TRACK(n) (((n) >= 1 && (n) <= 32) ? (1ULL << (31 + (n))) : 0ULL)

/* Native Audio Frame Descriptor */
typedef struct oavx_audio_frame {
    uint32_t               sample_rate;
    uint16_t               channels;
    uint32_t               frames;           /* Number of sample frames per channel */
    oavx_audio_format_t    format;

    /* Audio buffer pointers (planes[0..ch-1] for planar, planes[0] for interleaved) */
    void                  *planes[OAVX_MAX_AUDIO_CHANNELS];
    size_t                 stride_bytes;

    int64_t                pts_ns;
    uint64_t               channel_mask;     /* Spatial speaker mask (oavx_audio_channel_mask_t) or 0 for default */
    const char            *metadata;
    size_t                 metadata_len;
    void                  *user_ctx;
} oavx_audio_frame_t;

/* Interleaved Audio Format Descriptors */
typedef struct oavx_audio_frame_interleaved_16s_t {
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t frames;
    int64_t  pts_ns;
    int64_t  reference_level;
    int16_t *data;
} oavx_audio_frame_interleaved_16s_t;

typedef struct oavx_audio_frame_interleaved_32f_t {
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t frames;
    int64_t  pts_ns;
    float   *data;
} oavx_audio_frame_interleaved_32f_t;

typedef struct oavx_audio_frame_interleaved_32s_t {
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t frames;
    int64_t  pts_ns;
    int32_t *data;
} oavx_audio_frame_interleaved_32s_t;

/* Metadata Frame Descriptor */
typedef struct oavx_metadata_frame {
    size_t      length;
    int64_t     pts_ns;
    char       *data;
} oavx_metadata_frame_t;

/* Tally Status Flags & Descriptor */
typedef enum oavx_tally_flags {
    OAVX_TALLY_NONE      = 0,
    OAVX_TALLY_PROGRAM   = (1 << 0),
    OAVX_TALLY_PREVIEW   = (1 << 1),
    OAVX_TALLY_RECORDING = (1 << 2)
} oavx_tally_flags_t;

typedef struct oavx_tally {
    uint32_t flags;            /* Bitmask of oavx_tally_flags_t */
} oavx_tally_t;

/* Modern Color Format & Bandwidth Profiles */
typedef enum oavx_color_format_e {
    OAVX_COLOR_FORMAT_BGRX_BGRA     = 0,
    OAVX_COLOR_FORMAT_UYVY_BGRA     = 1,
    OAVX_COLOR_FORMAT_RGBX_RGBA     = 2,
    OAVX_COLOR_FORMAT_UYVY_RGBA     = 3,
    OAVX_COLOR_FORMAT_FASTEST       = 4,
    OAVX_COLOR_FORMAT_BEST          = 5,
    OAVX_COLOR_FORMAT_COMPRESSED_V1 = 6,
    OAVX_COLOR_FORMAT_P216          = 7,
    OAVX_COLOR_FORMAT_COUNT         = 8
} oavx_color_format_e;

typedef enum oavx_bandwidth_e {
    OAVX_BANDWIDTH_METADATA_ONLY = 0,
    OAVX_BANDWIDTH_AUDIO_ONLY    = 1,
    OAVX_BANDWIDTH_LOWEST        = 2,
    OAVX_BANDWIDTH_HIGHEST       = 3,
    OAVX_BANDWIDTH_COUNT         = 4
} oavx_bandwidth_e;

typedef enum oavx_transport_e {
    OAVX_TRANSPORT_TCP  = 0, /* Default reliable stream transport (TCP) */
    OAVX_TRANSPORT_RUDP = 1, /* Reliable UDP with Selective NACK retransmission */
    OAVX_TRANSPORT_UDP  = 2  /* Unicast/Multicast raw datagram transport */
} oavx_transport_e;

/* Performance & Queue Telemetry */
typedef struct oavx_performance_t {
    int64_t video_frames;
    int64_t audio_frames;
    int64_t metadata_frames;
} oavx_performance_t;

typedef struct oavx_queue_t {
    int video_frames;
    int audio_frames;
    int metadata_frames;
} oavx_queue_t;

/* Comprehensive Stream Observability & Real-Time Telemetry */
typedef struct oavx_telemetry_t {
    /* 1. Media Pipeline Metrics */
    int64_t  video_frames_total;      /* Total video frames delivered */
    int64_t  audio_frames_total;      /* Total audio frames delivered */
    int64_t  metadata_frames_total;   /* Total metadata frames delivered */
    int64_t  dropped_frames_total;    /* Total frames dropped */
    float    fps;                     /* Instantaneous delivered video frames per second */

    /* 2. Transport Dynamics */
    float    bitrate_mbps;            /* Instantaneous wire throughput in megabits/second */
    uint64_t total_wire_bytes;        /* Total payload + protocol bytes transferred */
    float    inter_frame_jitter_ms;   /* Inter-frame arrival time variance (EMA, in ms) */

    /* 3. OS Kernel & Socket Diagnostics */
    float    kernel_rtt_ms;           /* Kernel TCP Round-Trip Time in milliseconds (from TCP_INFO) */
    uint32_t tcp_retransmissions;     /* Cumulative TCP packet retransmissions */
    uint32_t socket_send_q_bytes;     /* Outgoing unacknowledged bytes in kernel send queue (SIOCOUTQ) */
    uint32_t socket_recv_q_bytes;     /* Incoming unread bytes in kernel receive queue (FIONREAD) */
    uint32_t congestion_level;        /* Dynamic link congestion: 0=Normal (60fps), 1=Moderate (30fps), 2=Severe (15fps) */

    /* 4. Stream & Format Attributes */
    char     remote_endpoint[128];    /* IP:Port of remote peer */
    uint32_t fourcc;                  /* Video FourCC codec/format (e.g., UYVY, P216, SHQ0) */
    uint32_t width;                   /* Video width in pixels */
    uint32_t height;                  /* Video height in pixels */
    bool     is_connected;            /* Active connection state */
} oavx_telemetry_t;

/* ========================================================================= */
/* OpenAVX Standard Status & Error Codes                                     */
/* ========================================================================= */
typedef enum oavx_status {
    OAVX_SUCCESS            =  0,  /* Operation completed successfully */
    OAVX_ERR_GENERIC        = -1,  /* General or unspecified runtime error */
    OAVX_ERR_INVALID_ARG    = -2,  /* Null pointer or invalid parameter passed */
    OAVX_ERR_TIMEOUT        = -3,  /* Operation timed out before completion */
    OAVX_ERR_NOT_CONNECTED  = -4,  /* Network socket is not connected or remote closed */
    OAVX_ERR_QUEUE_FULL     = -5,  /* Pipeline queue capacity reached; frame dropped */
    OAVX_ERR_UNSUPPORTED    = -6,  /* Requested format, codec, or feature is unsupported */
    OAVX_ERR_WOULD_BLOCK    = -7   /* Non-blocking transport would block (EAGAIN/EWOULDBLOCK) */
} oavx_status_t;

OPENAVX_API const char* oavx_status_to_string(oavx_status_t status);

/* ========================================================================= */
/* OpenAVX Native Unified Media Transport API (oavx_*)                       */
/* Native, high-performance, stream-oriented media pipeline interface       */
/* ========================================================================= */

typedef struct oavx_context oavx_context_t;
typedef struct oavx_stream oavx_stream_t;
typedef struct oavx_discovery oavx_discovery_t;

/* Stream Direction */
typedef enum oavx_direction {
    OAVX_DIR_TRANSMIT = 1,
    OAVX_DIR_RECEIVE  = 2
} oavx_direction_t;

/* Wire Framing Profile Descriptor (for profile-based wire transport) */
#ifndef OAVX_WIRE_PROFILE_DEFINED
#define OAVX_WIRE_PROFILE_DEFINED
typedef struct oavx_wire_profile {
    uint32_t magic_video;
    uint32_t magic_audio;
    uint32_t magic_metadata;
    uint32_t magic_udp;
    uint32_t compat_magic_video;
    uint32_t compat_magic_audio;
    uint32_t compat_magic_metadata;
    uint32_t compat_magic_udp;
    uint16_t base_port;
    const char* mdns_service;
    const char* handshake_tag;
    const char* tally_tag;
    const char* color_info_tag;
    const char* ptz_tag;
    const char* record_tag;
} oavx_wire_profile_t;
#endif

/* Stream Configuration */
typedef struct oavx_stream_config {
    const char                 *name;            /* Stream name (e.g. "STUDIO_CAM_1") */
    const char                 *endpoint;        /* Remote IP:Port or URL (for receiver) */
    oavx_direction_t            direction;       /* TRANSMIT or RECEIVE */
    oavx_color_format_e         color_format;    /* Preferred color format */
    oavx_bandwidth_e            bandwidth;       /* Bandwidth profile */
    bool                        clock_sync;      /* Pacing and clock sync */
    const char                 *groups;          /* Announcement / Finder groups */
    const oavx_wire_profile_t  *wire_profile;    /* Optional custom wire profile override (NULL = context default) */
    uint32_t                    max_connections; /* Max simultaneous client connections (0 = default 32, max 64) */
    const char                 *bind_ip;         /* Explicit local IP or NIC interface to bind (e.g. "10.0.0.50" or "eth1", NULL = auto) */
    oavx_transport_e            transport;       /* Transport protocol: TCP (default) or RUDP */
    uint32_t                    video_codec_fourcc; /* Target compressed video FourCC (0 = raw/pass-through, or OAVX_FOURCC_VIDEO_TYPE_H264 / HEVC / SpeedHQ) */
    uint32_t                    video_bitrate_kbps; /* Target video bitrate in kbps (0 = automatic default) */
    uint32_t                    keyframe_interval;  /* Keyframe interval in frames (0 = default 60 frames) */
    const char                 *passphrase;         /* Optional passphrase for native E2EE (ChaCha20-Poly1305, NULL = plaintext) */
} oavx_stream_config_t;

/* Device PTZ Camera State Descriptor */
typedef struct oavx_ptz_state {
    float pan;
    float tilt;
    float zoom;
    float focus;
} oavx_ptz_state_t;

/* Context Lifecycle */
/**
 * Creates an OpenAVX engine context and initializes networking and SIMD dispatch.
 * Reference-counted; returns existing instance if already initialized.
 *
 * @return Context pointer, or NULL on allocation/socket failure.
 */
OPENAVX_API oavx_context_t* oavx_context_create(void);
OPENAVX_API void            oavx_context_destroy(oavx_context_t *ctx);
OPENAVX_API const char*     oavx_version(void);
OPENAVX_API bool            oavx_is_supported_cpu(void);

/**
 * Overrides wire protocol magic tags and ports for the context and process.
 * Pass NULL to restore default OpenAVX protocol signatures.
 */
OPENAVX_API void            oavx_context_set_wire_profile(oavx_context_t *ctx, const oavx_wire_profile_t *profile);
OPENAVX_API void            oavx_context_get_wire_profile(oavx_context_t *ctx, oavx_wire_profile_t *out_profile);

/* Unified Stream Lifecycle */
/**
 * Opens a streaming session (transmitter or receiver) with background I/O threads.
 *
 * @param ctx Active library context.
 * @param config Session parameters (name, direction, IP/port, codecs, encryption).
 * @return Stream handle, or NULL on connection/binding failure.
 */
OPENAVX_API oavx_stream_t*  oavx_stream_open(oavx_context_t *ctx, const oavx_stream_config_t *config);
OPENAVX_API void            oavx_stream_close(oavx_stream_t *stream);

/* Stream Data Transmission */
/**
 * Submits an uncompressed or compressed video frame to connected receivers.
 * Non-blocking; drops frame if client transmission queues are saturated.
 * Caller retains ownership of frame and memory planes.
 */
OPENAVX_API oavx_status_t   oavx_stream_send_video(oavx_stream_t *stream, const oavx_video_frame_t *frame);

/**
 * Submits an audio frame (planar float or interleaved PCM).
 * Caller retains ownership of audio buffers.
 */
OPENAVX_API oavx_status_t   oavx_stream_send_audio(oavx_stream_t *stream, const oavx_audio_frame_t *frame);
OPENAVX_API oavx_status_t   oavx_stream_send_metadata(oavx_stream_t *stream, const oavx_metadata_frame_t *meta);

/**
 * Configures audio encoding mode (0=uncompressed float, 1=Opus/ADPCM, 2=adaptive LAN/WAN).
 */
OPENAVX_API oavx_status_t   oavx_stream_set_audio_compression(oavx_stream_t *stream, oavx_audio_compression_mode_e mode, int target_bitrate_kbps);

/**
 * Sets runtime video compression (e.g. SpeedHQ, H.264, or HEVC) and target bitrate.
 * Pass 0 to restore raw uncompressed pass-through.
 */
OPENAVX_API oavx_status_t   oavx_stream_set_video_compression(oavx_stream_t *stream, uint32_t target_fourcc, uint32_t bitrate_kbps);
OPENAVX_API uint32_t        oavx_stream_get_video_compression(oavx_stream_t *stream);
OPENAVX_API oavx_status_t   oavx_stream_set_video_quality(oavx_stream_t *stream, uint8_t quality);
OPENAVX_API uint8_t         oavx_stream_get_video_quality(oavx_stream_t *stream);

/**
 * Starts an embedded RTSP 1.0 streaming server on this transmitter stream.
 */
OPENAVX_API oavx_status_t   oavx_stream_enable_rtsp_server(oavx_stream_t *stream, uint16_t port, const char *path);
OPENAVX_API void            oavx_stream_disable_rtsp_server(oavx_stream_t *stream);
OPENAVX_API uint16_t        oavx_stream_get_rtsp_port(oavx_stream_t *stream);
OPENAVX_API int             oavx_stream_get_rtsp_client_count(oavx_stream_t *stream);

/* Embedded HTTP / fMP4 Low-Latency Web Preview */
/**
 * Starts an embedded HTTP/fMP4 web preview server for zero-plugin browser playback.
 */
OPENAVX_API oavx_status_t   oavx_stream_enable_web_preview(oavx_stream_t *stream, uint16_t port);
OPENAVX_API void            oavx_stream_disable_web_preview(oavx_stream_t *stream);
OPENAVX_API uint16_t        oavx_stream_get_web_preview_port(oavx_stream_t *stream);
OPENAVX_API const char*     oavx_stream_get_web_preview_url(oavx_stream_t *stream, char *out_url, size_t max_len);
OPENAVX_API int             oavx_stream_get_web_client_count(oavx_stream_t *stream);

/* Stream Data Reception */
/**
 * Fetches the next available frame from the receive queue.
 * Populates only the descriptor corresponding to the returned frame type.
 *
 * @param timeout_ms Max milliseconds to wait (0 = non-blocking poll).
 * @return Received frame type (VIDEO, AUDIO, METADATA), or NONE on timeout.
 * @note Call oavx_stream_release_video() after processing a returned video frame.
 */
OPENAVX_API oavx_frame_type_t oavx_stream_receive(
    oavx_stream_t *stream,
    oavx_video_frame_t *v,
    oavx_audio_frame_t *a,
    oavx_metadata_frame_t *m,
    uint32_t timeout_ms);

/**
 * Releases internal buffer hold on a video frame returned by oavx_stream_receive().
 */
OPENAVX_API void            oavx_stream_release_video(oavx_stream_t *stream, const oavx_video_frame_t *frame);
OPENAVX_API void            oavx_stream_release_audio(oavx_stream_t *stream, const oavx_audio_frame_t *frame);
OPENAVX_API void            oavx_stream_release_metadata(oavx_stream_t *stream, const oavx_metadata_frame_t *meta);

/* Stream Device & Camera Controls */
OPENAVX_API bool            oavx_stream_set_ptz(oavx_stream_t *stream, const oavx_ptz_state_t *ptz);
OPENAVX_API bool            oavx_stream_set_tally(oavx_stream_t *stream, const oavx_tally_t *tally);
OPENAVX_API bool            oavx_stream_get_tally(oavx_stream_t *stream, oavx_tally_t *out_tally, uint32_t timeout_ms);
OPENAVX_API bool            oavx_stream_record_trigger(oavx_stream_t *stream, const char *action, const char *filename_hint);
OPENAVX_API bool            oavx_stream_record_status(oavx_stream_t *stream, bool *is_recording, const char **filename, const char **error);
OPENAVX_API bool            oavx_stream_record_set_audio_level(oavx_stream_t *stream, float level_db);

/* Network Discovery Grid */
/**
 * Spawns an mDNS background discovery scanner for active OpenAVX sources.
 *
 * @param groups Comma-separated discovery groups, or NULL for default ("public").
 */
OPENAVX_API oavx_discovery_t* oavx_discovery_create(oavx_context_t *ctx, const char *groups);
OPENAVX_API void              oavx_discovery_destroy(oavx_discovery_t *disc);

/**
 * Copies currently discovered endpoints into caller-provided array.
 *
 * @return Number of endpoints written (<= max_count).
 */
OPENAVX_API size_t            oavx_discovery_get_sources(oavx_discovery_t *disc, oavx_endpoint_t *out_sources, size_t max_count);

/**
 * Blocks until the discovered source list changes or timeout expires.
 */
OPENAVX_API bool              oavx_discovery_wait(oavx_discovery_t *disc, uint32_t timeout_ms);

/* Stream Connection & Advanced Endpoint Controls */
/**
 * Connects receiver to a remote stream endpoint.
 */
OPENAVX_API void                   oavx_stream_connect(oavx_stream_t *stream, const oavx_endpoint_t *endpoint);
OPENAVX_API const oavx_endpoint_t* oavx_stream_get_endpoint(oavx_stream_t *stream);
OPENAVX_API oavx_frame_type_t      oavx_stream_capture_metadata(oavx_stream_t *stream, oavx_metadata_frame_t *meta, uint32_t timeout_ms);
OPENAVX_API void                   oavx_stream_clear_connection_metadata(oavx_stream_t *stream);
OPENAVX_API void                   oavx_stream_add_connection_metadata(oavx_stream_t *stream, const oavx_metadata_frame_t *meta);

/**
 * Configures backup failover endpoint if primary stream stops receiving frames.
 */
OPENAVX_API void                   oavx_stream_set_failover(oavx_stream_t *stream, const oavx_endpoint_t *failover_endpoint);
OPENAVX_API bool                   oavx_stream_is_on_failover(oavx_stream_t *stream);
OPENAVX_API void                   oavx_stream_set_failover_timeout(oavx_stream_t *stream, uint32_t timeout_ms);
OPENAVX_API void                   oavx_stream_set_auto_revert(oavx_stream_t *stream, bool auto_revert, uint32_t hold_time_ms);
OPENAVX_API int                    oavx_stream_get_connection_count(oavx_stream_t *stream, uint32_t timeout_ms);
OPENAVX_API bool                   oavx_stream_get_performance(oavx_stream_t *stream, oavx_performance_t *total, oavx_performance_t *dropped);
OPENAVX_API bool                   oavx_stream_get_queue(oavx_stream_t *stream, oavx_queue_t *total);
OPENAVX_API bool                   oavx_stream_get_telemetry(oavx_stream_t *stream, oavx_telemetry_t *out_telemetry);
OPENAVX_API size_t                 oavx_telemetry_to_json(const oavx_telemetry_t *telemetry, char *out_json, size_t max_len);
OPENAVX_API const char*            oavx_stream_get_web_control(oavx_stream_t *stream);

/* SpeedHQ Dynamic Quantization & Bitrate Control */
OPENAVX_API void                   oavx_speedhq_set_quality(uint8_t quality);
OPENAVX_API uint8_t                oavx_speedhq_get_quality(void);

/* Async Socket & Event-Loop Integration */
OPENAVX_API int                    oavx_stream_get_fd(oavx_stream_t *stream);
OPENAVX_API int                    oavx_discovery_get_fd(oavx_discovery_t *disc);

/* Dynamic Virtual Matrix Router */
typedef struct oavx_router oavx_router_t;
OPENAVX_API oavx_router_t*  oavx_router_create(oavx_context_t *ctx, const char *name, const char *groups);
OPENAVX_API void            oavx_router_destroy(oavx_router_t *router);

/**
 * Dynamically switches router output to a new upstream source without dropping downstream client connections.
 */
OPENAVX_API bool            oavx_router_route(oavx_router_t *router, const oavx_endpoint_t *source);
OPENAVX_API bool            oavx_router_clear(oavx_router_t *router);
OPENAVX_API oavx_endpoint_t oavx_router_get_source(oavx_router_t *router);

/* Media Clock & Frame Synchronizer */
typedef struct oavx_framesync oavx_framesync_t;
OPENAVX_API oavx_framesync_t* oavx_framesync_create(oavx_stream_t *stream);
OPENAVX_API void              oavx_framesync_destroy(oavx_framesync_t *fs);

/**
 * Pulls the freshest video frame from the stream clock, repeating prior frame on underrun.
 * Call oavx_framesync_free_video() after consuming the frame.
 */
OPENAVX_API void              oavx_framesync_capture_video(oavx_framesync_t *fs, oavx_video_frame_t *frame, int field_type);
OPENAVX_API void              oavx_framesync_free_video(oavx_framesync_t *fs, oavx_video_frame_t *frame);
OPENAVX_API void              oavx_framesync_capture_audio(oavx_framesync_t *fs, oavx_audio_frame_t *frame, uint32_t sample_rate, uint16_t channels, uint32_t num_samples);
OPENAVX_API void              oavx_framesync_free_audio(oavx_framesync_t *fs, oavx_audio_frame_t *frame);
OPENAVX_API int               oavx_framesync_audio_queue_depth(oavx_framesync_t *fs);

/* Unified Media Utilities */
OPENAVX_API void oavx_util_audio_to_interleaved_16s(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_16s_t* dst);
OPENAVX_API void oavx_util_audio_from_interleaved_16s(const oavx_audio_frame_interleaved_16s_t* src, oavx_audio_frame_t* dst);
OPENAVX_API void oavx_util_audio_to_interleaved_32f(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_32f_t* dst);
OPENAVX_API void oavx_util_audio_from_interleaved_32f(const oavx_audio_frame_interleaved_32f_t* src, oavx_audio_frame_t* dst);
OPENAVX_API void oavx_util_audio_to_interleaved_32s(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_32s_t* dst);
OPENAVX_API void oavx_util_audio_from_interleaved_32s(const oavx_audio_frame_interleaved_32s_t* src, oavx_audio_frame_t* dst);

/**
 * Unpacks SMPTE 292M 10-bit V210 video into planar 16-bit P216 (and reverse).
 */
OPENAVX_API void oavx_util_V210_to_P216(const oavx_video_frame_t* src, oavx_video_frame_t* dst);
OPENAVX_API void oavx_util_P216_to_V210(const oavx_video_frame_t* src, oavx_video_frame_t* dst);

/* Library Logging & Diagnostics */
typedef enum oavx_log_level_e {
    OAVX_LOG_QUIET = 0,
    OAVX_LOG_ERROR = 1,
    OAVX_LOG_WARN  = 2,
    OAVX_LOG_INFO  = 3,
    OAVX_LOG_DEBUG = 4
} oavx_log_level_e;

typedef void (*oavx_log_callback_t)(oavx_log_level_e level, const char *module, const char *message, void *user_ctx);

OPENAVX_API void             oavx_set_log_level(oavx_log_level_e level);
OPENAVX_API oavx_log_level_e oavx_get_log_level(void);
OPENAVX_API void             oavx_set_log_callback(oavx_log_callback_t cb, void *user_ctx);
OPENAVX_API void             oavx_log_write(oavx_log_level_e level, const char *module, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* OPENAVX_H */
