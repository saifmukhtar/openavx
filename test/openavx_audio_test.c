/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_audio_test.c - Unit and Verification Suite for Audio Engine
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "openavx.h"
#include "avx_audio.h"
#include "avx_simd.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s:%d: Assertion '%s' failed\n", __FILE__, __LINE__, #cond); \
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

/* ========================================================================= */
/* TEST 1: 5.1 DOWNMIXING TO STEREO                                          */
/* ========================================================================= */

static void test_downmix_5_1(void) {
    printf("[*] Testing 5.1 -> Stereo Downmixing...\n");
    int n = 1024;
    float* fl  = (float*)calloc(n, sizeof(float));
    float* fr  = (float*)calloc(n, sizeof(float));
    float* c   = (float*)calloc(n, sizeof(float));
    float* lfe = (float*)calloc(n, sizeof(float));
    float* bl  = (float*)calloc(n, sizeof(float));
    float* br  = (float*)calloc(n, sizeof(float));

    float* out_l = (float*)malloc(n * sizeof(float));
    float* out_r = (float*)malloc(n * sizeof(float));
    const float* const in[6] = { fl, fr, c, lfe, bl, br };

    /* 1A: Center-only signal (should appear symmetrically in L and R with -3dB attenuation) */
    for (int i = 0; i < n; ++i) c[i] = 1.0f;
    avx_audio_downmix_5_1_to_stereo(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 0.70710678f, 1e-4f);
        ASSERT_NEAR(out_r[i], 0.70710678f, 1e-4f);
    }
    memset(c, 0, n * sizeof(float));

    /* 1B: LFE-only signal (should be muted in ITU-R BS.775 stereo downmix) */
    for (int i = 0; i < n; ++i) lfe[i] = 1.0f;
    avx_audio_downmix_5_1_to_stereo(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 0.0f, 1e-6f);
        ASSERT_NEAR(out_r[i], 0.0f, 1e-6f);
    }
    memset(lfe, 0, n * sizeof(float));

    /* 1C: Left surround only (should only appear in Left output) */
    for (int i = 0; i < n; ++i) bl[i] = 0.8f;
    avx_audio_downmix_5_1_to_stereo(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 0.8f * 0.70710678f, 1e-4f);
        ASSERT_NEAR(out_r[i], 0.0f, 1e-6f);
    }
    memset(bl, 0, n * sizeof(float));

    /* 1D: Full-scale coherent signal across all channels with Peak Normalization */
    for (int i = 0; i < n; ++i) {
        fl[i] = 1.0f;
        fr[i] = 1.0f;
        c[i]  = 1.0f;
        bl[i] = 1.0f;
        br[i] = 1.0f;
    }
    avx_audio_downmix_5_1_to_stereo(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_PEAK);
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(out_l[i] <= 1.0001f);
        ASSERT_TRUE(out_r[i] <= 1.0001f);
        ASSERT_NEAR(out_l[i], 1.0f, 1e-4f);
        ASSERT_NEAR(out_r[i], 1.0f, 1e-4f);
    }

    /* 1E: Attenuation (-3dB) Normalization */
    avx_audio_downmix_5_1_to_stereo(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_ATTEN_3DB);
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(out_l[i] <= 1.0f);
        ASSERT_TRUE(out_r[i] <= 1.0f);
        ASSERT_NEAR(out_l[i], 1.0f, 1e-4f); /* Full-scale inputs normalized by sum */
    }

    free(fl); free(fr); free(c); free(lfe); free(bl); free(br);
    free(out_l); free(out_r);
    printf("  [+] 5.1 Downmix: PASSED\n");
}

/* ========================================================================= */
/* TEST 2: 7.1 DOWNMIXING TO STEREO                                          */
/* ========================================================================= */

static void test_downmix_7_1(void) {
    printf("[*] Testing 7.1 -> Stereo Downmixing...\n");
    int n = 512;
    float* ch[8];
    for (int i = 0; i < 8; ++i) ch[i] = (float*)calloc(n, sizeof(float));
    float* out_l = (float*)malloc(n * sizeof(float));
    float* out_r = (float*)malloc(n * sizeof(float));

    /* Side Left active */
    for (int i = 0; i < n; ++i) ch[6][i] = 0.5f; /* SL */
    avx_audio_downmix_7_1_to_stereo((const float* const*)ch, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 0.5f * 0.70710678f, 1e-4f);
        ASSERT_NEAR(out_r[i], 0.0f, 1e-6f);
    }

    /* Side Right active */
    memset(ch[6], 0, n * sizeof(float));
    for (int i = 0; i < n; ++i) ch[7][i] = 0.5f; /* SR */
    avx_audio_downmix_7_1_to_stereo((const float* const*)ch, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 0.0f, 1e-6f);
        ASSERT_NEAR(out_r[i], 0.5f * 0.70710678f, 1e-4f);
    }

    for (int i = 0; i < 8; ++i) free(ch[i]);
    free(out_l); free(out_r);
    printf("  [+] 7.1 Downmix: PASSED\n");
}

/* ========================================================================= */
/* TEST 3: QUAD & STEREO TO MONO                                             */
/* ========================================================================= */

static void test_quad_and_mono(void) {
    printf("[*] Testing Quad & Stereo-to-Mono Downmixing...\n");
    int n = 256;
    float* q[4];
    for (int i = 0; i < 4; ++i) q[i] = (float*)calloc(n, sizeof(float));
    float* out_l = (float*)malloc(n * sizeof(float));
    float* out_r = (float*)malloc(n * sizeof(float));

    for (int i = 0; i < n; ++i) {
        q[0][i] = 0.6f; /* FL */
        q[1][i] = 0.4f; /* FR */
        q[2][i] = 0.2f; /* BL */
        q[3][i] = 0.2f; /* BR */
    }

    avx_audio_downmix_quad_to_stereo((const float* const*)q, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 0.6f + 0.2f * 0.70710678f, 1e-4f);
        ASSERT_NEAR(out_r[i], 0.4f + 0.2f * 0.70710678f, 1e-4f);
    }

    /* Test Stereo to Mono */
    float* mono = (float*)malloc(n * sizeof(float));
    avx_audio_downmix_stereo_to_mono(out_l, out_r, mono, n, AVX_AUDIO_NORMALIZE_ATTEN_3DB);
    for (int i = 0; i < n; ++i) {
        float expected = 0.5f * (out_l[i] + out_r[i]);
        ASSERT_NEAR(mono[i], expected, 1e-4f);
    }

    for (int i = 0; i < 4; ++i) free(q[i]);
    free(out_l); free(out_r); free(mono);
    printf("  [+] Quad & Stereo-to-Mono: PASSED\n");
}

/* ========================================================================= */
/* TEST 4: ARBITRARY MATRIX MIXING                                           */
/* ========================================================================= */

