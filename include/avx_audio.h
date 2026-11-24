/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_audio.h - Audio resampling and channel downmixing
 */

#ifndef AVX_AUDIO_H
#define AVX_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard Channel Layout Identifiers */
typedef enum avx_audio_layout_e {
    AVX_AUDIO_LAYOUT_UNKNOWN        = 0,
    AVX_AUDIO_LAYOUT_MONO           = 1,   /* [0] = Center/Mono (M) */
    AVX_AUDIO_LAYOUT_STEREO         = 2,   /* [0] = Left (L), [1] = Right (R) */
    AVX_AUDIO_LAYOUT_2_1            = 3,   /* [0] = L, [1] = R, [2] = LFE */
    AVX_AUDIO_LAYOUT_QUAD           = 4,   /* [0] = FL, [1] = FR, [2] = BL, [3] = BR */
    AVX_AUDIO_LAYOUT_5_1            = 6,   /* [0] = FL, [1] = FR, [2] = C, [3] = LFE, [4] = BL, [5] = BR (ITU-R BS.775) */
    AVX_AUDIO_LAYOUT_7_1            = 8,   /* [0] = FL, [1] = FR, [2] = C, [3] = LFE, [4] = BL, [5] = BR, [6] = SL, [7] = SR */
    AVX_AUDIO_LAYOUT_7_1_4          = 12,  /* 7.1 + 4 Height [TFL, TFR, TBL, TBR] */
    AVX_AUDIO_LAYOUT_DISCRETE       = 100, /* Generic discrete tracks without fixed spatial positioning */
    AVX_AUDIO_LAYOUT_BROADCAST_16CH = 116, /* SDI 16-channel broadcast matrix */
    AVX_AUDIO_LAYOUT_BROADCAST_32CH = 132  /* 32-channel pro-audio / Dante matrix */
} avx_audio_layout_e;

/* Channel Layout & Semantic Metadata Helpers */
OPENAVX_API oavx_audio_channel_mask_t avx_audio_layout_to_mask(avx_audio_layout_e layout);
OPENAVX_API avx_audio_layout_e        avx_audio_layout_from_channels(uint16_t channels);
OPENAVX_API avx_audio_layout_e        avx_audio_layout_from_mask(oavx_audio_channel_mask_t mask, uint16_t channels);
OPENAVX_API const char*               avx_audio_layout_name(avx_audio_layout_e layout);
OPENAVX_API const char*               avx_audio_channel_name(uint16_t channel_idx, avx_audio_layout_e layout);
OPENAVX_API uint16_t                  avx_audio_layout_channels(avx_audio_layout_e layout);

/* Normalization mode for downmixing */
typedef enum avx_audio_normalize_e {
    AVX_AUDIO_NORMALIZE_NONE      = 0, /* Direct weighted sum; hard-clips exceeding [-1.0, 1.0] */
    AVX_AUDIO_NORMALIZE_PEAK      = 1, /* Computes peak amplitude; normalizes frame if peak > 1.0 */
    AVX_AUDIO_NORMALIZE_ATTEN_3DB = 2  /* Scales by coefficient sum to guarantee no clipping */
} avx_audio_normalize_e;

/* Resampling interpolation algorithm */
typedef enum avx_audio_resample_mode_e {
    AVX_AUDIO_RESAMPLE_LINEAR = 0, /* Fast 2-point linear interpolation */
    AVX_AUDIO_RESAMPLE_CUBIC  = 1, /* 4-point Catmull-Rom cubic Hermite spline (broadcast quality) */
    AVX_AUDIO_RESAMPLE_SINC   = 2  /* Windowed Sinc filter with Blackman window (studio grade) */
} avx_audio_resample_mode_e;

/* Opaque streaming resampler context */
typedef struct avx_resampler_t avx_resampler_t;

/* ========================================================================= */
/* 1. MULTI-CHANNEL DOWNMIXING ROUTINES                                      */
/* ========================================================================= */

