/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_bench.c - Micro-benchmarking & Bitwise Verification Suite for openavx SIMD Vectorization
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "avx_simd.h"
#include "avx_color.h"
#include "avx_audio.h"
#include "avx_fec.h"
#include "avx_crypto.h"

static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void fill_random_bytes(uint8_t *buf, size_t size, uint32_t seed) {
    uint32_t s = seed ? seed : 123456789;
    for (size_t i = 0; i < size; ++i) {
        s = s * 1664525 + 1013904223;
        buf[i] = (uint8_t)(s >> 24);
    }
}

static void benchmark_color_conversions(int width, int height, const char *label) {
    size_t num_pixels = (size_t)width * height;
    size_t uyvy_bytes = num_pixels * 2;
    size_t rgba_bytes = num_pixels * 4;

    printf("\n----------------------------------------------------------------------\n");
    printf("  Benchmark: Color Conversions @ %s (%dx%d, %.2f MP)\n", label, width, height, (double)num_pixels / 1e6);
    printf("----------------------------------------------------------------------\n");

    uint8_t *src_uyvy = (uint8_t*)malloc(uyvy_bytes);
    uint8_t *dst_rgba_s = (uint8_t*)malloc(rgba_bytes);
    uint8_t *dst_rgba_v = (uint8_t*)malloc(rgba_bytes);

    uint8_t *src_rgba = (uint8_t*)malloc(rgba_bytes);
    uint8_t *src_bg   = (uint8_t*)malloc(rgba_bytes);
    uint8_t *dst_uyvy_s = (uint8_t*)malloc(uyvy_bytes);
    uint8_t *dst_uyvy_v = (uint8_t*)malloc(uyvy_bytes);

    assert(src_uyvy && dst_rgba_s && dst_rgba_v && src_rgba && src_bg && dst_uyvy_s && dst_uyvy_v);

    fill_random_bytes(src_uyvy, uyvy_bytes, 42);
    fill_random_bytes(src_rgba, rgba_bytes, 84);
    fill_random_bytes(src_bg,   rgba_bytes, 126);

    const int iterations = 100;

    /* 1. UYVY -> RGBA */
    {
        avx_scalar_uyvy_to_rgba(src_uyvy, dst_rgba_s, width, height);
        avx_simd_uyvy_to_rgba(src_uyvy, dst_rgba_v, width, height);
        assert(memcmp(dst_rgba_s, dst_rgba_v, rgba_bytes) == 0);

        double t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_scalar_uyvy_to_rgba(src_uyvy, dst_rgba_s, width, height);
        }
        double t_scalar = get_time_sec() - t0;

        t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_simd_uyvy_to_rgba(src_uyvy, dst_rgba_v, width, height);
        }
        double t_simd = get_time_sec() - t0;

        double fps_s = (double)iterations / t_scalar;
        double fps_v = (double)iterations / t_simd;
        double gb_s = (fps_s * (double)(uyvy_bytes + rgba_bytes)) / (1024.0 * 1024.0 * 1024.0);
        double gb_v = (fps_v * (double)(uyvy_bytes + rgba_bytes)) / (1024.0 * 1024.0 * 1024.0);
        double speedup = t_scalar / t_simd;

        printf("  [UYVY -> RGBA] (Bitwise Match: 100%%)\n");
        printf("    Scalar: %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame]\n", fps_s, gb_s, (t_scalar / iterations) * 1000.0);
        printf("    SIMD:   %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame] -> \033[1;32m%.2fx FASTER\033[0m\n", fps_v, gb_v, (t_simd / iterations) * 1000.0, speedup);
    }

    /* 2. UYVY -> BGRA */
    {
        avx_scalar_uyvy_to_bgra(src_uyvy, dst_rgba_s, width, height);
        avx_simd_uyvy_to_bgra(src_uyvy, dst_rgba_v, width, height);
        assert(memcmp(dst_rgba_s, dst_rgba_v, rgba_bytes) == 0);

        double t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_scalar_uyvy_to_bgra(src_uyvy, dst_rgba_s, width, height);
        }
        double t_scalar = get_time_sec() - t0;

        t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_simd_uyvy_to_bgra(src_uyvy, dst_rgba_v, width, height);
        }
        double t_simd = get_time_sec() - t0;

        double fps_s = (double)iterations / t_scalar;
        double fps_v = (double)iterations / t_simd;
        double gb_s = (fps_s * (double)(uyvy_bytes + rgba_bytes)) / (1024.0 * 1024.0 * 1024.0);
        double gb_v = (fps_v * (double)(uyvy_bytes + rgba_bytes)) / (1024.0 * 1024.0 * 1024.0);
        double speedup = t_scalar / t_simd;

        printf("  [UYVY -> BGRA] (Bitwise Match: 100%%)\n");
        printf("    Scalar: %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame]\n", fps_s, gb_s, (t_scalar / iterations) * 1000.0);
        printf("    SIMD:   %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame] -> \033[1;32m%.2fx FASTER\033[0m\n", fps_v, gb_v, (t_simd / iterations) * 1000.0, speedup);
    }

    /* 3. RGBA -> UYVY */
    {
        avx_scalar_rgba_to_uyvy(src_rgba, dst_uyvy_s, width, height);
        avx_simd_rgba_to_uyvy(src_rgba, dst_uyvy_v, width, height);
        assert(memcmp(dst_uyvy_s, dst_uyvy_v, uyvy_bytes) == 0);

        double t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_scalar_rgba_to_uyvy(src_rgba, dst_uyvy_s, width, height);
        }
        double t_scalar = get_time_sec() - t0;

        t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_simd_rgba_to_uyvy(src_rgba, dst_uyvy_v, width, height);
        }
        double t_simd = get_time_sec() - t0;

        double fps_s = (double)iterations / t_scalar;
        double fps_v = (double)iterations / t_simd;
        double gb_s = (fps_s * (double)(rgba_bytes + uyvy_bytes)) / (1024.0 * 1024.0 * 1024.0);
        double gb_v = (fps_v * (double)(rgba_bytes + uyvy_bytes)) / (1024.0 * 1024.0 * 1024.0);
        double speedup = t_scalar / t_simd;

        printf("  [RGBA -> UYVY] (Bitwise Match: 100%%)\n");
        printf("    Scalar: %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame]\n", fps_s, gb_s, (t_scalar / iterations) * 1000.0);
        printf("    SIMD:   %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame] -> \033[1;32m%.2fx FASTER\033[0m\n", fps_v, gb_v, (t_simd / iterations) * 1000.0, speedup);
    }

    /* 4. BGRA -> UYVY */
    {
        avx_scalar_bgra_to_uyvy(src_bg, dst_uyvy_s, width, height);
        avx_simd_bgra_to_uyvy(src_bg, dst_uyvy_v, width, height);
        assert(memcmp(dst_uyvy_s, dst_uyvy_v, uyvy_bytes) == 0);

        double t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_scalar_bgra_to_uyvy(src_bg, dst_uyvy_s, width, height);
        }
        double t_scalar = get_time_sec() - t0;

        t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_simd_bgra_to_uyvy(src_bg, dst_uyvy_v, width, height);
        }
        double t_simd = get_time_sec() - t0;

        double fps_s = (double)iterations / t_scalar;
        double fps_v = (double)iterations / t_simd;
        double gb_s = (fps_s * (double)(rgba_bytes + uyvy_bytes)) / (1024.0 * 1024.0 * 1024.0);
        double gb_v = (fps_v * (double)(rgba_bytes + uyvy_bytes)) / (1024.0 * 1024.0 * 1024.0);
        double speedup = t_scalar / t_simd;

        printf("  [BGRA -> UYVY] (Bitwise Match: 100%%)\n");
        printf("    Scalar: %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame]\n", fps_s, gb_s, (t_scalar / iterations) * 1000.0);
        printf("    SIMD:   %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame] -> \033[1;32m%.2fx FASTER\033[0m\n", fps_v, gb_v, (t_simd / iterations) * 1000.0, speedup);
    }

    /* 5. RGBA <-> BGRA Channel Swap */
    {
        avx_scalar_rgba_to_bgra(src_rgba, dst_rgba_s, width, height);
        avx_simd_rgba_to_bgra(src_rgba, dst_rgba_v, width, height);
        assert(memcmp(dst_rgba_s, dst_rgba_v, rgba_bytes) == 0);

        double t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_scalar_rgba_to_bgra(src_rgba, dst_rgba_s, width, height);
        }
        double t_scalar = get_time_sec() - t0;

        t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_simd_rgba_to_bgra(src_rgba, dst_rgba_v, width, height);
        }
        double t_simd = get_time_sec() - t0;

        double fps_s = (double)iterations / t_scalar;
        double fps_v = (double)iterations / t_simd;
        double gb_s = (fps_s * (double)(rgba_bytes * 2)) / (1024.0 * 1024.0 * 1024.0);
        double gb_v = (fps_v * (double)(rgba_bytes * 2)) / (1024.0 * 1024.0 * 1024.0);
        double speedup = t_scalar / t_simd;

        printf("  [RGBA <-> BGRA Swap] (Bitwise Match: 100%%)\n");
        printf("    Scalar: %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame]\n", fps_s, gb_s, (t_scalar / iterations) * 1000.0);
        printf("    SIMD:   %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame] -> \033[1;32m%.2fx FASTER\033[0m\n", fps_v, gb_v, (t_simd / iterations) * 1000.0, speedup);
    }

    /* 6. Alpha Blend RGBA */
    {
        avx_scalar_alpha_blend_rgba(src_rgba, src_bg, dst_rgba_s, width, height);
        avx_simd_alpha_blend_rgba(src_rgba, src_bg, dst_rgba_v, width, height);
        assert(memcmp(dst_rgba_s, dst_rgba_v, rgba_bytes) == 0);

        double t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_scalar_alpha_blend_rgba(src_rgba, src_bg, dst_rgba_s, width, height);
        }
        double t_scalar = get_time_sec() - t0;

        t0 = get_time_sec();
        for (int i = 0; i < iterations; ++i) {
            avx_simd_alpha_blend_rgba(src_rgba, src_bg, dst_rgba_v, width, height);
        }
        double t_simd = get_time_sec() - t0;

        double fps_s = (double)iterations / t_scalar;
        double fps_v = (double)iterations / t_simd;
        double gb_s = (fps_s * (double)(rgba_bytes * 3)) / (1024.0 * 1024.0 * 1024.0);
        double gb_v = (fps_v * (double)(rgba_bytes * 3)) / (1024.0 * 1024.0 * 1024.0);
        double speedup = t_scalar / t_simd;

        printf("  [RGBA Alpha Blend] (Bitwise Match: 100%%)\n");
        printf("    Scalar: %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame]\n", fps_s, gb_s, (t_scalar / iterations) * 1000.0);
        printf("    SIMD:   %8.1f FPS  (%6.2f GB/s)  [%6.2f ms/frame] -> \033[1;32m%.2fx FASTER\033[0m\n", fps_v, gb_v, (t_simd / iterations) * 1000.0, speedup);
    }

    free(src_uyvy);
    free(dst_rgba_s);
    free(dst_rgba_v);
    free(src_rgba);
    free(src_bg);
    free(dst_uyvy_s);
    free(dst_uyvy_v);
}

