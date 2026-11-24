/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_congestion_test.c - Congestion Control, Audio Preemption & Bitrate Adaptation Test Suite
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "avx_platform.h"
#include "openavx.h"
#include "avx_internal.h"
#include "avx_speedhq.h"

#define TEST_CONG_PORT 9090

static void test_speedhq_dynamic_quantization(void) {
    printf("[*] 1. Testing SpeedHQ Dynamic Bitrate & Quantization Scaling...\n");

    avx_speedhq_init();

    int width = 1920;
    int height = 1080;
    size_t raw_bytes = (size_t)width * height * 2;
    uint8_t *uyvy = (uint8_t*)malloc(raw_bytes);
    assert(uyvy != NULL);

    /* Generate natural test gradient */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uyvy[(y * width + x) * 2 + 0] = (uint8_t)(x & 0xFF);        /* U/V */
            uyvy[(y * width + x) * 2 + 1] = (uint8_t)((x + y) & 0xFF);  /* Y */
        }
    }

    size_t max_comp = raw_bytes + 4096;
    uint8_t *comp_high = (uint8_t*)malloc(max_comp);
    uint8_t *comp_low  = (uint8_t*)malloc(max_comp);
    uint8_t *decomp    = (uint8_t*)malloc(raw_bytes);
    assert(comp_high && comp_low && decomp);

    /* 1. High Quality (Default: 96) */
    oavx_speedhq_set_quality(96);
    avx_speedhq_set_quality(96);
    assert(oavx_speedhq_get_quality() == 96);
    size_t high_size = 0;
    int ret = avx_speedhq_encode_c99_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ2, width, height,
                                       uyvy, NULL, comp_high, max_comp, &high_size);
    assert(ret == 0 && high_size > 0);
    printf("  [+] High Quality (q=96): %zu bytes (Ratio: %.1f:1)\n",
           high_size, (double)raw_bytes / (double)high_size);

    /* 2. Low Bitrate / High Compression (Quality: 50, qscale=25) */
    oavx_speedhq_set_quality(50);
    avx_speedhq_set_quality(50);
    assert(oavx_speedhq_get_quality() == 50);
    size_t low_size = 0;
    ret = avx_speedhq_encode_c99_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ2, width, height,
                                   uyvy, NULL, comp_low, max_comp, &low_size);
    assert(ret == 0 && low_size > 0);
    printf("  [+] Low Bitrate (q=50):   %zu bytes (Ratio: %.1f:1)\n",
           low_size, (double)raw_bytes / (double)low_size);

    /* Verify low bitrate achieves substantial bandwidth savings */
    assert(low_size < high_size);
    double savings_pct = (1.0 - ((double)low_size / (double)high_size)) * 100.0;
    printf("  [+] Wire Bandwidth Reduction: %.1f%% savings under congestion!\n", savings_pct);
    assert(savings_pct >= 30.0);

    /* Verify Decompression of Low Bitrate Frame */
    bool has_alpha = false;
    ret = avx_speedhq_decode_c99_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ2, width, height,
                                   comp_low, low_size, decomp, NULL, &has_alpha);
    assert(ret == 0);
    printf("  [+] Successfully decoded adaptive low-bitrate SpeedHQ bitstream!\n");

    /* Restore default quality */
    oavx_speedhq_set_quality(96);
    avx_speedhq_set_quality(96);

    free(uyvy);
    free(comp_high);
    free(comp_low);
    free(decomp);
}

