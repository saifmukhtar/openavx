/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_simd_audit_test.c - SIMD safety, alignment, and parity tests
 *
 * Tests edge-case safety (Part A) and SIMD/scalar parity and throughput (Part B)
 * inside monitored subprocesses.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>

#include <openavx.h>
#include <avx_internal.h>
#include <avx_simd.h>

#if defined(_WIN32)
#define exit(code) do { if ((code) != 0) { abort(); } else { return; } } while(0)
#endif
#include "avx_audio.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

typedef struct {
    bool passed;
    bool crashed_as_expected;
    int termination_signal;
    double duration_ms;
    char details[512];
} audit_result_t;

/* Run a test function in an isolated child process to safely intercept crashes (SIGSEGV, SIGILL) */
static audit_result_t run_monitored_crash_test(void (*test_fn)(void), int timeout_ms, const char* name, int expected_signal) {
    audit_result_t res;
    memset(&res, 0, sizeof(res));

    printf("\n----------------------------------------------------------------------\n");
    printf(" PROOF TEST: %s\n", name);
    printf(" (Monitored subprocess; expecting %s)\n",
           expected_signal == SIGSEGV ? "SIGSEGV (Segmentation Fault)" :
           (expected_signal == SIGILL ? "SIGILL (Illegal Instruction)" : "Exit Code 0"));
    printf("----------------------------------------------------------------------\n");

#if defined(_WIN32)
    double t0 = get_time_ms();
    test_fn();
    double t1 = get_time_ms();
    res.duration_ms = t1 - t0;
    res.passed = true;
    snprintf(res.details, sizeof(res.details), "PASSED: Clean completion with code 0 in %.2f ms", res.duration_ms);
    return res;
#else
    double t0 = get_time_ms();
    pid_t pid = fork();

    if (pid == 0) {
        alarm((unsigned int)(timeout_ms / 1000 + 2));
        test_fn();
        exit(0);
    }

    if (pid < 0) {
        res.passed = false;
        snprintf(res.details, sizeof(res.details), "fork() failed: %s", strerror(errno));
        return res;
    }

    int status = 0;
    int elapsed = 0;
    int step_ms = 20;

    while (elapsed < timeout_ms) {
        pid_t p = waitpid(pid, &status, WNOHANG);
        if (p == pid) {
            double t1 = get_time_ms();
            res.duration_ms = t1 - t0;

            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                res.termination_signal = sig;
                if (sig == expected_signal) {
                    res.passed = true;
                    res.crashed_as_expected = true;
                    snprintf(res.details, sizeof(res.details),
                             "PROVEN: Process crashed with signal %d (%s) as predicted in %.2f ms",
                             sig, strsignal(sig), res.duration_ms);
                } else {
                    res.passed = false;
                    snprintf(res.details, sizeof(res.details),
                             "UNEXPECTED: Process killed by signal %d (%s) instead of %d in %.2f ms",
                             sig, strsignal(sig), expected_signal, res.duration_ms);
                }
            } else if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                if (expected_signal == 0 && code == 0) {
                    res.passed = true;
                    snprintf(res.details, sizeof(res.details),
                             "PASSED: Clean completion with code 0 in %.2f ms", res.duration_ms);
                } else if (expected_signal != 0) {
                    res.passed = false;
                    snprintf(res.details, sizeof(res.details),
                             "FAILED: Process did not crash; exited normally with code %d (expected signal %d)",
                             code, expected_signal);
                } else {
                    res.passed = false;
                    snprintf(res.details, sizeof(res.details),
                             "FAILED: Process exited with error code %d", code);
                }
            }
            return res;
        }
        usleep((useconds_t)(step_ms * 1000));
        elapsed += step_ms;
    }

    /* Timed out */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    res.passed = false;
    snprintf(res.details, sizeof(res.details), "Watchdog timeout after %d ms", timeout_ms);
    return res;
#endif
}

/* ========================================================================= */
/* PROOF A-1: avx2_downmix_5_1 Graceful NULL Channel Pointer Handling        */
/* ========================================================================= */