static void test_matrix_mix(void) {
    printf("[*] Testing Arbitrary Matrix Mixing...\n");
    int n = 128;
    float* in_ch[3];
    for (int i = 0; i < 3; ++i) in_ch[i] = (float*)malloc(n * sizeof(float));
    for (int i = 0; i < n; ++i) {
        in_ch[0][i] = 1.0f;
        in_ch[1][i] = 2.0f;
        in_ch[2][i] = 3.0f;
    }

    float* out_ch[2];
    for (int i = 0; i < 2; ++i) out_ch[i] = (float*)malloc(n * sizeof(float));

    /* 2x3 matrix */
    const float matrix[6] = {
        0.5f,  0.25f, 0.1f,   /* Out 0 = 0.5*1 + 0.25*2 + 0.1*3 = 0.5 + 0.5 + 0.3 = 1.3 */
        0.0f,  1.0f,  0.5f    /* Out 1 = 0 + 1.0*2 + 0.5*3 = 2.0 + 1.5 = 3.5 */
    };

    avx_audio_matrix_mix((const float* const*)in_ch, 3, out_ch, 2, matrix, n, AVX_AUDIO_NORMALIZE_PEAK);
    /* Peak is 3.5, normalized to 1.0 */
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_ch[1][i], 1.0f, 1e-4f);
        ASSERT_NEAR(out_ch[0][i], 1.3f / 3.5f, 1e-4f);
    }

    for (int i = 0; i < 3; ++i) free(in_ch[i]);
    for (int i = 0; i < 2; ++i) free(out_ch[i]);
    printf("  [+] Matrix Mix: PASSED\n");
}

/* ========================================================================= */
/* TEST 5: OPENAVX AUDIO FRAME DOWNMIX HELPER                                */
/* ========================================================================= */

static void test_frame_downmix(void) {
    printf("[*] Testing oavx_audio_frame_t Frame Downmix Helper...\n");
    int samples = 480;
    int channels = 6;
    float* data = (float*)calloc((size_t)channels * samples, sizeof(float));
    /* Fill channel 0 (FL) and 1 (FR) */
    for (int i = 0; i < samples; ++i) {
        data[0 * samples + i] = 0.5f;
        data[1 * samples + i] = 0.25f;
    }

    oavx_audio_frame_t src;
    memset(&src, 0, sizeof(src));
    src.sample_rate = 48000;
    src.channels = channels;
    src.frames = samples;
    src.format = OAVX_AUDIO_FMT_FLT_PLANAR;
    src.pts_ns = 12345678LL;
    src.planes[0] = data;
    src.planes[1] = data + samples;
    src.stride_bytes = samples * sizeof(float);
    src.metadata = "test_meta";
    src.metadata_len = strlen("test_meta");

    oavx_audio_frame_t dst;
    memset(&dst, 0, sizeof(dst));
    bool ok = avx_audio_frame_downmix_to_stereo(&src, &dst, AVX_AUDIO_NORMALIZE_NONE);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(dst.channels == 2);
    ASSERT_TRUE(dst.frames == (uint32_t)samples);
    ASSERT_TRUE(dst.sample_rate == 48000);
    ASSERT_TRUE(dst.pts_ns == 12345678LL);

    const float* out_l = (const float*)dst.planes[0];
    const float* out_r = (const float*)dst.planes[1];
    for (int i = 0; i < samples; ++i) {
        ASSERT_NEAR(out_l[i], 0.5f, 1e-5f);
        ASSERT_NEAR(out_r[i], 0.25f, 1e-5f);
    }

    avx_audio_frame_free_data(&dst);
    ASSERT_TRUE(dst.planes[0] == NULL);
    free(data);
    printf("  [+] Frame Downmix Helper: PASSED\n");
}

/* ========================================================================= */
/* TEST 6: STREAMING RESAMPLER EXACT SAMPLE ACCURACY                         */
/* ========================================================================= */

static void test_resampler_exact_count(void) {
    printf("[*] Testing Streaming Resampler Continuity & Exact Sample Counts...\n");

    const int chunk_sizes[] = { 128, 256, 512, 1000 };
    const avx_audio_resample_mode_e modes[] = {
        AVX_AUDIO_RESAMPLE_LINEAR,
        AVX_AUDIO_RESAMPLE_CUBIC,
        AVX_AUDIO_RESAMPLE_SINC
    };
    const char* mode_names[] = { "Linear", "Cubic", "Sinc" };

    /* 6A: 44.1 kHz -> 48 kHz (exact 48000 output samples for 44100 input samples) */
    for (int m = 0; m < 3; ++m) {
        for (int cs = 0; cs < 4; ++cs) {
            int chunk = chunk_sizes[cs];
            int in_rate = 44100;
            int out_rate = 48000;
            int total_in = in_rate;

            avx_resampler_t* r = avx_resampler_create(in_rate, out_rate, 2, modes[m]);
            ASSERT_TRUE(r != NULL);

            float* in_l = (float*)malloc(chunk * sizeof(float));
            float* in_r = (float*)malloc(chunk * sizeof(float));
            float* out_l = (float*)malloc(2048 * sizeof(float));
            float* out_r = (float*)malloc(2048 * sizeof(float));
            const float* in_ch[2] = { in_l, in_r };
            float* out_ch[2] = { out_l, out_r };

            int total_out = 0;
            int processed_in = 0;

            while (processed_in < total_in) {
                int this_chunk = chunk;
                if (processed_in + this_chunk > total_in) {
                    this_chunk = total_in - processed_in;
                }
                for (int i = 0; i < this_chunk; ++i) {
                    float t = (float)(processed_in + i) / (float)in_rate;
                    in_l[i] = sinf(2.0f * (float)M_PI * 440.0f * t);
                    in_r[i] = cosf(2.0f * (float)M_PI * 440.0f * t);
                }

                int n = avx_resampler_process(r, in_ch, this_chunk, out_ch, 2048);
                total_out += n;
                processed_in += this_chunk;
            }

            int flushed = avx_resampler_flush(r, out_ch, 2048);
            total_out += flushed;

            ASSERT_TRUE(total_out == out_rate);

            free(in_l); free(in_r); free(out_l); free(out_r);
            avx_resampler_destroy(r);
        }
        printf("  [+] 44.1k -> 48k Exact 48,000 Samples (%s across all chunk sizes): PASSED\n", mode_names[m]);
    }

    /* 6B: 48 kHz -> 44.1 kHz (exact 44100 output samples for 48000 input samples) */
    for (int m = 0; m < 3; ++m) {
        avx_resampler_t* r = avx_resampler_create(48000, 44100, 1, modes[m]);
        ASSERT_TRUE(r != NULL);

        int chunk = 512;
        int total_in = 48000;
        int processed_in = 0;
        int total_out = 0;

        float* in = (float*)malloc(chunk * sizeof(float));
        float* out = (float*)malloc(2048 * sizeof(float));
        const float* in_ch[1] = { in };
        float* out_ch[1] = { out };

        while (processed_in < total_in) {
            int this_chunk = chunk;
            if (processed_in + this_chunk > total_in) {
                this_chunk = total_in - processed_in;
            }
            for (int i = 0; i < this_chunk; ++i) {
                in[i] = 0.5f;
            }
            int n = avx_resampler_process(r, in_ch, this_chunk, out_ch, 2048);
            total_out += n;
            processed_in += this_chunk;
        }

        int flushed = avx_resampler_flush(r, out_ch, 2048);
        total_out += flushed;

        ASSERT_TRUE(total_out == 44100);

        free(in); free(out);
        avx_resampler_destroy(r);
        printf("  [+] 48k -> 44.1k Exact 44,100 Samples (%s): PASSED\n", mode_names[m]);
    }
}

