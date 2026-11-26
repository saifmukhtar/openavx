/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_alpha_color_test.c - 4:4:4:4 Alpha Transparency & RGB Color Formats Test
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <math.h>

#include "openavx.h"
#include "avx_color.h"

static void test_color_math(void) {
    printf("[*] 1. Testing Color Conversion Math & Roundtrips...\n");

    /* 8 pixels (pairs for 4:2:2 chroma subsampling): Red x2, Green x2, Blue x2, White x2 */
    uint8_t rgba_in[32] = {
        255, 0,   0,   255,   255, 0,   0,   255,   /* Red pair */
        0,   255, 0,   255,   0,   255, 0,   255,   /* Green pair */
        0,   0,   255, 255,   0,   0,   255, 255,   /* Blue pair */
        255, 255, 255, 255,   255, 255, 255, 255    /* White pair */
    };
    uint8_t uyvy[16];
    uint8_t rgba_out[32];

    avx_color_rgba_to_uyvy(rgba_in, uyvy, 8, 1);
    avx_color_uyvy_to_rgba(uyvy, rgba_out, 8, 1);

    for (int i = 0; i < 8; ++i) {
        int r_diff = abs((int)rgba_in[i * 4 + 0] - (int)rgba_out[i * 4 + 0]);
        int g_diff = abs((int)rgba_in[i * 4 + 1] - (int)rgba_out[i * 4 + 1]);
        int b_diff = abs((int)rgba_in[i * 4 + 2] - (int)rgba_out[i * 4 + 2]);
        assert(rgba_out[i * 4 + 3] == 255);
        /* Allow small integer rounding tolerance <= 2 */
        assert(r_diff <= 2);
        assert(g_diff <= 2);
        assert(b_diff <= 2);
    }
    printf("    -> RGBA <-> UYVY conversion roundtrip verified (tolerances <= 2).\n");

    /* Test channel swap RGBA <-> BGRA */
    uint8_t bgra[32];
    uint8_t rgba_restored[32];
    avx_color_rgba_to_bgra(rgba_in, bgra, 8, 1);

    /* Red (255,0,0,255) in RGBA becomes (0,0,255,255) in BGRA */
    assert(bgra[0] == 0 && bgra[1] == 0 && bgra[2] == 255 && bgra[3] == 255);
    /* Blue (0,0,255,255) in RGBA becomes (255,0,0,255) in BGRA */
    assert(bgra[16] == 255 && bgra[17] == 0 && bgra[18] == 0 && bgra[19] == 255);

    avx_color_rgba_to_bgra(bgra, rgba_restored, 8, 1);
    assert(memcmp(rgba_in, rgba_restored, sizeof(rgba_in)) == 0);
    printf("    -> RGBA <-> BGRA channel swap symmetry verified.\n");
}

static void test_alpha_compositing(void) {
    printf("[*] 2. Testing Alpha Compositing & Transparency Engine...\n");

    int w = 64, h = 64;
    size_t bytes = (size_t)w * h * 4;
    uint8_t *fg = (uint8_t*)malloc(bytes);
    uint8_t *bg = (uint8_t*)malloc(bytes);
    uint8_t *out = (uint8_t*)malloc(bytes);
    assert(fg && bg && out);

    /* Background: 100% opaque Solid Blue (0, 0, 255, 255) */
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        bg[i * 4 + 0] = 0;
        bg[i * 4 + 1] = 0;
        bg[i * 4 + 2] = 255;
        bg[i * 4 + 3] = 255;
    }

    /* Foreground:
     * Top half: 100% transparent (0, 255, 0, 0)
     * Middle: 50% semi-transparent Red (255, 0, 0, 128)
     * Bottom: 100% opaque Green (0, 255, 0, 255)
     */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = ((size_t)y * w + x) * 4;
            if (y < 20) {
                fg[idx + 0] = 0;
                fg[idx + 1] = 255;
                fg[idx + 2] = 0;
                fg[idx + 3] = 0;    /* 0% alpha */
            } else if (y < 40) {
                fg[idx + 0] = 255;
                fg[idx + 1] = 0;
                fg[idx + 2] = 0;
                fg[idx + 3] = 128;  /* 50% alpha */
            } else {
                fg[idx + 0] = 0;
                fg[idx + 1] = 255;
                fg[idx + 2] = 0;
                fg[idx + 3] = 255;  /* 100% alpha */
            }
        }
    }

    avx_color_alpha_blend_rgba(fg, bg, out, w, h);

    /* Verify top half (A=0): should be pure background (Blue) */
    assert(out[0] == 0 && out[1] == 0 && out[2] == 255 && out[3] == 255);

    /* Verify middle (A=128): blend of Red fg and Blue bg */
    size_t mid_idx = (30 * w + 30) * 4;
    int r = out[mid_idx + 0];
    int g = out[mid_idx + 1];
    int b = out[mid_idx + 2];
    assert(abs(r - 128) <= 2);
    assert(g == 0);
    assert(abs(b - 127) <= 2);
    assert(out[mid_idx + 3] == 255);

    /* Verify bottom half (A=255): pure foreground (Green) */
    size_t bot_idx = (50 * w + 30) * 4;
    assert(out[bot_idx + 0] == 0 && out[bot_idx + 1] == 255 && out[bot_idx + 2] == 0 && out[bot_idx + 3] == 255);

    printf("    -> Alpha blending (0%% pass-through, 50%% blend, 100%% overwrite) verified.\n");

    free(fg);
    free(bg);
    free(out);
}