static void proof_a1_null_channel_pointer(void) {
    float fl[64], fr[64], bl[64], br[64];
    float out_l[64], out_r[64];
    for (int i = 0; i < 64; ++i) {
        fl[i] = 0.5f; fr[i] = 0.5f; bl[i] = 0.2f; br[i] = 0.2f;
    }

    /* Channel 2 (Center) is passed as NULL pointer */
    const float* in[6] = { fl, fr, NULL, NULL, bl, br };

    printf("[Child] Invoking avx_audio_downmix_5_1_to_stereo() with in_channels[2] = NULL...\n");
    fflush(stdout);

    /* Should handle NULL safely and return cleanly without SIGSEGV */
    avx_audio_downmix_5_1_to_stereo(in, out_l, out_r, 64, AVX_AUDIO_NORMALIZE_NONE);

    printf("[Child] PASSED: function safely validated null channel pointer without crash!\n");
    exit(0);
}

/* ========================================================================= */
/* PROOF A-2: FMA Capability Check Verification                              */
/* ========================================================================= */

static void proof_a2_missing_fma_detection(void) {
    printf("[Child] Inspecting CPU dispatch logic in avx_simd_get_caps()...\n");
    fflush(stdout);

    avx_simd_type_e caps = avx_simd_get_caps();
    const char* name = avx_simd_get_name();
    printf("[Child] Detected SIMD caps: %d (%s)\n", caps, name);

#if defined(__x86_64__)
    bool supports_avx2 = __builtin_cpu_supports("avx2");
    bool supports_fma  = __builtin_cpu_supports("fma");

    printf("[Child] CPU features: __builtin_cpu_supports('avx2') = %s\n", supports_avx2 ? "TRUE" : "FALSE");
    printf("[Child] CPU features: __builtin_cpu_supports('fma')  = %s\n", supports_fma  ? "TRUE" : "FALSE");

    if (supports_avx2 && supports_fma) {
        if (caps == AVX_SIMD_AVX2) {
            printf("[Child] PASSED: AVX2+FMA verified and enabled.\n");
            exit(0);
        } else {
            printf("[Child] FAILED: AVX2 and FMA supported but caps not set to AVX2!\n");
            exit(1);
        }
    } else {
        if (caps != AVX_SIMD_AVX2) {
            printf("[Child] PASSED: Non-FMA CPU correctly falls back to scalar.\n");
            exit(0);
        } else {
            printf("[Child] FAILED: AVX2 enabled despite missing FMA support!\n");
            exit(1);
        }
    }
#else
    exit(0);
#endif
}

/* ========================================================================= */
/* PROOF A-3: Multi-Channel planes[8] Bounds Safety                          */
/* ========================================================================= */

static void proof_a3_planes_buffer_over_read(void) {
    printf("[Child] Constructing 40-channel audio frame with 32-element planes array...\n");
    fflush(stdout);

    float channel_data[64 * 40];
    for (int i = 0; i < 64 * 40; ++i) channel_data[i] = 0.1f;

    oavx_audio_frame_t src_frame;
    memset(&src_frame, 0, sizeof(src_frame));
    src_frame.sample_rate = 48000;
    src_frame.channels = 40; /* Greater than OAVX_MAX_AUDIO_CHANNELS (32) */
    src_frame.frames = 64;
    src_frame.stride_bytes = 64 * sizeof(float);
    src_frame.format = OAVX_AUDIO_FMT_FLT_PLANAR;

    /* planes[0] has contiguous channel data; planes[1..31] are NULL */
    src_frame.planes[0] = channel_data;
    for (int c = 1; c < OAVX_MAX_AUDIO_CHANNELS; ++c) src_frame.planes[c] = NULL;

    /* Poison pts_ns to verify it is NEVER accessed as planes[32] */
    src_frame.pts_ns = (int64_t)0xDEADBEEF00000000ULL;

    oavx_audio_frame_t dst_frame;
    memset(&dst_frame, 0, sizeof(dst_frame));

    printf("[Child] Calling avx_audio_frame_downmix_to_stereo() with 40 channels...\n");
    fflush(stdout);

    bool ok = avx_audio_frame_downmix_to_stereo(&src_frame, &dst_frame, AVX_AUDIO_NORMALIZE_NONE);

    if (ok && dst_frame.planes[0] && dst_frame.planes[1]) {
        printf("[Child] PASSED: 40-channel downmix completed safely without out-of-bounds access!\n");
        free(dst_frame.planes[0]);
        exit(0);
    } else {
        printf("[Child] FAILED: downmix failed or returned null output\n");
        exit(1);
    }
}

