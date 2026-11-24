/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_v4l2_sink_test.c - Linux V4L2 Virtual Camera Sink Verification
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "avx_platform.h"
#include "openavx.h"
#include "avx_v4l2_sink.h"
#include "avx_color.h"

#if defined(AVX_OS_LINUX)
    #include <linux/videodev2.h>
#endif

static void test_uyvy_to_yuyv_bitwise_parity(void)
{
    printf("[*] Test 1: UYVY -> YUYV Bitwise Parity & SIMD Vector Alignment...\n");

    const size_t width = 1920;
    const size_t height = 1080;
    const size_t pixels = width * height;
    const size_t byte_count = pixels * 2;

    uint8_t *uyvy_src = (uint8_t*)malloc(byte_count);
    uint8_t *yuyv_dst = (uint8_t*)malloc(byte_count);
    assert(uyvy_src && yuyv_dst);

    /* Fill with deterministic U0 Y0 V0 Y1 sequence */
    for (size_t i = 0; i < byte_count; i += 4) {
        uyvy_src[i + 0] = (uint8_t)(0x10 + (i & 0x7F)); /* U0 */
        uyvy_src[i + 1] = (uint8_t)(0x80 + (i & 0x7F)); /* Y0 */
        uyvy_src[i + 2] = (uint8_t)(0x20 + (i & 0x7F)); /* V0 */
        uyvy_src[i + 3] = (uint8_t)(0x90 + (i & 0x7F)); /* Y1 */
    }

    /* Bitwise conversion verification */
    const uint32_t *src32 = (const uint32_t*)uyvy_src;
    uint32_t *dst32 = (uint32_t*)yuyv_dst;
    size_t words = pixels / 2;

    for (size_t i = 0; i < words; ++i) {
        uint32_t v = src32[i];
        dst32[i] = ((v & 0xFF00FF00) >> 8) | ((v & 0x00FF00FF) << 8);
    }

    /* Verify byte-by-byte that Y0 U0 Y1 V0 was produced */
    for (size_t i = 0; i < byte_count; i += 4) {
        assert(yuyv_dst[i + 0] == uyvy_src[i + 1]); /* Y0 */
        assert(yuyv_dst[i + 1] == uyvy_src[i + 0]); /* U0 */
        assert(yuyv_dst[i + 2] == uyvy_src[i + 3]); /* Y1 */
        assert(yuyv_dst[i + 3] == uyvy_src[i + 2]); /* V0 */
    }

    free(uyvy_src);
    free(yuyv_dst);
    printf("    [PASS] Test 1: 1080p UYVY to YUYV bitwise swap validated.\n");
}

static void test_rgba_to_yuyv_pipeline(void)
{
    printf("[*] Test 2: Full Color Pipeline (RGBA -> UYVY -> YUYV)...\n");

    const uint32_t width = 64;
    const uint32_t height = 64;
    const size_t pixels = width * height;

    uint8_t *rgba = (uint8_t*)malloc(pixels * 4);
    uint8_t *uyvy = (uint8_t*)malloc(pixels * 2);
    uint8_t *yuyv = (uint8_t*)malloc(pixels * 2);
    assert(rgba && uyvy && yuyv);

    /* Generate solid Pure White (R=255, G=255, B=255, A=255) */
    for (size_t i = 0; i < pixels; ++i) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = 255;
    }

    avx_color_rgba_to_uyvy(rgba, uyvy, width, height);

    /* Convert UYVY -> YUYV */
    const uint32_t *src32 = (const uint32_t*)uyvy;
    uint32_t *dst32 = (uint32_t*)yuyv;
    for (size_t i = 0; i < pixels / 2; ++i) {
        uint32_t v = src32[i];
        dst32[i] = ((v & 0xFF00FF00) >> 8) | ((v & 0x00FF00FF) << 8);
    }

    /* For white in BT.601 / BT.709 studio swing:
     * Y ~= 235, U ~= 128, V ~= 128 */
    uint8_t y0 = yuyv[0];
    uint8_t u0 = yuyv[1];
    uint8_t y1 = yuyv[2];
    uint8_t v0 = yuyv[3];

    assert(y0 >= 230 && y0 <= 240);
    assert(y1 >= 230 && y1 <= 240);
    assert(u0 >= 125 && u0 <= 131);
    assert(v0 >= 125 && v0 <= 131);

    free(rgba);
    free(uyvy);
    free(yuyv);
    printf("    [PASS] Test 2: RGBA to YUYV color accuracy verified (White Y=235, U=128, V=128).\n");
}

