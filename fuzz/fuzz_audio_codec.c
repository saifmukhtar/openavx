/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_audio_codec.c - libFuzzer Target for Opus & IMA ADPCM Audio Codecs
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "openavx.h"
#include "avx_audio_codec.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size < 8) return 0;

    /* Extract channels and codec parameters from the first 4 bytes */
    int channel_options[] = { 1, 2, 4, 6, 8 };
    int channels = channel_options[data[0] % 5];
    int sample_rate = (data[1] % 2 == 0) ? 48000 : 44100;
    uint32_t fourcc = (data[2] % 2 == 0) ? OAVX_FOURCC_AUDIO_TYPE_OPUS : OAVX_FOURCC_AUDIO_TYPE_ADPC;

    const uint8_t *payload = data + 4;
    size_t payload_len = size - 4;

    /* 1. Fuzz Standalone IMA ADPCM Decoder */
    const int max_samples = 4096;
    float *planes[8] = {NULL};
    for (int c = 0; c < 8; ++c) {
        planes[c] = (float*)calloc(max_samples, sizeof(float));
        if (!planes[c]) {
            for (int j = 0; j < c; ++j) free(planes[j]);
            return 0;
        }
    }

    avx_adpcm_decode_to_planar_float(payload, payload_len, channels, planes, max_samples);

    /* 2. Fuzz IMA ADPCM Encoder with synthetic float data */
    int synth_samples = (int)(payload_len / (sizeof(float) * channels));
    if (synth_samples > 0 && synth_samples <= max_samples) {
        for (int c = 0; c < channels; ++c) {
            memcpy(planes[c], payload + (size_t)c * synth_samples * sizeof(float), synth_samples * sizeof(float));
        }
        uint8_t out_adpcm[65536];
        avx_adpcm_encode_planar_float((const float* const*)planes, channels, synth_samples, out_adpcm, sizeof(out_adpcm));
    }

    /* 3. Fuzz Opus / IMA ADPCM Audio Decoder */
    avx_audio_decoder_t *dec = avx_audio_decoder_create(sample_rate, channels, fourcc);
    if (dec) {
        avx_audio_decoder_decode(dec, fourcc, payload, payload_len, planes, max_samples);
        avx_audio_decoder_destroy(dec);
    }

    /* 4. Fuzz Audio Encoder Feed, Pull, and Flush */
    avx_audio_encoder_t *enc = avx_audio_encoder_create(sample_rate, channels, AVX_AUDIO_CODEC_OPUS, 128);
    if (enc) {
        int feed_samples = (int)(data[3] % 240) + 1;
        if (feed_samples <= max_samples) {
            avx_audio_encoder_feed(enc, (const float* const*)planes, feed_samples, 0);

            uint8_t out_pkt[65536];
            size_t out_pkt_size = 0;
            int out_samples = 0;
            int64_t out_pts = 0;
            uint32_t out_fourcc = 0;

            avx_audio_encoder_pull(enc, out_pkt, sizeof(out_pkt), &out_pkt_size, &out_samples, &out_pts, &out_fourcc);
            avx_audio_encoder_flush(enc, out_pkt, sizeof(out_pkt), &out_pkt_size, &out_samples, &out_pts, &out_fourcc);
        }
        avx_audio_encoder_destroy(enc);
    }

    for (int c = 0; c < 8; ++c) {
        free(planes[c]);
    }

    return 0;
}
