/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/oavx_stream_rudp_test.c - End-to-end RUDP Media Transport Verification Suite
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "openavx.h"
#include "avx_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("=== OpenAVX End-to-End RUDP Stream Tests ===\n\n");

    /* 1. Initialize Context */
    printf("[*] 1. Creating oavx_context_t runtime engine...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);
    printf("  [+] Context created: %p\n", (void*)ctx);

    /* 2. Open Transmit Stream in RUDP Mode */
    printf("[*] 2. Spawning RUDP Transmit Stream 'RUDP_BROADCAST_FEED'...\n");
    oavx_stream_config_t tx_cfg = {
        .name = "RUDP_BROADCAST_FEED",
        .direction = OAVX_DIR_TRANSMIT,
        .transport = OAVX_TRANSPORT_RUDP,
        .clock_sync = false,
        .groups = "Public"
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    assert(tx != NULL);

    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
    assert(ep != NULL && ep->address != NULL);
    printf("  [+] Transmit stream active at endpoint: %s (RUDP)\n", ep->address);

    /* 3. Open Receive Stream in RUDP Mode */
    printf("[*] 3. Spawning RUDP Receive Stream connecting to %s...\n", ep->address);
    oavx_stream_config_t rx_cfg = {
        .name = "RUDP_RECEIVER_CLIENT",
        .endpoint = ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .transport = OAVX_TRANSPORT_RUDP,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .groups = "Public"
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    assert(rx != NULL);
    printf("  [+] Receive stream active (RUDP session negotiated)\n");

    /* Allow network session handshake and port negotiation */
    avx_sleep_ms(80);

    /* 4. Stream 20 Video Frames over RUDP */
    printf("[*] 4. Transmitting Video Frames over RUDP...\n");
    const uint32_t width = 640;
    const uint32_t height = 360;
    size_t line_stride = (size_t)width * 2;
    size_t frame_bytes = line_stride * height;
    uint8_t *video_buf = (uint8_t*)malloc(frame_bytes);
    assert(video_buf != NULL);

    int received_video_count = 0;
    for (int i = 0; i < 20; ++i) {
        memset(video_buf, (uint8_t)(0x30 + i), frame_bytes);

        oavx_video_frame_t tx_v = {
            .width = width,
            .height = height,
            .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
            .scan_type = OAVX_SCAN_PROGRESSIVE,
            .planes = { video_buf, NULL, NULL, NULL },
            .strides = { line_stride, 0, 0, 0 },
            .framerate = { .num = 60000, .den = 1000 },
            .aspect_ratio = { .num = 16, .den = 9 },
            .pts_ns = 1000000000LL + (int64_t)i * 16666666LL
        };

        oavx_status_t s_stat = oavx_stream_send_video(tx, &tx_v);
        assert(s_stat == OAVX_SUCCESS);

        oavx_video_frame_t rx_v = {0};
        oavx_frame_type_t ftype = oavx_stream_receive(rx, &rx_v, NULL, NULL, 200);
        if (ftype == OAVX_FRAME_TYPE_VIDEO) {
            assert(rx_v.width == width);
            assert(rx_v.height == height);
            assert(rx_v.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY);
            assert(rx_v.planes[0] != NULL);
            assert(((uint8_t*)rx_v.planes[0])[0] == (uint8_t)(0x30 + i));
            received_video_count++;
            oavx_stream_release_video(rx, &rx_v);
        }
    }
    printf("  [+] Successfully received %d/20 video frames over RUDP!\n", received_video_count);
    assert(received_video_count >= 15); /* High reliability over local loopback */

    /* 5. Stream 20 Audio Frames over RUDP */
    printf("[*] 5. Transmitting Audio Frames over RUDP...\n");
    const uint32_t sample_rate = 48000;
    const uint16_t channels = 2;
    const uint32_t num_samples = 480; /* 10 ms at 48kHz */
    size_t audio_ch_bytes = (size_t)num_samples * sizeof(float);
    float *left_ch = (float*)malloc(audio_ch_bytes);
    float *right_ch = (float*)malloc(audio_ch_bytes);
    assert(left_ch && right_ch);

    int received_audio_count = 0;
    for (int i = 0; i < 20; ++i) {
        for (uint32_t s = 0; s < num_samples; ++s) {
            left_ch[s] = 0.5f + (float)i * 0.01f;
            right_ch[s] = -0.5f - (float)i * 0.01f;
        }

        oavx_audio_frame_t tx_a = {
            .sample_rate = sample_rate,
            .channels = channels,
            .frames = num_samples,
            .format = OAVX_AUDIO_FMT_FLT_PLANAR,
            .planes = { (void*)left_ch, (void*)right_ch, NULL, NULL },
            .stride_bytes = audio_ch_bytes,
            .pts_ns = 1000000000LL + (int64_t)i * 10000000LL
        };

        oavx_status_t a_stat = oavx_stream_send_audio(tx, &tx_a);
        assert(a_stat == OAVX_SUCCESS);

        oavx_audio_frame_t rx_a = {0};
        oavx_frame_type_t ftype = oavx_stream_receive(rx, NULL, &rx_a, NULL, 200);
        if (ftype == OAVX_FRAME_TYPE_AUDIO) {
            assert(rx_a.sample_rate == sample_rate);
            assert(rx_a.channels == channels);
            assert(rx_a.frames == num_samples);
            assert(rx_a.planes[0] != NULL);
            received_audio_count++;
            oavx_stream_release_audio(rx, &rx_a);
        }
    }
    printf("  [+] Successfully received %d/20 audio frames over RUDP!\n", received_audio_count);
    assert(received_audio_count >= 15);

    /* 6. Verify Tally Control across Session */
    printf("[*] 6. Verifying Bidirectional Tally Control across session...\n");
    oavx_tally_t tally_set = { .flags = OAVX_TALLY_PROGRAM | OAVX_TALLY_PREVIEW };
    bool tally_ok = oavx_stream_set_tally(rx, &tally_set);
    assert(tally_ok);

    avx_sleep_ms(50);
    oavx_tally_t tally_get = {0};
    bool get_ok = oavx_stream_get_tally(tx, &tally_get, 200);
    assert(get_ok);
    assert((tally_get.flags & OAVX_TALLY_PROGRAM) != 0);
    assert((tally_get.flags & OAVX_TALLY_PREVIEW) != 0);
    printf("  [+] Tally status successfully reflected: 0x%02X (PROGRAM | PREVIEW)\n", tally_get.flags);

    /* 7. Teardown */
    printf("[*] 7. Closing RUDP streams and destroying context...\n");
    free(video_buf);
    free(left_ch);
    free(right_ch);
    oavx_stream_close(rx);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);

    printf("\n[PASS] oavx_stream_rudp_test\n");
    return 0;
}