static void benchmark_speedhq_dct(void) {
    printf("\n----------------------------------------------------------------------\n");
    printf("  Benchmark: SpeedHQ-2 8x8 DCT Transforms (65,280 Blocks = 1 1080p Frame)\n");
    printf("----------------------------------------------------------------------\n");

    const int num_blocks = 65280;
    int16_t *blocks_in = (int16_t*)malloc(num_blocks * 64 * sizeof(int16_t));
    int16_t *blocks_out_s = (int16_t*)malloc(num_blocks * 64 * sizeof(int16_t));
    int16_t *blocks_out_v = (int16_t*)malloc(num_blocks * 64 * sizeof(int16_t));

    uint8_t *idct_out_s = (uint8_t*)malloc(num_blocks * 64);
    uint8_t *idct_out_v = (uint8_t*)malloc(num_blocks * 64);

    assert(blocks_in && blocks_out_s && blocks_out_v && idct_out_s && idct_out_v);

    fill_random_bytes((uint8_t*)blocks_in, num_blocks * 64 * sizeof(int16_t), 999);

    /* 1. Forward DCT 8x8 */
    {
        for (int b = 0; b < num_blocks; ++b) {
            avx_scalar_fdct_8x8(&blocks_in[b * 64], &blocks_out_s[b * 64]);
            avx_simd_fdct_8x8(&blocks_in[b * 64], &blocks_out_v[b * 64]);
        }
        assert(memcmp(blocks_out_s, blocks_out_v, num_blocks * 64 * sizeof(int16_t)) == 0);

        const int iters = 5;
        double t0 = get_time_sec();
        for (int i = 0; i < iters; ++i) {
            for (int b = 0; b < num_blocks; ++b) {
                avx_scalar_fdct_8x8(&blocks_in[b * 64], &blocks_out_s[b * 64]);
            }
        }
        double t_scalar = get_time_sec() - t0;

        t0 = get_time_sec();
        for (int i = 0; i < iters; ++i) {
            for (int b = 0; b < num_blocks; ++b) {
                avx_simd_fdct_8x8(&blocks_in[b * 64], &blocks_out_v[b * 64]);
            }
        }
        double t_simd = get_time_sec() - t0;

        double mblocks_s = (double)(num_blocks * iters) / (t_scalar * 1e6);
        double mblocks_v = (double)(num_blocks * iters) / (t_simd * 1e6);
        double ns_per_block_s = (t_scalar / (num_blocks * iters)) * 1e9;
        double ns_per_block_v = (t_simd / (num_blocks * iters)) * 1e9;
        double speedup = t_scalar / t_simd;

        printf("  [SpeedHQ-2 8x8 Forward DCT] (Bitwise Match: 100%%)\n");
        printf("    Scalar: %6.2f Mblocks/sec  [%5.1f ns/block]  [%5.2f ms/1080p frame]\n", mblocks_s, ns_per_block_s, (t_scalar / iters) * 1000.0);
        printf("    SIMD:   %6.2f Mblocks/sec  [%5.1f ns/block]  [%5.2f ms/1080p frame] -> \033[1;32m%.2fx FASTER\033[0m\n", mblocks_v, ns_per_block_v, (t_simd / iters) * 1000.0, speedup);
    }

    /* 2. Inverse DCT 8x8 Put */
    {
        for (int b = 0; b < num_blocks; ++b) {
            avx_scalar_idct_8x8_put(&blocks_out_s[b * 64], &idct_out_s[b * 64], 8);
            avx_simd_idct_8x8_put(&blocks_out_s[b * 64], &idct_out_v[b * 64], 8);
        }
        assert(memcmp(idct_out_s, idct_out_v, num_blocks * 64) == 0);

        const int iters = 5;
        double t0 = get_time_sec();
        for (int i = 0; i < iters; ++i) {
            for (int b = 0; b < num_blocks; ++b) {
                avx_scalar_idct_8x8_put(&blocks_out_s[b * 64], &idct_out_s[b * 64], 8);
            }
        }
        double t_scalar = get_time_sec() - t0;

        t0 = get_time_sec();
        for (int i = 0; i < iters; ++i) {
            for (int b = 0; b < num_blocks; ++b) {
                avx_simd_idct_8x8_put(&blocks_out_s[b * 64], &idct_out_v[b * 64], 8);
            }
        }
        double t_simd = get_time_sec() - t0;

        double mblocks_s = (double)(num_blocks * iters) / (t_scalar * 1e6);
        double mblocks_v = (double)(num_blocks * iters) / (t_simd * 1e6);
        double ns_per_block_s = (t_scalar / (num_blocks * iters)) * 1e9;
        double ns_per_block_v = (t_simd / (num_blocks * iters)) * 1e9;
        double speedup = t_scalar / t_simd;

        printf("  [SpeedHQ-2 8x8 Inverse DCT] (Bitwise Match: 100%%)\n");
        printf("    Scalar: %6.2f Mblocks/sec  [%5.1f ns/block]  [%5.2f ms/1080p frame]\n", mblocks_s, ns_per_block_s, (t_scalar / iters) * 1000.0);
        printf("    SIMD:   %6.2f Mblocks/sec  [%5.1f ns/block]  [%5.2f ms/1080p frame] -> \033[1;32m%.2fx FASTER\033[0m\n", mblocks_v, ns_per_block_v, (t_simd / iters) * 1000.0, speedup);
    }

    free(blocks_in);
    free(blocks_out_s);
    free(blocks_out_v);
    free(idct_out_s);
    free(idct_out_v);
}

