/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_audio_codec.h - Opus and IMA ADPCM audio codecs
 */

#ifndef AVX_AUDIO_CODEC_H
#define AVX_AUDIO_CODEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Supported Audio Compression Codec Types */
typedef enum avx_audio_codec_type_e {
    AVX_AUDIO_CODEC_NONE  = 0,
    AVX_AUDIO_CODEC_OPUS  = 1,
    AVX_AUDIO_CODEC_ADPCM = 2
} avx_audio_codec_type_e;

/* Check whether the dynamic runtime libopus loader successfully bound symbols */
OPENAVX_API bool avx_audio_codec_has_opus(void);

/* Opaque Audio Encoder & Decoder Handles */
typedef struct avx_audio_encoder_t avx_audio_encoder_t;
typedef struct avx_audio_decoder_t avx_audio_decoder_t;

/**
 * Creates an audio encoder instance.
 * @param sample_rate Sample rate in Hz (e.g. 48000).
 * @param channels Number of channels (1 to 32: Mono, Stereo, 5.1, 7.1, 16ch Broadcast, 32ch Pro-Audio).
 *                 Opus is used for <= 8 channels; ADPCM is automatically used for > 8 channels.
 * @param preferred_codec Desired codec (AVX_AUDIO_CODEC_OPUS or AVX_AUDIO_CODEC_ADPCM).
 *                        If OPUS is requested but libopus is absent or channels > 8, auto-falls back to ADPCM.
 * @param bitrate_kbps Target bitrate in kbps (0 for automatic: 128 stereo, 384 7.1).
 */
OPENAVX_API avx_audio_encoder_t* avx_audio_encoder_create(
    int sample_rate,
    int channels,
    avx_audio_codec_type_e preferred_codec,
    int bitrate_kbps
);

/**
 * Destroys an audio encoder instance and releases its internal buffers.
 */
OPENAVX_API void avx_audio_encoder_destroy(avx_audio_encoder_t* enc);

/**
 * Returns the active codec type currently being used by the encoder.
 */
OPENAVX_API avx_audio_codec_type_e avx_audio_encoder_get_codec(const avx_audio_encoder_t* enc);

/**
 * Returns the FourCC of the active codec ('OPUS' or 'ADPC').
 */
OPENAVX_API uint32_t avx_audio_encoder_get_fourcc(const avx_audio_encoder_t* enc);

/**
 * Feeds uncompressed planar float audio frames into the encoder.
 * Audio is accumulated and chunked into optimal packet boundaries (20 ms for Opus).
 */
OPENAVX_API bool avx_audio_encoder_feed(
    avx_audio_encoder_t* enc,
    const float* const* planes,
    int frames,
    int64_t pts_ns
);

/**
 * Pulls the next available compressed audio packet from the encoder.
 * Returns true if a packet was retrieved, false if more input frames are needed.
 */
OPENAVX_API bool avx_audio_encoder_pull(
    avx_audio_encoder_t* enc,
    uint8_t* out_pkt,
    size_t max_pkt_size,
    size_t* out_pkt_size,
    int* out_samples,
    int64_t* out_pts_ns,
    uint32_t* out_codec_fourcc
);

/**
 * Flushes any remaining buffered samples in the encoder.
 */
OPENAVX_API bool avx_audio_encoder_flush(
    avx_audio_encoder_t* enc,
    uint8_t* out_pkt,
    size_t max_pkt_size,
    size_t* out_pkt_size,
    int* out_samples,
    int64_t* out_pts_ns,
    uint32_t* out_codec_fourcc
);

/**
 * Creates an audio decoder instance.
 * @param sample_rate Target sample rate (e.g. 48000).
 * @param channels Number of channels (1 to 32).
 * @param codec_fourcc OAVX_FOURCC_AUDIO_TYPE_OPUS or OAVX_FOURCC_AUDIO_TYPE_ADPC.
 */
OPENAVX_API avx_audio_decoder_t* avx_audio_decoder_create(
    int sample_rate,
    int channels,
    uint32_t codec_fourcc
);

/**
 * Destroys an audio decoder instance and releases resources.
 */
OPENAVX_API void avx_audio_decoder_destroy(avx_audio_decoder_t* dec);

/**
 * Decodes a compressed packet into planar float buffers.
 * @return Number of decoded sample frames per channel, or negative on error.
 */
OPENAVX_API int avx_audio_decoder_decode(
    avx_audio_decoder_t* dec,
    uint32_t codec_fourcc,
    const uint8_t* pkt_data,
    size_t pkt_size,
    float* const* out_planes,
    int max_samples
);

/* ========================================================================= */
/* STANDALONE PURE C99 IMA ADPCM ENCODE / DECODE (ZERO DEPENDENCIES)         */
/* ========================================================================= */

/**
 * Encodes planar float audio to 4-bit IMA ADPCM bitstream.
 * Achieves 4:1 compression ratio over 16-bit PCM (8:1 over 32-bit float).
 */
OPENAVX_API size_t avx_adpcm_encode_planar_float(
    const float* const* in_planes,
    int channels,
    int samples,
    uint8_t* out_buf,
    size_t max_out_bytes
);

/**
 * Decodes 4-bit IMA ADPCM bitstream back into planar float audio.
 */
OPENAVX_API int avx_adpcm_decode_to_planar_float(
    const uint8_t* in_buf,
    size_t in_bytes,
    int channels,
    float* const* out_planes,
    int max_samples
);

#ifdef __cplusplus
}
#endif

#endif /* AVX_AUDIO_CODEC_H */