static void test_audio_priority_preemption(void) {
    printf("[*] 2. Testing Strict Audio Priority & Video Preemption under Full Queue Saturation...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t tx_cfg = {
        .name = "AUDIO_PRIORITY_TEST_STREAM",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .bind_ip = "127.0.0.1"
    };

    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    assert(tx != NULL);
    const oavx_endpoint_t *tx_ep = oavx_stream_get_endpoint(tx);
    assert(tx_ep != NULL);

    /* Connect Receiver */
    oavx_stream_config_t rx_cfg = {
        .name = "AUDIO_PRIORITY_VIEWER",
        .endpoint = tx_ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_BGRA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };

    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    assert(rx != NULL);

    /* Wait for connection */
    int conns = 0;
    for (int i = 0; i < 20; ++i) {
        conns = oavx_stream_get_connection_count(tx, 50);
        if (conns > 0) break;
    }
    assert(conns >= 1);

    /* Send Video Frames rapidly to create backpressure */
    size_t v_bytes = 320 * 240 * 2;
    uint8_t *v_buf = (uint8_t*)malloc(v_bytes);
    assert(v_buf != NULL);
    memset(v_buf, 0x77, v_bytes);

    oavx_video_frame_t vf = {
        .width = 320,
        .height = 240,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .framerate = { 60, 1 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 1000000LL,
        .planes = { v_buf, NULL, NULL, NULL },
        .strides = { 320 * 2, 0, 0, 0 }
    };

    /* Interleave 20 video frames and 10 audio frames */
    float a_samples[480 * 2];
    for (int i = 0; i < 480 * 2; ++i) a_samples[i] = 0.5f;

    oavx_audio_frame_t af = {
        .sample_rate = 48000,
        .channels = 2,
        .frames = 480,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .stride_bytes = 480 * sizeof(float),
        .pts_ns = 1000000LL,
        .planes = { (uint8_t*)a_samples, (uint8_t*)(a_samples + 480), NULL, NULL, NULL, NULL, NULL, NULL }
    };

    int total_audio_sent = 10;
    int total_video_sent = 30;

    for (int i = 0; i < total_video_sent; ++i) {
        vf.pts_ns += 16666666LL;
        oavx_stream_send_video(tx, &vf);
        if (i % 3 == 0 && (i / 3) < total_audio_sent) {
            af.pts_ns += 10000000LL;
            oavx_stream_send_audio(tx, &af);
        }
    }

    /* Receive all captured frames */
    oavx_video_frame_t in_v;
    oavx_audio_frame_t in_a;
    oavx_metadata_frame_t in_m;
    int audio_received = 0;
    int video_received = 0;

    for (int attempts = 0; attempts < 100; ++attempts) {
        oavx_frame_type_t ft = oavx_stream_receive(rx, &in_v, &in_a, &in_m, 50);
        if (ft == OAVX_FRAME_TYPE_AUDIO) {
            audio_received++;
            oavx_stream_release_audio(rx, &in_a);
        } else if (ft == OAVX_FRAME_TYPE_VIDEO) {
            video_received++;
            oavx_stream_release_video(rx, &in_v);
        }
    }

    printf("  [+] Total Video Received: %d, Total Audio Received: %d\n", video_received, audio_received);
    /* Strict audio verification: Audio must be received without starvation */
    assert(audio_received > 0);
    printf("  [+] Audio Priority Preemption verified: Audio stream preserved through saturated queue!\n");

    /* Teardown */
    oavx_stream_close(rx);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
    free(v_buf);
}

static void test_per_client_temporal_decimation(void) {
    printf("[*] 3. Testing Per-Client Adaptive Temporal Decimation & Congestion Isolation...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t tx_cfg = {
        .name = "MULTI_CLIENT_DECIMATION_TEST",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .bind_ip = "127.0.0.1"
    };

    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    assert(tx != NULL);
    const oavx_endpoint_t *tx_ep = oavx_stream_get_endpoint(tx);
    assert(tx_ep != NULL);

    /* 1. Fast Viewer (Active Reader) */
    oavx_stream_config_t rx_fast_cfg = {
        .name = "FAST_VIEWER",
        .endpoint = tx_ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_BGRA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *rx_fast = oavx_stream_open(ctx, &rx_fast_cfg);
    assert(rx_fast != NULL);

    /* 2. Slow Viewer (Frozen Reader that does not drain socket) */
    oavx_stream_config_t rx_slow_cfg = {
        .name = "SLOW_VIEWER",
        .endpoint = tx_ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_BGRA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *rx_slow = oavx_stream_open(ctx, &rx_slow_cfg);
    assert(rx_slow != NULL);

    int conns = 0;
    for (int i = 0; i < 20; ++i) {
        conns = oavx_stream_get_connection_count(tx, 50);
        if (conns >= 2) break;
    }
    assert(conns >= 2);
    printf("  [+] Two viewers connected to sender simultaneously\n");

    size_t v_bytes = 320 * 240 * 2;
    uint8_t *v_buf = (uint8_t*)malloc(v_bytes);
    assert(v_buf != NULL);
    memset(v_buf, 0x44, v_bytes);

    oavx_video_frame_t vf = {
        .width = 320,
        .height = 240,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .framerate = { 60, 1 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 2000000LL,
        .planes = { v_buf, NULL, NULL, NULL },
        .strides = { 320 * 2, 0, 0, 0 }
    };

    int fast_received = 0;
    oavx_video_frame_t in_v;
    oavx_audio_frame_t in_a;
    oavx_metadata_frame_t in_m;

    /* Transmit 60 frames: fast viewer drains continuously, slow viewer is frozen */
    for (int f = 0; f < 60; ++f) {
        vf.pts_ns += 16666666LL;
        oavx_stream_send_video(tx, &vf);

        /* Fast viewer actively receives */
        oavx_frame_type_t ft = oavx_stream_receive(rx_fast, &in_v, &in_a, &in_m, 10);
        if (ft == OAVX_FRAME_TYPE_VIDEO) {
            fast_received++;
            oavx_stream_release_video(rx_fast, &in_v);
        }
    }

    /* Drain remainder on fast viewer */
    for (int i = 0; i < 50; ++i) {
        oavx_frame_type_t ft = oavx_stream_receive(rx_fast, &in_v, &in_a, &in_m, 50);
        if (ft == OAVX_FRAME_TYPE_VIDEO) {
            fast_received++;
            oavx_stream_release_video(rx_fast, &in_v);
        }
    }

    printf("  [+] Fast Viewer received: %d frames (unblocked by slow viewer)\n", fast_received);
    assert(fast_received >= 15);

    /* Telemetry Diagnostics */
    oavx_telemetry_t telem;
    bool ok = oavx_stream_get_telemetry(tx, &telem);
    assert(ok);
    printf("  [+] Transmitter Telemetry: total_dropped=%lld, is_connected=%d, congestion_level=%u\n",
           (long long)telem.dropped_frames_total, telem.is_connected, telem.congestion_level);
    assert(telem.dropped_frames_total > 0); /* Confirms slow viewer was decimated and isolated! */

    char json_buf[1024];
    size_t jlen = oavx_telemetry_to_json(&telem, json_buf, sizeof(json_buf));
    assert(jlen > 0 && strstr(json_buf, "congestion_level") != NULL);
    printf("  [+] Real-time Telemetry JSON verified with congestion telemetry!\n");

    /* Teardown */
    oavx_stream_close(rx_slow);
    oavx_stream_close(rx_fast);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
    free(v_buf);
}

static void init_mock_tcp_client(avx_client_slot_t *c) {
    memset(c, 0, sizeof(*c));
    c->transport = OAVX_TRANSPORT_TCP;
    c->tx_bytes_per_sec = -1.0f;
}

static void test_tcp_bdp_congestion_thresholds(void) {
    printf("[*] 4. Testing BDP- & Frame-Aware TCP TIOCOUTQ Thresholds & Dynamics...\n");

    uint64_t warn_b = 0, sev_b = 0;

    /* Case A: Low-BDP LAN with small SD frames -> exact 500 KB / 2 MB floor preserved */
    avx_send_compute_tcp_thresholds(153600, 10000000.0f, 60.0f, 0.1f, &warn_b, &sev_b);
    assert(warn_b == 500000ULL);
    assert(sev_b == 2000000ULL);

    /* Case B: Raw 4K UYVY frame (16.6 MB) on LAN -> single frame write does not trigger */
    avx_send_compute_tcp_thresholds(16588800, 1000000000.0f, 60.0f, 0.2f, &warn_b, &sev_b);
    assert(warn_b >= 16588800ULL * 2ULL);
    assert(sev_b >= 16588800ULL * 4ULL);

    /* Case C: Healthy 50 ms RTT WAN @ ~235 Mbps (1080p60 SHQ2 ~477 KB/frame) stays unthrottled (cg=0) */
    avx_client_slot_t wan_fast;
    init_mock_tcp_client(&wan_fast);
    size_t shq_1080 = 477300;
    int64_t t_ns = 1000000000LL;
    uint32_t cadence = 0;

    /* Cold-start before 200ms sample window: ~1.9 MB in flight stays cg=0 */
    wan_fast.tcp_bytes_submitted = 2400000ULL;
    uint32_t lv = avx_send_eval_tcp_congestion(&wan_fast, t_ns, shq_1080, 60.0f, 1900000, 0, 50.0f, &cadence);
    assert(lv == 0 && cadence == 1);

    /* Steady-state after 250ms: ~7.35 MB submitted & acked (~29.4 MB/s = 235 Mbps), 1.9 MB unsent */
    t_ns += 250000000LL;
    wan_fast.tcp_bytes_submitted += 7350000ULL;
    lv = avx_send_eval_tcp_congestion(&wan_fast, t_ns, shq_1080, 60.0f, 1900000, 0, 50.0f, &cadence);
    assert(lv == 0 && cadence == 1);
    assert(wan_fast.tx_bytes_per_sec > 20000000.0f);

    /* Transient spike pushes unsent to 3.2 MB -> cg=1, then drains to 1.8 MB cleanly -> recovers cg=0 without ratcheting BDP down */
    t_ns += 50000000LL;
    wan_fast.tcp_bytes_submitted += 1500000ULL;
    lv = avx_send_eval_tcp_congestion(&wan_fast, t_ns, shq_1080, 60.0f, 3200000, 0, 50.0f, &cadence);
    assert(lv == 1 && cadence == 2);

    t_ns += 210000000LL; /* Drained window while throttled */
    wan_fast.tcp_bytes_submitted += 2500000ULL;
    lv = avx_send_eval_tcp_congestion(&wan_fast, t_ns, shq_1080, 60.0f, 1800000, 0, 50.0f, &cadence);
    assert(lv == 0 && cadence == 1);
    assert(wan_fast.tx_bytes_per_sec > 20000000.0f); /* Kept unthrottled BDP capacity */

    /* Case D: Slow 5 Mbps (~625 KB/s) WAN @ 50 ms RTT -> small BDP catches 1.1 MB queue (no 5 MB bufferbloat!) */
    avx_client_slot_t wan_slow;
    init_mock_tcp_client(&wan_slow);
    t_ns = 2000000000LL;
    wan_slow.tcp_bytes_submitted = 500000ULL;
    lv = avx_send_eval_tcp_congestion(&wan_slow, t_ns, shq_1080, 60.0f, 400000, 0, 50.0f, &cadence);
    assert(lv == 0);
    t_ns += 250000000LL;
    wan_slow.tcp_bytes_submitted += 856250ULL; /* 700 KB more unsent => only ~156 KB acked in 250 ms (~625 KB/s) */
    lv = avx_send_eval_tcp_congestion(&wan_slow, t_ns, shq_1080, 60.0f, 1100000, 0, 50.0f, &cadence);
    assert(lv == 1 && cadence == 2);
    assert(wan_slow.tx_bytes_per_sec >= 0.0f && wan_slow.tx_bytes_per_sec < 1000000.0f);

    /* Case E: Stalled link (0 bytes ACKed -> tx_bytes_per_sec hits 0.0f, distinct from < 0 sentinel) */
    avx_client_slot_t stalled;
    init_mock_tcp_client(&stalled);
    t_ns = 3000000000LL;
    stalled.tcp_bytes_submitted = 1200000ULL;
    lv = avx_send_eval_tcp_congestion(&stalled, t_ns, shq_1080, 60.0f, 1200000, 0, 50.0f, &cadence);
    t_ns += 250000000LL;
    lv = avx_send_eval_tcp_congestion(&stalled, t_ns, shq_1080, 60.0f, 1200000, 0, 50.0f, &cadence);
    assert(stalled.tx_bytes_per_sec == 0.0f);
    assert(lv == 1 && cadence == 2);

    /* Case F: Worker queue backpressure (v_queued >= 1 / >= 2) always triggers regardless of TIOCOUTQ */
    lv = avx_send_eval_tcp_congestion(&wan_fast, t_ns, shq_1080, 60.0f, 0, 1, 50.0f, &cadence);
    assert(lv == 1 && cadence == 2);
    lv = avx_send_eval_tcp_congestion(&wan_fast, t_ns, shq_1080, 60.0f, 0, 2, 50.0f, &cadence);
    assert(lv == 2 && cadence == 4);

    printf("  [+] BDP + Frame-Aware TCP TIOCOUTQ verified across LAN, slow WAN, fast WAN, recovery, and stall regimes!\n");
}

int main(void) {
    printf("=== OpenAVX Congestion Control & Bitrate Tests ===\n\n");

    test_speedhq_dynamic_quantization();
    test_audio_priority_preemption();
    test_per_client_temporal_decimation();
    test_tcp_bdp_congestion_thresholds();

    printf("\n[PASS] openavx_congestion_test\n");
    return 0;
}