/* ========================================================================= */
/* TEST 7: RESAMPLER SPECTRAL PURITY & SNR                                   */
/* ========================================================================= */

static void test_resampler_snr(void) {
    printf("[*] Testing Resampler Numerical SNR (Signal-to-Noise Ratio)...\n");

    int in_rate = 44100;
    int out_rate = 48000;
    float f0 = 1000.0f; /* 1 kHz test tone */

    float* in_buf = (float*)malloc(in_rate * sizeof(float));
    for (int i = 0; i < in_rate; ++i) {
        float t = (float)i / (float)in_rate;
        in_buf[i] = sinf(2.0f * (float)M_PI * f0 * t);
    }

    const avx_audio_resample_mode_e modes[] = {
        AVX_AUDIO_RESAMPLE_LINEAR,
        AVX_AUDIO_RESAMPLE_CUBIC,
        AVX_AUDIO_RESAMPLE_SINC
    };
    const float min_snr[] = { 45.0f, 70.0f, 70.0f };
    const char* mode_names[] = { "Linear", "Cubic", "Windowed Sinc" };

    for (int m = 0; m < 3; ++m) {
        float* out_buf = (float*)malloc((out_rate + 256) * sizeof(float));

        int written = avx_audio_resample_simple(in_buf, in_rate, in_rate, out_buf, out_rate + 256, out_rate, modes[m]);
        ASSERT_TRUE(written >= out_rate);

        /* Compute SNR against theoretical 1 kHz sine at out_rate */
        /* Ignore 100 samples at edges to avoid boundary transient */
        double sig_power = 0.0;
        double noise_power = 0.0;
        int count = 0;

        for (int i = 100; i < out_rate - 100; ++i) {
            float t = (float)i / (float)out_rate;
            float ref = sinf(2.0f * (float)M_PI * f0 * t);
            float actual = out_buf[i];
            float err = actual - ref;

            sig_power += (double)ref * ref;
            noise_power += (double)err * err;
            count++;
        }

        ASSERT_TRUE(count > 0);
        double snr_db = 10.0 * log10(sig_power / noise_power);
        printf("  [+] %-15s SNR: %.2f dB (Threshold: >= %.1f dB)\n", mode_names[m], snr_db, min_snr[m]);
        ASSERT_TRUE(snr_db >= min_snr[m]);

        free(out_buf);
    }

    free(in_buf);
}

/* ========================================================================= */
/* TEST 8: FRAME RESAMPLING HELPER                                           */
/* ========================================================================= */

static void test_frame_resample(void) {
    printf("[*] Testing oavx_audio_frame_t Frame Resampling Helper...\n");

    avx_resampler_t* r = avx_resampler_create(44100, 48000, 2, AVX_AUDIO_RESAMPLE_CUBIC);
    ASSERT_TRUE(r != NULL);

    int in_samples = 441;
    float* in_data = (float*)calloc(2 * in_samples, sizeof(float));
    for (int i = 0; i < in_samples; ++i) {
        in_data[i] = 0.3f;
        in_data[in_samples + i] = 0.7f;
    }

    oavx_audio_frame_t src;
    memset(&src, 0, sizeof(src));
    src.sample_rate = 44100;
    src.channels = 2;
    src.frames = in_samples;
    src.format = OAVX_AUDIO_FMT_FLT_PLANAR;
    src.pts_ns = 55555LL;
    src.planes[0] = in_data;
    src.planes[1] = in_data + in_samples;
    src.stride_bytes = in_samples * sizeof(float);
    src.metadata = NULL;

    oavx_audio_frame_t dst;
    memset(&dst, 0, sizeof(dst));
    bool ok = avx_audio_frame_resample(r, &src, &dst);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(dst.sample_rate == 48000);
    ASSERT_TRUE(dst.channels == 2);
    ASSERT_TRUE(dst.frames > 0);
    ASSERT_TRUE(dst.planes[0] != NULL);

    avx_audio_frame_free_data(&dst);
    ASSERT_TRUE(dst.planes[0] == NULL);

    free(in_data);
    avx_resampler_destroy(r);
    printf("  [+] Frame Resampling Helper: PASSED\n");
}

/* ========================================================================= */
/* TEST 9: ADAPTIVE AUDIO JITTER BUFFER & DRIFT SLEWING                     */
/* ========================================================================= */

