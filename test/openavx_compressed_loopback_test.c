/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_compressed_loopback_test.c - End-to-end SpeedHQ-2 compressed streaming test
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "avx_platform.h"

#include "openavx.h"

#define TEST_WIDTH 1920
#define TEST_HEIGHT 1080
#define TEST_PORT 7960

typedef struct {
    oavx_stream_t *p_send;
    volatile bool running;
    volatile bool frame_sent;
    uint8_t* video_data;
} sender_thread_arg_t;

static void* sender_thread_func(void* arg) {
    sender_thread_arg_t* s_arg = (sender_thread_arg_t*)arg;

    oavx_video_frame_t video_frame = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_SHQ2, /* Tell OpenAVX to compress to SpeedHQ-2 */
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { s_arg->video_data, NULL, NULL, NULL },
        .strides = { TEST_WIDTH * 2, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 99887766,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    while (s_arg->running) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 15000000L }; /* ~60 fps cadence */
        nanosleep(&ts, NULL);
        oavx_stream_send_video(s_arg->p_send, &video_frame);
        s_arg->frame_sent = true;
    }
    return NULL;
}

int main(void) {
    printf("\n=======================================================\n");
    printf(" OpenAVX End-to-End SpeedHQ-2 Compressed Stream Test\n");
    printf("=======================================================\n\n");

    /* 1. Initialize */
    printf("[*] 1. Initializing OpenAVX...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 2. Create Sender */
    printf("[*] 2. Creating Sender with on-the-fly SpeedHQ-2 compression on port %d...\n", TEST_PORT);
    oavx_stream_config_t send_create = {
        .name = "OPENAVX_COMPRESSED_SOURCE",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);

    /* Allocate test frame: gradient UYVY pattern */
    size_t raw_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 2;
    uint8_t* raw_uyvy = malloc(raw_size);
    assert(raw_uyvy != NULL);
    for (int y = 0; y < TEST_HEIGHT; ++y) {
        uint8_t* line = raw_uyvy + (size_t)y * (TEST_WIDTH * 2);
        for (int x = 0; x < TEST_WIDTH / 2; ++x) {
            line[4 * x + 0] = (uint8_t)((x * 255) / (TEST_WIDTH / 2)); /* U */
            line[4 * x + 1] = (uint8_t)((y * 255) / TEST_HEIGHT);     /* Y0 */
            line[4 * x + 2] = 128;                                     /* V */
            line[4 * x + 3] = (uint8_t)((y * 255) / TEST_HEIGHT);     /* Y1 */
        }
    }

    sender_thread_arg_t s_arg = {
        .p_send = p_send,
        .running = true,
        .frame_sent = false,
        .video_data = raw_uyvy
    };
    avx_thread_t s_thread;
    avx_thread_create(&s_thread, sender_thread_func, &s_arg);

    /* Give sender thread time to start listening */
    avx_sleep_ms(50);

    /* 3. Test Mode A: Receiver requesting uncompressed UYVY (auto-decompressing SpeedHQ) */
    printf("[*] 3. Testing Receiver Mode A: Auto-Decompressing SpeedHQ-2 to UYVY...\n");
    oavx_stream_config_t recv_create = {
        .name = "SpeedHQ_Test_Client_A",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    oavx_video_frame_t v_frame = {0};
    oavx_audio_frame_t a_frame = {0};
    oavx_metadata_frame_t m_frame = {0};

    int attempts = 0;
    bool video_received = false;
    while (attempts++ < 50 && !video_received) {
        oavx_frame_type_t ftype = oavx_stream_receive(p_recv, &v_frame, &a_frame, &m_frame, 200);
        if (ftype == OAVX_FRAME_TYPE_VIDEO) {
            video_received = true;
            printf("    [+] Captured VIDEO frame in Mode A:\n");
            printf("        - Resolution: %u x %u\n", v_frame.width, v_frame.height);
            printf("        - FourCC:     0x%08X (%c%c%c%c)\n",
                   v_frame.fourcc,
                   (v_frame.fourcc) & 0xFF,
                   (v_frame.fourcc >> 8) & 0xFF,
                   (v_frame.fourcc >> 16) & 0xFF,
                   (v_frame.fourcc >> 24) & 0xFF);
            printf("        - Stride:     %zu bytes\n", v_frame.strides[0]);
            printf("        - PTS:        %ld ns\n", (long)v_frame.pts_ns);

            assert(v_frame.width == TEST_WIDTH);
            assert(v_frame.height == TEST_HEIGHT);
            assert(v_frame.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY);
            assert(v_frame.planes[0] != NULL);
            assert(v_frame.strides[0] == TEST_WIDTH * 2);

            oavx_stream_release_video(p_recv, &v_frame);
        }
    }
    assert(video_received);
    oavx_stream_close(p_recv);

    /* 4. Test Mode B: Receiver requesting raw compressed frames (OAVX_COLOR_FORMAT_COMPRESSED_V1) */
    printf("[*] 4. Testing Receiver Mode B: Raw Compressed Pass-Through (Zero Decode CPU)...\n");
    recv_create.color_format = OAVX_COLOR_FORMAT_COMPRESSED_V1;
    recv_create.name = "SpeedHQ_Test_Client_B";
    p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    video_received = false;
    attempts = 0;
    while (attempts++ < 50 && !video_received) {
        oavx_frame_type_t ftype = oavx_stream_receive(p_recv, &v_frame, &a_frame, &m_frame, 200);
        if (ftype == OAVX_FRAME_TYPE_VIDEO) {
            video_received = true;
            printf("    [+] Captured VIDEO frame in Mode B:\n");
            printf("        - Resolution: %u x %u\n", v_frame.width, v_frame.height);
            printf("        - FourCC:     0x%08X (%c%c%c%c)\n",
                   v_frame.fourcc,
                   (v_frame.fourcc) & 0xFF,
                   (v_frame.fourcc >> 8) & 0xFF,
                   (v_frame.fourcc >> 16) & 0xFF,
                   (v_frame.fourcc >> 24) & 0xFF);
            printf("        - Compressed Size: %zu bytes (Wire Savings: %.1f%%)\n",
                   v_frame.strides[0],
                   100.0 - (double)v_frame.strides[0] * 100.0 / (double)raw_size);

            assert(v_frame.width == TEST_WIDTH);
            assert(v_frame.height == TEST_HEIGHT);
            assert(v_frame.fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ2);
            assert(v_frame.strides[0] < raw_size / 10); /* Must be compressed by >10x */

            oavx_stream_release_video(p_recv, &v_frame);
        }
    }
    assert(video_received);

    /* 5. Cleanup */
    printf("[*] 5. Tearing down sessions...\n");
    s_arg.running = false;
    avx_thread_join(s_thread);
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);
    free(raw_uyvy);

    printf("\n[PASS] openavx_compressed_loopback_test\n\n");
    return 0;
}
