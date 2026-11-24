/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - High-Performance Audio/Video Network Library
 * test/openavx_audio_codec_test.c - Unit & Verification Suite for Audio Compression (Opus & IMA ADPCM)
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <assert.h>

#include "openavx.h"
#include "avx_audio_codec.h"
#include "avx_audio.h"
#include "avx_platform.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s:%d: Assertion '%s' failed\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "[FAIL] %s:%d: Assertion '%s == %s' failed (%ld != %ld)\n", \
                __FILE__, __LINE__, #a, #b, (long)(a), (long)(b)); \
        exit(1); \
    } \
} while(0)

#define ASSERT_NEAR(a, b, eps) do { \
    float _diff = fabsf((float)(a) - (float)(b)); \
    if (_diff > (eps)) { \
        fprintf(stderr, "[FAIL] %s:%d: Assertion '%s' =~ '%s' failed (diff=%f, eps=%f, a=%f, b=%f)\n", \
                __FILE__, __LINE__, #a, #b, _diff, (float)(eps), (float)(a), (float)(b)); \
        exit(1); \
    } \
} while(0)

/* Helper: Generate planar sine wave */
static void generate_sine_wave(float *plane, int samples, int sample_rate, float freq, float amplitude) {
    for (int i = 0; i < samples; ++i) {
        plane[i] = amplitude * sinf((float)(2.0 * M_PI * freq * (double)i / (double)sample_rate));
    }
}

/* ========================================================================= */
/* TEST 1: PURE C99 IMA ADPCM ENCODE / DECODE ROUNDTRIP                      */
/* ========================================================================= */

static void test_adpcm_roundtrip(void) {
    printf("[*] 1. Testing Pure C99 IMA ADPCM Multi-Channel Roundtrip...\n");

    const int channel_configs[] = { 1, 2, 4, 6, 8, 16, 32 };
    const int num_configs = sizeof(channel_configs) / sizeof(channel_configs[0]);
    const int samples = 960;
    const int sample_rate = 48000;

    for (int c_idx = 0; c_idx < num_configs; ++c_idx) {
        int channels = channel_configs[c_idx];
        printf("    -> Testing ADPCM with %d channel(s)...\n", channels);

        float *in_planes[32];
        float *out_planes[32];
        for (int c = 0; c < channels; ++c) {
            in_planes[c] = (float*)malloc(samples * sizeof(float));
            out_planes[c] = (float*)malloc(samples * sizeof(float));
            float freq = 220.0f * (float)(c + 1);
            generate_sine_wave(in_planes[c], samples, sample_rate, freq, 0.8f);
            memset(out_planes[c], 0, samples * sizeof(float));
        }

        /* Encode planar float to ADPCM */
        uint8_t adpcm_buf[65536];
        size_t encoded_bytes = avx_adpcm_encode_planar_float(
            (const float* const*)in_planes, channels, samples, adpcm_buf, sizeof(adpcm_buf));

        ASSERT_TRUE(encoded_bytes > 0);
        /* Expected size: 8-byte global header + per-channel (4-byte header + (samples - 1)/2 nibbles) */
        size_t expected_size = 8 + (size_t)channels * (4 + (samples > 1 ? (size_t)(samples - 1 + 1) / 2 : 0));
        ASSERT_EQ(encoded_bytes, expected_size);

        /* Decode ADPCM back to planar float */
        int decoded_samples = avx_adpcm_decode_to_planar_float(
            adpcm_buf, encoded_bytes, channels, out_planes, samples);

        ASSERT_EQ(decoded_samples, samples);

        /* Verify accuracy: IMA ADPCM is 4-bit lossy, check max sample difference < 0.12 */
        for (int c = 0; c < channels; ++c) {
            float max_diff = 0.0f;
            float energy_in = 0.0f;
            float energy_out = 0.0f;
            for (int s = 0; s < samples; ++s) {
                float diff = fabsf(in_planes[c][s] - out_planes[c][s]);
                if (diff > max_diff) max_diff = diff;
                energy_in += in_planes[c][s] * in_planes[c][s];
                energy_out += out_planes[c][s] * out_planes[c][s];
            }
            ASSERT_TRUE(max_diff < 0.12f);
            ASSERT_TRUE(energy_out > 0.5f * energy_in);
            free(in_planes[c]);
            free(out_planes[c]);
        }
        printf("       [+] %d channel(s) passed successfully (compression ratio: 8:1 over float32)\n", channels);
    }
}

