/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_speedhq_alpha_test.c - SpeedHQ Alpha Transparency & Interoperability Test
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "openavx.h"
#include "avx_speedhq.h"
#include "avx_color.h"

static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void generate_test_graphics_uyva(int width, int height, uint8_t *uyvy, uint8_t *alpha)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            /* Background gradient video (UYVY) */
            uyvy[(y * width + x) * 2 + 0] = (uint8_t)((x * 255) / width);
            uyvy[(y * width + x) * 2 + 1] = (uint8_t)((y * 255) / height);

            /* Overlay graphics with transparency (Lower-third banner and transparent background) */
            if (y < height / 2) {
                /* Top half: fully transparent overlay (alpha = 0) */
                alpha[y * width + x] = 0;
            } else if (y < (height * 3) / 4) {
                /* Lower-third semi-transparent backdrop (alpha = 180) */
                alpha[y * width + x] = 180;
            } else {
                /* Solid opaque lower badge (alpha up to 255) with horizontal ramp */
                alpha[y * width + x] = (uint8_t)(128 + (x * 127) / (width - 1));
            }
        }
    }
}

int main(void)
{
    printf("\n=== OpenAVX SpeedHQ Alpha Channel (SHQ3/UYVA) Verification ===\n");

    const int width = 1920;
    const int height = 1080;
    const size_t num_pixels = (size_t)width * height;
    const size_t uyvy_size = num_pixels * 2;
    const size_t alpha_size = num_pixels;

    uint8_t *src_uyvy = (uint8_t*)malloc(uyvy_size);
    uint8_t *src_alpha = (uint8_t*)malloc(alpha_size);
    assert(src_uyvy && src_alpha);

    generate_test_graphics_uyva(width, height, src_uyvy, src_alpha);

    /* 1. Pure C99 SpeedHQ-3 Standalone Codec Test */
    printf("[*] 1. Testing C99 SpeedHQ-3 Standalone Alpha Compression (1080p)...\n");
    size_t max_shq_size = uyvy_size + alpha_size;
    uint8_t *shq_buf = (uint8_t*)malloc(max_shq_size);
    assert(shq_buf);

    size_t c99_shq_size = 0;
    double t0 = get_time_ms();
    int ret = avx_speedhq_encode_c99_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ3,
                                       width, height,
                                       src_uyvy, src_alpha,
                                       shq_buf, max_shq_size, &c99_shq_size);
    double t1 = get_time_ms();
    assert(ret == 0);
    assert(c99_shq_size > 64);
    printf("    [+] C99 Encoded 1080p SHQ3 frame: %zu bytes (ratio: %.1f:1) in %.2f ms\n",
           c99_shq_size, (double)(uyvy_size + alpha_size) / (double)c99_shq_size, t1 - t0);

    /* Decompress with C99 */
    uint8_t *dst_uyvy = (uint8_t*)malloc(uyvy_size);
    uint8_t *dst_alpha = (uint8_t*)malloc(alpha_size);
    assert(dst_uyvy && dst_alpha);
    bool has_alpha = false;

    double t2 = get_time_ms();
    ret = avx_speedhq_decode_c99_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ3,
                                    width, height,
                                    shq_buf, c99_shq_size,
                                    dst_uyvy, dst_alpha, &has_alpha);
    double t3 = get_time_ms();
    assert(ret == 0);
    assert(has_alpha);
    printf("    [+] C99 Decoded 1080p SHQ3 frame in %.2f ms (has_alpha: true)\n", t3 - t2);

    /* Verify lossless alpha restoration */
    size_t alpha_errors = 0;
    for (size_t i = 0; i < num_pixels; ++i) {
        if (src_alpha[i] != dst_alpha[i]) {
            alpha_errors++;
        }
    }
    printf("    [+] C99 alpha check: %zu mismatches / %zu pixels\n",
           alpha_errors, num_pixels);
    assert(alpha_errors == 0);

    /* 2. Standard SpeedHQ-3 Encode/Decode Path */
    printf("[*] 2. Testing avx_speedhq_encode_ex/decode_ex (SHQ3, 1080p)...\n");
    size_t ex_shq_size = 0;
    ret = avx_speedhq_encode_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ3,
                                width, height,
                                src_uyvy, src_alpha,
                                shq_buf, max_shq_size, &ex_shq_size);
    assert(ret == 0 && ex_shq_size > 0);

    memset(dst_alpha, 0, alpha_size);
    has_alpha = false;
    ret = avx_speedhq_decode_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ3,
                                width, height,
                                shq_buf, ex_shq_size,
                                dst_uyvy, dst_alpha, &has_alpha);
    assert(ret == 0);
    assert(has_alpha);

    alpha_errors = 0;
    for (size_t i = 0; i < num_pixels; ++i) {
        if (src_alpha[i] != dst_alpha[i]) alpha_errors++;
    }
    printf("    [+] SHQ3 alpha check: %zu mismatches\n", alpha_errors);
    assert(alpha_errors == 0);

    /* 3. End-to-End OpenAVX Network Loopback with Transparency */
    printf("[*] 3. Testing End-to-End OpenAVX Stream Loopback with Alpha Channel...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t tx_cfg = {
        .name = "OpenAVX SpeedHQ-3 Alpha Test",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &tx_cfg);
    assert(p_send != NULL);

    oavx_stream_config_t rx_cfg = {
        .name = "SpeedHQ Alpha Receiver",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_RGBX_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &rx_cfg);
    assert(p_recv != NULL);

    /* Connect receiver */
    oavx_endpoint_t src = {
        .name = "SpeedHQ-3 Alpha Source",
        .address = "127.0.0.1:7960"
    };
    oavx_stream_connect(p_recv, &src);

    /* Build packed UYVA payload for transmission */
    uint8_t *uyva_payload = (uint8_t*)malloc(uyvy_size + alpha_size);
    assert(uyva_payload);
    memcpy(uyva_payload, src_uyvy, uyvy_size);
    memcpy(uyva_payload + uyvy_size, src_alpha, alpha_size);

    oavx_video_frame_t tx_frame = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_SHQ3,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { uyva_payload, NULL, NULL, NULL },
        .strides = { (size_t)width * 2, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 123456789LL,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* Send compressed SpeedHQ-3 frame */
    oavx_stream_send_video(p_send, &tx_frame);

    /* Capture on receiver */
    oavx_video_frame_t rx_frame;
    memset(&rx_frame, 0, sizeof(rx_frame));
    oavx_frame_type_e frame_type = oavx_stream_receive(p_recv, &rx_frame, NULL, NULL, 3000);
    assert(frame_type == OAVX_FRAME_TYPE_VIDEO);
    assert(rx_frame.planes[0] != NULL);
    assert(rx_frame.width == (uint32_t)width && rx_frame.height == (uint32_t)height);
    printf("    [+] Received video frame: %ux%u, FourCC: 0x%08X (RGBA expected: 0x%08X)\n",
           rx_frame.width, rx_frame.height, rx_frame.fourcc, OAVX_FOURCC_VIDEO_TYPE_RGBA);
    assert(rx_frame.fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA);

    /* Sample transparency from top (transparent) and bottom (opaque) */
    uint8_t rx_top_alpha = rx_frame.planes[0][3]; /* (0,0) pixel alpha */
    uint8_t rx_bot_alpha = rx_frame.planes[0][(num_pixels - 1) * 4 + 3]; /* last pixel alpha */
    printf("    [+] Preserved Alpha: Top-Left=%u (expected %u), Bottom-Right=%u (expected %u)\n",
           rx_top_alpha, src_alpha[0], rx_bot_alpha, src_alpha[num_pixels - 1]);
    assert(rx_top_alpha == src_alpha[0]);
    assert(rx_bot_alpha == src_alpha[num_pixels - 1]);

    oavx_stream_release_video(p_recv, &rx_frame);
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    free(src_uyvy);
    free(src_alpha);
    free(dst_uyvy);
    free(dst_alpha);
    free(shq_buf);
    free(uyva_payload);

    printf("\n[PASS] openavx_speedhq_alpha_test\n\n");
    return 0;
}