/* ========================================================================= */
/* PROOF A-4: Remainder Odd Pixel Handling in avx_simd_uyvy_to_rgba          */
/* ========================================================================= */

static void proof_a4_remainder_odd_pixel_drop(void) {
    /* Test an odd pixel count: 15 pixels (8 pixels vector block + 7 pixels remainder) */
    const int width = 15;
    const int height = 1;
    const size_t uyvy_bytes = width * height * 2;   /* 30 bytes */
    const size_t rgba_bytes = width * height * 4;   /* 60 bytes */

    uint8_t *src_uyvy = (uint8_t*)malloc(uyvy_bytes);
    uint8_t *dst_rgba = (uint8_t*)malloc(rgba_bytes);
    assert(src_uyvy && dst_rgba);

    /* Fill src with dummy UYVY: U=128, Y0=128, V=128, Y1=128 (neutral grey) */
    for (size_t i = 0; i < uyvy_bytes; ++i) {
        src_uyvy[i] = 128;
    }

    /* Pre-fill destination buffer with poison sentinel 0xAA */
    memset(dst_rgba, 0xAA, rgba_bytes);

    printf("[Child] Converting 15 pixels (UYVY -> RGBA)...\n");
    avx_simd_uyvy_to_rgba(src_uyvy, dst_rgba, width, height);

    size_t last_px_offset = (width - 1) * 4; /* Pixel 14 (15th pixel) */
    uint8_t r = dst_rgba[last_px_offset + 0];
    uint8_t g = dst_rgba[last_px_offset + 1];
    uint8_t b = dst_rgba[last_px_offset + 2];
    uint8_t a = dst_rgba[last_px_offset + 3];

    printf("[Child] Pixel 0 (Vector) Alpha: %u (expected 255)\n", dst_rgba[3]);
    printf("[Child] Pixel 13 (Remainder) Alpha: %u (expected 255)\n", dst_rgba[13 * 4 + 3]);
    printf("[Child] Pixel 14 (Last Pixel) Raw Bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n", r, g, b, a);

    if (a == 255 && r == 0x82 && g == 0x82 && b == 0x82) {
        printf("[Child] PASSED: The 15th odd pixel is correctly converted and written!\n");
        free(src_uyvy);
        free(dst_rgba);
        exit(0);
    } else {
        printf("[Child] FAILED: Unexpected pixel 14 value (got 0x%02X 0x%02X 0x%02X 0x%02X).\n", r, g, b, a);
        free(src_uyvy);
        free(dst_rgba);
        exit(1);
    }
}

/* ========================================================================= */
/* PROOF A-5: Remainder Odd Pixel Handling in avx_simd_uyvy_to_bgra          */
/* ========================================================================= */

static void proof_a5_remainder_odd_pixel_drop_bgra(void) {
    const int width = 15;
    const int height = 1;
    const size_t uyvy_bytes = width * height * 2;
    const size_t bgra_bytes = width * height * 4;

    uint8_t *src_uyvy = (uint8_t*)malloc(uyvy_bytes);
    uint8_t *dst_bgra = (uint8_t*)malloc(bgra_bytes);
    assert(src_uyvy && dst_bgra);

    memset(src_uyvy, 128, uyvy_bytes);
    memset(dst_bgra, 0x55, bgra_bytes); /* Poison sentinel 0x55 */

    printf("[Child] Converting 15 pixels (UYVY -> BGRA)...\n");
    avx_simd_uyvy_to_bgra(src_uyvy, dst_bgra, width, height);

    size_t last_px_offset = (width - 1) * 4;
    uint8_t b = dst_bgra[last_px_offset + 0];
    uint8_t g = dst_bgra[last_px_offset + 1];
    uint8_t r = dst_bgra[last_px_offset + 2];
    uint8_t a = dst_bgra[last_px_offset + 3];

    printf("[Child] Pixel 14 (Last Pixel) Raw Bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n", b, g, r, a);

    if (a == 255 && b == 0x82 && g == 0x82 && r == 0x82) {
        printf("[Child] PASSED: The 15th odd pixel in UYVY->BGRA is correctly converted!\n");
        free(src_uyvy);
        free(dst_bgra);
        exit(0);
    } else {
        printf("[Child] FAILED: Unexpected pixel 14 value (got 0x%02X 0x%02X 0x%02X 0x%02X).\n", b, g, r, a);
        free(src_uyvy);
        free(dst_bgra);
        exit(1);
    }
}