/* ========================================================================= */
/* TEST 2: OPUS MULTISTREAM DYNAMIC ENCODER / DECODER MATRIX                  */
/* ========================================================================= */

static void test_opus_multistream(void) {
    printf("[*] 2. Testing Opus Multi-Stream Dynamic Codec (or ADPCM Fallback)...\n");

    bool has_opus = avx_audio_codec_has_opus();
    printf("    -> Host libopus runtime status: %s\n", has_opus ? "LOADED DYNAMICALLY (dlopen)" : "ABSENT (will test ADPCM fallback)");

    const int channel_configs[] = { 1, 2, 4, 6, 8 };
    const int num_configs = sizeof(channel_configs) / sizeof(channel_configs[0]);
    const int samples = 960;
    const int sample_rate = 48000;

    for (int c_idx = 0; c_idx < num_configs; ++c_idx) {
        int channels = channel_configs[c_idx];
        printf("    -> Testing encoder/decoder with %d channel(s)...\n", channels);

        avx_audio_encoder_t *enc = avx_audio_encoder_create(
            sample_rate, channels, AVX_AUDIO_CODEC_OPUS, 0);
        ASSERT_TRUE(enc != NULL);

        if (has_opus) {
            ASSERT_EQ(avx_audio_encoder_get_codec(enc), AVX_AUDIO_CODEC_OPUS);
            ASSERT_EQ(avx_audio_encoder_get_fourcc(enc), OAVX_FOURCC_AUDIO_TYPE_OPUS);
        } else {
            ASSERT_EQ(avx_audio_encoder_get_codec(enc), AVX_AUDIO_CODEC_ADPCM);
            ASSERT_EQ(avx_audio_encoder_get_fourcc(enc), OAVX_FOURCC_AUDIO_TYPE_ADPC);
        }

        uint32_t active_fourcc = avx_audio_encoder_get_fourcc(enc);
        avx_audio_decoder_t *dec = avx_audio_decoder_create(sample_rate, channels, active_fourcc);
        ASSERT_TRUE(dec != NULL);

        float *in_planes[8];
        float *out_planes[8];
        for (int c = 0; c < channels; ++c) {
            in_planes[c] = (float*)malloc(samples * sizeof(float));
            out_planes[c] = (float*)malloc(samples * sizeof(float));
            float freq = 300.0f + (float)c * 150.0f;
            generate_sine_wave(in_planes[c], samples, sample_rate, freq, 0.7f);
            memset(out_planes[c], 0, samples * sizeof(float));
        }

        /* Feed 960 samples into encoder */
        bool fed = avx_audio_encoder_feed(enc, (const float* const*)in_planes, samples, 1000000000LL);
        ASSERT_TRUE(fed);

        uint8_t pkt[65536];
        size_t pkt_size = 0;
        int out_samples = 0;
        int64_t out_pts = 0;
        uint32_t out_fourcc = 0;

        bool pulled = avx_audio_encoder_pull(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc);
        ASSERT_TRUE(pulled);
        ASSERT_TRUE(pkt_size > 0);
        ASSERT_EQ(out_samples, samples);
        ASSERT_EQ(out_fourcc, active_fourcc);

        /* Decode packet back into planar buffers */
        int decoded = avx_audio_decoder_decode(dec, out_fourcc, pkt, pkt_size, out_planes, samples);
        ASSERT_EQ(decoded, samples);

        /* Verify output has energy and no nan/inf */
        for (int c = 0; c < channels; ++c) {
            float energy = 0.0f;
            for (int s = 0; s < samples; ++s) {
                ASSERT_TRUE(!isnan(out_planes[c][s]) && !isinf(out_planes[c][s]));
                energy += out_planes[c][s] * out_planes[c][s];
            }
            ASSERT_TRUE(energy > 1.0f);
            free(in_planes[c]);
            free(out_planes[c]);
        }

        avx_audio_decoder_destroy(dec);
        avx_audio_encoder_destroy(enc);
        printf("       [+] %d channel(s) encoder/decoder roundtrip verified\n", channels);
    }
}

/* ========================================================================= */
/* TEST 3: IRREGULAR CHUNK ACCUMULATOR & FLUSH ENGINE                        */
/* ========================================================================= */