static void test_live_alpha_streaming(void) {
    printf("[*] 3. Testing Live OpenAVX Stream with RGBA Alpha Channel...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 1. Create Sender transmitting RGBA frames */
    oavx_stream_config_t send_create = {
        .name = "ALPHA_GRAPHICS_OVERLAY_1",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);

    int width = 320;
    int height = 180;
    size_t frame_bytes = (size_t)width * height * 4;
    uint8_t *test_rgba = (uint8_t*)malloc(frame_bytes);
    assert(test_rgba);

    /* Upper third: fully transparent (0, 0, 0, 0)
     * Lower two thirds: semi-transparent red banner (200, 20, 20, 180)
     */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = ((size_t)y * width + x) * 4;
            if (y < 60) {
                test_rgba[idx + 0] = 0;
                test_rgba[idx + 1] = 0;
                test_rgba[idx + 2] = 0;
                test_rgba[idx + 3] = 0;
            } else {
                test_rgba[idx + 0] = 200;
                test_rgba[idx + 1] = 20;
                test_rgba[idx + 2] = 20;
                test_rgba[idx + 3] = 180;
            }
        }
    }

    oavx_video_frame_t video_frame = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_RGBA,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { test_rgba, NULL, NULL, NULL },
        .strides = { (size_t)width * 4, 0, 0, 0 },
        .framerate = { 60, 1 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 1000,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* 2. Connect Receiver requesting RGBX_RGBA */
    oavx_stream_config_t recv_create = {
        .name = "Alpha_Receiver_1",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_RGBX_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    /* Send frame */
    oavx_stream_send_video(p_send, &video_frame);

    /* Capture frame */
    oavx_video_frame_t captured_video;
    oavx_frame_type_t frame_type = oavx_stream_receive(p_recv, &captured_video, NULL, NULL, 1000);
    assert(frame_type == OAVX_FRAME_TYPE_VIDEO);
    assert(captured_video.fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA);
    assert(captured_video.width == (uint32_t)width);
    assert(captured_video.height == (uint32_t)height);
    assert(captured_video.planes[0] != NULL);

    /* Check alpha values are preserved across network transmission */
    assert(captured_video.planes[0][3] == 0); /* Top pixel alpha = 0 */
    size_t banner_idx = ((size_t)100 * width + 50) * 4;
    assert(captured_video.planes[0][banner_idx + 0] == 200);
    assert(captured_video.planes[0][banner_idx + 1] == 20);
    assert(captured_video.planes[0][banner_idx + 2] == 20);
    assert(captured_video.planes[0][banner_idx + 3] == 180); /* Alpha preserved! */

    oavx_stream_release_video(p_recv, &captured_video);
    oavx_stream_close(p_recv);

    /* 3. Connect Receiver requesting BGRX_BGRA to test channel swapping negotiation */
    recv_create.color_format = OAVX_COLOR_FORMAT_BGRX_BGRA;
    recv_create.name = "BGRA_Receiver_2";
    p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    oavx_stream_send_video(p_send, &video_frame);
    frame_type = oavx_stream_receive(p_recv, &captured_video, NULL, NULL, 1000);
    assert(frame_type == OAVX_FRAME_TYPE_VIDEO);
    assert(captured_video.fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA);
    assert(captured_video.strides[0] == (size_t)width * 4);

    /* Verify swapped Red and Blue in BGRA output */
    assert(captured_video.planes[0][banner_idx + 0] == 20);  /* Blue (swapped) */
    assert(captured_video.planes[0][banner_idx + 1] == 20);  /* Green */
    assert(captured_video.planes[0][banner_idx + 2] == 200); /* Red (swapped) */
    assert(captured_video.planes[0][banner_idx + 3] == 180); /* Alpha */

    oavx_stream_release_video(p_recv, &captured_video);
    oavx_stream_close(p_recv);

    /* 4. Connect Receiver requesting UYVY_RGBA (adaptive: keep RGBA when alpha present) */
    recv_create.color_format = OAVX_COLOR_FORMAT_UYVY_RGBA;
    recv_create.name = "Adaptive_Receiver_3";
    p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    oavx_stream_send_video(p_send, &video_frame);
    frame_type = oavx_stream_receive(p_recv, &captured_video, NULL, NULL, 1000);
    assert(frame_type == OAVX_FRAME_TYPE_VIDEO);
    /* Since source is RGBA (has alpha), UYVY_RGBA keeps it in RGBA */
    assert(captured_video.fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA);
    assert(captured_video.planes[0][banner_idx + 3] == 180);

    oavx_stream_release_video(p_recv, &captured_video);
    oavx_stream_close(p_recv);

    free(test_rgba);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    printf("    -> Live streaming negotiation (RGBA, BGRA, Adaptive UYVY_RGBA) verified.\n");
}

static void test_nv12_i420_conversions(void) {
    printf("[*] 4. Testing NV12 & I420 Semi-Planar Conversions & Multi-Plane Packing...\n");

    int w = 8, h = 4;
    size_t y_bytes = (size_t)w * h;
    size_t uv_bytes = ((size_t)w * h) / 2;
    size_t u_bytes = ((size_t)w * h) / 4;
    size_t rgba_bytes = (size_t)w * h * 4;

    uint8_t nv12_y[32];
    uint8_t nv12_uv[16];
    uint8_t i420_u[8];
    uint8_t i420_v[8];
    uint8_t uyvy_out[64];
    uint8_t rgba_out[128];
    uint8_t nv12_y_rt[32];
    uint8_t nv12_uv_rt[16];
    uint8_t i420_u_rt[8];
    uint8_t i420_v_rt[8];

    for (int i = 0; i < 32; ++i) nv12_y[i] = (uint8_t)(100 + i * 4);
    for (int i = 0; i < 8; ++i) {
        nv12_uv[2 * i + 0] = (uint8_t)(80 + i * 10);  /* U */
        nv12_uv[2 * i + 1] = (uint8_t)(160 + i * 10); /* V */
        i420_u[i] = nv12_uv[2 * i + 0];
        i420_v[i] = nv12_uv[2 * i + 1];
    }

    /* 1. NV12 -> UYVY */
    avx_color_nv12_to_uyvy(nv12_y, w, nv12_uv, w, uyvy_out, w * 2, w, h);
    /* Verify pixel 0 (row 0, col 0) */
    assert(uyvy_out[0] == nv12_uv[0]); /* U */
    assert(uyvy_out[1] == nv12_y[0]);  /* Y0 */
    assert(uyvy_out[2] == nv12_uv[1]); /* V */
    assert(uyvy_out[3] == nv12_y[1]);  /* Y1 */

    /* 2. UYVY -> NV12 roundtrip */
    avx_color_uyvy_to_nv12(uyvy_out, w * 2, nv12_y_rt, w, nv12_uv_rt, w, w, h);
    assert(memcmp(nv12_y, nv12_y_rt, y_bytes) == 0);
    assert(memcmp(nv12_uv, nv12_uv_rt, uv_bytes) == 0);
    printf("    -> NV12 <-> UYVY bit-exact conversion roundtrip verified.\n");

    /* 3. I420 -> UYVY */
    memset(uyvy_out, 0, sizeof(uyvy_out));
    avx_color_i420_to_uyvy(nv12_y, w, i420_u, w / 2, i420_v, w / 2, uyvy_out, w * 2, w, h);
    assert(uyvy_out[0] == i420_u[0]);
    assert(uyvy_out[1] == nv12_y[0]);
    assert(uyvy_out[2] == i420_v[0]);
    assert(uyvy_out[3] == nv12_y[1]);

    /* 4. UYVY -> I420 roundtrip */
    avx_color_uyvy_to_i420(uyvy_out, w * 2, nv12_y_rt, w, i420_u_rt, w / 2, i420_v_rt, w / 2, w, h);
    assert(memcmp(nv12_y, nv12_y_rt, y_bytes) == 0);
    assert(memcmp(i420_u, i420_u_rt, u_bytes) == 0);
    assert(memcmp(i420_v, i420_v_rt, u_bytes) == 0);
    printf("    -> I420 <-> UYVY bit-exact conversion roundtrip verified.\n");

    /* 5. NV12 -> RGBA & BGRA & I420 -> RGBA direct conversions */
    avx_color_nv12_to_rgba(nv12_y, w, nv12_uv, w, rgba_out, w * 4, w, h);
    assert(rgba_out[3] == 255); /* Alpha channel opaque */

    uint8_t bgra_out[256]; /* Extra stride capacity */
    memset(bgra_out, 0xA5, sizeof(bgra_out));
    size_t padded_bgra_stride = (size_t)w * 4 + 16;
    avx_color_nv12_to_bgra(nv12_y, w, nv12_uv, w, bgra_out, padded_bgra_stride, w, h);
    for (int py = 0; py < h; ++py) {
        for (int px = 0; px < w; ++px) {
            const uint8_t *rp = rgba_out + ((size_t)py * w + px) * 4;
            const uint8_t *bp = bgra_out + (size_t)py * padded_bgra_stride + (size_t)px * 4;
            assert(bp[0] == rp[2]);
            assert(bp[1] == rp[1]);
            assert(bp[2] == rp[0]);
            assert(bp[3] == 255);
        }
    }

    uint8_t rgba_out_i420[128];
    avx_color_i420_to_rgba(nv12_y, w, i420_u, w / 2, i420_v, w / 2, rgba_out_i420, w * 4, w, h);
    assert(memcmp(rgba_out, rgba_out_i420, rgba_bytes) == 0);
    printf("    -> NV12 (RGBA + single-pass strided BGRA) & I420 direct color conversions verified.\n");

    /* 6. YV12 (Y + V + U) conversions */
    uint8_t yv12_v_rt[8], yv12_u_rt[8];
    avx_color_uyvy_to_yv12(uyvy_out, w * 2, nv12_y_rt, w, yv12_v_rt, w / 2, yv12_u_rt, w / 2, w, h);
    assert(memcmp(yv12_v_rt, i420_v, u_bytes) == 0);
    assert(memcmp(yv12_u_rt, i420_u, u_bytes) == 0);

    uint8_t uyvy_yv12[64];
    avx_color_yv12_to_uyvy(nv12_y, w, i420_v, w / 2, i420_u, w / 2, uyvy_yv12, w * 2, w, h);
    assert(memcmp(uyvy_out, uyvy_yv12, sizeof(uyvy_out)) == 0);

    uint8_t rgba_out_yv12[128];
    avx_color_yv12_to_rgba(nv12_y, w, i420_v, w / 2, i420_u, w / 2, rgba_out_yv12, w * 4, w, h);
    assert(memcmp(rgba_out, rgba_out_yv12, rgba_bytes) == 0);
    printf("    -> YV12 (swapped chroma) UYVY and RGBA conversions verified.\n");

    /* 7. Direct RGBA -> NV12, RGBA -> I420, RGBA -> YV12 */
    uint8_t src_rgba[128];
    /* Initialize 2x2 pixel blocks (quads matching 4:2:0 chroma subsampling blocks) */
    for (int y2 = 0; y2 < h / 2; ++y2) {
        for (int x2 = 0; x2 < w / 2; ++x2) {
            int block = y2 * (w / 2) + x2;
            uint8_t r = (uint8_t)(60 + block * 15);
            uint8_t g = (uint8_t)(80 + block * 10);
            uint8_t b = (uint8_t)(50 + block * 12);
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int px = x2 * 2 + dx;
                    int py = y2 * 2 + dy;
                    int idx = py * w + px;
                    src_rgba[4 * idx + 0] = r;
                    src_rgba[4 * idx + 1] = g;
                    src_rgba[4 * idx + 2] = b;
                    src_rgba[4 * idx + 3] = 255;
                }
            }
        }
    }

    uint8_t direct_nv12_y[32], direct_nv12_uv[16];
    avx_color_rgba_to_nv12(src_rgba, w * 4, direct_nv12_y, w, direct_nv12_uv, w, w, h);

    /* Verify roundtrip from NV12 back to RGBA */
    uint8_t rt_rgba[128];
    avx_color_nv12_to_rgba(direct_nv12_y, w, direct_nv12_uv, w, rt_rgba, w * 4, w, h);
    for (int i = 0; i < 32; ++i) {
        assert(abs((int)src_rgba[4 * i + 0] - (int)rt_rgba[4 * i + 0]) <= 2);
        assert(abs((int)src_rgba[4 * i + 1] - (int)rt_rgba[4 * i + 1]) <= 2);
        assert(abs((int)src_rgba[4 * i + 2] - (int)rt_rgba[4 * i + 2]) <= 2);
        assert(rt_rgba[4 * i + 3] == 255);
    }

    uint8_t direct_i420_y[32], direct_i420_u[8], direct_i420_v[8];
    avx_color_rgba_to_i420(src_rgba, w * 4, direct_i420_y, w, direct_i420_u, w / 2, direct_i420_v, w / 2, w, h);
    assert(memcmp(direct_nv12_y, direct_i420_y, y_bytes) == 0);
    for (int i = 0; i < 8; ++i) {
        assert(direct_i420_u[i] == direct_nv12_uv[2 * i + 0]);
        assert(direct_i420_v[i] == direct_nv12_uv[2 * i + 1]);
    }

    uint8_t direct_yv12_y[32], direct_yv12_v[8], direct_yv12_u[8];
    avx_color_rgba_to_yv12(src_rgba, w * 4, direct_yv12_y, w, direct_yv12_v, w / 2, direct_yv12_u, w / 2, w, h);
    assert(memcmp(direct_i420_y, direct_yv12_y, y_bytes) == 0);
    assert(memcmp(direct_i420_u, direct_yv12_u, u_bytes) == 0);
    assert(memcmp(direct_i420_v, direct_yv12_v, u_bytes) == 0);
    printf("    -> Direct RGBA -> NV12, I420, and YV12 conversions verified (error <= 2).\n");
}