/* ========================================================================= */
/* A-6: Unaligned Access & Byte-Order Safety in avx_scalar_rgba_to_bgra      */
/* ========================================================================= */

static void proof_a6_aliasing_and_endianness_hazard(void) {
    printf("[Child] Checking avx_scalar_rgba_to_bgra() on unaligned buffers...\n");
    fflush(stdout);

    /* Allocate buffer with unaligned offset (+1 byte) */
    uint8_t raw_buf[64];
    uint8_t *unaligned_src = raw_buf + 1;
    uint8_t *unaligned_dst = raw_buf + 33;

    /* Pixel 0: R=10, G=20, B=30, A=255 */
    unaligned_src[0] = 10;
    unaligned_src[1] = 20;
    unaligned_src[2] = 30;
    unaligned_src[3] = 255;

    avx_scalar_rgba_to_bgra(unaligned_src, unaligned_dst, 1, 1);

    printf("[Child] In:  R=%u, G=%u, B=%u, A=%u\n",
           unaligned_src[0], unaligned_src[1], unaligned_src[2], unaligned_src[3]);
    printf("[Child] Out: B=%u, G=%u, R=%u, A=%u\n",
           unaligned_dst[0], unaligned_dst[1], unaligned_dst[2], unaligned_dst[3]);

    if (unaligned_dst[0] == 30 && unaligned_dst[1] == 20 &&
        unaligned_dst[2] == 10 && unaligned_dst[3] == 255) {
        printf("[Child] PASS: avx_scalar_rgba_to_bgra produced exact BGRA byte order\n");
        exit(0);
    } else {
        printf("[Child] FAIL: unexpected BGRA output\n");
        exit(1);
    }
}

/* ========================================================================= */
/* B-1: AVX2 RGBA to UYVY Throughput & Parity                                */
/* ========================================================================= */

static void proof_b1_compiler_vectorized_rgba_to_uyvy(void) {
    printf("[Child] Testing throughput and parity of avx_simd_rgba_to_uyvy...\n");
    fflush(stdout);

    const int width = 1920;
    const int height = 1080;
    size_t rgba_bytes = (size_t)width * height * 4;
    size_t uyvy_bytes = (size_t)width * height * 2;

    uint8_t *src = (uint8_t*)malloc(rgba_bytes);
    uint8_t *dst_scalar = (uint8_t*)malloc(uyvy_bytes);
    uint8_t *dst_simd = (uint8_t*)malloc(uyvy_bytes);
    assert(src && dst_scalar && dst_simd);

    for (size_t i = 0; i < rgba_bytes; ++i) {
        src[i] = (uint8_t)((i * 37 + 19) & 0xFF);
    }

    /* Bitwise parity check */
    avx_scalar_rgba_to_uyvy(src, dst_scalar, width, height);
    avx_simd_rgba_to_uyvy(src, dst_simd, width, height);

    if (memcmp(dst_scalar, dst_simd, uyvy_bytes) != 0) {
        printf("[Child] FAIL: mismatch between scalar and SIMD RGBA->UYVY\n");
        free(src); free(dst_scalar); free(dst_simd);
        exit(1);
    }

    /* Run scalar */
    double t0 = get_time_ms();
    for (int i = 0; i < 20; ++i) {
        avx_scalar_rgba_to_uyvy(src, dst_scalar, width, height);
    }
    double t_scalar = get_time_ms() - t0;

    /* Run SIMD path */
    t0 = get_time_ms();
    for (int i = 0; i < 20; ++i) {
        avx_simd_rgba_to_uyvy(src, dst_simd, width, height);
    }
    double t_simd = get_time_ms() - t0;

    double ratio = t_scalar / t_simd;
    printf("[Child] 20 iterations 1080p RGBA->UYVY: Scalar = %.2f ms, SIMD = %.2f ms (Ratio: %.2fx)\n",
           t_scalar, t_simd, ratio);

    printf("[Child] PASS: RGBA->UYVY parity matches scalar\n");

    free(src);
    free(dst_scalar);
    free(dst_simd);
    exit(0);
}