static void test_audio_jitter_buffer(void) {
    printf("[*] Testing Adaptive Audio Jitter Buffer & Micro-Slewing...\n");

    const int rate = 48000;
    const int chs = 2;
    avx_audio_jitter_config_t cfg = {
        .sample_rate = rate,
        .channels = chs,
        .target_cushion_ms = 15, /* 720 samples */
        .min_cushion_ms = 5,     /* 240 samples */
        .max_cushion_ms = 35     /* 1680 samples */
    };

    avx_audio_jitter_t* jb = avx_audio_jitter_create(&cfg);
    ASSERT_TRUE(jb != NULL);
    ASSERT_TRUE(avx_audio_jitter_get_depth_samples(jb) == 0);
    ASSERT_NEAR(avx_audio_jitter_get_depth_ms(jb), 0.0f, 1e-4f);

    /* 1. Pre-roll verification: pushing less than target cushion outputs pre-roll silence */
    const int chunk1_len = 480;
    float* in_l = (float*)malloc(chunk1_len * sizeof(float));
    float* in_r = (float*)malloc(chunk1_len * sizeof(float));
    for (int i = 0; i < chunk1_len; ++i) {
        in_l[i] = 0.5f;
        in_r[i] = -0.5f;
    }
    const float* in_ch[2] = { in_l, in_r };
    avx_audio_jitter_push(jb, in_ch, chunk1_len);
    ASSERT_TRUE(avx_audio_jitter_get_depth_samples(jb) == 480);

    /* Pull 240 samples during pre-roll -> should deliver pre-roll silence */
    float* out_l = (float*)malloc(480 * sizeof(float));
    float* out_r = (float*)malloc(480 * sizeof(float));
    float* out_ch[2] = { out_l, out_r };

    int pulled = avx_audio_jitter_pull(jb, out_ch, 240);
    ASSERT_TRUE(pulled == 240);
    for (int i = 0; i < 240; ++i) {
        ASSERT_TRUE(out_l[i] == 0.0f);
        ASSERT_TRUE(out_r[i] == 0.0f);
    }
    /* Depth should still be 480 because pre-roll did not consume buffered samples */
    ASSERT_TRUE(avx_audio_jitter_get_depth_samples(jb) == 480);

    /* 2. Push second chunk to cross target cushion (480 + 480 = 960 > 720) */
    avx_audio_jitter_push(jb, in_ch, chunk1_len);
    ASSERT_TRUE(avx_audio_jitter_get_depth_samples(jb) == 960);

    /* Now pull 240 samples -> should deliver actual audio (0.5f / -0.5f) */
    pulled = avx_audio_jitter_pull(jb, out_ch, 240);
    ASSERT_TRUE(pulled == 240);
    ASSERT_NEAR(out_l[10], 0.5f, 1e-3f);
    ASSERT_NEAR(out_r[10], -0.5f, 1e-3f);
    ASSERT_TRUE(avx_audio_jitter_get_depth_samples(jb) < 960);
    printf("  [+] Pre-Roll Cushion & Priming Phase: PASSED\n");

    /* 3. Burst Jitter & Micro-Slewing Clock Steer */
    /* Overfill buffer to 1500 samples (far above target cushion 720) */
    for (int b = 0; b < 2; ++b) {
        avx_audio_jitter_push(jb, in_ch, chunk1_len);
    }
    int initial_depth = avx_audio_jitter_get_depth_samples(jb);
    ASSERT_TRUE(initial_depth > 1200);

    /* Pull multiple 128-sample blocks; micro-slewing at 1.0005 should gently drain depth */
    for (int p = 0; p < 5; ++p) {
        pulled = avx_audio_jitter_pull(jb, out_ch, 128);
        ASSERT_TRUE(pulled == 128);
    }
    int steered_depth = avx_audio_jitter_get_depth_samples(jb);
    ASSERT_TRUE(steered_depth < initial_depth);
    printf("  [+] Micro-Slewing Clock Drift Correction: PASSED\n");

    /* 4. Smooth Cosine Concealment on Underrun */
    /* Drain entire buffer down to starvation */
    while (avx_audio_jitter_get_depth_samples(jb) > 0) {
        avx_audio_jitter_pull(jb, out_ch, 128);
    }
    ASSERT_TRUE(avx_audio_jitter_get_depth_samples(jb) == 0);

    /* Pulling during starvation should not pop: smooth fade out to 0 */
    pulled = avx_audio_jitter_pull(jb, out_ch, 128);
    ASSERT_TRUE(pulled == 128);
    ASSERT_TRUE(out_l[127] == 0.0f);
    printf("  [+] Smooth Cosine Concealment on Underrun: PASSED\n");

    /* Cleanup */
    free(in_l);
    free(in_r);
    free(out_l);
    free(out_r);
    avx_audio_jitter_destroy(jb);
    printf("  [+] Adaptive Jitter Buffer Lifecycle: PASSED\n");
}

/* ========================================================================= */
/* TEST 10: CHANNEL LAYOUTS, MASKS & 32-CHANNEL SEMANTICS                    */
/* ========================================================================= */

static void test_channel_layouts_and_masks(void) {
    printf("[*] Testing Channel Layouts, Semantic Masks & 32-Channel Semantics...\n");

    /* 1. Spatial Layouts: Channel Counts & Standard Masks */
    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_MONO) == 1);
    ASSERT_TRUE(avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_MONO) == OAVX_AUDIO_MASK_MONO);
    ASSERT_TRUE(strcmp(avx_audio_layout_name(AVX_AUDIO_LAYOUT_MONO), "Mono (1.0)") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(0, AVX_AUDIO_LAYOUT_MONO), "Center / Mono") == 0);

    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_STEREO) == 2);
    ASSERT_TRUE(avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_STEREO) == OAVX_AUDIO_MASK_STEREO);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(0, AVX_AUDIO_LAYOUT_STEREO), "Left") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(1, AVX_AUDIO_LAYOUT_STEREO), "Right") == 0);

    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_2_1) == 3);
    ASSERT_TRUE(avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_2_1) == OAVX_AUDIO_MASK_2_1);

    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_QUAD) == 4);
    ASSERT_TRUE(avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_QUAD) == OAVX_AUDIO_MASK_QUAD);

    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_5_1) == 6);
    ASSERT_TRUE(avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_5_1) == OAVX_AUDIO_MASK_5_1);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(2, AVX_AUDIO_LAYOUT_5_1), "Center") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(3, AVX_AUDIO_LAYOUT_5_1), "LFE / Sub") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(4, AVX_AUDIO_LAYOUT_5_1), "Surround Left") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(5, AVX_AUDIO_LAYOUT_5_1), "Surround Right") == 0);

    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_7_1) == 8);
    ASSERT_TRUE(avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_7_1) == OAVX_AUDIO_MASK_7_1);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(6, AVX_AUDIO_LAYOUT_7_1), "Side Left") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(7, AVX_AUDIO_LAYOUT_7_1), "Side Right") == 0);

    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_7_1_4) == 12);
    ASSERT_TRUE(avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_7_1_4) == OAVX_AUDIO_MASK_7_1_4);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(8, AVX_AUDIO_LAYOUT_7_1_4), "Top Front Left") == 0);

    /* 2. Broadcast Discrete Stems: 16-channel & 32-channel */
    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_BROADCAST_16CH) == 16);
    ASSERT_TRUE(strcmp(avx_audio_layout_name(AVX_AUDIO_LAYOUT_BROADCAST_16CH), "Broadcast 16-Channel SDI") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(0, AVX_AUDIO_LAYOUT_BROADCAST_16CH), "Track 1") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(15, AVX_AUDIO_LAYOUT_BROADCAST_16CH), "Track 16") == 0);

    ASSERT_TRUE(avx_audio_layout_channels(AVX_AUDIO_LAYOUT_BROADCAST_32CH) == 32);
    ASSERT_TRUE(strcmp(avx_audio_layout_name(AVX_AUDIO_LAYOUT_BROADCAST_32CH), "Broadcast 32-Channel Pro-Audio") == 0);
    ASSERT_TRUE(strcmp(avx_audio_channel_name(31, AVX_AUDIO_LAYOUT_BROADCAST_32CH), "Track 32") == 0);

    /* 3. Automatic Deduction & Round-Tripping */
    ASSERT_TRUE(avx_audio_layout_from_channels(1) == AVX_AUDIO_LAYOUT_MONO);
    ASSERT_TRUE(avx_audio_layout_from_channels(2) == AVX_AUDIO_LAYOUT_STEREO);
    ASSERT_TRUE(avx_audio_layout_from_channels(6) == AVX_AUDIO_LAYOUT_5_1);
    ASSERT_TRUE(avx_audio_layout_from_channels(8) == AVX_AUDIO_LAYOUT_7_1);
    ASSERT_TRUE(avx_audio_layout_from_channels(12) == AVX_AUDIO_LAYOUT_7_1_4);
    ASSERT_TRUE(avx_audio_layout_from_channels(16) == AVX_AUDIO_LAYOUT_BROADCAST_16CH);
    ASSERT_TRUE(avx_audio_layout_from_channels(32) == AVX_AUDIO_LAYOUT_BROADCAST_32CH);
    ASSERT_TRUE(avx_audio_layout_from_channels(24) == AVX_AUDIO_LAYOUT_DISCRETE);

    ASSERT_TRUE(avx_audio_layout_from_mask(OAVX_AUDIO_MASK_5_1, 6) == AVX_AUDIO_LAYOUT_5_1);
    ASSERT_TRUE(avx_audio_layout_from_mask(OAVX_AUDIO_MASK_7_1, 8) == AVX_AUDIO_LAYOUT_7_1);
    ASSERT_TRUE(avx_audio_layout_from_mask(OAVX_AUDIO_MASK_7_1_4, 12) == AVX_AUDIO_LAYOUT_7_1_4);

    /* 4. Audio Frame 32-Channel Capacity */
    ASSERT_TRUE(OAVX_MAX_AUDIO_CHANNELS == 32);
    oavx_audio_frame_t f32;
    memset(&f32, 0, sizeof(f32));
    f32.channels = 32;
    f32.channel_mask = avx_audio_layout_to_mask(AVX_AUDIO_LAYOUT_BROADCAST_32CH);
    for (int i = 0; i < 32; ++i) {
        f32.planes[i] = (void*)(uintptr_t)(0x1000 + i * 8);
    }
    ASSERT_TRUE(f32.planes[31] == (void*)(uintptr_t)(0x1000 + 31 * 8));

    printf("  [+] Channel Layouts, Semantic Masks & 32-Channel Semantics: PASSED\n");
}