/**
 * Downmix 5.1 Surround to Stereo (ITU-R BS.775 / ATSC A/52 standard).
 * in_channels: array of 6 float pointers [FL, FR, C, LFE, BL, BR].
 */
void avx_audio_downmix_5_1_to_stereo(
    const float* const in_channels[6],
    float* out_left,
    float* out_right,
    int frames,
    avx_audio_normalize_e normalize_mode
);

/**
 * Downmix 7.1 Surround to Stereo.
 * in_channels: array of 8 float pointers [FL, FR, C, LFE, BL, BR, SL, SR].
 */
void avx_audio_downmix_7_1_to_stereo(
    const float* const in_channels[8],
    float* out_left,
    float* out_right,
    int frames,
    avx_audio_normalize_e normalize_mode
);

/**
 * Downmix Quadraphonic 4.0 to Stereo.
 * in_channels: array of 4 float pointers [FL, FR, BL, BR].
 */
void avx_audio_downmix_quad_to_stereo(
    const float* const in_channels[4],
    float* out_left,
    float* out_right,
    int frames,
    avx_audio_normalize_e normalize_mode
);

/**
 * Downmix Stereo to Mono.
 */
void avx_audio_downmix_stereo_to_mono(
    const float* in_left,
    const float* in_right,
    float* out_mono,
    int frames,
    avx_audio_normalize_e normalize_mode
);

/**
 * Arbitrary M x N channel matrix remixing.
 * matrix is out_channel_count x in_channel_count row-major float matrix.
 */
void avx_audio_matrix_mix(
    const float* const* in_channels,
    int in_channel_count,
    float* const* out_channels,
    int out_channel_count,
    const float* matrix,
    int frames,
    avx_audio_normalize_e normalize_mode
);

/**
 * High-level OpenAVX audio frame downmix helper.
 * If src_frame has > 2 channels, downmixes to stereo in dst_frame.
 * Allocates dst_frame->planes[0]; must be freed with avx_audio_frame_free_data().
 */
bool avx_audio_frame_downmix_to_stereo(
    const oavx_audio_frame_t* src_frame,
    oavx_audio_frame_t* dst_frame,
    avx_audio_normalize_e normalize_mode
);

/**
 * Frees memory allocated by avx_audio_frame_downmix_to_stereo or avx_audio_frame_resample.
 */
void avx_audio_frame_free_data(oavx_audio_frame_t* frame);

/* ========================================================================= */
/* 2. BROADCAST SAMPLE RATE CONVERSION (RESAMPLING)                          */
/* ========================================================================= */

/**
 * Creates a stateful streaming audio resampler.
 * Uses exact rational phase fraction stepping based on gcd(in_rate, out_rate).
 */
avx_resampler_t* avx_resampler_create(
    int in_rate,
    int out_rate,
    int channels,
    avx_audio_resample_mode_e mode
);

/**
 * Processes incoming planar audio chunk through the resampler.
 * in_channels: array of channels planar float pointers.
 * out_channels: array of channels planar float output buffers.
 * Returns the exact number of output samples generated in this call.
 */
int avx_resampler_process(
    avx_resampler_t* resampler,
    const float* const* in_channels,
    int in_samples,
    float* const* out_channels,
    int max_out_samples
);

/**
 * Flushes any remaining samples buffered in the resampler at end of stream.
 * Returns the number of flushed samples generated.
 */
int avx_resampler_flush(
    avx_resampler_t* resampler,
    float* const* out_channels,
    int max_out_samples
);

/**
 * Resets the resampler's internal FIFO buffers and phase accumulator.
 */
void avx_resampler_reset(avx_resampler_t* resampler);

/**
 * Destroys a resampler instance and frees all associated buffers.
 */
void avx_resampler_destroy(avx_resampler_t* resampler);

/**
 * High-level OpenAVX audio frame resampling helper.
 * Resamples src_frame into dst_frame using the provided resampler instance.
 * Allocates dst_frame->planes[0]; must be freed with avx_audio_frame_free_data().
 */
bool avx_audio_frame_resample(
    avx_resampler_t* resampler,
    const oavx_audio_frame_t* src_frame,
    oavx_audio_frame_t* dst_frame
);

