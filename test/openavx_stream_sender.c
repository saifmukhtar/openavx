/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_stream_sender.c - Test Stream Sender Utility
 */

#include "openavx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

int main(void) {
    printf("[*] Starting OpenAVX Stream Sender...\n");
    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        fprintf(stderr, "[-] Failed to initialize OpenAVX context\n");
        return 1;
    }

    oavx_stream_config_t config = {
        .name = "OpenAVX (Probe Stream)",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };

    oavx_stream_t *p_send = oavx_stream_open(ctx, &config);
    if (!p_send) {
        fprintf(stderr, "[-] Failed to create OpenAVX stream transmitter\n");
        oavx_context_destroy(ctx);
        return 1;
    }
    printf("[+] OpenAVX Sender online on port 7960!\n");

    /* Allocate a 1920x1080 UYVY test pattern (2 bytes per pixel) */
    int xres = 1920;
    int yres = 1080;
    size_t frame_size = (size_t)xres * yres * 2;
    uint8_t* p_frame = (uint8_t*)malloc(frame_size);
    if (!p_frame) {
        oavx_stream_close(p_send);
        oavx_context_destroy(ctx);
        return 1;
    }

    /* Fill with test pattern: Y=128, U=128, V=128 (Neutral grey) */
    for (size_t i = 0; i < frame_size; i += 4) {
        p_frame[i + 0] = 0x80; /* U */
        p_frame[i + 1] = 0xeb; /* Y0 (white bar) */
        p_frame[i + 2] = 0x80; /* V */
        p_frame[i + 3] = 0xeb; /* Y1 */
    }

    oavx_video_frame_t video_frame = {
        .width = (uint32_t)xres,
        .height = (uint32_t)yres,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { p_frame, NULL, NULL, NULL },
        .strides = { (size_t)xres * 2, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 1000000,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* Allocate a 48kHz stereo test tone buffer */
    float audio_buf[2 * 1024];
    for (int i = 0; i < 2 * 1024; ++i) audio_buf[i] = 0.5f;
    oavx_audio_frame_t audio_frame = {
        .sample_rate = 48000,
        .channels = 2,
        .frames = 1024,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .planes = { audio_buf, audio_buf + 1024, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = 1024 * sizeof(float),
        .pts_ns = 1000000,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    printf("[*] Streaming video and audio frames for 3 seconds...\n");
    for (int frame_idx = 1; frame_idx <= 60; ++frame_idx) {
        video_frame.pts_ns = frame_idx * 16666666LL;
        oavx_stream_send_video(p_send, &video_frame);
        
        audio_frame.pts_ns = frame_idx * 16666666LL;
        oavx_stream_send_audio(p_send, &audio_frame);
        
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 16666666L }; /* ~60 FPS */
        nanosleep(&ts, NULL);
    }

    printf("[+] Finished streaming 60 frames.\n");
    free(p_frame);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);
    printf("[+] Sender shutdown cleanly.\n");
    return 0;
}
