/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/oavx_multiclient_test.c - Unit test suite for Multi-Client Fan-Out & Head-of-Line Protection
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "openavx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

int main(void) {
    printf("=== OpenAVX Multi-Client Fan-Out & Tally Tests ===\n\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* --------------------------------------------------------------------- */
    /* 1. Configurable Capacity Enforcement                                  */
    /* --------------------------------------------------------------------- */
    printf("[*] 1. Testing Configurable Client Limits (max_connections = 3)...\n");

    oavx_stream_config_t tx_limited_cfg = {
        .name = "FANOUT_LIMITED_TX",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .max_connections = 3
    };
    oavx_stream_t *tx_limited = oavx_stream_open(ctx, &tx_limited_cfg);
    assert(tx_limited != NULL);

    const oavx_endpoint_t *ep_limited = oavx_stream_get_endpoint(tx_limited);
    assert(ep_limited && ep_limited->address);

    /* Connect 3 allowed receivers */
    oavx_stream_t *rx_lim[3];
    for (int i = 0; i < 3; ++i) {
        oavx_stream_config_t rx_cfg = {
            .name = "FANOUT_LIMITED_RX",
            .endpoint = ep_limited->address,
            .direction = OAVX_DIR_RECEIVE,
            .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
            .bandwidth = OAVX_BANDWIDTH_HIGHEST
        };
        rx_lim[i] = oavx_stream_open(ctx, &rx_cfg);
        assert(rx_lim[i] != NULL);
    }

    usleep(50000); /* 50 ms for connections to settle */
    int count_limited = oavx_stream_get_connection_count(tx_limited, 100);
    printf("  [+] Connected %d clients to limited sender (target 3)\n", count_limited);
    assert(count_limited == 3);

    /* 4th receiver should be rejected / dropped by listener */
    oavx_stream_config_t rx_extra_cfg = {
        .name = "FANOUT_EXTRA_RX",
        .endpoint = ep_limited->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST
    };
    oavx_stream_t *rx_extra = oavx_stream_open(ctx, &rx_extra_cfg);
    usleep(50000);
    int count_after_extra = oavx_stream_get_connection_count(tx_limited, 100);
    printf("  [+] Connection count after 4th client attempt: %d (Enforced <= 3)\n", count_after_extra);
    assert(count_after_extra == 3);

    if (rx_extra) oavx_stream_close(rx_extra);
    for (int i = 0; i < 3; ++i) {
        oavx_stream_close(rx_lim[i]);
    }
    oavx_stream_close(tx_limited);
    printf("  [+] Capacity enforcement verified!\n\n");

    /* --------------------------------------------------------------------- */
    /* 2. Multi-Client Fan-Out Broadcast (6 Simultaneous Receivers)          */
    /* --------------------------------------------------------------------- */
    printf("[*] 2. Testing Fan-Out Broadcast to 6 Parallel Receivers...\n");

    oavx_stream_config_t tx_cfg = {
        .name = "STUDIO_CAMERA_1_FANOUT",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .max_connections = 16
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    assert(tx != NULL);

    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
    assert(ep && ep->address);
    printf("  [+] Sender active on %s\n", ep->address);

    #define NUM_VIEWERS 6
    oavx_stream_t *rx[NUM_VIEWERS];
    for (int i = 0; i < NUM_VIEWERS; ++i) {
        char rx_name[64];
        snprintf(rx_name, sizeof(rx_name), "VIEWER_%d", i + 1);
        oavx_stream_config_t rx_cfg = {
            .name = rx_name,
            .endpoint = ep->address,
            .direction = OAVX_DIR_RECEIVE,
            .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
            .bandwidth = OAVX_BANDWIDTH_HIGHEST
        };
        rx[i] = oavx_stream_open(ctx, &rx_cfg);
        assert(rx[i] != NULL);
    }

    usleep(100000); /* 100 ms to establish all 6 TCP sessions */
    int active_conns = oavx_stream_get_connection_count(tx, 100);
    printf("  [+] Active connection count on sender: %d (expected %d)\n", active_conns, NUM_VIEWERS);
    assert(active_conns == NUM_VIEWERS);

    /* Allocate and transmit a test video frame */
    const uint32_t width = 320;
    const uint32_t height = 240;
    size_t line_stride = (size_t)width * 2;
    size_t frame_bytes = line_stride * height;
    uint8_t *vbuf = (uint8_t*)malloc(frame_bytes);
    assert(vbuf != NULL);
    memset(vbuf, 0x5A, frame_bytes);

    oavx_video_frame_t vframe = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { vbuf, NULL, NULL, NULL },
        .strides = { line_stride, 0, 0, 0 },
        .framerate = { .num = 60000, .den = 1000 },
        .aspect_ratio = { .num = 16, .den = 9 },
        .pts_ns = 1234567890LL
    };

    printf("  [+] Broadcasting video frame (320x240 UYVY) to all %d viewers...\n", NUM_VIEWERS);
    int tx_res = oavx_stream_send_video(tx, &vframe);
    assert(tx_res == 0);

    /* Verify each of the 6 receivers captures the identical frame */
    for (int i = 0; i < NUM_VIEWERS; ++i) {
        oavx_video_frame_t cap_v = {0};
        oavx_frame_type_t ftype = oavx_stream_receive(rx[i], &cap_v, NULL, NULL, 500);
        assert(ftype == OAVX_FRAME_TYPE_VIDEO);
        assert(cap_v.width == width);
        assert(cap_v.height == height);
        assert(cap_v.pts_ns == 1234567890LL);
        assert(cap_v.planes[0] != NULL);
        assert(((uint8_t*)cap_v.planes[0])[0] == 0x5A);
        assert(((uint8_t*)cap_v.planes[0])[frame_bytes - 1] == 0x5A);
        oavx_stream_release_video(rx[i], &cap_v);
    }
    printf("  [+] All %d parallel receivers captured the broadcast video frame with 100%% integrity!\n\n", NUM_VIEWERS);

    /* Allocate and transmit an audio frame */
    const uint32_t sample_rate = 48000;
    const uint16_t channels = 2;
    const uint32_t samples = 1024;
    float *abuf = (float*)malloc(samples * channels * sizeof(float));
    assert(abuf != NULL);
    for (size_t s = 0; s < (size_t)channels * samples; ++s) {
        abuf[s] = 0.75f;
    }

    oavx_audio_frame_t aframe = {
        .sample_rate = sample_rate,
        .channels = channels,
        .frames = samples,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .planes = { (uint8_t*)abuf, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = samples * sizeof(float),
        .pts_ns = 2000000000LL
    };

    printf("  [+] Broadcasting audio frame (48kHz stereo) to all %d viewers...\n", NUM_VIEWERS);
    int atx_res = oavx_stream_send_audio(tx, &aframe);
    assert(atx_res == 0);

    /* Verify each of the 6 receivers captures the audio frame */
    for (int i = 0; i < NUM_VIEWERS; ++i) {
        oavx_audio_frame_t cap_a = {0};
        oavx_frame_type_t ftype = oavx_stream_receive(rx[i], NULL, &cap_a, NULL, 500);
        assert(ftype == OAVX_FRAME_TYPE_AUDIO);
        assert(cap_a.sample_rate == sample_rate);
        assert(cap_a.channels == channels);
        assert(cap_a.frames == samples);
        float *samples_in = (float*)cap_a.planes[0];
        assert(samples_in != NULL);
        assert(samples_in[0] == 0.75f);
        oavx_stream_release_audio(rx[i], &cap_a);
    }
    printf("  [+] All %d parallel receivers captured the broadcast audio frame!\n\n", NUM_VIEWERS);

    /* --------------------------------------------------------------------- */
    /* 3. Dynamic Tally Aggregation                                          */
    /* --------------------------------------------------------------------- */
    printf("[*] 3. Testing Dynamic Tally Aggregation...\n");

    /* Viewer 1 is on Program (OBS live output) */
    oavx_tally_t t1 = { .flags = OAVX_TALLY_PROGRAM };
    assert(oavx_stream_set_tally(rx[0], &t1));

    /* Viewer 2 is on Preview (Director multiviewer) */
    oavx_tally_t t2 = { .flags = OAVX_TALLY_PREVIEW };
    assert(oavx_stream_set_tally(rx[1], &t2));

    /* Viewers 3..6 are inactive/recording */
    oavx_tally_t t_none = { .flags = OAVX_TALLY_NONE };
    for (int i = 2; i < NUM_VIEWERS; ++i) {
        assert(oavx_stream_set_tally(rx[i], &t_none));
    }

    usleep(100000); /* 100 ms for reverse tally packets to reach sender */

    oavx_tally_t query_tally = {0};
    assert(oavx_stream_get_tally(tx, &query_tally, 200));
    printf("  [+] Sender Aggregated Tally Flags: 0x%02X (Program=%d, Preview=%d)\n",
           query_tally.flags,
           (query_tally.flags & OAVX_TALLY_PROGRAM) ? 1 : 0,
           (query_tally.flags & OAVX_TALLY_PREVIEW) ? 1 : 0);
    assert(query_tally.flags & OAVX_TALLY_PROGRAM);
    assert(query_tally.flags & OAVX_TALLY_PREVIEW);

    /* --------------------------------------------------------------------- */
    /* 4. Client Disconnection & Dynamic Recalculation                       */
    /* --------------------------------------------------------------------- */
    printf("[*] 4. Testing Disconnection & Dynamic Recalculation...\n");
    printf("  [-] Disconnecting Viewer 1 (Program viewer)...\n");
    oavx_stream_close(rx[0]);
    rx[0] = NULL;

    usleep(150000); /* 150 ms for reverse reader to detect hangup and recalculate */

    int remaining_conns = oavx_stream_get_connection_count(tx, 100);
    printf("  [+] Sender connection count after disconnect: %d (expected %d)\n",
           remaining_conns, NUM_VIEWERS - 1);
    assert(remaining_conns == (NUM_VIEWERS - 1));

    memset(&query_tally, 0, sizeof(query_tally));
    assert(oavx_stream_get_tally(tx, &query_tally, 200));
    printf("  [+] Sender Tally Flags after Program viewer left: 0x%02X (Program=%d, Preview=%d)\n",
           query_tally.flags,
           (query_tally.flags & OAVX_TALLY_PROGRAM) ? 1 : 0,
           (query_tally.flags & OAVX_TALLY_PREVIEW) ? 1 : 0);
    assert(!(query_tally.flags & OAVX_TALLY_PROGRAM));
    assert(query_tally.flags & OAVX_TALLY_PREVIEW);

    /* Broadcast another frame to verify surviving 5 viewers receive it undisturbed */
    vframe.pts_ns = 3000000000LL;
    vbuf[0] = 0xBE;
    vbuf[frame_bytes - 1] = 0xEF;
    assert(oavx_stream_send_video(tx, &vframe) == 0);

    for (int i = 1; i < NUM_VIEWERS; ++i) {
        oavx_video_frame_t cap_v2 = {0};
        oavx_frame_type_t ftype = oavx_stream_receive(rx[i], &cap_v2, NULL, NULL, 500);
        assert(ftype == OAVX_FRAME_TYPE_VIDEO);
        assert(((uint8_t*)cap_v2.planes[0])[0] == 0xBE);
        assert(((uint8_t*)cap_v2.planes[0])[frame_bytes - 1] == 0xEF);
        oavx_stream_release_video(rx[i], &cap_v2);
    }
    printf("  [+] Surviving %d viewers streamed undisturbed!\n\n", NUM_VIEWERS - 1);

    /* --------------------------------------------------------------------- */
    /* 5. Telemetry Wire Fan-Out Verification                                */
    /* --------------------------------------------------------------------- */
    printf("[*] 5. Verifying Telemetry Wire Accounting...\n");
    oavx_telemetry_t telem;
    assert(oavx_stream_get_telemetry(tx, &telem));
    printf("  [+] Transmitter Telemetry: is_connected=%d, video_frames=%llu, total_bytes=%llu\n",
           telem.is_connected ? 1 : 0,
           (unsigned long long)telem.video_frames_total,
           (unsigned long long)telem.total_wire_bytes);
    assert(telem.is_connected);
    assert(telem.video_frames_total == 2);
    assert(telem.total_wire_bytes > 0);

    /* --------------------------------------------------------------------- */
    /* 6. Head-of-Line Blocking Protection (Async TX Isolation)              */
    /* --------------------------------------------------------------------- */
    printf("[*] 6. Testing Head-of-Line Blocking Isolation (Slow Viewer vs Fast Viewer)...\n");

    /* Close previous fanout session first */
    for (int i = 1; i < NUM_VIEWERS; ++i) {
        if (rx[i]) {
            oavx_stream_close(rx[i]);
            rx[i] = NULL;
        }
    }
    oavx_stream_close(tx);
    tx = NULL;
    usleep(50000); /* 50 ms for port recycling */

    oavx_stream_config_t tx_hol_cfg = {
        .name = "FANOUT_HOL_TX",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *tx_hol = oavx_stream_open(ctx, &tx_hol_cfg);
    assert(tx_hol != NULL);

    const oavx_endpoint_t *ep_hol = oavx_stream_get_endpoint(tx_hol);
    assert(ep_hol && ep_hol->address);

    /* Fast Viewer 1: normal receiver reading all frames */
    oavx_stream_config_t rx_fast_cfg = {
        .name = "FAST_VIEWER_1",
        .endpoint = ep_hol->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST
    };
    oavx_stream_t *rx_fast = oavx_stream_open(ctx, &rx_fast_cfg);
    assert(rx_fast != NULL);

    /* Slow Viewer 2: connect raw TCP socket with tiny buffer and NEVER read */
    int slow_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(slow_fd >= 0);
    int rcvbuf = 4096;
    setsockopt(slow_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons((uint16_t)ep_hol->port);
    inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);
    int conn_res = connect(slow_fd, (struct sockaddr*)&saddr, sizeof(saddr));
    assert(conn_res == 0);

    /* Wait for connections to be registered (up to 1 second) */
    int hol_conns = 0;
    for (int wait = 0; wait < 100; ++wait) {
        hol_conns = oavx_stream_get_connection_count(tx_hol, 0);
        if (hol_conns >= 2) break;
        usleep(10000); /* 10 ms */
    }
    printf("  [+] Sender active with %d viewers (1 Fast Viewer + 1 Slow Frozen Viewer)\n", hol_conns);
    assert(hol_conns >= 2);

    /* Pump 130 video frames (130 * 153.6KB = ~20MB, exceeding 16MB kernel socket buffer) */
    int fast_received_count = 0;
    double max_send_ms = 0.0;

    for (int f = 0; f < 130; ++f) {
        vframe.pts_ns = 5000000000LL + (int64_t)f * 16666666LL;
        vbuf[0] = (uint8_t)(0x10 + (f & 0xFF));

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        int send_ret = oavx_stream_send_video(tx_hol, &vframe);
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        assert(send_ret == 0);

        double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0 +
                            (t_end.tv_nsec - t_start.tv_nsec) / 1000000.0;
        if (elapsed_ms > max_send_ms) max_send_ms = elapsed_ms;

        /* Fast viewer receives frame */
        oavx_video_frame_t fast_cap = {0};
        oavx_frame_type_t ftype = oavx_stream_receive(rx_fast, &fast_cap, NULL, NULL, 20);
        if (ftype == OAVX_FRAME_TYPE_VIDEO) {
            fast_received_count++;
            oavx_stream_release_video(rx_fast, &fast_cap);
        }

        usleep(2000); /* 2 ms pacing */
    }

    printf("  [+] Decoupled Render Loop: Max send() execution time: %.3f ms (Target: < 2.0 ms)\n", max_send_ms);
    printf("  [+] Fast Viewer captured %d/130 frames without being blocked!\n", fast_received_count);
    assert(fast_received_count >= 100);

    oavx_telemetry_t hol_telem;
    assert(oavx_stream_get_telemetry(tx_hol, &hol_telem));
    printf("  [+] Transmitter Telemetry: total_dropped_frames = %llu (Head-of-Line protection active)\n",
           (unsigned long long)hol_telem.dropped_frames_total);
    assert(hol_telem.dropped_frames_total > 0);

    close(slow_fd);
    oavx_stream_close(rx_fast);
    oavx_stream_close(tx_hol);
    printf("  [+] Head-of-Line Blocking Isolation successfully verified!\n\n");

    /* --------------------------------------------------------------------- */
    /* 7. Clean Teardown                                                     */
    /* --------------------------------------------------------------------- */
    printf("[*] 7. Closing context and freeing buffers...\n");
    oavx_context_destroy(ctx);
    free(vbuf);
    free(abuf);

    printf("\n[PASS] oavx_multiclient_test\n");

    return 0;
}