/* ========================================================================= */
/* TEST 11: CHANNEL ROUTING & SHUFFLING (UP TO 32 CHANNELS)                  */
/* ========================================================================= */

static void test_audio_route_channels(void) {
    printf("[*] Testing Audio Channel Routing & Shuffling...\n");
    int n = 256;

    /* 1. 1:1 Identity routing with NULL map */
    {
        float* in[4];
        float* out[4];
        for (int c = 0; c < 4; ++c) {
            in[c] = (float*)malloc(n * sizeof(float));
            out[c] = (float*)malloc(n * sizeof(float));
            for (int i = 0; i < n; ++i) in[c][i] = (float)(c + 1) * 10.0f + (float)i;
        }

        avx_audio_route_channels((const float* const*)in, 4, out, 4, NULL, NULL, n);
        for (int c = 0; c < 4; ++c) {
            for (int i = 0; i < n; ++i) {
                ASSERT_NEAR(out[c][i], in[c][i], 1e-6f);
            }
        }

        for (int c = 0; c < 4; ++c) { free(in[c]); free(out[c]); }
    }

    /* 2. Custom shuffle, track duplication, and muting (-1) with gains */
    {
        float* in[4];
        float* out[4];
        for (int c = 0; c < 4; ++c) {
            in[c] = (float*)malloc(n * sizeof(float));
            out[c] = (float*)malloc(n * sizeof(float));
            for (int i = 0; i < n; ++i) in[c][i] = 1.0f + (float)c;
        }

        /* out[0] = in[3] * 0.5f
         * out[1] = in[0] (gain 1.0f)
         * out[2] = -1 (muted -> 0.0f)
         * out[3] = in[0] * 0.25f (duplicated in[0])
         */
        int8_t map[4] = { 3, 0, -1, 0 };
        float gains[4] = { 0.5f, 1.0f, 1.0f, 0.25f };

        avx_audio_route_channels((const float* const*)in, 4, out, 4, map, gains, n);

        for (int i = 0; i < n; ++i) {
            ASSERT_NEAR(out[0][i], 4.0f * 0.5f, 1e-6f);  /* in[3]=4.0 * 0.5 = 2.0 */
            ASSERT_NEAR(out[1][i], 1.0f * 1.0f, 1e-6f);  /* in[0]=1.0 * 1.0 = 1.0 */
            ASSERT_NEAR(out[2][i], 0.0f, 1e-6f);         /* muted */
            ASSERT_NEAR(out[3][i], 1.0f * 0.25f, 1e-6f); /* in[0]=1.0 * 0.25 = 0.25 */
        }

        for (int c = 0; c < 4; ++c) { free(in[c]); free(out[c]); }
    }

    /* 3. In-place aliasing channel swap */
    {
        float* ch[2];
        ch[0] = (float*)malloc(n * sizeof(float));
        ch[1] = (float*)malloc(n * sizeof(float));
        for (int i = 0; i < n; ++i) {
            ch[0][i] = 111.0f;
            ch[1][i] = 222.0f;
        }

        int8_t swap_map[2] = { 1, 0 };
        avx_audio_route_channels((const float* const*)ch, 2, ch, 2, swap_map, NULL, n);

        for (int i = 0; i < n; ++i) {
            ASSERT_NEAR(ch[0][i], 222.0f, 1e-6f);
            ASSERT_NEAR(ch[1][i], 111.0f, 1e-6f);
        }

        free(ch[0]); free(ch[1]);
    }

    /* 4. 32-Channel Broadcast Shuffler */
    {
        float* in32[32];
        float* out32[32];
        for (int c = 0; c < 32; ++c) {
            in32[c] = (float*)malloc(n * sizeof(float));
            out32[c] = (float*)malloc(n * sizeof(float));
            for (int i = 0; i < n; ++i) in32[c][i] = (float)(c + 1);
        }

        /* Reverse 32 channels */
        int8_t rev_map[32];
        for (int c = 0; c < 32; ++c) rev_map[c] = (int8_t)(31 - c);

        avx_audio_route_channels((const float* const*)in32, 32, out32, 32, rev_map, NULL, n);
        for (int c = 0; c < 32; ++c) {
            float expected = (float)(32 - c);
            for (int i = 0; i < n; ++i) {
                ASSERT_NEAR(out32[c][i], expected, 1e-6f);
            }
        }

        for (int c = 0; c < 32; ++c) { free(in32[c]); free(out32[c]); }
    }

    /* 5. Frame-level routing helper (avx_audio_frame_route_channels) */
    {
        oavx_audio_frame_t src_frame;
        memset(&src_frame, 0, sizeof(src_frame));
        src_frame.channels = 2;
        src_frame.frames = (uint32_t)n;
        src_frame.sample_rate = 48000;
        src_frame.format = OAVX_AUDIO_FMT_FLT_PLANAR;
        float* p0 = (float*)malloc(2 * n * sizeof(float));
        src_frame.planes[0] = p0;
        src_frame.planes[1] = p0 + n;
        for (int i = 0; i < n; ++i) {
            ((float*)src_frame.planes[0])[i] = 10.0f;
            ((float*)src_frame.planes[1])[i] = 20.0f;
        }

        oavx_audio_frame_t dst_frame;
        int8_t map4[4] = { 1, 0, -1, 0 };
        bool ok = avx_audio_frame_route_channels(&src_frame, &dst_frame, map4, NULL, 4);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(dst_frame.channels == 4);
        ASSERT_TRUE(dst_frame.frames == (uint32_t)n);

        for (int i = 0; i < n; ++i) {
            ASSERT_NEAR(((float*)dst_frame.planes[0])[i], 20.0f, 1e-6f);
            ASSERT_NEAR(((float*)dst_frame.planes[1])[i], 10.0f, 1e-6f);
            ASSERT_NEAR(((float*)dst_frame.planes[2])[i], 0.0f, 1e-6f);
            ASSERT_NEAR(((float*)dst_frame.planes[3])[i], 10.0f, 1e-6f);
        }

        avx_audio_frame_free_data(&dst_frame);
        free(p0);
    }

    printf("  [+] Audio Channel Routing & Shuffling: PASSED\n");
}