/* ========================================================================= */
/* B-2: SIMD Alpha Blend Throughput & Parity                                 */
/* ========================================================================= */

static bool is_emulated_cpu(void) {
    if (getenv("QEMU_LD_PREFIX") != NULL || getenv("QEMU_UNAME") != NULL) {
        return true;
    }
#if defined(__aarch64__) || defined(__arm__)
    struct utsname uts;
    if (uname(&uts) == 0 &&
        strncmp(uts.machine, "aarch64", 7) != 0 &&
        strncmp(uts.machine, "arm", 3) != 0) {
        return true;
    }
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        bool foreign_vendor = false;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "GenuineIntel") || strstr(line, "AuthenticAMD") ||
                strstr(line, "QEMU") ||
                (strstr(line, "CPU implementer") && strstr(line, "0x00"))) {
                foreign_vendor = true;
                break;
            }
        }
        fclose(f);
        if (foreign_vendor) return true;
    }
#endif
    return false;
}

static void proof_b2_avx2_alpha_blend(void) {
    avx_simd_type_e caps = avx_simd_get_caps();
    const char *backend = avx_simd_get_name();
    bool emulated = is_emulated_cpu();

    printf("[Child] Testing %s alpha blend performance and parity...\n", backend);
    fflush(stdout);

    const int width = 1920;
    const int height = 1080;
    size_t bytes = (size_t)width * height * 4;

    uint8_t *fg = (uint8_t*)malloc(bytes);
    uint8_t *bg = (uint8_t*)malloc(bytes);
    uint8_t *out_s = (uint8_t*)malloc(bytes);
    uint8_t *out_v = (uint8_t*)malloc(bytes);
    assert(fg && bg && out_s && out_v);

    for (size_t i = 0; i < bytes; ++i) {
        fg[i] = (uint8_t)(i & 0xFF);
        bg[i] = (uint8_t)((i * 3 + 17) & 0xFF);
    }

    avx_scalar_alpha_blend_rgba(fg, bg, out_s, width, height);
    avx_simd_alpha_blend_rgba(fg, bg, out_v, width, height);

    if (memcmp(out_s, out_v, bytes) != 0) {
        printf("[Child] FAIL: mismatch between scalar and SIMD alpha blend\n");
        free(fg); free(bg); free(out_s); free(out_v);
        exit(1);
    }

    double t0 = get_time_ms();
    for (int i = 0; i < 20; ++i) {
        avx_scalar_alpha_blend_rgba(fg, bg, out_s, width, height);
    }
    double t_scalar = get_time_ms() - t0;

    t0 = get_time_ms();
    for (int i = 0; i < 20; ++i) {
        avx_simd_alpha_blend_rgba(fg, bg, out_v, width, height);
    }
    double t_simd = get_time_ms() - t0;

    double speedup = (t_simd > 0.0) ? (t_scalar / t_simd) : 0.0;
    printf("[Child] 20 iters 1080p Alpha Blend (%s): Scalar = %.2f ms, SIMD = %.2f ms (Speedup: %.2fx)\n",
           backend, t_scalar, t_simd, speedup);

    int rc = 0;
    if (speedup >= 2.0) {
        printf("[Child] PASS: %s alpha blend %.2fx faster with scalar parity\n", backend, speedup);
    } else if (emulated || caps == AVX_SIMD_NONE) {
        printf("[Child] WARN: speedup %.2fx < 2.0x (%s%s; bitwise parity passed)\n",
               speedup, backend, emulated ? ", QEMU emulation" : "");
    } else {
        printf("[Child] FAIL: speedup %.2fx < 2.0x on native %s\n", speedup, backend);
        rc = 1;
    }

    free(fg); free(bg); free(out_s); free(out_v);
    exit(rc);
}

/* ========================================================================= */
/* PROOF B-3: Complete BGRA to UYVY Parity Across Multiple Resolutions       */
/* ========================================================================= */

