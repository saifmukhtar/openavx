/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_monitor_test.c - Automated Terminal Player & Monitor Verification with Audio
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "avx_platform.h"

#include "openavx.h"
#include "avx_internal.h"

static volatile bool s_sender_running = true;

static int run_monitor_cmd(const char* args) {
    char cmd[512];
    const char* candidates[] = {
        "./bin/openavx_monitor",
        "./bin/openavx-monitor",
        "./build/bin/openavx_monitor",
        "./build_asan/bin/openavx_monitor",
        "./openavx_monitor",
        "./openavx-monitor"
    };
    int last_ret = -1;
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(cmd, sizeof(cmd), "ASAN_OPTIONS=detect_leaks=0 %s %s > /dev/null 2>&1", candidates[i], args);
            int ret = system(cmd);
            if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) return 0;
            last_ret = ret;
        }
    }
    return last_ret;
}

static void* sender_thread(void* arg) {
    oavx_stream_t* p_send = (oavx_stream_t*)arg;
    int width = 320;
    int height = 180;
    size_t frame_bytes = (size_t)width * height * 4; /* RGBA */
    uint8_t* rgba = (uint8_t*)malloc(frame_bytes);
    if (!rgba) return NULL;

    /* Synthetic gradient pattern */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t idx = ((size_t)y * width + x) * 4;
            rgba[idx + 0] = (uint8_t)((x * 255) / width);       /* R */
            rgba[idx + 1] = (uint8_t)((y * 255) / height);      /* G */
            rgba[idx + 2] = (uint8_t)(255 - (x * 255) / width); /* B */
            rgba[idx + 3] = 255;                                 /* A */
        }
    }

    oavx_video_frame_t vf = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_RGBA,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { rgba, NULL, NULL, NULL },
        .strides = { (size_t)width * 4, 0, 0, 0 },
        .framerate = { 30, 1 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 100,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* 48000 Hz, stereo 1024 samples per frame (~21.3ms) */
    int sample_rate = 48000;
    int channels = 2;
    int frames = 1024;
    float* audio_buf = (float*)malloc((size_t)channels * frames * sizeof(float));
    if (!audio_buf) {
        free(rgba);
        return NULL;
    }

    /* 440 Hz test sine wave */
    for (int s = 0; s < frames; ++s) {
        float sample = (float)sin(2.0 * M_PI * 440.0 * (double)s / (double)sample_rate) * 0.3f;
        audio_buf[s] = sample;                          /* Left channel */
        audio_buf[frames + s] = sample;                 /* Right channel */
    }

    oavx_audio_frame_t af = {
        .sample_rate = sample_rate,
        .channels = channels,
        .frames = frames,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .planes = { audio_buf, audio_buf + frames, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = frames * sizeof(float),
        .pts_ns = 100,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    while (s_sender_running) {
        vf.pts_ns += 33333333;
        af.pts_ns = vf.pts_ns;
        oavx_stream_send_video(p_send, &vf);
        oavx_stream_send_audio(p_send, &af);
        usleep(33000); /* ~30 fps */
    }

    free(rgba);
    free(audio_buf);
    return NULL;
}

int main(void) {
    printf("\n==========================================================\n");
    printf(" OpenAVX Standalone Terminal Player & Audio Monitor Test\n");
    printf("==========================================================\n\n");

    /* 1. Test CLI Help Banner */
    printf("[*] 1. Testing CLI Help Banner (--help)...\n");
    int ret_help = run_monitor_cmd("--help");
    assert(ret_help == 0);
    printf("    [+] Help flag on openavx_monitor returned exit code 0.\n");

    /* 2. Initialize openavx context & Start Test Sender */
    printf("[*] 2. Spawning Live A/V Test Streamer on port 7960...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t send_cfg = {
        .name = "MONITOR_AV_CAM",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_cfg);
    assert(p_send != NULL);

    uint16_t port = 7960;
    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(p_send);
    if (ep && ep->address) {
        const char *colon = strchr(ep->address, ':');
        if (colon) port = (uint16_t)atoi(colon + 1);
    }
    char url[64];
    snprintf(url, sizeof(url), "127.0.0.1:%u", port);
    printf("    [+] Test A/V stream online at %s\n", url);

    pthread_t th;
    s_sender_running = true;
    pthread_create(&th, NULL, sender_thread, p_send);
    usleep(100000); /* 100ms for sender to start */

    /* 3. Test TrueColor Half-Block Rendering with Audio Backend: Auto */
    printf("[*] 3. Testing TrueColor Monitor with Audio Backend (auto)...\n");
    char args[256];
    snprintf(args, sizeof(args), "--url %s --frames 4 -W 40 -H 10 --audio auto", url);
    int ret_auto = run_monitor_cmd(args);
    assert(ret_auto == 0);
    printf("    [+] Monitor with auto audio backend executed successfully (exit 0).\n");

    /* 4. Test Native PulseAudio Backend */
    printf("[*] 4. Testing Native PulseAudio Backend (--audio pulse)...\n");
    snprintf(args, sizeof(args), "--url %s --frames 4 -W 40 -H 10 --audio pulse", url);
    int ret_pulse = run_monitor_cmd(args);
    assert(ret_pulse == 0);
    printf("    [+] PulseAudio live audio backend executed successfully (exit 0).\n");

    /* 5. Test Native ALSA Backend */
    printf("[*] 5. Testing Native ALSA Backend (--audio alsa)...\n");
    snprintf(args, sizeof(args), "--url %s --frames 4 -W 40 -H 10 --audio alsa", url);
    int ret_alsa = run_monitor_cmd(args);
    assert(ret_alsa == 0);
    printf("    [+] ALSA live audio backend executed successfully (exit 0).\n");

    /* 6. Test Muted Audio Mode */
    printf("[*] 6. Testing Muted Audio Mode (--no-audio)...\n");
    snprintf(args, sizeof(args), "--url %s --frames 4 -W 40 -H 10 --no-audio", url);
    int ret_muted = run_monitor_cmd(args);
    assert(ret_muted == 0);
    printf("    [+] Muted mode executed successfully (exit 0).\n");

    /* 7. Test Unicode Braille 2x4 Dot Matrix Mode */
    printf("[*] 7. Testing Unicode Braille 2x4 Dot Matrix Mode (--braille)...\n");
    snprintf(args, sizeof(args), "--url %s --frames 4 -W 40 -H 10 --braille", url);
    int ret_braille = run_monitor_cmd(args);
    assert(ret_braille == 0);
    printf("    [+] Braille matrix mode executed successfully (exit 0).\n");

    /* 8. Teardown */
    printf("[*] 8. Tearing down Test Streamer...\n");
    s_sender_running = false;
    pthread_join(th, NULL);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_monitor_test\n\n");
    return 0;
}