/* ========================================================================= */
/* TEST 12: 5.1 TO LT/RT MATRIX DOWNMIXING                                   */
/* ========================================================================= */

static void test_downmix_5_1_ltrt(void) {
    printf("[*] Testing 5.1 -> Lt/Rt Matrix Downmixing...\n");
    int n = 1024;
    float* fl  = (float*)calloc(n, sizeof(float));
    float* fr  = (float*)calloc(n, sizeof(float));
    float* c   = (float*)calloc(n, sizeof(float));
    float* lfe = (float*)calloc(n, sizeof(float));
    float* bl  = (float*)calloc(n, sizeof(float));
    float* br  = (float*)calloc(n, sizeof(float));

    float* out_l = (float*)malloc(n * sizeof(float));
    float* out_r = (float*)malloc(n * sizeof(float));
    const float* const in[6] = { fl, fr, c, lfe, bl, br };

    /* 1. Center-only: appears in-phase in both Lt and Rt at 0.7071 */
    for (int i = 0; i < n; ++i) c[i] = 1.0f;
    avx_audio_downmix_5_1_to_ltrt(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 0.70710678f, 1e-4f);
        ASSERT_NEAR(out_r[i], 0.70710678f, 1e-4f);
        /* In matrix decoder, (Lt - Rt) cancels Center completely */
        ASSERT_NEAR(out_l[i] - out_r[i], 0.0f, 1e-5f);
    }
    for (int i = 0; i < n; ++i) c[i] = 0.0f;

    /* 2. Left-only / Right-only */
    for (int i = 0; i < n; ++i) fl[i] = 1.0f;
    avx_audio_downmix_5_1_to_ltrt(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 1.0f, 1e-4f);
        ASSERT_NEAR(out_r[i], 0.0f, 1e-4f);
    }
    for (int i = 0; i < n; ++i) fl[i] = 0.0f;

    for (int i = 0; i < n; ++i) fr[i] = 1.0f;
    avx_audio_downmix_5_1_to_ltrt(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], 0.0f, 1e-4f);
        ASSERT_NEAR(out_r[i], 1.0f, 1e-4f);
    }
    for (int i = 0; i < n; ++i) fr[i] = 0.0f;

    /* 3. Surround Left (BL): Lt = -0.7071, Rt = +0.7071 (180 deg phase steering) */
    for (int i = 0; i < n; ++i) bl[i] = 1.0f;
    avx_audio_downmix_5_1_to_ltrt(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], -0.70710678f, 1e-4f);
        ASSERT_NEAR(out_r[i], +0.70710678f, 1e-4f);
        /* In matrix decoder sum (Lt + Rt), Surround cancels completely */
        ASSERT_NEAR(out_l[i] + out_r[i], 0.0f, 1e-5f);
    }
    for (int i = 0; i < n; ++i) bl[i] = 0.0f;

    /* 4. Surround Right (BR): Lt = -0.7071, Rt = +0.7071 */
    for (int i = 0; i < n; ++i) br[i] = 1.0f;
    avx_audio_downmix_5_1_to_ltrt(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_NONE);
    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out_l[i], -0.70710678f, 1e-4f);
        ASSERT_NEAR(out_r[i], +0.70710678f, 1e-4f);
    }
    for (int i = 0; i < n; ++i) br[i] = 0.0f;

    /* 5. Normalization modes: ATTEN_3DB and PEAK */
    for (int i = 0; i < n; ++i) {
        fl[i] = 0.8f;
        fr[i] = 0.8f;
        c[i]  = 0.8f;
        bl[i] = 0.8f;
        br[i] = 0.8f;
    }
    avx_audio_downmix_5_1_to_ltrt(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_ATTEN_3DB);
    for (int i = 0; i < n; ++i) {
        ASSERT_TRUE(out_l[i] <= 1.0f && out_l[i] >= -1.0f);
        ASSERT_TRUE(out_r[i] <= 1.0f && out_r[i] >= -1.0f);
    }

    avx_audio_downmix_5_1_to_ltrt(in, out_l, out_r, n, AVX_AUDIO_NORMALIZE_PEAK);
    float max_p = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (fabsf(out_l[i]) > max_p) max_p = fabsf(out_l[i]);
        if (fabsf(out_r[i]) > max_p) max_p = fabsf(out_r[i]);
    }
    ASSERT_NEAR(max_p, 1.0f, 1e-4f);

    free(fl); free(fr); free(c); free(lfe); free(bl); free(br);
    free(out_l); free(out_r);
    printf("  [+] 5.1 -> Lt/Rt Matrix Downmixing: PASSED\n");
}

/* ========================================================================= */
/* TEST 13: STEREO TO 5.1 SURROUND UPMIXER                                   */
/* ========================================================================= */