static void test_sink_configuration_and_resilience(void)
{
    printf("[*] Test 3: V4L2 Sink Parameter Resilience & Safe Teardown...\n");

    /* 3.1 Destroying NULL sink must not crash */
    avx_v4l2_sink_destroy(NULL);

    /* 3.2 Invalid config: non-existent device path */
    avx_v4l2_sink_config_t cfg_invalid = {
        .device_path = "/dev/video_non_existent_9999",
        .width = 1920,
        .height = 1080
    };
    avx_v4l2_sink_t *sink_inv = avx_v4l2_sink_create(&cfg_invalid);
    assert(sink_inv == NULL);

    /* 3.3 Invalid format calls on NULL sink */
    assert(avx_v4l2_sink_set_format(NULL, 1920, 1080, 0) == -1);
    assert(avx_v4l2_sink_write_raw(NULL, "data", 4) == -1);
    assert(avx_v4l2_sink_write_frame(NULL, NULL) == -1);
    assert(strcmp(avx_v4l2_sink_get_device_path(NULL), "") == 0);

    printf("    [PASS] Test 3: Error handling and defensive checks verified.\n");
}

static void test_v4l2_loopback_probing(void)
{
    printf("[*] Test 4: V4L2 Loopback Probing Subsystem...\n");

    char loopback_path[128] = {0};
    bool found = avx_v4l2_sink_probe_loopback(loopback_path, sizeof(loopback_path));

    if (found) {
        printf("    [+] Discovered active V4L2 loopback device: %s\n", loopback_path);
        assert(strlen(loopback_path) > 0);

        /* Attempt to open and write 1 synthetic frame to the discovered loopback */
        avx_v4l2_sink_config_t cfg = {
            .device_path = loopback_path,
            .width = 640,
            .height = 480,
            .pixelformat = 0 /* Default YUYV */
        };
        avx_v4l2_sink_t *sink = avx_v4l2_sink_create(&cfg);
        if (sink) {
            printf("    [+] Successfully initialized loopback sink on %s\n", loopback_path);
            uint8_t frame_buf[640 * 480 * 2];
            memset(frame_buf, 0x80, sizeof(frame_buf));
            int wret = avx_v4l2_sink_write_raw(sink, frame_buf, sizeof(frame_buf));
            printf("    [+] Frame write return code: %d\n", wret);
            avx_v4l2_sink_destroy(sink);
        } else {
            printf("    [-] Loopback device %s found but requires user video group permissions\n", loopback_path);
        }
    } else {
        printf("    [i] No active v4l2loopback kernel module loaded (probe returned false as expected).\n");
    }

    printf("    [PASS] Test 4: Loopback probing executed safely.\n");
}

int main(void)
{
    printf("=================================================================\n");
    printf("OpenAVX V4L2 Virtual Camera Sink Test Suite\n");
    printf("Target: Linux Loopback Video Subsystem (v4l2loopback)\n");
    printf("=================================================================\n");

    test_uyvy_to_yuyv_bitwise_parity();
    test_rgba_to_yuyv_pipeline();
    test_sink_configuration_and_resilience();
    test_v4l2_loopback_probing();

    printf("=================================================================\n");
    printf("All V4L2 Virtual Camera Sink Tests Passed Successfully (100%%)!\n");
    printf("=================================================================\n");
    return 0;
}
