/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_audio_resampler.c - libFuzzer Target for Audio Resampling & Downmixing
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "avx_audio.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size < 12) return 0;

    /* Extract parameters from header */
    int in_rate = ((int)(data[0] | (data[1] << 8)) % 200000) + 1;
    int out_rate = ((int)(data[2] | (data[3] << 8)) % 200000) + 1;
    int channels = (data[4] % 8) + 1;
    avx_audio_resample_mode_e mode = (avx_audio_resample_mode_e)(data[5] % 3);
    avx_audio_normalize_e norm_mode = (avx_audio_normalize_e)(data[6] % 3);

    const uint8_t *sample_bytes = data + 12;
    size_t sample_len = size - 12;
    int total_floats = (int)(sample_len / sizeof(float));
    int frames = total_floats / channels;
    if (frames > 1024) frames = 1024;
    if (frames < 1) frames = 1;

    /* Prepare planar input arrays */
    float *in_data[8] = {NULL};
    float *out_data[8] = {NULL};
    int max_out = frames * 4 + 256;

    for (int c = 0; c < 8; ++c) {
        in_data[c] = (float *)calloc((size_t)frames + 16, sizeof(float));
        out_data[c] = (float *)calloc((size_t)max_out + 16, sizeof(float));
        if (!in_data[c] || !out_data[c]) {
            for (int j = 0; j <= c; ++j) {
                free(in_data[j]);
                free(out_data[j]);
            }
            return 0;
        }
    }

    /* Fill in_data with fuzzer bytes if available */
    if (total_floats > 0) {
        for (int c = 0; c < channels; ++c) {
            for (int f = 0; f < frames; ++f) {
                size_t src_idx = (size_t)f * (size_t)channels + (size_t)c;
                if (src_idx < (size_t)total_floats) {
                    memcpy(&in_data[c][f], sample_bytes + src_idx * sizeof(float), sizeof(float));
                }
            }
        }
    }

    /* 1. Test stateful resampler */
    avx_resampler_t *r = avx_resampler_create(in_rate, out_rate, channels, mode);
    if (r) {
        (void)avx_resampler_process(r, (const float * const *)in_data, frames, out_data, max_out);
        (void)avx_resampler_flush(r, out_data, max_out);
        avx_resampler_reset(r);
        avx_resampler_destroy(r);
    }

    /* 2. Test one-shot simple resample */
    (void)avx_audio_resample_simple(in_data[0], frames, in_rate, out_data[0], max_out, out_rate, mode);

    /* 3. Test downmixing routines */
    if (frames > 0) {
        avx_audio_downmix_stereo_to_mono(in_data[0], in_data[1], out_data[0], frames, norm_mode);
        avx_audio_downmix_quad_to_stereo((const float * const *)in_data, out_data[0], out_data[1], frames, norm_mode);
        avx_audio_downmix_5_1_to_stereo((const float * const *)in_data, out_data[0], out_data[1], frames, norm_mode);
        avx_audio_downmix_7_1_to_stereo((const float * const *)in_data, out_data[0], out_data[1], frames, norm_mode);
    }

    for (int c = 0; c < 8; ++c) {
        free(in_data[c]);
        free(out_data[c]);
    }

    return 0;
}