static void benchmark_audio_dsp(void) {
    printf("\n----------------------------------------------------------------------\n");
    printf("  Benchmark: Audio DSP (Resampling & Multi-Channel Downmix)\n");
    printf("----------------------------------------------------------------------\n");

    /* 1. Resampling 48 kHz -> 44.1 kHz (48,000 samples = 1.0 second of audio) */
    const int in_rate = 48000;
    const int out_rate = 44100;
    const int in_samples = 48000;
    const int max_out = 48000;
    float *audio_in = (float*)malloc(in_samples * sizeof(float));
    float *audio_out = (float*)malloc(max_out * sizeof(float));
    assert(audio_in && audio_out);

    for (int i = 0; i < in_samples; ++i) {
        audio_in[i] = (float)((i % 100) - 50) / 50.0f;
    }

    struct {
        const char *name;
        avx_audio_resample_mode_e mode;
    } resample_modes[] = {
        {"Linear Interpolation", AVX_AUDIO_RESAMPLE_LINEAR},
        {"Cubic Catmull-Rom   ", AVX_AUDIO_RESAMPLE_CUBIC},
        {"Blackman Sinc Filter", AVX_AUDIO_RESAMPLE_SINC}
    };

    const int resample_iters = 50;
    for (size_t m = 0; m < sizeof(resample_modes)/sizeof(resample_modes[0]); ++m) {
        double t0 = get_time_sec();
        int written = 0;
        for (int it = 0; it < resample_iters; ++it) {
            written = avx_audio_resample_simple(audio_in, in_samples, in_rate, audio_out, max_out, out_rate, resample_modes[m].mode);
        }
        double elapsed = get_time_sec() - t0;
        assert(written > 0);

        double total_samples = (double)in_samples * resample_iters;
        double msamples_sec = total_samples / (elapsed * 1e6);
        double ms_per_sec = (elapsed / resample_iters) * 1000.0;
        double realtime_x = 1000.0 / ms_per_sec;

        printf("  [Resample 48k->44.1k: %s]\n", resample_modes[m].name);
        printf("    Throughput: %6.2f MSamples/s  [%5.2f ms / 1s audio] -> \033[1;32m%6.1fx REAL-TIME\033[0m\n",
               msamples_sec, ms_per_sec, realtime_x);
    }

    free(audio_in);
    free(audio_out);

    /* 2. Downmix 48 kHz audio buffers (48,000 frames = 1.0 second per channel) */
    const int frames = 48000;
    float *ch[8];
    for (int c = 0; c < 8; ++c) {
        ch[c] = (float*)malloc(frames * sizeof(float));
        assert(ch[c]);
        for (int i = 0; i < frames; ++i) {
            ch[c][i] = (float)(((i + c * 13) % 200) - 100) / 100.0f;
        }
    }

    float *out_l = (float*)malloc(frames * sizeof(float));
    float *out_r = (float*)malloc(frames * sizeof(float));
    float *out_m = (float*)malloc(frames * sizeof(float));
    assert(out_l && out_r && out_m);

    const int dmix_iters = 200;

    /* 7.1 to Stereo */
    {
        double t0 = get_time_sec();
        for (int it = 0; it < dmix_iters; ++it) {
            avx_audio_downmix_7_1_to_stereo((const float* const*)ch, out_l, out_r, frames, AVX_AUDIO_NORMALIZE_NONE);
        }
        double elapsed = get_time_sec() - t0;
        double total_in_samples = (double)frames * 8.0 * dmix_iters;
        double msamples_sec = total_in_samples / (elapsed * 1e6);
        double ms_per_sec = (elapsed / dmix_iters) * 1000.0;
        double realtime_x = 1000.0 / ms_per_sec;
        double gb_sec = (total_in_samples * sizeof(float)) / (elapsed * 1024.0 * 1024.0 * 1024.0);

        printf("  [Downmix 7.1 -> Stereo (8 ch -> 2 ch)]\n");
        printf("    Throughput: %6.2f MSamples/s  (%5.2f GB/s)  [%5.3f ms / 1s audio] -> \033[1;32m%6.1fx REAL-TIME\033[0m\n",
               msamples_sec, gb_sec, ms_per_sec, realtime_x);
    }

    /* 5.1 to Stereo */
    {
        double t0 = get_time_sec();
        for (int it = 0; it < dmix_iters; ++it) {
            avx_audio_downmix_5_1_to_stereo((const float* const*)ch, out_l, out_r, frames, AVX_AUDIO_NORMALIZE_NONE);
        }
        double elapsed = get_time_sec() - t0;
        double total_in_samples = (double)frames * 6.0 * dmix_iters;
        double msamples_sec = total_in_samples / (elapsed * 1e6);
        double ms_per_sec = (elapsed / dmix_iters) * 1000.0;
        double realtime_x = 1000.0 / ms_per_sec;
        double gb_sec = (total_in_samples * sizeof(float)) / (elapsed * 1024.0 * 1024.0 * 1024.0);

        printf("  [Downmix 5.1 -> Stereo (6 ch -> 2 ch)]\n");
        printf("    Throughput: %6.2f MSamples/s  (%5.2f GB/s)  [%5.3f ms / 1s audio] -> \033[1;32m%6.1fx REAL-TIME\033[0m\n",
               msamples_sec, gb_sec, ms_per_sec, realtime_x);
    }

    /* 5.1 to Lt/Rt Matrix */
    {
        double t0 = get_time_sec();
        for (int it = 0; it < dmix_iters; ++it) {
            avx_audio_downmix_5_1_to_ltrt((const float* const*)ch, out_l, out_r, frames, AVX_AUDIO_NORMALIZE_NONE);
        }
        double elapsed = get_time_sec() - t0;
        double total_in_samples = (double)frames * 6.0 * dmix_iters;
        double msamples_sec = total_in_samples / (elapsed * 1e6);
        double ms_per_sec = (elapsed / dmix_iters) * 1000.0;
        double realtime_x = 1000.0 / ms_per_sec;
        double gb_sec = (total_in_samples * sizeof(float)) / (elapsed * 1024.0 * 1024.0 * 1024.0);

        printf("  [Downmix 5.1 -> Lt/Rt Matrix (6 ch -> 2 ch ProLogic)]\n");
        printf("    Throughput: %6.2f MSamples/s  (%5.2f GB/s)  [%5.3f ms / 1s audio] -> \033[1;32m%6.1fx REAL-TIME\033[0m\n",
               msamples_sec, gb_sec, ms_per_sec, realtime_x);
    }

    /* Quad 4.0 to Stereo */
    {
        double t0 = get_time_sec();
        for (int it = 0; it < dmix_iters; ++it) {
            avx_audio_downmix_quad_to_stereo((const float* const*)ch, out_l, out_r, frames, AVX_AUDIO_NORMALIZE_NONE);
        }
        double elapsed = get_time_sec() - t0;
        double total_in_samples = (double)frames * 4.0 * dmix_iters;
        double msamples_sec = total_in_samples / (elapsed * 1e6);
        double ms_per_sec = (elapsed / dmix_iters) * 1000.0;
        double realtime_x = 1000.0 / ms_per_sec;
        double gb_sec = (total_in_samples * sizeof(float)) / (elapsed * 1024.0 * 1024.0 * 1024.0);

        printf("  [Downmix Quad -> Stereo (4 ch -> 2 ch)]\n");
        printf("    Throughput: %6.2f MSamples/s  (%5.2f GB/s)  [%5.3f ms / 1s audio] -> \033[1;32m%6.1fx REAL-TIME\033[0m\n",
               msamples_sec, gb_sec, ms_per_sec, realtime_x);
    }

    /* Stereo to Mono */
    {
        double t0 = get_time_sec();
        for (int it = 0; it < dmix_iters; ++it) {
            avx_audio_downmix_stereo_to_mono(ch[0], ch[1], out_m, frames, AVX_AUDIO_NORMALIZE_NONE);
        }
        double elapsed = get_time_sec() - t0;
        double total_in_samples = (double)frames * 2.0 * dmix_iters;
        double msamples_sec = total_in_samples / (elapsed * 1e6);
        double ms_per_sec = (elapsed / dmix_iters) * 1000.0;
        double realtime_x = 1000.0 / ms_per_sec;
        double gb_sec = (total_in_samples * sizeof(float)) / (elapsed * 1024.0 * 1024.0 * 1024.0);

        printf("  [Downmix Stereo -> Mono (2 ch -> 1 ch)]\n");
        printf("    Throughput: %6.2f MSamples/s  (%5.2f GB/s)  [%5.3f ms / 1s audio] -> \033[1;32m%6.1fx REAL-TIME\033[0m\n",
               msamples_sec, gb_sec, ms_per_sec, realtime_x);
    }

    for (int c = 0; c < 8; ++c) free(ch[c]);
    free(out_l);
    free(out_r);
    free(out_m);
}

