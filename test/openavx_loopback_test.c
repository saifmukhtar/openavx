/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_loopback_test.c - Basic Loopback Integration Test
 */

#include "openavx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "avx_platform.h"

typedef struct {
    oavx_stream_t* p_send;
    volatile bool running;
    size_t frame_bytes;
    uint8_t* p_vframe;
    size_t audio_bytes;
    float* p_aframe;
} sender_thread_arg_t;

static void* sender_worker(void* arg) {
    sender_thread_arg_t* sa = (sender_thread_arg_t*)arg;

    oavx_video_frame_t video_out = {
        .width = 1920,
        .height = 1080,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { sa->p_vframe, NULL, NULL, NULL },
        .strides = { 1920 * 2, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 12345678,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    oavx_audio_frame_t audio_out = {
        .sample_rate = 48000,
        .channels = 2,
        .frames = 1024,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .planes = { sa->p_aframe, sa->p_aframe + 1024, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = 1024 * sizeof(float),
        .pts_ns = 87654321,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    while (sa->running) {
        /* Only transmit if receiver is connected */
        if (oavx_stream_get_connection_count(sa->p_send, 0) > 0) {
            oavx_stream_send_video(sa->p_send, &video_out);
            oavx_stream_send_audio(sa->p_send, &audio_out);
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 16666666L }; /* ~60 FPS */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(void) {
    printf("==========================================\n");
    printf(" OpenAVX End-to-End Loopback Stream Test (oavx_*)\n");
    printf("==========================================\n\n");

    printf("[*] 1. Initializing OpenAVX Context Engine...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    printf("[*] 2. Creating Sender on port 7960...\n");
    oavx_stream_config_t send_settings = {
        .name = "OpenAVX (Loopback Stream)",
        .groups = NULL,
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_settings);
    assert(p_send != NULL);
    printf("[+] Sender stream created!\n");

    /* Allocate payload buffers */
    size_t frame_bytes = (size_t)1920 * 1080 * 2;
    uint8_t* p_vframe = (uint8_t*)malloc(frame_bytes);
    assert(p_vframe != NULL);
    memset(p_vframe, 0xAA, frame_bytes); /* Fill with known signature 0xAA */

    size_t audio_bytes = (size_t)2 * 1024 * sizeof(float);
    float* p_aframe = (float*)malloc(audio_bytes);
    assert(p_aframe != NULL);
    for (int i = 0; i < 2 * 1024; ++i) {
        p_aframe[i] = 0.5f;
    }

    sender_thread_arg_t sarg = {
        .p_send = p_send,
        .running = true,
        .frame_bytes = frame_bytes,
        .p_vframe = p_vframe,
        .audio_bytes = audio_bytes,
        .p_aframe = p_aframe
    };

    avx_thread_t sthread;
    int thread_res = avx_thread_create(&sthread, sender_worker, &sarg);
    assert(thread_res == 0);

    /* Give sender server time to spin up */
    avx_sleep_ms(100);

    const oavx_endpoint_t* p_src_info = oavx_stream_get_endpoint(p_send);
    const char *addr = (p_src_info && p_src_info->address) ? p_src_info->address : "127.0.0.1:7960";

    printf("[*] 3. Creating Receiver and connecting to %s...\n", addr);
    oavx_stream_config_t recv_settings = {
        .name = "OpenAVX (Loopback Receiver)",
        .endpoint = addr,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_BGRA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_settings);
    assert(p_recv != NULL);
    printf("[+] Receiver stream created and connected!\n");

    int connections = oavx_stream_get_connection_count(p_send, 100);
    printf("[+] Active connections on sender: %d (Expected: >= 1)\n", connections);
    assert(connections >= 1);

    printf("[*] 4. Capturing Video and Audio Frames on Receiver via oavx_stream_receive...\n");
    oavx_video_frame_t video_in;
    oavx_audio_frame_t audio_in;
    oavx_metadata_frame_t meta_in;

    bool got_video = false;
    bool got_audio = false;

    for (int attempts = 0; attempts < 100 && (!got_video || !got_audio); ++attempts) {
        oavx_frame_type_t ftype = oavx_stream_receive(p_recv, &video_in, &audio_in, &meta_in, 100);
        if (ftype == OAVX_FRAME_TYPE_VIDEO && !got_video) {
            printf("[+] Captured VIDEO frame:\n");
            printf("    - Resolution: %u x %u\n", video_in.width, video_in.height);
            printf("    - FourCC:     0x%08X (UYVY)\n", video_in.fourcc);
            printf("    - Framerate:  %u / %u\n", video_in.framerate.num, video_in.framerate.den);
            printf("    - PTS:        %ld ns\n", (long)video_in.pts_ns);
            assert(video_in.width == 1920);
            assert(video_in.height == 1080);
            assert(video_in.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY);
            assert(video_in.planes[0] != NULL);
            assert(video_in.planes[0][0] == 0xAA);
            assert(video_in.planes[0][frame_bytes - 1] == 0xAA);
            printf("[+] Video frame data integrity verified (0xAA signature matched)!\n");
            oavx_stream_release_video(p_recv, &video_in);
            got_video = true;
        } else if (ftype == OAVX_FRAME_TYPE_AUDIO && !got_audio) {
            float* samples = (float*)audio_in.planes[0];
            printf("[+] Captured AUDIO frame:\n");
            printf("    - Sample Rate: %u Hz\n", audio_in.sample_rate);
            printf("    - Channels:    %u\n", audio_in.channels);
            printf("    - Frames:      %u\n", audio_in.frames);
            printf("    - Sample Val:  %.2f\n", samples[0]);
            assert(audio_in.sample_rate == 48000);
            assert(audio_in.channels == 2);
            assert(audio_in.frames == 1024);
            assert(samples[0] == 0.5f);
            printf("[+] Audio frame data integrity verified (0.5f float matched)!\n");
            oavx_stream_release_audio(p_recv, &audio_in);
            got_audio = true;
        }
    }

    assert(got_video);
    assert(got_audio);

    printf("[*] 5. Performance Statistics...\n");
    oavx_performance_t perf, dropped;
    oavx_stream_get_performance(p_recv, &perf, &dropped);
    printf("[+] Receiver performance: %ld video frames, %ld audio frames\n",
           (long)perf.video_frames, (long)perf.audio_frames);
    assert(perf.video_frames >= 1);
    assert(perf.audio_frames >= 1);

    printf("[*] 6. Tearing down Stream Sessions...\n");
    sarg.running = false;
    avx_thread_join(sthread);

    free(p_vframe);
    free(p_aframe);

    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_loopback_test\n");
    return 0;
}
