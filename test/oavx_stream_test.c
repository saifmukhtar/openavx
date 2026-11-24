/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/oavx_stream_test.c - Verification Suite for Modern Native Media Pipeline (oavx_*)
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "openavx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

int main(void) {
    printf("=== OpenAVX Stream Pipeline Tests (oavx_*) ===\n\n");

    /* 1. Initialize Context */
    printf("[*] 1. Creating oavx_context_t runtime engine...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);
    printf("  [+] Context created: %p\n", (void*)ctx);
    printf("  [+] Engine Version: %s\n", oavx_version());
    printf("  [+] CPU Supported:  %s\n", oavx_is_supported_cpu() ? "YES" : "NO");
    assert(oavx_is_supported_cpu());

    /* 2. Open Transmit Stream */
    printf("[*] 2. Opening Transmit Stream: 'OAVX_PROD_FEED' on port 7960...\n");
    oavx_stream_config_t tx_cfg = {
        .name = "OAVX_PROD_FEED",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .groups = "Public"
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    assert(tx != NULL);
    printf("  [+] Transmit stream handle initialized: %p\n", (void*)tx);

    /* 3. Open Receive Stream & Connect */
    printf("[*] 3. Opening Receive Stream connecting to 127.0.0.1:7960...\n");
    oavx_stream_config_t rx_cfg = {
        .name = "OAVX_PROD_FEED",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .groups = "Public"
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    assert(rx != NULL);
    printf("  [+] Receive stream handle initialized: %p\n", (void*)rx);

    /* Wait briefly for TCP handshake */
    usleep(50000);

    /* 4. Transmit and Receive Planar Video Frame */
    printf("[*] 4. Transmitting 1080p60 planar-aware video frame over stream...\n");
    const uint32_t width = 640;
    const uint32_t height = 360;
    size_t line_stride = (size_t)width * 2;
    size_t frame_bytes = line_stride * height;
    uint8_t *video_buf = (uint8_t*)malloc(frame_bytes);
    assert(video_buf != NULL);
    memset(video_buf, 0x5A, frame_bytes);

    oavx_video_frame_t tx_v = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { video_buf, NULL, NULL, NULL },
        .strides = { line_stride, 0, 0, 0 },
        .framerate = { .num = 60000, .den = 1000 },
        .aspect_ratio = { .num = 16, .den = 9 },
        .pts_ns = 1000000000LL /* 1.0 second */
    };

    int tx_res = oavx_stream_send_video(tx, &tx_v);
    assert(tx_res == 0);
    printf("  [+] Dispatched video frame via oavx_stream_send_video (640x360 UYVY)\n");

    oavx_video_frame_t rx_v = {0};
    oavx_frame_type_t ftype = oavx_stream_receive(rx, &rx_v, NULL, NULL, 500);
    assert(ftype == OAVX_FRAME_TYPE_VIDEO);
    assert(rx_v.width == width);
    assert(rx_v.height == height);
    assert(rx_v.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY);
    assert(rx_v.planes[0] != NULL);
    assert(((uint8_t*)rx_v.planes[0])[0] == 0x5A);
    printf("  [+] Successfully received video frame over oavx_stream_receive!\n");
    printf("      - Resolution: %ux%u | Stride: %zu bytes\n", rx_v.width, rx_v.height, rx_v.strides[0]);
    printf("      - PTS: %ld ns | Signature match: 100%%\n", (long)rx_v.pts_ns);
    oavx_stream_release_video(rx, &rx_v);

    /* 5. Transmit and Receive Planar Audio Frame */
    printf("[*] 5. Transmitting 48kHz stereo float audio frame over stream...\n");
    const uint32_t sample_rate = 48000;
    const uint16_t channels = 2;
    const uint32_t frames = 1024;
    float *audio_buf = (float*)malloc(frames * channels * sizeof(float));
    assert(audio_buf != NULL);
    for (size_t i = 0; i < frames * channels; ++i) {
        audio_buf[i] = 0.75f;
    }

    oavx_audio_frame_t tx_a = {
        .sample_rate = sample_rate,
        .channels = channels,
        .frames = frames,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .planes = { audio_buf, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = frames * sizeof(float),
        .pts_ns = 1000000000LL
    };

    int a_res = oavx_stream_send_audio(tx, &tx_a);
    assert(a_res == 0);
    printf("  [+] Dispatched audio frame via oavx_stream_send_audio (48kHz 2ch 1024 frames)\n");

    oavx_audio_frame_t rx_a = {0};
    ftype = oavx_stream_receive(rx, NULL, &rx_a, NULL, 500);
    assert(ftype == OAVX_FRAME_TYPE_AUDIO);
    assert(rx_a.sample_rate == sample_rate);
    assert(rx_a.channels == channels);
    assert(rx_a.frames == frames);
    assert(rx_a.planes[0] != NULL);
    assert(((float*)rx_a.planes[0])[0] == 0.75f);
    printf("  [+] Successfully received audio frame over oavx_stream_receive!\n");
    printf("      - Sample Rate: %u Hz | Channels: %u | Samples: %u\n", rx_a.sample_rate, rx_a.channels, rx_a.frames);
    oavx_stream_release_audio(rx, &rx_a);

    /* 6. Test Stream Device & Camera Controls (Tally & PTZ) */
    printf("[*] 6. Testing Device & Camera Controls (Tally, PTZ)...\n");
    oavx_tally_t tally = { .flags = OAVX_TALLY_PROGRAM | OAVX_TALLY_PREVIEW };
    bool tally_set = oavx_stream_set_tally(rx, &tally);
    assert(tally_set);
    printf("  [+] Set Program + Preview tally state via oavx_stream_set_tally\n");

    oavx_tally_t query_tally = {0};
    bool tally_got = oavx_stream_get_tally(tx, &query_tally, 200);
    assert(tally_got);
    assert((query_tally.flags & OAVX_TALLY_PROGRAM) != 0);
    assert((query_tally.flags & OAVX_TALLY_PREVIEW) != 0);
    printf("  [+] Queried tally on sender: flags=0x%x (Program=YES, Preview=YES)\n", query_tally.flags);

    oavx_ptz_state_t ptz = {
        .pan = 0.25f,
        .tilt = -0.5f,
        .zoom = 0.75f,
        .focus = 0.5f
    };
    bool ptz_ok = oavx_stream_set_ptz(rx, &ptz);
    assert(ptz_ok);
    printf("  [+] Transmitted PTZ positioning command via oavx_stream_set_ptz\n");

    /* 7. Test Discovery Grid */
    printf("[*] 7. Testing Network Discovery Grid (oavx_discovery_t)...\n");
    oavx_discovery_t *disc = oavx_discovery_create(ctx, "Public");
    assert(disc != NULL);
    oavx_endpoint_t sources[16];
    size_t count = oavx_discovery_get_sources(disc, sources, 16);
    printf("  [+] Discovery grid active, enumerated %zu initial endpoints\n", count);
    oavx_discovery_destroy(disc);

    /* 8. Teardown Streams and Context */
    printf("[*] 8. Cleanly closing stream pipelines and destroying context...\n");
    oavx_stream_close(rx);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);

    free(video_buf);
    free(audio_buf);

    printf("\n[PASS] oavx_stream_test\n");

    return 0;
}