static void benchmark_fec_smpte2022(void) {
    printf("\n----------------------------------------------------------------------\n");
    printf("  Benchmark: SMPTE ST 2022-1 2D XOR FEC (D=10, L=5, 50 Media Fragments)\n");
    printf("----------------------------------------------------------------------\n");

    const uint16_t D = 10;
    const uint16_t L = 5;
    const uint16_t block_frags = D * L; /* 50 packets */
    const size_t frag_size = 1316;      /* standard MPEG-TS/RTP packet size */
    const size_t total_media_bytes = block_frags * frag_size; /* 65.8 KB */

    uint8_t media_data[50][1400];
    avx_fec_slice_t media_slices[50];
    for (uint16_t i = 0; i < block_frags; ++i) {
        fill_random_bytes(media_data[i], frag_size, (uint32_t)(i + 1) * 7919);
        media_slices[i].data = media_data[i];
        media_slices[i].len = frag_size;
    }

    uint8_t col_buf[AVX_FEC_MAX_COLS][1400];
    avx_fec_slice_t col_slices[AVX_FEC_MAX_COLS];
    bool col_present[AVX_FEC_MAX_COLS];
    for (uint16_t j = 0; j < D; ++j) {
        col_slices[j].data = col_buf[j];
        col_slices[j].len = 0;
        col_present[j] = true;
    }

    uint8_t row_buf[AVX_FEC_MAX_ROWS][1400];
    avx_fec_slice_t row_slices[AVX_FEC_MAX_ROWS];
    bool row_present[AVX_FEC_MAX_ROWS];
    for (uint16_t k = 0; k < L; ++k) {
        row_slices[k].data = row_buf[k];
        row_slices[k].len = 0;
        row_present[k] = true;
    }

    const int iters = 2000;

    /* 1. 1D Column Parity Generation */
    {
        double t0 = get_time_sec();
        for (int i = 0; i < iters; ++i) {
            avx_fec_generate_block(media_slices, block_frags, D, L, false, col_slices, NULL);
        }
        double elapsed = get_time_sec() - t0;
        double total_bytes = (double)total_media_bytes * iters;
        double gb_sec = total_bytes / (elapsed * 1024.0 * 1024.0 * 1024.0);
        double us_per_block = (elapsed / iters) * 1e6;

        printf("  [1D Column Parity Generation (10 Parity / 50 Media)]\n");
        printf("    Throughput: %6.2f GB/s  [%5.2f us / 50-packet block]  [%5.2f ns / fragment]\n",
               gb_sec, us_per_block, (us_per_block * 1000.0) / block_frags);
    }

    /* 2. 2D Column + Row Parity Generation */
    {
        double t0 = get_time_sec();
        for (int i = 0; i < iters; ++i) {
            avx_fec_generate_block(media_slices, block_frags, D, L, true, col_slices, row_slices);
        }
        double elapsed = get_time_sec() - t0;
        double total_bytes = (double)total_media_bytes * iters;
        double gb_sec = total_bytes / (elapsed * 1024.0 * 1024.0 * 1024.0);
        double us_per_block = (elapsed / iters) * 1e6;

        printf("  [2D Column + Row Parity Generation (15 Parity / 50 Media)]\n");
        printf("    Throughput: %6.2f GB/s  [%5.2f us / 50-packet block]  [%5.2f ns / fragment]\n",
               gb_sec, us_per_block, (us_per_block * 1000.0) / block_frags);
    }

    /* 3. 2D Iterative Reconstruction under Packet Loss */
    {
        /* Ensure parity is fresh */
        avx_fec_generate_block(media_slices, block_frags, D, L, true, col_slices, row_slices);

        /* Simulate losing 5 packets (10% burst loss): frags 0, 11, 22, 33, 44 */
        uint16_t lost_indices[] = {0, 11, 22, 33, 44};
        uint8_t rx_buf[50][1400];
        avx_fec_slice_t rx_slices[50];
        bool is_present[50];

        double t0 = get_time_sec();
        int total_recovered = 0;
        for (int it = 0; it < iters; ++it) {
            for (uint16_t k = 0; k < block_frags; ++k) {
                memcpy(rx_buf[k], media_data[k], frag_size);
                rx_slices[k].data = rx_buf[k];
                rx_slices[k].len = frag_size;
                is_present[k] = true;
            }
            for (size_t l = 0; l < sizeof(lost_indices)/sizeof(lost_indices[0]); ++l) {
                memset(rx_buf[lost_indices[l]], 0, frag_size);
                rx_slices[lost_indices[l]].len = 0;
                is_present[lost_indices[l]] = false;
            }

            int rec = avx_fec_reconstruct_block(rx_slices, is_present, block_frags, D, L,
                                                col_slices, col_present, row_slices, row_present);
            total_recovered += rec;
        }
        double elapsed = get_time_sec() - t0;
        double us_per_repair = (elapsed / iters) * 1e6;
        assert(total_recovered == (int)(sizeof(lost_indices)/sizeof(lost_indices[0]) * iters));

        printf("  [2D FEC Reconstruction (10%% Burst Packet Loss - 5 Lost Packets)]\n");
        printf("    Recovery Rate: 100%% (%d/%d packets restored)\n", (int)(sizeof(lost_indices)/sizeof(lost_indices[0])), (int)(sizeof(lost_indices)/sizeof(lost_indices[0])));
        printf("    Repair Time:   %6.2f us / block  [%5.2f us / recovered packet]\n",
               us_per_repair, us_per_repair / (sizeof(lost_indices)/sizeof(lost_indices[0])));
    }
}