static void test_accumulator_and_flush(void) {
    printf("[*] 3. Testing 20 ms Chunk Accumulator & Flush Engine (60 fps 800-sample feeds)...\n");

    const int channels = 2;
    const int sample_rate = 48000;
    const int feed_frames = 800; /* Typical 48 kHz audio per frame at 60 fps */

    avx_audio_encoder_t *enc = avx_audio_encoder_create(
        sample_rate, channels, AVX_AUDIO_CODEC_OPUS, 128);
    ASSERT_TRUE(enc != NULL);

    float *in_planes[2];
    in_planes[0] = (float*)malloc(feed_frames * sizeof(float));
    in_planes[1] = (float*)malloc(feed_frames * sizeof(float));
    generate_sine_wave(in_planes[0], feed_frames, sample_rate, 440.0f, 0.8f);
    generate_sine_wave(in_planes[1], feed_frames, sample_rate, 880.0f, 0.8f);

    uint8_t pkt[65536];
    size_t pkt_size = 0;
    int out_samples = 0;
    int64_t out_pts = 0;
    uint32_t out_fourcc = 0;

    bool is_opus = (avx_audio_encoder_get_codec(enc) == AVX_AUDIO_CODEC_OPUS);

    if (is_opus) {
        /* Feed 1: 800 samples in accumulator (Opus needs 960). Pull should return false! */
        bool fed1 = avx_audio_encoder_feed(enc, (const float* const*)in_planes, feed_frames, 1000LL);
        ASSERT_TRUE(fed1);
        bool pulled1 = avx_audio_encoder_pull(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc);
        ASSERT_TRUE(!pulled1);
        printf("    [+] Feed 1 (800 samples): Correctly buffered, pull deferred\n");

        /* Feed 2: 800 samples (total 1600 samples). Pull should return 960 samples! Remaining 640. */
        bool fed2 = avx_audio_encoder_feed(enc, (const float* const*)in_planes, feed_frames, 2000LL);
        ASSERT_TRUE(fed2);
        bool pulled2 = avx_audio_encoder_pull(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc);
        ASSERT_TRUE(pulled2);
        ASSERT_EQ(out_samples, 960);
        printf("    [+] Feed 2 (800 samples, total 1600): Pulled 960 samples successfully\n");

        /* Second pull without feeding should return false because only 640 remain (< 960) */
        bool pulled2_sub = avx_audio_encoder_pull(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc);
        ASSERT_TRUE(!pulled2_sub);

        /* Flush: Should encode the remaining samples (zero-padded to 960 for Opus RFC 6716 boundary) */
        bool flushed = avx_audio_encoder_flush(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc);
        ASSERT_TRUE(flushed);
        ASSERT_EQ(out_samples, 960);
        printf("    [+] Flush: Flushed remaining samples cleanly (out_samples=%d)\n", out_samples);
    } else {
        /* ADPCM fallback encodes arbitrary accumulated sample counts immediately without 960-sample framing */
        bool fed1 = avx_audio_encoder_feed(enc, (const float* const*)in_planes, feed_frames, 1000LL);
        ASSERT_TRUE(fed1);
        bool pulled1 = avx_audio_encoder_pull(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc);
        ASSERT_TRUE(pulled1);
        ASSERT_EQ(out_samples, feed_frames);
        ASSERT_EQ(out_pts, 1000LL);
        ASSERT_TRUE(!avx_audio_encoder_pull(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc));
        printf("    [+] ADPCM Feed 1 (800 samples): Pulled immediately cleanly\n");

        bool fed2 = avx_audio_encoder_feed(enc, (const float* const*)in_planes, 640, 2000LL);
        ASSERT_TRUE(fed2);
        bool flushed = avx_audio_encoder_flush(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc);
        ASSERT_TRUE(flushed);
        ASSERT_EQ(out_samples, 640);
        ASSERT_EQ(out_pts, 2000LL);
        printf("    [+] ADPCM Flush (640 samples): Flushed cleanly (out_samples=%d)\n", out_samples);
    }

    /* Subsequent pull and flush should return false (accumulator empty) */
    ASSERT_TRUE(!avx_audio_encoder_pull(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc));
    ASSERT_TRUE(!avx_audio_encoder_flush(enc, pkt, sizeof(pkt), &pkt_size, &out_samples, &out_pts, &out_fourcc));

    free(in_planes[0]);
    free(in_planes[1]);
    avx_audio_encoder_destroy(enc);
    printf("    [+] Accumulator and flush verified\n");
}

