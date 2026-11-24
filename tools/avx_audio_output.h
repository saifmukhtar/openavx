/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/avx_audio_output.h - Audio playback backend interface
 */

#ifndef AVX_AUDIO_OUTPUT_H
#define AVX_AUDIO_OUTPUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum avx_audio_backend_e {
    AVX_AUDIO_BACKEND_AUTO = 0,
    AVX_AUDIO_BACKEND_PULSE,
    AVX_AUDIO_BACKEND_ALSA,
    AVX_AUDIO_BACKEND_PIPE,
    AVX_AUDIO_BACKEND_NONE
} avx_audio_backend_e;

typedef struct avx_audio_output_t avx_audio_output_t;

/**
 * Creates an audio output sink for the given backend, sample rate, and channels.
 * Passing NULL or "auto" selects the best available backend (Pulse -> ALSA -> Pipe -> None).
 */
avx_audio_output_t* avx_audio_output_create(const char* backend_name, int sample_rate, int channels);

/**
 * Writes an audio frame to the active audio device/stream.
 * Automatically handles planar-to-interleaved float conversion.
 */
bool avx_audio_output_write(avx_audio_output_t* ao, const oavx_audio_frame_t* aframe);

/**
 * Returns the name of the active audio backend.
 */
const char* avx_audio_output_get_backend_name(const avx_audio_output_t* ao);

/**
 * Closes the audio output device and frees resources.
 */
void avx_audio_output_destroy(avx_audio_output_t* ao);

#ifdef __cplusplus
}
#endif

#endif /* AVX_AUDIO_OUTPUT_H */