static void test_upmix_stereo_to_5_1(void) {
    printf("[*] Testing Stereo -> 5.1 Surround Upmixer...\n");
    int n = 1000;
    int sample_rate = 48000;
    float* in_l = (float*)calloc(n, sizeof(float));
    float* in_r = (float*)calloc(n, sizeof(float));

    float* out[6];
    for (int c = 0; c < 6; ++c) out[c] = (float*)malloc(n * sizeof(float));

    /* 1. Pure Mono Dialogue: L = 1.0, R = 1.0
     * Should extract Center C = 0.7071
     * Surrounds BL, BR must be 0.0 (no bleed of dialogue into rear speakers)
     * Fronts FL and FR are attenuated by center extraction (1.0 - 0.5 * 0.7071 ~= 0.64645)
     */
    for (int i = 0; i < n; ++i) { in_l[i] = 1.0f; in_r[i] = 1.0f; }
    avx_audio_upmix_cfg_t cfg;
    avx_audio_upmix_cfg_default(&cfg);
    cfg.ambient_delay_ms = 0; /* test direct without delay first */

    avx_audio_upmix_stereo_to_5_1(in_l, in_r, out, n, sample_rate, &cfg);

    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out[2][i], 0.70710678f, 1e-4f);          /* Center */
        ASSERT_NEAR(out[4][i], 0.0f, 1e-5f);                /* Back Left = 0 */
        ASSERT_NEAR(out[5][i], 0.0f, 1e-5f);                /* Back Right = 0 */
        ASSERT_NEAR(out[0][i], 1.0f - 0.5f * 0.70710678f, 1e-4f); /* FL */
        ASSERT_NEAR(out[1][i], 1.0f - 0.5f * 0.70710678f, 1e-4f); /* FR */
    }

    /* 2. Pure Out-of-Phase Ambience: L = 1.0, R = -1.0
     * Center C must be 0.0 (cancels completely)
     * Surrounds BL and BR receive maximum energy with opposite phase
     */
    for (int i = 0; i < n; ++i) { in_l[i] = 1.0f; in_r[i] = -1.0f; }
    avx_audio_upmix_stereo_to_5_1(in_l, in_r, out, n, sample_rate, &cfg);

    for (int i = 0; i < n; ++i) {
        ASSERT_NEAR(out[2][i], 0.0f, 1e-5f);                /* Center = 0 */
        ASSERT_NEAR(out[4][i], 0.70710678f, 1e-4f);         /* BL */
        ASSERT_NEAR(out[5][i], -0.70710678f, 1e-4f);        /* BR */
        ASSERT_NEAR(out[0][i], 1.0f, 1e-4f);                /* FL */
        ASSERT_NEAR(out[1][i], -1.0f, 1e-4f);               /* FR */
    }

    /* 3. Streaming Stateful Upmixer & Ambient Delay Ring Buffer
     * Test 15ms delay at 48kHz (720 samples).
     * Feed an impulse at sample 0 of chunk 1, verify it emerges in chunk 2 at sample 720.
     */
    {
        avx_audio_upmix_cfg_default(&cfg);
        cfg.ambient_delay_ms = 15; /* 15 ms * 48 = 720 samples */
        avx_audio_upmixer_t* u = avx_audio_upmixer_create(sample_rate, &cfg);
        ASSERT_TRUE(u != NULL);

        int chunk_size = 480; /* 10 ms chunks */
        float* chk_in_l = (float*)calloc(chunk_size, sizeof(float));
        float* chk_in_r = (float*)calloc(chunk_size, sizeof(float));
        float* chk_out[6];
        for (int c = 0; c < 6; ++c) chk_out[c] = (float*)calloc(chunk_size, sizeof(float));

        /* Put out-of-phase impulse at index 0 of chunk 0: L=1.0, R=-1.0 */
        chk_in_l[0] = 1.0f;
        chk_in_r[0] = -1.0f;

        /* Chunk 0 (samples 0..479): impulse is at t=0, so delayed surround at t=0 should be 0 */
        avx_audio_upmixer_process(u, chk_in_l, chk_in_r, chk_out, chunk_size);
        ASSERT_NEAR(chk_out[4][0], 0.0f, 1e-5f);

        /* Chunk 1 (samples 480..959): sample 720 corresponds to local index (720 - 480) = 240 */
        memset(chk_in_l, 0, chunk_size * sizeof(float));
        memset(chk_in_r, 0, chunk_size * sizeof(float));
        avx_audio_upmixer_process(u, chk_in_l, chk_in_r, chk_out, chunk_size);

        /* At index 240, delayed impulse must emerge */
        ASSERT_NEAR(chk_out[4][240], 0.70710678f, 1e-4f);
        ASSERT_NEAR(chk_out[5][240], -0.70710678f, 1e-4f);
        /* And neighbouring samples must be 0 */
        ASSERT_NEAR(chk_out[4][239], 0.0f, 1e-5f);
        ASSERT_NEAR(chk_out[4][241], 0.0f, 1e-5f);

        avx_audio_upmixer_destroy(u);
        for (int c = 0; c < 6; ++c) free(chk_out[c]);
        free(chk_in_l); free(chk_in_r);
    }

    /* 4. LFE Butterworth Low-Pass Filter Frequency Response:
     * - 50 Hz sine wave (well below 120 Hz cutoff) -> should pass with high amplitude (~0.5f * lfe_level)
     * - 5000 Hz sine wave (well above 120 Hz cutoff) -> should be heavily attenuated (< 0.01)
     */
    {
        int num_filt_samples = 4800; /* 100 ms */
        float* low_l = (float*)malloc(num_filt_samples * sizeof(float));
        float* low_r = (float*)malloc(num_filt_samples * sizeof(float));
        float* filt_out[6];
        for (int c = 0; c < 6; ++c) filt_out[c] = (float*)malloc(num_filt_samples * sizeof(float));

        /* 50 Hz sine wave */
        for (int i = 0; i < num_filt_samples; ++i) {
            float s = sinf(2.0f * (float)M_PI * 50.0f * (float)i / 48000.0f);
            low_l[i] = s;
            low_r[i] = s;
        }

        avx_audio_upmix_cfg_default(&cfg);
        cfg.ambient_delay_ms = 0;
        avx_audio_upmix_stereo_to_5_1(low_l, low_r, filt_out, num_filt_samples, 48000, &cfg);

        /* Measure steady state peak of LFE (after filter settling at 2400 samples) */
        float peak_50hz = 0.0f;
        for (int i = 2400; i < num_filt_samples; ++i) {
            if (fabsf(filt_out[3][i]) > peak_50hz) peak_50hz = fabsf(filt_out[3][i]);
        }
        /* With cfg.lfe_level = 0.5f, 50 Hz should be passed near 0.5f (at least 0.45f) */
        ASSERT_TRUE(peak_50hz > 0.45f);

        /* 5000 Hz sine wave */
        for (int i = 0; i < num_filt_samples; ++i) {
            float s = sinf(2.0f * (float)M_PI * 5000.0f * (float)i / 48000.0f);
            low_l[i] = s;
            low_r[i] = s;
        }
        avx_audio_upmix_stereo_to_5_1(low_l, low_r, filt_out, num_filt_samples, 48000, &cfg);

        float peak_5khz = 0.0f;
        for (int i = 2400; i < num_filt_samples; ++i) {
            if (fabsf(filt_out[3][i]) > peak_5khz) peak_5khz = fabsf(filt_out[3][i]);
        }
        /* 5000 Hz is over 5 octaves above 120 Hz -> > 40 dB attenuation */
        ASSERT_TRUE(peak_5khz < 0.01f);

        for (int c = 0; c < 6; ++c) free(filt_out[c]);
        free(low_l); free(low_r);
    }

    for (int c = 0; c < 6; ++c) free(out[c]);
    free(in_l); free(in_r);

    printf("  [+] Stereo -> 5.1 Surround Upmixer: PASSED\n");
}