static void benchmark_crypto_e2ee(void) {
    printf("\n----------------------------------------------------------------------\n");
    printf("  Benchmark: Native E2EE Authenticated Encryption (XChaCha20-Poly1305)\n");
    printf("----------------------------------------------------------------------\n");

    avx_crypto_ctx_t sender_ctx, receiver_ctx;
    const char *passphrase = "OpenAVX-Broadcast-Security-2026!";
    const char *stream_name = "CAM1_E2EE";

    assert(avx_crypto_init(&sender_ctx, passphrase, stream_name) == 0);
    assert(avx_crypto_init(&receiver_ctx, passphrase, stream_name) == 0);

    const size_t payload_sz = 1376; /* Standard network fragment payload */
    uint8_t plain[1376];
    fill_random_bytes(plain, payload_sz, 0xCAFEBABE);

    uint8_t header_ad[32];
    fill_random_bytes(header_ad, sizeof(header_ad), 0x12345678);

    uint8_t cipher_buf[1376 + AVX_CRYPTO_OVERHEAD];
    uint8_t dec_buf[1376];
    size_t cipher_len = sizeof(cipher_buf);
    size_t dec_len = sizeof(dec_buf);

    const int iters = 50000;

    /* 1. XChaCha20-Poly1305 Encrypt */
    {
        double t0 = get_time_sec();
        for (int i = 0; i < iters; ++i) {
            cipher_len = sizeof(cipher_buf);
            avx_crypto_encrypt_payload(&sender_ctx, (uint32_t)i, 0, 1,
                                       header_ad, sizeof(header_ad),
                                       plain, payload_sz,
                                       cipher_buf, &cipher_len);
        }
        double elapsed = get_time_sec() - t0;
        double total_bytes = (double)payload_sz * iters;
        double gb_sec = total_bytes / (elapsed * 1024.0 * 1024.0 * 1024.0);
        double us_per_pkt = (elapsed / iters) * 1e6;

        printf("  [XChaCha20-Poly1305 AEAD Encryption (1,376 Byte Packet)]\n");
        printf("    Throughput: %6.2f GB/s  [%5.2f us / packet]  [%5.2f Kpkts/sec]\n",
               gb_sec, us_per_pkt, (double)iters / (elapsed * 1e3));
    }

    /* 2. XChaCha20-Poly1305 Decrypt + MAC Verification */
    {
        cipher_len = sizeof(cipher_buf);
        avx_crypto_encrypt_payload(&sender_ctx, 100, 0, 1,
                                   header_ad, sizeof(header_ad),
                                   plain, payload_sz,
                                   cipher_buf, &cipher_len);

        double t0 = get_time_sec();
        for (int i = 0; i < iters; ++i) {
            dec_len = sizeof(dec_buf);
            int ret = avx_crypto_decrypt_payload(&receiver_ctx, 100, 0, 1,
                                                 header_ad, sizeof(header_ad),
                                                 cipher_buf, cipher_len,
                                                 dec_buf, &dec_len);
            assert(ret == 0);
        }
        double elapsed = get_time_sec() - t0;
        double total_bytes = (double)payload_sz * iters;
        double gb_sec = total_bytes / (elapsed * 1024.0 * 1024.0 * 1024.0);
        double us_per_pkt = (elapsed / iters) * 1e6;

        assert(memcmp(dec_buf, plain, payload_sz) == 0);

        printf("  [XChaCha20-Poly1305 AEAD Decryption + MAC Auth (1,376 Byte Packet)]\n");
        printf("    Throughput: %6.2f GB/s  [%5.2f us / packet]  [%5.2f Kpkts/sec]\n",
               gb_sec, us_per_pkt, (double)iters / (elapsed * 1e3));
    }

    avx_crypto_wipe(&sender_ctx);
    avx_crypto_wipe(&receiver_ctx);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("=== OpenAVX SIMD & Core Subsystem Benchmarks ===\n");
    printf("[*] Active SIMD: %s\n", avx_simd_get_name());

    benchmark_color_conversions(1920, 1080, "1080p Full HD");
    benchmark_color_conversions(3840, 2160, "4K Ultra HD");
    benchmark_speedhq_dct();
    benchmark_audio_dsp();
    benchmark_fec_smpte2022();
    benchmark_crypto_e2ee();

    printf("\n[PASS] openavx_bench\n");
    return 0;
}
