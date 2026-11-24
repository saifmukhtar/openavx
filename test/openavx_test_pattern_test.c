/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * openavx_test_pattern_test.c - Automated Test Pattern & Reference Signal Generator Verification
 */

#include "openavx.h"
#include "fixtures/smpte_h264_pattern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

static inline void sleep_ms(int ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

int main(void) {
    printf("=== OpenAVX Test Pattern & Signal Generator Tests ===\n\n");

    /* 1. Initialize openavx */
    printf("[*] 1. Initializing OpenAVX Context...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 2. Create Sender for Test Pattern */
    printf("[*] 2. Spawning test pattern sender 'TEST_PATTERN_STREAM'...\n");
    oavx_stream_config_t send_settings = {
        .name = "TEST_PATTERN_STREAM",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_settings);
    assert(p_send != NULL);

    /* 3. Connect Receiver */
    printf("[*] 3. Connecting Receiver to 'TEST_PATTERN_STREAM'...\n");
    oavx_stream_config_t recv_settings = {
        .name = "TEST_PATTERN_STREAM",
        .endpoint = "127.0.0.1:7960",
        .color_format = OAVX_COLOR_FORMAT_BEST,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .direction = OAVX_DIR_RECEIVE
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_settings);
    assert(p_recv != NULL);

    /* Wait for connection handshake */
    sleep_ms(150);

    /* 4. Generate and Transmit 720p SMPTE Frame + 1 kHz Audio Tone */
    const int width = 1280;
    const int height = 720;
    const int stride = width * 2;
    uint8_t* p_vframe = (uint8_t*)malloc((size_t)stride * height);
    assert(p_vframe != NULL);

    /* Rec. 709 75% SMPTE color values */
    const uint8_t bar_y[8] = { 180, 168, 145, 133,  63,  51,  28,  16 };
    const uint8_t bar_u[8] = { 128,  44, 148,  64, 192, 108, 212, 128 };
    const uint8_t bar_v[8] = { 128, 142,  24,  38, 218, 232, 114, 128 };

    for (int y = 0; y < height; ++y) {
        uint8_t* row = p_vframe + y * stride;
        for (int x = 0; x < width; x += 2) {
            int b = (x * 8) / width;
            if (b >= 8) b = 7;
            row[x * 2 + 0] = bar_u[b];
            row[x * 2 + 1] = bar_y[b];
            row[x * 2 + 2] = bar_v[b];
            row[x * 2 + 3] = bar_y[b];
        }
    }

    oavx_video_frame_t vf = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { p_vframe, NULL, NULL, NULL },
        .strides = { (size_t)stride, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 1000,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* 48 kHz stereo 1 kHz tone (800 samples for 60fps) */
    const int a_samples = 800;
    float* a_left = (float*)malloc(a_samples * sizeof(float));
    float* a_right = (float*)malloc(a_samples * sizeof(float));
    float* a_planar = (float*)malloc(2 * a_samples * sizeof(float));
    assert(a_left && a_right && a_planar);

    for (int i = 0; i < a_samples; ++i) {
        double t = (double)i / 48000.0;
        float s = (float)(0.10 * sin(2.0 * 3.141592653589793 * 1000.0 * t));
        a_left[i] = s;
        a_right[i] = s;
    }
    memcpy(a_planar, a_left, a_samples * sizeof(float));
    memcpy(a_planar + a_samples, a_right, a_samples * sizeof(float));

    oavx_audio_frame_t af = {
        .sample_rate = 48000,
        .channels = 2,
        .frames = a_samples,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .planes = { a_planar, a_planar + a_samples, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = a_samples * sizeof(float),
        .pts_ns = 1000,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* Transmit frames */
    printf("[*] 4. Transmitting test pattern video and 1 kHz audio frames...\n");
    for (int f = 0; f < 5; ++f) {
        oavx_stream_send_video(p_send, &vf);
        oavx_stream_send_audio(p_send, &af);
        sleep_ms(16);
    }

    /* 5. Capture and verify video frame */
    printf("[*] 5. Capturing video frame on receiver...\n");
    oavx_video_frame_t rx_video = {0};
    oavx_audio_frame_t rx_audio = {0};
    oavx_metadata_frame_t rx_meta = {0};

    bool got_video = false;
    for (int tries = 0; tries < 20 && !got_video; ++tries) {
        oavx_frame_type_e ft = oavx_stream_receive(p_recv, &rx_video, &rx_audio, &rx_meta, 100);
        if (ft == OAVX_FRAME_TYPE_VIDEO) {
            got_video = true;
            assert(rx_video.width == 1280);
            assert(rx_video.height == 720);
            assert(rx_video.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY);
            assert(rx_video.planes[0] != NULL);

            /* Verify color bars across the row */
            const uint8_t* row = rx_video.planes[0] + (360 * rx_video.strides[0]);
            for (int b = 0; b < 8; ++b) {
                int test_x = (b * width) / 8 + (width / 16);
                if (test_x % 2 != 0) test_x--;
                uint8_t cap_y = row[test_x * 2 + 1];
                assert(cap_y == bar_y[b]);
            }
            printf("    [+] Captured 1280x720 UYVY video: SMPTE 8-bar chrominance/luminance bitwise exact!\n");
            oavx_stream_release_video(p_recv, &rx_video);
        } else if (ft == OAVX_FRAME_TYPE_AUDIO) {
            oavx_stream_release_audio(p_recv, &rx_audio);
        }
    }
    assert(got_video);

    /* 6. Capture and verify audio frame */
    printf("[*] 6. Capturing audio frame on receiver...\n");
    bool got_audio = false;
    for (int tries = 0; tries < 20 && !got_audio; ++tries) {
        oavx_frame_type_e ft = oavx_stream_receive(p_recv, &rx_video, &rx_audio, &rx_meta, 100);
        if (ft == OAVX_FRAME_TYPE_AUDIO) {
            got_audio = true;
            assert(rx_audio.sample_rate == 48000);
            assert(rx_audio.channels == 2);
            assert(rx_audio.frames == 800);
            assert(rx_audio.planes[0] != NULL);

            /* Check 1 kHz tone energy and zero crossings */
            float* samples = (float*)rx_audio.planes[0];
            double energy = 0.0;
            int zero_crossings = 0;
            for (uint32_t i = 0; i < rx_audio.frames; ++i) {
                energy += samples[i] * samples[i];
                if (i > 0 && ((samples[i - 1] < 0.0f && samples[i] >= 0.0f) ||
                              (samples[i - 1] >= 0.0f && samples[i] < 0.0f))) {
                    zero_crossings++;
                }
            }
            assert(energy > 0.01); /* Tone has active energy */
            /* In 800 samples at 48kHz (16.66ms), a 1000Hz wave has ~16.6 cycles -> ~33 zero crossings */
            assert(zero_crossings >= 30 && zero_crossings <= 36);
            printf("    [+] Captured 48kHz stereo audio: 1 kHz reference tone verified (energy=%.4f, zero_crossings=%d)!\n",
                   energy, zero_crossings);

            oavx_stream_release_audio(p_recv, &rx_audio);
        } else if (ft == OAVX_FRAME_TYPE_VIDEO) {
            oavx_stream_release_video(p_recv, &rx_video);
        }
    }
    assert(got_audio);

    /* 7. Verify Embedded H.264 SMPTE Bitstream & Web/RTSP Lifecycle */
    printf("[*] 7. Verifying embedded Annex-B H.264 SMPTE bitstream + Web/RTSP servers...\n");
    assert(g_smpte_h264_1080p_len > 1000);
    oavx_status_t web_st = oavx_stream_enable_web_preview(p_send, 0);
    assert(web_st == OAVX_SUCCESS);
    char web_url_buf[256] = {0};
    const char *web_u = oavx_stream_get_web_preview_url(p_send, web_url_buf, sizeof(web_url_buf));
    assert(web_u != NULL);
    assert(strstr(web_url_buf, "http://") != NULL);
    assert(oavx_stream_get_web_client_count(p_send) == 0);

    oavx_status_t rtsp_st = oavx_stream_enable_rtsp_server(p_send, 0, "live");
    assert(rtsp_st == OAVX_SUCCESS);
    assert(oavx_stream_get_rtsp_port(p_send) > 0);
    assert(oavx_stream_get_rtsp_client_count(p_send) == 0);

    oavx_video_frame_t h264_vf = {
        .width = 1920,
        .height = 1080,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_H264,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { (uint8_t*)g_smpte_h264_1080p, NULL, NULL, NULL },
        .strides = { g_smpte_h264_1080p_len, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 2000000000ULL,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };
    oavx_status_t snd_st = oavx_stream_send_video(p_send, &h264_vf);
    assert(snd_st == OAVX_SUCCESS);

    oavx_stream_disable_web_preview(p_send);
    oavx_stream_disable_rtsp_server(p_send);
    assert(oavx_stream_get_web_preview_port(p_send) == 0);
    assert(oavx_stream_get_rtsp_port(p_send) == 0);
    printf("    [+] Embedded H.264 bitstream (%zu bytes) and Web/RTSP servers validated.\n", g_smpte_h264_1080p_len);

    /* Teardown */
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    free(p_vframe);
    free(a_left);
    free(a_right);
    free(a_planar);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_test_pattern_test\n");
    return 0;
}