/* ========================================================================= */
/* TEST 4: END-TO-END STREAM TRANSMIT & RECEIVE WITH AUDIO COMPRESSION        */
/* ========================================================================= */

static void test_stream_compressed_loopback(void) {
    printf("[*] 4. Testing End-to-End Stream Pipeline with Compressed Audio ('AVXO')...\n");

    oavx_context_t *ctx = oavx_context_create();
    ASSERT_TRUE(ctx != NULL);

    oavx_stream_config_t tx_cfg = {
        .name = "OAVX_COMP_AUDIO_STREAM",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .groups = "Public"
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    ASSERT_TRUE(tx != NULL);

    /* Enable Manual Audio Compression with 192 kbps target */
    oavx_status_t st = oavx_stream_set_audio_compression(tx, OAVX_AUDIO_COMPRESSION_MANUAL, 192);
    ASSERT_EQ(st, OAVX_SUCCESS);

    oavx_stream_config_t rx_cfg = {
        .name = "OAVX_COMP_AUDIO_STREAM",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .groups = "Public"
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    ASSERT_TRUE(rx != NULL);

    /* Allow TCP connection handshake to complete */
    usleep(150000);

    /* Transmit 5.1 Surround audio frames (6 channels, 48 kHz, 960 frames = 20 ms) */
    const int channels = 6;
    const int frames = 960;
    const int sample_rate = 48000;

    float *tx_planes[6];
    for (int c = 0; c < channels; ++c) {
        tx_planes[c] = (float*)malloc(frames * sizeof(float));
        generate_sine_wave(tx_planes[c], frames, sample_rate, 440.0f + (float)c * 100.0f, 0.7f);
    }

    oavx_audio_frame_t a_send;
    memset(&a_send, 0, sizeof(a_send));
    a_send.sample_rate = sample_rate;
    a_send.channels = channels;
    a_send.frames = frames;
    a_send.format = OAVX_AUDIO_FMT_FLT_PLANAR;
    a_send.stride_bytes = frames * sizeof(float);
    a_send.pts_ns = 1234567890LL;
    for (int c = 0; c < channels; ++c) {
        a_send.planes[c] = tx_planes[c];
    }

    oavx_status_t send_st = oavx_stream_send_audio(tx, &a_send);
    ASSERT_EQ(send_st, OAVX_SUCCESS);

    /* Receive on the other side */
    oavx_video_frame_t v_recv;
    oavx_audio_frame_t a_recv;
    oavx_metadata_frame_t m_recv;
    memset(&v_recv, 0, sizeof(v_recv));
    memset(&a_recv, 0, sizeof(a_recv));
    memset(&m_recv, 0, sizeof(m_recv));

    oavx_frame_type_t ft = oavx_stream_receive(rx, &v_recv, &a_recv, &m_recv, 1000);
    ASSERT_EQ(ft, OAVX_FRAME_TYPE_AUDIO);

    /* Verify decoded audio frame properties */
    ASSERT_EQ(a_recv.sample_rate, (uint32_t)sample_rate);
    ASSERT_EQ(a_recv.channels, (uint16_t)channels);
    ASSERT_EQ(a_recv.frames, (uint32_t)frames);
    ASSERT_EQ(a_recv.format, OAVX_AUDIO_FMT_FLT_PLANAR);
    ASSERT_EQ(a_recv.pts_ns, 1234567890LL);
    ASSERT_TRUE(a_recv.planes[0] != NULL);
    ASSERT_TRUE(a_recv.planes[5] != NULL);

    /* Check telemetry */
    oavx_telemetry_t telem;
    ASSERT_TRUE(oavx_stream_get_telemetry(tx, &telem));
    ASSERT_TRUE(telem.audio_frames_total >= 1);
    ASSERT_TRUE(telem.total_wire_bytes > 0);

    oavx_stream_release_audio(rx, &a_recv);

    for (int c = 0; c < channels; ++c) {
        free(tx_planes[c]);
    }

    oavx_stream_close(rx);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
    printf("    [+] End-to-end compressed audio transmission verified\n");
}

/* ========================================================================= */
/* TEST 5: ADPCM CORRUPTED / OVERFLOW HEADER RESILIENCE (FUZZER REGRESSION)  */
/* ========================================================================= */

static void test_adpcm_fuzz_bounds(void) {
    printf("[*] 5. Testing ADPCM corrupted / overflow header resilience...\n");

    /* Buffer with 'AD' header and byte 7 = 0xFF (tests signed int shift overflow) */
    uint8_t corrupt_hdr[32];
    memset(corrupt_hdr, 0, sizeof(corrupt_hdr));
    corrupt_hdr[0] = 0x41; /* 'A' */
    corrupt_hdr[1] = 0x44; /* 'D' */
    corrupt_hdr[2] = 2;    /* 2 channels */
    corrupt_hdr[3] = 0;
    corrupt_hdr[4] = 0x00;
    corrupt_hdr[5] = 0x00;
    corrupt_hdr[6] = 0x00;
    corrupt_hdr[7] = 0xFF; /* 255 in MSB: previously caused UBSan signed shift overflow */

    float p0[100], p1[100];
    float* out_planes[2] = { p0, p1 };

    int ret = avx_adpcm_decode_to_planar_float(corrupt_hdr, sizeof(corrupt_hdr), 2, out_planes, 100);
    ASSERT_EQ(ret, -1);

    /* Zero samples */
    corrupt_hdr[7] = 0x00;
    ret = avx_adpcm_decode_to_planar_float(corrupt_hdr, sizeof(corrupt_hdr), 2, out_planes, 100);
    ASSERT_EQ(ret, -1);

    /* Truncated buffer */
    ret = avx_adpcm_decode_to_planar_float(corrupt_hdr, 4, 2, out_planes, 100);
    ASSERT_EQ(ret, -1);

    /* Non-positive max_samples boundary checks */
    ret = avx_adpcm_decode_to_planar_float(corrupt_hdr, sizeof(corrupt_hdr), 2, out_planes, 0);
    ASSERT_EQ(ret, -1);
    ret = avx_adpcm_decode_to_planar_float(corrupt_hdr, sizeof(corrupt_hdr), 2, out_planes, -1);
    ASSERT_EQ(ret, -1);

    printf("    [+] ADPCM header boundary and shift overflow protection verified\n");
}

/* ========================================================================= */
/* TEST 6: MULTI-CHANNEL LIVE TRANSPORT (16-CH SDI & 32-CH DAW)              */
/* ========================================================================= */

static void test_multichannel_transport_16ch_32ch(void) {
    printf("[*] 6. Testing End-to-End Multi-Channel Broadcast Transport (16ch & 32ch)...\n");

    oavx_context_t *ctx = oavx_context_create();
    ASSERT_TRUE(ctx != NULL);

    /* --------------------------------------------------------------------- */
    /* PART A: 16-Channel Broadcast SDI Audio Stream over TCP                */
    /* --------------------------------------------------------------------- */
    printf("    -> Part A: 16-Channel SDI Broadcast Stream over TCP...\n");
    {
        oavx_stream_config_t tx_cfg = {
            .name = "OAVX_16CH_SDI_FEED",
            .direction = OAVX_DIR_TRANSMIT,
            .clock_sync = false,
            .groups = "Public"
        };
        oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
        ASSERT_TRUE(tx != NULL);

        const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
        ASSERT_TRUE(ep != NULL && ep->address != NULL);

        oavx_stream_config_t rx_cfg = {
            .name = "OAVX_16CH_SDI_RECV",
            .endpoint = ep->address,
            .direction = OAVX_DIR_RECEIVE,
            .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
            .bandwidth = OAVX_BANDWIDTH_HIGHEST,
            .groups = "Public"
        };
        oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
        ASSERT_TRUE(rx != NULL);

        usleep(150000);

        const int channels = 16;
        const int frames = 960;
        const int sample_rate = 48000;

        float *tx_planes[16];
        for (int c = 0; c < channels; ++c) {
            tx_planes[c] = (float*)malloc(frames * sizeof(float));
            generate_sine_wave(tx_planes[c], frames, sample_rate, 100.0f * (float)(c + 1), 0.75f);
        }

        oavx_audio_frame_t a_send;
        memset(&a_send, 0, sizeof(a_send));
        a_send.sample_rate = sample_rate;
        a_send.channels = channels;
        a_send.frames = frames;
        a_send.format = OAVX_AUDIO_FMT_FLT_PLANAR;
        a_send.stride_bytes = frames * sizeof(float);
        a_send.pts_ns = 2000000000LL;
        a_send.channel_mask = avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_16CH);
        for (int c = 0; c < channels; ++c) {
            a_send.planes[c] = tx_planes[c];
        }

        oavx_status_t send_st = oavx_stream_send_audio(tx, &a_send);
        ASSERT_EQ(send_st, OAVX_SUCCESS);

        oavx_audio_frame_t a_recv;
        memset(&a_recv, 0, sizeof(a_recv));
        oavx_frame_type_t ft = oavx_stream_receive(rx, NULL, &a_recv, NULL, 1000);
        ASSERT_EQ(ft, OAVX_FRAME_TYPE_AUDIO);

        ASSERT_EQ(a_recv.sample_rate, (uint32_t)sample_rate);
        ASSERT_EQ(a_recv.channels, 16);
        ASSERT_EQ(a_recv.frames, (uint32_t)frames);
        ASSERT_EQ(a_recv.format, OAVX_AUDIO_FMT_FLT_PLANAR);
        ASSERT_EQ(a_recv.channel_mask, avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_16CH));

        /* Verify bit-exact fidelity across all 16 channels */
        for (int c = 0; c < channels; ++c) {
            ASSERT_TRUE(a_recv.planes[c] != NULL);
            const float *rx_plane = (const float*)a_recv.planes[c];
            for (int s = 0; s < frames; ++s) {
                ASSERT_NEAR(rx_plane[s], tx_planes[c][s], 1e-6f);
            }
        }
        for (int c = channels; c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
            ASSERT_TRUE(a_recv.planes[c] == NULL);
        }

        oavx_stream_release_audio(rx, &a_recv);
        for (int c = 0; c < channels; ++c) free(tx_planes[c]);

        oavx_stream_close(rx);
        oavx_stream_close(tx);
        printf("       [+] 16-channel uncompressed SDI stream verified with bit-exact fidelity\n");
    }

    /* --------------------------------------------------------------------- */
    /* PART B: 32-Channel Pro-Audio / Dante DAW Stream over TCP              */
    /* --------------------------------------------------------------------- */
    printf("    -> Part B: 32-Channel Pro-Audio Matrix Stream over TCP...\n");
    {
        oavx_stream_config_t tx_cfg = {
            .name = "OAVX_32CH_DAW_FEED",
            .direction = OAVX_DIR_TRANSMIT,
            .clock_sync = false,
            .groups = "Public"
        };
        oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
        ASSERT_TRUE(tx != NULL);

        const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
        ASSERT_TRUE(ep != NULL && ep->address != NULL);

        oavx_stream_config_t rx_cfg = {
            .name = "OAVX_32CH_DAW_RECV",
            .endpoint = ep->address,
            .direction = OAVX_DIR_RECEIVE,
            .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
            .bandwidth = OAVX_BANDWIDTH_HIGHEST,
            .groups = "Public"
        };
        oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
        ASSERT_TRUE(rx != NULL);

        usleep(150000);

        const int channels = 32;
        const int frames = 960;
        const int sample_rate = 48000;

        float *tx_planes[32];
        for (int c = 0; c < channels; ++c) {
            tx_planes[c] = (float*)malloc(frames * sizeof(float));
            generate_sine_wave(tx_planes[c], frames, sample_rate, 50.0f * (float)(c + 1), 0.70f);
        }

        oavx_audio_frame_t a_send;
        memset(&a_send, 0, sizeof(a_send));
        a_send.sample_rate = sample_rate;
        a_send.channels = channels;
        a_send.frames = frames;
        a_send.format = OAVX_AUDIO_FMT_FLT_PLANAR;
        a_send.stride_bytes = frames * sizeof(float);
        a_send.pts_ns = 3000000000LL;
        a_send.channel_mask = avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_32CH);
        for (int c = 0; c < channels; ++c) {
            a_send.planes[c] = tx_planes[c];
        }

        oavx_status_t send_st = oavx_stream_send_audio(tx, &a_send);
        ASSERT_EQ(send_st, OAVX_SUCCESS);

        oavx_audio_frame_t a_recv;
        memset(&a_recv, 0, sizeof(a_recv));
        oavx_frame_type_t ft = oavx_stream_receive(rx, NULL, &a_recv, NULL, 1000);
        ASSERT_EQ(ft, OAVX_FRAME_TYPE_AUDIO);

        ASSERT_EQ(a_recv.sample_rate, (uint32_t)sample_rate);
        ASSERT_EQ(a_recv.channels, 32);
        ASSERT_EQ(a_recv.frames, (uint32_t)frames);
        ASSERT_EQ(a_recv.format, OAVX_AUDIO_FMT_FLT_PLANAR);
        ASSERT_EQ(a_recv.channel_mask, avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_32CH));

        /* Verify bit-exact fidelity across all 32 channels */
        for (int c = 0; c < channels; ++c) {
            ASSERT_TRUE(a_recv.planes[c] != NULL);
            const float *rx_plane = (const float*)a_recv.planes[c];
            for (int s = 0; s < frames; ++s) {
                ASSERT_NEAR(rx_plane[s], tx_planes[c][s], 1e-6f);
            }
        }

        oavx_stream_release_audio(rx, &a_recv);
        for (int c = 0; c < channels; ++c) free(tx_planes[c]);

        oavx_stream_close(rx);
        oavx_stream_close(tx);
        printf("       [+] 32-channel uncompressed Pro-Audio stream verified with bit-exact fidelity\n");
    }

    /* --------------------------------------------------------------------- */
    /* PART C: 16-Channel Broadcast SDI Audio Stream over RUDP               */
    /* --------------------------------------------------------------------- */
    printf("    -> Part C: 16-Channel SDI Broadcast Stream over RUDP...\n");
    {
        oavx_stream_config_t tx_cfg = {
            .name = "OAVX_16CH_RUDP_FEED",
            .direction = OAVX_DIR_TRANSMIT,
            .transport = OAVX_TRANSPORT_RUDP,
            .clock_sync = false,
            .groups = "Public"
        };
        oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
        ASSERT_TRUE(tx != NULL);

        const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
        ASSERT_TRUE(ep != NULL && ep->address != NULL);

        oavx_stream_config_t rx_cfg = {
            .name = "OAVX_16CH_RUDP_RECV",
            .endpoint = ep->address,
            .direction = OAVX_DIR_RECEIVE,
            .transport = OAVX_TRANSPORT_RUDP,
            .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
            .bandwidth = OAVX_BANDWIDTH_HIGHEST,
            .groups = "Public"
        };
        oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
        ASSERT_TRUE(rx != NULL);

        usleep(150000);

        const int channels = 16;
        const int frames = 480;
        const int sample_rate = 48000;

        float *tx_planes[16];
        for (int c = 0; c < channels; ++c) {
            tx_planes[c] = (float*)malloc(frames * sizeof(float));
            generate_sine_wave(tx_planes[c], frames, sample_rate, 120.0f * (float)(c + 1), 0.70f);
        }

        oavx_audio_frame_t a_send;
        memset(&a_send, 0, sizeof(a_send));
        a_send.sample_rate = sample_rate;
        a_send.channels = channels;
        a_send.frames = frames;
        a_send.format = OAVX_AUDIO_FMT_FLT_PLANAR;
        a_send.stride_bytes = frames * sizeof(float);
        a_send.pts_ns = 4000000000LL;
        a_send.channel_mask = avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_16CH);
        for (int c = 0; c < channels; ++c) {
            a_send.planes[c] = tx_planes[c];
        }

        oavx_status_t send_st = oavx_stream_send_audio(tx, &a_send);
        ASSERT_EQ(send_st, OAVX_SUCCESS);

        oavx_audio_frame_t a_recv;
        memset(&a_recv, 0, sizeof(a_recv));
        oavx_frame_type_t ft = oavx_stream_receive(rx, NULL, &a_recv, NULL, 500);
        ASSERT_EQ(ft, OAVX_FRAME_TYPE_AUDIO);

        ASSERT_EQ(a_recv.sample_rate, (uint32_t)sample_rate);
        ASSERT_EQ(a_recv.channels, 16);
        ASSERT_EQ(a_recv.frames, (uint32_t)frames);
        ASSERT_EQ(a_recv.format, OAVX_AUDIO_FMT_FLT_PLANAR);
        ASSERT_EQ(a_recv.channel_mask, avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_16CH));

        for (int c = 0; c < channels; ++c) {
            ASSERT_TRUE(a_recv.planes[c] != NULL);
            const float *rx_plane = (const float*)a_recv.planes[c];
            for (int s = 0; s < frames; ++s) {
                ASSERT_NEAR(rx_plane[s], tx_planes[c][s], 1e-6f);
            }
        }

        oavx_stream_release_audio(rx, &a_recv);
        for (int c = 0; c < channels; ++c) free(tx_planes[c]);

        oavx_stream_close(rx);
        oavx_stream_close(tx);
        printf("       [+] 16-channel RUDP stream verified with bit-exact fidelity\n");
    }

    /* --------------------------------------------------------------------- */
    /* PART D: 32-Channel Pro-Audio Matrix Stream over RUDP                  */
    /* --------------------------------------------------------------------- */
    printf("    -> Part D: 32-Channel Pro-Audio Matrix Stream over RUDP...\n");
    {
        oavx_stream_config_t tx_cfg = {
            .name = "OAVX_32CH_RUDP_FEED",
            .direction = OAVX_DIR_TRANSMIT,
            .transport = OAVX_TRANSPORT_RUDP,
            .clock_sync = false,
            .groups = "Public"
        };
        oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
        ASSERT_TRUE(tx != NULL);

        const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
        ASSERT_TRUE(ep != NULL && ep->address != NULL);

        oavx_stream_config_t rx_cfg = {
            .name = "OAVX_32CH_RUDP_RECV",
            .endpoint = ep->address,
            .direction = OAVX_DIR_RECEIVE,
            .transport = OAVX_TRANSPORT_RUDP,
            .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
            .bandwidth = OAVX_BANDWIDTH_HIGHEST,
            .groups = "Public"
        };
        oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
        ASSERT_TRUE(rx != NULL);

        usleep(150000);

        const int channels = 32;
        const int frames = 480;
        const int sample_rate = 48000;

        float *tx_planes[32];
        for (int c = 0; c < channels; ++c) {
            tx_planes[c] = (float*)malloc(frames * sizeof(float));
            generate_sine_wave(tx_planes[c], frames, sample_rate, 60.0f * (float)(c + 1), 0.65f);
        }

        oavx_audio_frame_t a_send;
        memset(&a_send, 0, sizeof(a_send));
        a_send.sample_rate = sample_rate;
        a_send.channels = channels;
        a_send.frames = frames;
        a_send.format = OAVX_AUDIO_FMT_FLT_PLANAR;
        a_send.stride_bytes = frames * sizeof(float);
        a_send.pts_ns = 5000000000LL;
        a_send.channel_mask = avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_32CH);
        for (int c = 0; c < channels; ++c) {
            a_send.planes[c] = tx_planes[c];
        }

        oavx_status_t send_st = oavx_stream_send_audio(tx, &a_send);
        ASSERT_EQ(send_st, OAVX_SUCCESS);

        oavx_audio_frame_t a_recv;
        memset(&a_recv, 0, sizeof(a_recv));
        oavx_frame_type_t ft = oavx_stream_receive(rx, NULL, &a_recv, NULL, 500);
        ASSERT_EQ(ft, OAVX_FRAME_TYPE_AUDIO);

        ASSERT_EQ(a_recv.sample_rate, (uint32_t)sample_rate);
        ASSERT_EQ(a_recv.channels, 32);
        ASSERT_EQ(a_recv.frames, (uint32_t)frames);
        ASSERT_EQ(a_recv.format, OAVX_AUDIO_FMT_FLT_PLANAR);
        ASSERT_EQ(a_recv.channel_mask, avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_32CH));

        /* Verify bit-exact fidelity across all 32 channels */
        for (int c = 0; c < channels; ++c) {
            ASSERT_TRUE(a_recv.planes[c] != NULL);
            const float *rx_plane = (const float*)a_recv.planes[c];
            for (int s = 0; s < frames; ++s) {
                ASSERT_NEAR(rx_plane[s], tx_planes[c][s], 1e-6f);
            }
        }

        oavx_stream_release_audio(rx, &a_recv);
        for (int c = 0; c < channels; ++c) free(tx_planes[c]);

        oavx_stream_close(rx);
        oavx_stream_close(tx);
        printf("       [+] 32-channel RUDP stream verified with bit-exact fidelity\n");
    }

    oavx_context_destroy(ctx);
    printf("  [+] Multi-Channel Broadcast Transport (16ch & 32ch): PASSED\n");
}

int main(void) {
    printf("=== OpenAVX Audio Codec Tests ===\n\n");

    test_adpcm_roundtrip();
    test_opus_multistream();
    test_accumulator_and_flush();
    test_stream_compressed_loopback();
    test_adpcm_fuzz_bounds();
    test_multichannel_transport_16ch_32ch();

    printf("\n[PASS] openavx_audio_codec_test\n");
    return 0;
}