static void test_interleaved_audio_send_formats(void) {
    printf("[14] Testing Interleaved Audio Send Formats (FLT, S16, S32)...\n");

    oavx_context_t *ctx = oavx_context_create();
    ASSERT_TRUE(ctx != NULL);

    const uint32_t frames = 960;
    const uint16_t channels = 2;

    float flt_in[960 * 2];
    int16_t s16_in[960 * 2];
    int32_t s32_in[960 * 2];
    for (uint32_t i = 0; i < frames; ++i) {
        float l = 0.5f * sinf(2.0f * (float)M_PI * 440.0f * (float)i / 48000.0f);
        float r = 0.5f * sinf(2.0f * (float)M_PI * 1000.0f * (float)i / 48000.0f);
        flt_in[2 * i + 0] = l;
        flt_in[2 * i + 1] = r;
        s16_in[2 * i + 0] = (int16_t)(l * 32768.0f);
        s16_in[2 * i + 1] = (int16_t)(r * 32768.0f);
        s32_in[2 * i + 0] = (int32_t)((double)l * 2147483648.0);
        s32_in[2 * i + 1] = (int32_t)((double)r * 2147483648.0);
    }

    const oavx_audio_format_t fmts[3] = {
        OAVX_AUDIO_FMT_FLT_INTERLEAVED,
        OAVX_AUDIO_FMT_S16_INTERLEAVED,
        OAVX_AUDIO_FMT_S32_INTERLEAVED
    };
    void *bufs[3] = { flt_in, s16_in, s32_in };
    const size_t strides[3] = { sizeof(flt_in), sizeof(s16_in), sizeof(s32_in) };

    for (int mode_idx = 0; mode_idx < 2; ++mode_idx) {
        oavx_audio_compression_mode_e comp = (mode_idx == 0)
            ? OAVX_AUDIO_COMPRESSION_OFF
            : OAVX_AUDIO_COMPRESSION_MANUAL;

        for (int f = 0; f < 3; ++f) {
            oavx_stream_config_t tx_cfg = {
                .name = "InterleavedAudioTest",
                .direction = OAVX_DIR_TRANSMIT,
                .clock_sync = false
            };
            oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
            ASSERT_TRUE(tx != NULL);
            if (comp != OAVX_AUDIO_COMPRESSION_OFF) {
                oavx_stream_set_audio_compression(tx, comp, 128);
            }

            const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
            ASSERT_TRUE(ep && ep->address);

            oavx_stream_config_t rx_cfg = {
                .name = "InterleavedAudioRecv",
                .endpoint = ep->address,
                .direction = OAVX_DIR_RECEIVE,
                .bandwidth = OAVX_BANDWIDTH_HIGHEST,
                .clock_sync = false
            };
            oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
            ASSERT_TRUE(rx != NULL);
            oavx_stream_get_connection_count(tx, 500);

            int got = 0;
            for (int k = 0; k < 5 && !got; ++k) {
                oavx_audio_frame_t aout = {
                    .sample_rate = 48000,
                    .channels = channels,
                    .frames = frames,
                    .format = fmts[f],
                    .planes = { bufs[f] },
                    .stride_bytes = strides[f],
                    .pts_ns = (int64_t)k * 20000000LL
                };
                oavx_stream_send_audio(tx, &aout);

                oavx_audio_frame_t ain;
                memset(&ain, 0, sizeof(ain));
                if (oavx_stream_receive(rx, NULL, &ain, NULL, 100) == OAVX_FRAME_TYPE_AUDIO && ain.frames == frames) {
                    const float *l_rx = (const float*)ain.planes[0];
                    const float *r_rx = (const float*)ain.planes[1];
                    ASSERT_TRUE(l_rx && r_rx);

                    if (comp == OAVX_AUDIO_COMPRESSION_OFF) {
                        for (uint32_t i = 0; i < frames; ++i) {
                            ASSERT_TRUE(fabsf(l_rx[i] - flt_in[2 * i + 0]) < 1e-4f);
                            ASSERT_TRUE(fabsf(r_rx[i] - flt_in[2 * i + 1]) < 1e-4f);
                        }
                    } else {
                        float rms_l = 0.0f, rms_r = 0.0f;
                        for (uint32_t i = 0; i < frames; ++i) {
                            rms_l += l_rx[i] * l_rx[i];
                            rms_r += r_rx[i] * r_rx[i];
                        }
                        ASSERT_TRUE(sqrtf(rms_l / frames) > 0.1f);
                        ASSERT_TRUE(sqrtf(rms_r / frames) > 0.1f);
                    }
                    oavx_stream_release_audio(rx, &ain);
                    got = 1;
                }
            }
            ASSERT_TRUE(got == 1);
            oavx_stream_close(rx);
            oavx_stream_close(tx);
        }
    }

    oavx_context_destroy(ctx);
    printf("  [+] Interleaved Audio Send Formats (FLT, S16, S32): PASSED\n");
}

int main(void) {
    printf("=== OpenAVX Audio Engine Tests ===\n");
    printf("SIMD: %s\n\n", avx_simd_get_name());

    test_downmix_5_1();
    test_downmix_7_1();
    test_quad_and_mono();
    test_matrix_mix();
    test_frame_downmix();
    test_resampler_exact_count();
    test_resampler_snr();
    test_frame_resample();
    test_audio_jitter_buffer();
    test_channel_layouts_and_masks();
    test_audio_route_channels();
    test_downmix_5_1_ltrt();
    test_upmix_stereo_to_5_1();
    test_interleaved_audio_send_formats();

    printf("\n[PASS] openavx_audio_test\n");
    return 0;
}