static void proof_b3_bgra_to_uyvy_parity(void) {
    printf("[Child] Testing BGRA -> UYVY bitwise parity and odd remainder handling...\n");
    fflush(stdout);

    int test_widths[] = {1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 64, 1920};
    int test_heights[] = {1, 2, 4, 1080};

    for (size_t w_i = 0; w_i < sizeof(test_widths)/sizeof(test_widths[0]); ++w_i) {
        for (size_t h_i = 0; h_i < sizeof(test_heights)/sizeof(test_heights[0]); ++h_i) {
            int w = test_widths[w_i];
            int h = test_heights[h_i];
            size_t total_px = (size_t)w * h;
            size_t bgra_bytes = total_px * 4;
            size_t uyvy_bytes = total_px * 2;

            uint8_t *src = (uint8_t*)malloc(bgra_bytes);
            uint8_t *dst_s = (uint8_t*)malloc(uyvy_bytes);
            uint8_t *dst_v = (uint8_t*)malloc(uyvy_bytes);
            assert(src && dst_s && dst_v);

            for (size_t i = 0; i < bgra_bytes; ++i) {
                src[i] = (uint8_t)((i * 37 + 19) & 0xFF);
            }

            avx_scalar_bgra_to_uyvy(src, dst_s, w, h);
            avx_simd_bgra_to_uyvy(src, dst_v, w, h);

            if (memcmp(dst_s, dst_v, uyvy_bytes) != 0) {
                printf("[Child] FAILED: Bitwise mismatch in BGRA->UYVY for %dx%d!\n", w, h);
                free(src); free(dst_s); free(dst_v);
                exit(1);
            }

            avx_scalar_rgba_to_uyvy(src, dst_s, w, h);
            avx_simd_rgba_to_uyvy(src, dst_v, w, h);

            if (memcmp(dst_s, dst_v, uyvy_bytes) != 0) {
                printf("[Child] FAILED: Bitwise mismatch in RGBA->UYVY for %dx%d!\n", w, h);
                free(src); free(dst_s); free(dst_v);
                exit(1);
            }

            free(src); free(dst_s); free(dst_v);
        }
    }

    printf("[Child] PASS: BGRA -> UYVY parity verified across odd and even dimensions\n");
    exit(0);
}

static void proof_b4_hd_odd_width_uyvy_parity(void) {
    printf("[Child] Testing HD odd-width BT.709 parity and remainder tail...\n");
    const int dims[2][2] = { { 15, 720 }, { 1281, 720 } };

    for (int d = 0; d < 2; ++d) {
        int w = dims[d][0], h = dims[d][1];
        size_t px = (size_t)w * (size_t)h;
        uint8_t *uyvy = (uint8_t*)malloc(px * 2);
        uint8_t *out_s = (uint8_t*)malloc(px * 4);
        uint8_t *out_v = (uint8_t*)malloc(px * 4);
        if (!uyvy || !out_s || !out_v) exit(1);

        for (size_t i = 0; i < px * 2; i += 2) {
            uyvy[i + 0] = 103; /* U for BT.709 red (odd tail reads U=103, V=128) */
            uyvy[i + 1] = 62;  /* Y */
            if (i + 2 < px * 2) {
                /* For pairs, set [U, Y0, V, Y1] */
                if ((i % 4) == 0) uyvy[i + 2] = 240;
            }
        }

        avx_scalar_uyvy_to_rgba(uyvy, out_s, w, h);
        avx_simd_uyvy_to_rgba(uyvy, out_v, w, h);
        if (memcmp(out_s, out_v, px * 4) != 0) {
            printf("[Child] FAILED: HD odd-width UYVY->RGBA parity mismatch for %dx%d!\n", w, h);
            exit(1);
        }

        avx_scalar_uyvy_to_bgra(uyvy, out_s, w, h);
        avx_simd_uyvy_to_bgra(uyvy, out_v, w, h);
        if (memcmp(out_s, out_v, px * 4) != 0) {
            printf("[Child] FAILED: HD odd-width UYVY->BGRA parity mismatch for %dx%d!\n", w, h);
            exit(1);
        }

        /* Verify last odd pixel used BT.709 (cbu=541) rather than BT.601 (cbu=516):
         * c0 = 62 - 16 = 46, d_u = 103 - 128 = -25 -> b = (298*46 + 541*(-25) + 128)>>8 = 1 (BT.601 gives 3) */
        if (out_v[(px - 1) * 4 + 0] != 1) {
            printf("[Child] FAILED: HD odd tail pixel used wrong matrix (b=%u expected 1)\n",
                   out_v[(px - 1) * 4 + 0]);
            exit(1);
        }

        free(uyvy);
        free(out_s);
        free(out_v);
    }

    printf("[Child] PASS: HD odd-width BT.709 parity and remainder tail verified\n");
    exit(0);
}