/**
 * Simple one-shot resample for contiguous single-channel buffers.
 * Returns number of output samples written, or -1 on error.
 */
int avx_audio_resample_simple(
    const float* in_data,
    int in_samples,
    int in_rate,
    float* out_data,
    int max_out_samples,
    int out_rate,
    avx_audio_resample_mode_e mode
);

/* ========================================================================= */
/* 3. ADAPTIVE AUDIO JITTER BUFFER & CLOCK SLEWING                           */
/* ========================================================================= */

typedef struct avx_audio_jitter_t avx_audio_jitter_t;

typedef struct avx_audio_jitter_config_t {
    int sample_rate;        /* Nominal sample rate (e.g. 48000) */
    int channels;           /* Channel count (e.g. 2, max 8) */
    int target_cushion_ms;  /* Base cushion setpoint in ms (default: 15 ms) */
    int min_cushion_ms;     /* Minimum allowable cushion on clean LAN (default: 5 ms) */
    int max_cushion_ms;     /* Maximum allowable cushion on congested links (default: 40 ms) */
} avx_audio_jitter_config_t;

/**
 * Creates an adaptive audio jitter buffer instance.
 */
avx_audio_jitter_t* avx_audio_jitter_create(const avx_audio_jitter_config_t* cfg);

/**
 * Destroys an adaptive audio jitter buffer instance and releases memory.
 */
void avx_audio_jitter_destroy(avx_audio_jitter_t* jb);

/**
 * Pushes incoming bursty/network audio chunk into the elastic planar ring buffer.
 */
void avx_audio_jitter_push(avx_audio_jitter_t* jb, const float* const* channels, int num_samples);

/**
 * Pulls steady, clock-steered audio samples for mixer/soundcard playback.
 * Automatically applies pre-roll buffering, micro-slewing drift correction,
 * and smooth cosine cross-fades on starvation.
 * Returns the actual number of samples generated.
 */
int avx_audio_jitter_pull(avx_audio_jitter_t* jb, float* const* out_channels, int requested_samples);

/**
 * Returns current buffered audio depth in samples.
 */
int avx_audio_jitter_get_depth_samples(const avx_audio_jitter_t* jb);

/**
 * Returns current buffered audio depth in milliseconds.
 */
float avx_audio_jitter_get_depth_ms(const avx_audio_jitter_t* jb);

/**
 * Resets the jitter buffer, clearing buffered samples and re-arming pre-roll.
 */
void avx_audio_jitter_reset(avx_audio_jitter_t* jb);

/* ========================================================================= */
/* 4. AUDIO MATRIXING, ROUTING & UPMIXING TOOLS                              */
/* ========================================================================= */

/**
 * Universal Audio Channel Router and Shuffler.
 * Maps, shuffles, mutes, and scales audio channels across arbitrary formats (up to 32ch).
 *
 * in_planes: array of in_channels planar float pointers.
 * out_planes: array of out_channels planar float pointers.
 * channel_map: array of length out_channels specifying the input channel index for each output channel.
 *              channel_map[i] = src_idx (0 <= src_idx < in_channels) maps input to output[i].
 *              channel_map[i] = -1 mutes output[i] (fills with silence).
 *              If channel_map is NULL, performs 1:1 mapping (identity up to min(in_channels, out_channels)).
 * channel_gains: optional array of length out_channels with linear gain multiplier per output channel.
 *                If NULL, unity gain (1.0f) is applied to all channels.
 * frames: number of samples per channel to process.
 */
OPENAVX_API void avx_audio_route_channels(
    const float* const* in_planes, int in_channels,
    float* const* out_planes, int out_channels,
    const int8_t* channel_map,
    const float* channel_gains,
    int frames
);

/**
 * High-level OpenAVX audio frame routing helper.
 * Routes channels from src_frame into dst_frame according to channel_map and gains.
 * Allocates dst_frame->planes[0]; must be freed with avx_audio_frame_free_data().
 */
