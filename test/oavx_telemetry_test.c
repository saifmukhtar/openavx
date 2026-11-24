/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/oavx_telemetry_test.c - Unit test suite for Full Stream Observability Telemetry Engine
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
    printf("=== OpenAVX Stream Telemetry Tests ===\n\n");

    /* --------------------------------------------------------------------- */
    /* 1. Defensive Input Validation                                         */
    /* --------------------------------------------------------------------- */
    printf("[*] 1. Testing defensive parameter validation...\n");
    assert(!oavx_stream_get_telemetry(NULL, NULL));
    oavx_telemetry_t dummy;
    assert(!oavx_stream_get_telemetry(NULL, &dummy));
    assert(oavx_telemetry_to_json(NULL, NULL, 0) == 0);
    assert(oavx_telemetry_to_json(&dummy, NULL, 0) == 0);
    char tiny_buf[8];
    assert(oavx_telemetry_to_json(NULL, tiny_buf, sizeof(tiny_buf)) == 0);
    printf("  [+] Defensive NULL and boundary validations passed.\n");

    /* --------------------------------------------------------------------- */
    /* 2. Context Engine and Stream Setup                                    */
    /* --------------------------------------------------------------------- */
    printf("[*] 2. Initializing Context and Transmitter Stream...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t tx_cfg = {
        .name = "TELEMETRY_TX_PROD",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *tx_stream = oavx_stream_open(ctx, &tx_cfg);
    assert(tx_stream != NULL);

    const oavx_endpoint_t *tx_ep = oavx_stream_get_endpoint(tx_stream);
    assert(tx_ep != NULL);
    assert(tx_ep->address != NULL && tx_ep->address[0] != '\0');
    printf("  [+] Transmitter bound to endpoint: %s\n", tx_ep->address);

    oavx_telemetry_t tx_telem;
    bool ok = oavx_stream_get_telemetry(tx_stream, &tx_telem);
    assert(ok);
    assert(!tx_telem.is_connected);
    assert(tx_telem.video_frames_total == 0);
    assert(tx_telem.total_wire_bytes == 0);
    printf("  [+] Initial pre-connection TX telemetry verified (0 frames, not connected).\n");

    /* --------------------------------------------------------------------- */
    /* 3. Connecting Receiver Stream                                         */
    /* --------------------------------------------------------------------- */
    printf("[*] 3. Connecting Receiver Stream to %s...\n", tx_ep->address);
    oavx_stream_config_t rx_cfg = {
        .name = "TELEMETRY_RX_PROD",
        .endpoint = tx_ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_FASTEST,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST
    };
    oavx_stream_t *rx_stream = oavx_stream_open(ctx, &rx_cfg);
    assert(rx_stream != NULL);

    /* Allow TCP handshake to establish */
    usleep(60000);

    oavx_telemetry_t rx_telem;
    ok = oavx_stream_get_telemetry(rx_stream, &rx_telem);
    assert(ok);
    printf("  [+] Post-handshake RX telemetry: connected=%s, endpoint=%s\n",
           rx_telem.is_connected ? "true" : "false", rx_telem.remote_endpoint);
    assert(rx_telem.is_connected);

    /* --------------------------------------------------------------------- */
    /* 4. Video & Audio Transmission Under Observability Monitoring          */
    /* --------------------------------------------------------------------- */
    printf("[*] 4. Transmitting 30 test video frames and audio chunks...\n");
    const uint32_t width = 320;
    const uint32_t height = 240;
    size_t frame_bytes = (size_t)width * height * 2; /* UYVY */
    uint8_t *yuv_buf = (uint8_t*)calloc(1, frame_bytes);
    assert(yuv_buf != NULL);

    for (int i = 0; i < 30; ++i) {
        oavx_video_frame_t vframe = {
            .width = width,
            .height = height,
            .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
            .planes = { yuv_buf, NULL, NULL, NULL },
            .strides = { width * 2, 0, 0, 0 },
            .pts_ns = (int64_t)i * 33333333LL,
            .dts_ns = (int64_t)i * 33333333LL,
            .framerate = { .num = 30, .den = 1 },
            .scan_type = OAVX_SCAN_PROGRESSIVE
        };
        oavx_status_t st = oavx_stream_send_video(tx_stream, &vframe);
        assert(st == OAVX_SUCCESS);

        /* Receive frame on receiver */
        oavx_video_frame_t recv_v = {0};
        oavx_frame_type_t ft = oavx_stream_receive(rx_stream, &recv_v, NULL, NULL, 50);
        if (ft == OAVX_FRAME_TYPE_VIDEO) {
            oavx_stream_release_video(rx_stream, &recv_v);
        }
        usleep(1000);
    }
    free(yuv_buf);

    /* --------------------------------------------------------------------- */
    /* 5. Deep Telemetry Layer Verification (Media, Transport, Kernel)       */
    /* --------------------------------------------------------------------- */
    printf("[*] 5. Querying and verifying full multi-layer telemetry...\n");
    memset(&rx_telem, 0, sizeof(rx_telem));
    ok = oavx_stream_get_telemetry(rx_stream, &rx_telem);
    assert(ok);

    printf("  --> Media Layer:\n");
    printf("      Delivered Video Frames: %lld\n", (long long)rx_telem.video_frames_total);
    printf("      Dropped Video Frames:   %lld\n", (long long)rx_telem.dropped_frames_total);
    printf("      Instantaneous FPS:      %.2f\n", (double)rx_telem.fps);
    assert(rx_telem.video_frames_total > 0);
    assert(rx_telem.width == width);
    assert(rx_telem.height == height);
    assert(rx_telem.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY);

    printf("  --> Transport Dynamics Layer:\n");
    printf("      Total Wire Bytes:       %llu bytes\n", (unsigned long long)rx_telem.total_wire_bytes);
    printf("      Instant Bitrate:        %.2f Mbps\n", (double)rx_telem.bitrate_mbps);
    printf("      Inter-frame Jitter:     %.3f ms\n", (double)rx_telem.inter_frame_jitter_ms);
    assert(rx_telem.total_wire_bytes > 0);
    assert(rx_telem.inter_frame_jitter_ms >= 0.0f);

    printf("  --> OS Kernel Diagnostics Layer:\n");
    printf("      Kernel TCP RTT:         %.3f ms\n", (double)rx_telem.kernel_rtt_ms);
    printf("      TCP Retransmissions:    %u\n", rx_telem.tcp_retransmissions);
    printf("      Socket Send Queue:      %u bytes\n", rx_telem.socket_send_q_bytes);
    printf("      Socket Recv Queue:      %u bytes\n", rx_telem.socket_recv_q_bytes);
    assert(rx_telem.kernel_rtt_ms >= 0.0f);

    /* --------------------------------------------------------------------- */
    /* 6. JSON Telemetry Export & Formatting Verification                    */
    /* --------------------------------------------------------------------- */
    printf("[*] 6. Testing oavx_telemetry_to_json() export format...\n");
    char json_buf[2048];
    size_t json_len = oavx_telemetry_to_json(&rx_telem, json_buf, sizeof(json_buf));
    assert(json_len > 0);
    assert(json_len < sizeof(json_buf));

    printf("  [+] Generated JSON Telemetry:\n%s\n", json_buf);

    /* Validate mandatory JSON fields */
    assert(strstr(json_buf, "\"connected\": true") != NULL);
    assert(strstr(json_buf, "\"fourcc\": \"UYVY\"") != NULL);
    assert(strstr(json_buf, "\"width\": 320") != NULL);
    assert(strstr(json_buf, "\"height\": 240") != NULL);
    assert(strstr(json_buf, "\"video_frames\":") != NULL);
    assert(strstr(json_buf, "\"bitrate_mbps\":") != NULL);
    assert(strstr(json_buf, "\"jitter_ms\":") != NULL);
    assert(strstr(json_buf, "\"rtt_ms\":") != NULL);
    assert(strstr(json_buf, "\"send_queue_bytes\":") != NULL);
    assert(strstr(json_buf, "\"recv_queue_bytes\":") != NULL);

    /* Truncation boundary test */
    char tiny_out[16];
    size_t trunc_len = oavx_telemetry_to_json(&rx_telem, tiny_out, sizeof(tiny_out));
    assert(trunc_len == sizeof(tiny_out) - 1);
    assert(tiny_out[sizeof(tiny_out) - 1] == '\0');
    printf("  [+] Buffer boundary safety and truncation verified.\n");

    /* --------------------------------------------------------------------- */
    /* 7. Clean Teardown                                                     */
    /* --------------------------------------------------------------------- */
    printf("[*] 7. Closing streams and destroying context...\n");
    oavx_stream_close(rx_stream);
    oavx_stream_close(tx_stream);
    oavx_context_destroy(ctx);

    printf("\n[PASS] oavx_telemetry_test\n");
    return 0;
}