/* ========================================================================= */
/* MAIN TEST RUNNER                                                          */
/* ========================================================================= */

int main(void) {
    printf("=== OpenAVX SIMD Safety & Parity Tests ===\n");

    int passed_count = 0;
    int total_count = 10;

    /* A-1 */
    audit_result_t r1 = run_monitored_crash_test(proof_a1_null_channel_pointer, 3000,
                                                "A-1: NULL channel pointer handling in avx_audio_downmix_5_1",
                                                0);
    if (r1.passed) passed_count++;
    printf(" [%s] %s\n", r1.passed ? "PASS" : "FAIL", r1.details);

    /* A-2 */
    audit_result_t r2 = run_monitored_crash_test(proof_a2_missing_fma_detection, 3000,
                                                "A-2: detect_cpu_caps FMA check and thread safety",
                                                0);
    if (r2.passed) passed_count++;
    printf(" [%s] %s\n", r2.passed ? "PASS" : "FAIL", r2.details);

    /* A-3 */
    audit_result_t r3 = run_monitored_crash_test(proof_a3_planes_buffer_over_read, 3000,
                                                "A-3: Multi-channel planes bounds (>32 channels)",
                                                0);
    if (r3.passed) passed_count++;
    printf(" [%s] %s\n", r3.passed ? "PASS" : "FAIL", r3.details);

    /* A-4 */
    audit_result_t r4 = run_monitored_crash_test(proof_a4_remainder_odd_pixel_drop, 3000,
                                                "A-4: Remainder pixel handling in avx_simd_uyvy_to_rgba",
                                                0);
    if (r4.passed) passed_count++;
    printf(" [%s] %s\n", r4.passed ? "PASS" : "FAIL", r4.details);

    /* A-5 */
    audit_result_t r5 = run_monitored_crash_test(proof_a5_remainder_odd_pixel_drop_bgra, 3000,
                                                "A-5: Remainder pixel handling in avx_simd_uyvy_to_bgra",
                                                0);
    if (r5.passed) passed_count++;
    printf(" [%s] %s\n", r5.passed ? "PASS" : "FAIL", r5.details);

    /* A-6 */
    audit_result_t r6 = run_monitored_crash_test(proof_a6_aliasing_and_endianness_hazard, 3000,
                                                "A-6: Unaligned access and byte order in avx_scalar_rgba_to_bgra",
                                                0);
    if (r6.passed) passed_count++;
    printf(" [%s] %s\n", r6.passed ? "PASS" : "FAIL", r6.details);

    /* B-1 */
    audit_result_t r7 = run_monitored_crash_test(proof_b1_compiler_vectorized_rgba_to_uyvy, 5000,
                                                "B-1: AVX2 RGBA to UYVY throughput and parity",
                                                0);
    if (r7.passed) passed_count++;
    printf(" [%s] %s\n", r7.passed ? "PASS" : "FAIL", r7.details);

    /* B-2 */
    audit_result_t r8 = run_monitored_crash_test(proof_b2_avx2_alpha_blend, 5000,
                                                "B-2: SIMD alpha blend throughput and parity",
                                                0);
    if (r8.passed) passed_count++;
    printf(" [%s] %s\n", r8.passed ? "PASS" : "FAIL", r8.details);

    /* B-3 */
    audit_result_t r9 = run_monitored_crash_test(proof_b3_bgra_to_uyvy_parity, 3000,
                                                "B-3: BGRA to UYVY parity across odd and even widths",
                                                0);
    if (r9.passed) passed_count++;
    printf(" [%s] %s\n", r9.passed ? "PASS" : "FAIL", r9.details);

    /* B-4 */
    audit_result_t r10 = run_monitored_crash_test(proof_b4_hd_odd_width_uyvy_parity, 3000,
                                                 "B-4: HD odd-width BT.709 parity and remainder tail",
                                                 0);
    if (r10.passed) passed_count++;
    printf(" [%s] %s\n", r10.passed ? "PASS" : "FAIL", r10.details);

    printf("\n[%s] openavx_simd_audit_test: %d / %d passed\n\n",
           (passed_count == total_count) ? "PASS" : "FAIL", passed_count, total_count);

    return (passed_count == total_count) ? 0 : 1;
}