OPENAVX_API bool avx_audio_frame_route_channels(
    const oavx_audio_frame_t* src_frame,
    oavx_audio_frame_t* dst_frame,
    const int8_t* channel_map,
    const float* channel_gains,
    uint16_t out_channels
);

/**
 * Broadcast Lt/Rt (Left total / Right total) Matrix Downmixer.
 * Produces matrix-encoded stereo from 5.1 surround compatible with Dolby Pro Logic decoders.
 * Formula:
 *   Lt = FL + 0.7071 * C - 0.7071 * BL - 0.7071 * BR
 *   Rt = FR + 0.7071 * C + 0.7071 * BL + 0.7071 * BR
 *
 * in_channels: array of 6 float pointers [FL, FR, C, LFE, BL, BR]. LFE is muted.
 * out_left, out_right: destination stereo buffers.
 * frames: number of samples to process.
 * normalize_mode: normalization mode (NONE, PEAK, ATTEN_3DB).
 */
OPENAVX_API void avx_audio_downmix_5_1_to_ltrt(
    const float* const in_channels[6],
    float* out_left,
    float* out_right,
    int frames,
    avx_audio_normalize_e normalize_mode
);

/**
 * Configuration parameters for Stereo to 5.1 Surround Upmixer.
 */
typedef struct avx_audio_upmix_cfg_t {
    float center_level;     /* Center/dialog level multiplier (default: 0.7071f) */
    float surround_level;   /* Surround/ambience level multiplier (default: 0.7071f) */
    float lfe_level;        /* Subwoofer/LFE low-pass level multiplier (default: 0.5000f) */
    int   ambient_delay_ms; /* Surround decorrelation delay in ms (default: 15 ms, range 0..50) */
    avx_audio_normalize_e normalize_mode; /* Output normalization mode (default: NONE) */
} avx_audio_upmix_cfg_t;

/**
 * Initializes upmix configuration with broadcast-standard defaults.
 */
OPENAVX_API void avx_audio_upmix_cfg_default(avx_audio_upmix_cfg_t* cfg);

/**
 * Opaque streaming stateful upmixer context.
 * Maintains surround delay ring buffers and 2nd-order Butterworth LFE filter state.
 */
typedef struct avx_audio_upmixer_t avx_audio_upmixer_t;

/**
 * Creates a stateful stereo to 5.1 surround upmixer.
 */
OPENAVX_API avx_audio_upmixer_t* avx_audio_upmixer_create(
    int sample_rate,
    const avx_audio_upmix_cfg_t* cfg
);

/**
 * Destroys a stateful upmixer and frees internal delay/filter buffers.
 */
OPENAVX_API void avx_audio_upmixer_destroy(avx_audio_upmixer_t* upmixer);

/**
 * Resets upmixer internal state (clears delay buffer and filter memory).
 */
OPENAVX_API void avx_audio_upmixer_reset(avx_audio_upmixer_t* upmixer);

/**
 * Processes incoming stereo audio chunk through the stateful upmixer.
 * out_channels: array of 6 float pointers [FL, FR, C, LFE, BL, BR].
 */
OPENAVX_API void avx_audio_upmixer_process(
    avx_audio_upmixer_t* upmixer,
    const float* in_left,
    const float* in_right,
    float* const out_channels[6],
    int frames
);

/**
 * One-shot Stereo to 5.1 Surround Upmixer.
 * Convenient standalone helper for single-buffer processing.
 *
 * in_left, in_right: input stereo channel buffers.
 * out_channels: array of 6 output pointers [FL, FR, C, LFE, BL, BR].
 * frames: number of samples to process.
 * sample_rate: audio sample rate (e.g. 48000).
 * cfg: optional configuration. If NULL, default parameters are used.
 */
OPENAVX_API void avx_audio_upmix_stereo_to_5_1(
    const float* in_left,
    const float* in_right,
    float* const out_channels[6],
    int frames,
    int sample_rate,
    const avx_audio_upmix_cfg_t* cfg
);

#ifdef __cplusplus
}
#endif

#endif /* AVX_AUDIO_H */