static void test_live_multiplane_separate_buffers(void) {
    printf("[*] 5. Testing Multi-Plane Separate Buffer Streaming (UYVA & NV12 Padded Strides)...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t tx_cfg = {
        .name = "MULTIPLANAR_SENDER",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    assert(tx != NULL);

    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
    assert(ep && ep->address);

    oavx_stream_config_t rx_cfg = {
        .name = "MULTIPLANAR_RECEIVER",
        .endpoint = ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_FASTEST,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    assert(rx != NULL);

    usleep(50000);

    /* 1. Transmit UYVA frame with completely separate malloc allocations & padded stride */
    uint32_t w = 160;
    uint32_t h = 90;
    size_t padded_stride_yuv = (size_t)w * 2 + 64; /* Padded stride */
    size_t padded_stride_alpha = (size_t)w + 32;

    uint8_t *uyvy_plane = (uint8_t*)malloc(padded_stride_yuv * h);
    uint8_t *alpha_plane = (uint8_t*)malloc(padded_stride_alpha * h);
    assert(uyvy_plane && alpha_plane);

    /* Fill pattern */
    for (uint32_t y = 0; y < h; ++y) {
        memset(uyvy_plane + y * padded_stride_yuv, (int)(y & 0xFF), (size_t)w * 2);
        memset(alpha_plane + y * padded_stride_alpha, 210, (size_t)w);
    }

    oavx_video_frame_t tx_uyva = {
        .width = w,
        .height = h,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVA,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { uyvy_plane, alpha_plane, NULL, NULL },
        .strides = { padded_stride_yuv, padded_stride_alpha, 0, 0 },
        .framerate = { 60, 1 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 5000000
    };

    assert(oavx_stream_send_video(tx, &tx_uyva) == OAVX_SUCCESS);

    oavx_video_frame_t rx_v = {0};
    oavx_frame_type_t ft = oavx_stream_receive(rx, &rx_v, NULL, NULL, 500);
    assert(ft == OAVX_FRAME_TYPE_VIDEO);
    assert(rx_v.width == w && rx_v.height == h);
    assert(rx_v.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA);
    assert(rx_v.planes[0] != NULL);
    assert(rx_v.planes[1] != NULL);
    assert(rx_v.planes[1] == rx_v.planes[0] + (size_t)w * h * 2);
    /* Verify reconstructed alpha */
    assert(rx_v.planes[1][0] == 210);
    assert(rx_v.planes[1][w * h - 1] == 210);
    oavx_stream_release_video(rx, &rx_v);

    free(uyvy_plane);
    free(alpha_plane);
    oavx_stream_close(rx);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
    printf("    -> Multi-plane separate buffer transmission with stride padding verified.\n");
}

static void test_bt709_hd_color_and_420(void) {
    printf("[6] Testing BT.709 HD Color Matrix & 4:2:0 Roundtrips...\n");

    const int w = 1280;
    const int h = 720;
    const size_t px = (size_t)w * (size_t)h;
    uint8_t *rgba_in  = (uint8_t*)malloc(px * 4);
    uint8_t *rgba_out = (uint8_t*)malloc(px * 4);
    uint8_t *uyvy     = (uint8_t*)malloc(px * 2);
    uint8_t *y_buf    = (uint8_t*)malloc(px);
    uint8_t *uv_buf   = (uint8_t*)malloc(px / 2);
    uint8_t *u_buf    = (uint8_t*)malloc(px / 4);
    uint8_t *v_buf    = (uint8_t*)malloc(px / 4);
    assert(rgba_in && rgba_out && uyvy && y_buf && uv_buf && u_buf && v_buf);

    /* Pure red HD frame: BT.709 produces Y=63, U=102, V=240 (vs BT.601 Y=81) */
    for (size_t i = 0; i < px; ++i) {
        rgba_in[4 * i + 0] = 255;
        rgba_in[4 * i + 1] = 0;
        rgba_in[4 * i + 2] = 0;
        rgba_in[4 * i + 3] = 255;
    }

    avx_color_rgba_to_uyvy(rgba_in, uyvy, w, h);
    assert(uyvy[0] == 102); /* U */
    assert(uyvy[1] == 63);  /* Y (BT.709, not BT.601 81) */
    assert(uyvy[2] == 240); /* V */
    assert(uyvy[3] == 63);

    avx_color_uyvy_to_rgba(uyvy, rgba_out, w, h);
    assert(abs((int)rgba_out[0] - 255) <= 3);
    assert(rgba_out[1] <= 3);
    assert(rgba_out[2] <= 3);
    assert(rgba_out[3] == 255);

    /* Neutral gray must have zero chroma drift */
    memset(rgba_in, 128, px * 4);
    avx_color_rgba_to_uyvy(rgba_in, uyvy, w, h);
    assert(uyvy[0] == 128 && uyvy[2] == 128);

    /* HD landscape (1280x720) & HD portrait (720x1280) NV12 + I420 roundtrips */
    const int dims[2][2] = { { 1280, 720 }, { 720, 1280 } };
    for (int d = 0; d < 2; ++d) {
        int dw = dims[d][0], dh = dims[d][1];
        size_t dpx = (size_t)dw * (size_t)dh;
        for (size_t i = 0; i < dpx; ++i) {
            rgba_in[4 * i + 0] = 255;
            rgba_in[4 * i + 1] = 0;
            rgba_in[4 * i + 2] = 0;
            rgba_in[4 * i + 3] = 255;
        }

        avx_color_rgba_to_nv12(rgba_in, (size_t)dw * 4, y_buf, (size_t)dw, uv_buf, (size_t)dw, dw, dh);
        assert(y_buf[0] == 63);
        avx_color_nv12_to_rgba(y_buf, (size_t)dw, uv_buf, (size_t)dw, rgba_out, (size_t)dw * 4, dw, dh);
        assert(abs((int)rgba_out[0] - 255) <= 3);
        assert(rgba_out[1] <= 3);
        assert(rgba_out[2] <= 3);

        avx_color_rgba_to_i420(rgba_in, (size_t)dw * 4, y_buf, (size_t)dw,
                               u_buf, (size_t)dw / 2, v_buf, (size_t)dw / 2, dw, dh);
        assert(y_buf[0] == 63);
        avx_color_i420_to_rgba(y_buf, (size_t)dw, u_buf, (size_t)dw / 2, v_buf, (size_t)dw / 2,
                               rgba_out, (size_t)dw * 4, dw, dh);
        assert(abs((int)rgba_out[0] - 255) <= 3);
        assert(rgba_out[1] <= 3);
        assert(rgba_out[2] <= 3);
    }

    free(rgba_in);
    free(rgba_out);
    free(uyvy);
    free(y_buf);
    free(uv_buf);
    free(u_buf);
    free(v_buf);
    printf("    -> BT.709 HD UYVY, NV12, and I420 (landscape & portrait) verified.\n");
}

int main(void) {
    printf("\n=== OpenAVX Alpha & RGB Format Tests ===\n\n");

    test_color_math();
    test_alpha_compositing();
    test_live_alpha_streaming();
    test_nv12_i420_conversions();
    test_live_multiplane_separate_buffers();
    test_bt709_hd_color_and_420();

    printf("\n[PASS] openavx_alpha_color_test\n\n");
    return 0;
}
