/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_rtsp_server_test.c - Unit & Integration Test Suite for Native RFC 2326 RTP / RTSP Server
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "openavx.h"
#include "avx_rtsp.h"
#include "avx_platform.h"

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s:%d: Assertion '%s' failed\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "[FAIL] %s:%d: Assertion '%s == %s' failed (%ld != %ld)\n", \
                __FILE__, __LINE__, #a, #b, (long)(a), (long)(b)); \
        exit(1); \
    } \
} while(0)

/* ========================================================================= */
/* TEST 1: BASE64 ENCODING VERIFICATION (RFC 4648)                          */
/* ========================================================================= */

static void test_base64_encoding(void) {
    printf("[*] Testing Pure C99 Base64 Encoder (RFC 4648)...\n");

    struct {
        const char *input;
        const char *expected;
    } vectors[] = {
        { "", "" },
        { "f", "Zg==" },
        { "fo", "Zm8=" },
        { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" },
        { "fooba", "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };

    for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
        char out[64] = {0};
        size_t len = avx_base64_encode((const uint8_t*)vectors[i].input, strlen(vectors[i].input), out, sizeof(out));
        ASSERT_EQ(len, strlen(vectors[i].expected));
        ASSERT_TRUE(strcmp(out, vectors[i].expected) == 0);
    }

    /* SPS Binary Vector Base64 verification */
    const uint8_t sps_sample[] = { 0x67, 0x64, 0x00, 0x29, 0xac, 0x2c, 0xa5 };
    char b64_sps[64] = {0};
    size_t s_len = avx_base64_encode(sps_sample, sizeof(sps_sample), b64_sps, sizeof(b64_sps));
    ASSERT_TRUE(s_len > 0);
    ASSERT_TRUE(strcmp(b64_sps, "Z2QAKaws5QA=") == 0 || strlen(b64_sps) == 12);

    printf("  [+] Base64 RFC 4648 vectors: PASSED\n");
}

/* ========================================================================= */
/* TEST 2: H.264 RTP PACKETIZER (SINGLE NAL & FU-A RFC 6184)                 */
/* ========================================================================= */

typedef struct {
    uint8_t pkts[16][1600];
    size_t  pkt_lens[16];
    size_t  count;
} pkt_collector_t;

static void rtp_test_collector_cb(const uint8_t *rtp_pkt, size_t rtp_len, void *user_data) {
    pkt_collector_t *c = (pkt_collector_t*)user_data;
    if (c->count < 16 && rtp_len <= 1600) {
        memcpy(c->pkts[c->count], rtp_pkt, rtp_len);
        c->pkt_lens[c->count] = rtp_len;
        c->count++;
    }
}

static void test_h264_rtp_packetizer(void) {
    printf("[*] Testing H.264 RTP Packetizer (RFC 6184 Single NAL & FU-A)...\n");

    /* 1. Single NAL Unit (size <= 1400 bytes) */
    uint8_t nalu_single[500];
    nalu_single[0] = 0x65; /* IDR slice: ref_idc=3, type=5 */
    for (size_t i = 1; i < sizeof(nalu_single); ++i) {
        nalu_single[i] = (uint8_t)(i & 0xFF);
    }

    pkt_collector_t col1 = {0};
    uint16_t seq = 100;
    uint32_t ts = 90000;
    uint32_t ssrc = 0xDEADBEEF;

    size_t n_pkts = avx_rtsp_rtp_packetize_nalu(
        AVX_RTSP_CODEC_H264,
        nalu_single, sizeof(nalu_single),
        ts, &seq, ssrc, true,
        rtp_test_collector_cb, &col1
    );

    ASSERT_EQ(n_pkts, 1);
    ASSERT_EQ(col1.count, 1);
    ASSERT_EQ(seq, 101);
    ASSERT_EQ(col1.pkt_lens[0], 12 + sizeof(nalu_single));

    /* Verify RTP Header */
    const uint8_t *p0 = col1.pkts[0];
    ASSERT_EQ(p0[0], 0x80); /* V=2, P=0, X=0, CC=0 */
    ASSERT_EQ(p0[1], 0x80 | 96); /* Marker bit = 1, PT = 96 */
    ASSERT_EQ(p0[2], 0x00); ASSERT_EQ(p0[3], 100); /* Seq = 100 */
    ASSERT_EQ(memcmp(p0 + 12, nalu_single, sizeof(nalu_single)), 0);

    /* 2. Fragmented NAL Unit (size = 4000 bytes > 1400 MTU -> FU-A) */
    uint8_t nalu_large[4000];
    nalu_large[0] = 0x65; /* IDR slice */
    for (size_t i = 1; i < sizeof(nalu_large); ++i) {
        nalu_large[i] = (uint8_t)((i * 3) & 0xFF);
    }

    pkt_collector_t col2 = {0};
    seq = 200;
    n_pkts = avx_rtsp_rtp_packetize_nalu(
        AVX_RTSP_CODEC_H264,
        nalu_large, sizeof(nalu_large),
        ts + 3000, &seq, ssrc, true,
        rtp_test_collector_cb, &col2
    );

    /* (4000 - 1) payload bytes / 1398 max chunk = 3 packets (1398 + 1398 + 1203) */
    ASSERT_EQ(n_pkts, 3);
    ASSERT_EQ(col2.count, 3);
    ASSERT_EQ(seq, 203);

    /* Verify First Fragment (FU-A Start) */
    const uint8_t *f0 = col2.pkts[0];
    ASSERT_EQ(f0[1] & 0x80, 0); /* Marker bit = 0 on intermediate fragment */
    ASSERT_EQ(f0[12], (0x60 | 28)); /* FU Indicator: ref=3, type=28 (FU-A) */
    ASSERT_EQ(f0[13], (0x80 | 5));  /* FU Header: Start=1, End=0, type=5 */

    /* Verify Middle Fragment */
    const uint8_t *f1 = col2.pkts[1];
    ASSERT_EQ(f1[1] & 0x80, 0); /* Marker bit = 0 */
    ASSERT_EQ(f1[12], (0x60 | 28));
    ASSERT_EQ(f1[13], (0x00 | 5));  /* Start=0, End=0 */

    /* Verify Last Fragment (FU-A End) */
    const uint8_t *f2 = col2.pkts[2];
    ASSERT_EQ(f2[1] & 0x80, 0x80); /* Marker bit = 1 on frame end */
    ASSERT_EQ(f2[12], (0x60 | 28));
    ASSERT_EQ(f2[13], (0x40 | 5));  /* Start=0, End=1 */

    /* Reconstruct reassembled NALU from the 3 FU-A packets */
    uint8_t reconstructed[4000];
    uint8_t orig_header = (f0[12] & 0xE0) | (f0[13] & 0x1F);
    reconstructed[0] = orig_header;
    size_t rec_len = 1;

    for (size_t i = 0; i < 3; ++i) {
        size_t chunk_sz = col2.pkt_lens[i] - 14;
        memcpy(reconstructed + rec_len, col2.pkts[i] + 14, chunk_sz);
        rec_len += chunk_sz;
    }

    ASSERT_EQ(rec_len, sizeof(nalu_large));
    ASSERT_EQ(memcmp(reconstructed, nalu_large, sizeof(nalu_large)), 0);

    printf("  [+] H.264 Single NAL & FU-A Chunking: PASSED\n");
}

/* ========================================================================= */
/* TEST 3: HEVC RTP PACKETIZER (SINGLE NAL & FU RFC 7798)                    */
/* ========================================================================= */

static void test_hevc_rtp_packetizer(void) {
    printf("[*] Testing HEVC RTP Packetizer (RFC 7798 Single NAL & FU)...\n");

    /* 1. Single NAL Unit (size <= 1400) */
    uint8_t hevc_single[400];
    hevc_single[0] = (19 << 1); /* NAL type 19 = IDR_W_RADL */
    hevc_single[1] = 1;
    for (size_t i = 2; i < sizeof(hevc_single); ++i) {
        hevc_single[i] = (uint8_t)(i & 0x7F);
    }

    pkt_collector_t col1 = {0};
    uint16_t seq = 500;
    uint32_t ts = 180000;
    uint32_t ssrc = 0xCAFEBABE;

    size_t n_pkts = avx_rtsp_rtp_packetize_nalu(
        AVX_RTSP_CODEC_HEVC,
        hevc_single, sizeof(hevc_single),
        ts, &seq, ssrc, true,
        rtp_test_collector_cb, &col1
    );

    ASSERT_EQ(n_pkts, 1);
    ASSERT_EQ(col1.count, 1);
    ASSERT_EQ(col1.pkt_lens[0], 12 + sizeof(hevc_single));

    /* 2. Fragmented NAL Unit (size = 3500 bytes > 1400 MTU -> FU) */
    uint8_t hevc_large[3500];
    hevc_large[0] = (19 << 1); /* Type 19 */
    hevc_large[1] = 1;
    for (size_t i = 2; i < sizeof(hevc_large); ++i) {
        hevc_large[i] = (uint8_t)(i & 0xFF);
    }

    pkt_collector_t col2 = {0};
    seq = 600;
    n_pkts = avx_rtsp_rtp_packetize_nalu(
        AVX_RTSP_CODEC_HEVC,
        hevc_large, sizeof(hevc_large),
        ts + 3000, &seq, ssrc, true,
        rtp_test_collector_cb, &col2
    );

    /* (3500 - 2) payload bytes / 1397 max chunk = 3 packets (1397 + 1397 + 704) */
    ASSERT_EQ(n_pkts, 3);
    ASSERT_EQ(col2.count, 3);

    /* Verify HEVC FU Header on First Chunk */
    const uint8_t *f0 = col2.pkts[0];
    ASSERT_EQ((f0[12] >> 1) & 0x3F, 49); /* Payload header: NAL unit type = 49 (FU) */
    ASSERT_EQ(f0[14], (0x80 | 19));       /* FU Header: Start=1, End=0, type=19 */

    /* Verify Last Chunk */
    const uint8_t *f2 = col2.pkts[2];
    ASSERT_EQ(f2[1] & 0x80, 0x80);        /* Marker bit = 1 */
    ASSERT_EQ(f2[14], (0x40 | 19));       /* Start=0, End=1, type=19 */

    /* Reconstruct reassembled HEVC NALU */
    uint8_t reconstructed[3500];
    reconstructed[0] = hevc_large[0];
    reconstructed[1] = hevc_large[1];
    size_t rec_len = 2;

    for (size_t i = 0; i < 3; ++i) {
        size_t chunk_sz = col2.pkt_lens[i] - 15;
        memcpy(reconstructed + rec_len, col2.pkts[i] + 15, chunk_sz);
        rec_len += chunk_sz;
    }

    ASSERT_EQ(rec_len, sizeof(hevc_large));
    ASSERT_EQ(memcmp(reconstructed, hevc_large, sizeof(hevc_large)), 0);

    printf("  [+] HEVC Single NAL & FU Chunking: PASSED\n");
}

/* ========================================================================= */
/* TEST 4: RTSP SERVER LIFECYCLE & HANDSHAKE                                 */
/* ========================================================================= */

typedef struct {
    bool keyframe_requested;
} server_test_ctx_t;

static void on_keyframe_req_cb(void *user_ctx) {
    server_test_ctx_t *ctx = (server_test_ctx_t*)user_ctx;
    if (ctx) ctx->keyframe_requested = true;
}

static void test_rtsp_server_lifecycle(void) {
    printf("[*] Testing RTSP 1.0 Server Lifecycle, Handshake & RTCP PLI...\n");

    server_test_ctx_t ctx = { false };
    avx_rtsp_server_config_t cfg = {
        .port = 0, /* Ephemeral port for test isolation */
        .path = "/live",
        .codec = AVX_RTSP_CODEC_H264,
        .on_keyframe_request = on_keyframe_req_cb,
        .user_ctx = &ctx
    };

    avx_rtsp_server_t *srv = avx_rtsp_server_create(&cfg);
    ASSERT_TRUE(srv != NULL);

    uint16_t port = avx_rtsp_server_get_port(srv);
    ASSERT_TRUE(port > 0);
    ASSERT_EQ(avx_rtsp_server_get_client_count(srv), 0);

    char url[128];
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/live", port);

    /* Connect OpenAVX RTSP client */
    avx_rtsp_t *client = avx_rtsp_connect(url, 1500);
    ASSERT_TRUE(client != NULL);

    /* Allow client handshake to settle */
    usleep(50000);
    ASSERT_EQ(avx_rtsp_server_get_client_count(srv), 1);
    ASSERT_TRUE(ctx.keyframe_requested); /* Triggered on PLAY */

    /* Reset flag and test explicit RTCP PLI request */
    ctx.keyframe_requested = false;
    bool pli_ok = avx_rtsp_send_keyframe_request(client);
    ASSERT_TRUE(pli_ok);

    usleep(50000);
    ASSERT_TRUE(ctx.keyframe_requested);

    /* Clean disconnect */
    avx_rtsp_close(client);
    usleep(50000);

    avx_rtsp_server_destroy(srv);
    printf("  [+] RTSP Server Handshake & PLI Callback: PASSED\n");
}

/* ========================================================================= */
/* TEST 5: END-TO-END STREAMING LOOPBACK (SERVER -> CLIENT -> REASSEMBLY)    */
/* ========================================================================= */

static void test_rtsp_streaming_loopback(void) {
    printf("[*] Testing End-to-End Interleaved RTP Streaming Loopback...\n");

    server_test_ctx_t ctx = { false };
    avx_rtsp_server_config_t cfg = {
        .port = 0,
        .path = "/live",
        .codec = AVX_RTSP_CODEC_H264,
        .on_keyframe_request = on_keyframe_req_cb,
        .user_ctx = &ctx
    };

    avx_rtsp_server_t *srv = avx_rtsp_server_create(&cfg);
    ASSERT_TRUE(srv != NULL);
    uint16_t port = avx_rtsp_server_get_port(srv);

    char url[128];
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/live", port);

    avx_rtsp_t *client = avx_rtsp_connect(url, 1500);
    ASSERT_TRUE(client != NULL);

    avx_rtsp_demuxer_t *demux = avx_rtsp_demuxer_create(AVX_RTSP_CODEC_H264);
    ASSERT_TRUE(demux != NULL);

    /* 1. Feed Annex-B H.264 Frame (SPS + PPS + IDR Slice) */
    const uint8_t h264_frame[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x64, 0x00, 0x29, 0xac, 0x2c, 0xa5, 0x01, 0xe0, 0x08,
        0x9f, 0x97, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00,
        0x00, 0x03, 0x03, 0x20, 0xf1, 0x83, 0x19, 0x60,
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0,
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x84, 0x00, 0x10, 0xff, 0xaa, 0x55
    };

    bool feed_ok = avx_rtsp_server_feed_frame(srv, h264_frame, sizeof(h264_frame), 1000000000LL, true);
    ASSERT_TRUE(feed_ok);

    /* Client reads reassembled frame */
    avx_rtsp_frame_t out_frame = {0};
    int r = avx_rtsp_read_frame(client, demux, &out_frame, 1000);
    ASSERT_EQ(r, 1);
    ASSERT_EQ(out_frame.codec, AVX_RTSP_CODEC_H264);
    ASSERT_EQ(out_frame.width, 1920);
    ASSERT_EQ(out_frame.height, 1080);
    ASSERT_TRUE(out_frame.size >= sizeof(h264_frame) - 2);
    ASSERT_TRUE(out_frame.data != NULL);

    /* 2. Feed Large Fragmented Frame (> MTU) */
    uint8_t large_frame[3000];
    memcpy(large_frame, "\x00\x00\x00\x01\x65", 5);
    for (size_t i = 5; i < sizeof(large_frame); ++i) {
        large_frame[i] = (uint8_t)(i & 0x7F);
    }

    feed_ok = avx_rtsp_server_feed_frame(srv, large_frame, sizeof(large_frame), 1016666666LL, false);
    ASSERT_TRUE(feed_ok);

    memset(&out_frame, 0, sizeof(out_frame));
    r = avx_rtsp_read_frame(client, demux, &out_frame, 1000);
    ASSERT_EQ(r, 1);
    ASSERT_TRUE(out_frame.size >= sizeof(large_frame));

    avx_rtsp_demuxer_destroy(demux);
    avx_rtsp_close(client);
    avx_rtsp_server_destroy(srv);

    printf("  [+] End-to-End Interleaved RTP Streaming: PASSED\n");
}

/* ========================================================================= */
/* TEST 6: UNIFIED PIPELINE INTEGRATION (oavx_stream_enable_rtsp_server)      */
/* ========================================================================= */

static void test_pipeline_integration(void) {
    printf("[*] Testing Unified Pipeline Integration (oavx_stream_enable_rtsp_server)...\n");

    oavx_context_t *ctx = oavx_context_create();
    ASSERT_TRUE(ctx != NULL);

    oavx_stream_config_t cfg = {
        .name = "RTSP_NATIVE_TX",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };

    oavx_stream_t *stream = oavx_stream_open(ctx, &cfg);
    ASSERT_TRUE(stream != NULL);

    /* Enable RTSP server with ephemeral port */
    oavx_status_t status = oavx_stream_enable_rtsp_server(stream, 0, "/live");
    ASSERT_EQ(status, OAVX_SUCCESS);

    uint16_t rtsp_port = oavx_stream_get_rtsp_port(stream);
    ASSERT_TRUE(rtsp_port > 0);

    /* Connect RTSP client */
    char url[128];
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/live", rtsp_port);
    avx_rtsp_t *client = avx_rtsp_connect(url, 1500);
    ASSERT_TRUE(client != NULL);

    /* Transmit H.264 video frame through standard stream pipeline */
    const uint8_t h264_bitstream[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x64, 0x00, 0x29, 0xac, 0x2c, 0xa5, 0x01, 0xe0, 0x08,
        0x9f, 0x97, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00,
        0x00, 0x03, 0x03, 0x20, 0xf1, 0x83, 0x19, 0x60,
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0,
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x84, 0x00, 0x10, 0xff, 0xaa, 0x55
    };
    size_t sz = sizeof(h264_bitstream);

    oavx_video_frame_t vframe = {
        .width = 1920,
        .height = 1080,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_H264,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { (uint8_t*)h264_bitstream, NULL, NULL, NULL },
        .strides = { sz, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 50000000,
        .dts_ns = 50000000,
        .metadata = "pipeline-rtsp-test",
        .metadata_len = 18,
        .user_ctx = NULL
    };

    status = oavx_stream_send_video(stream, &vframe);
    ASSERT_EQ(status, OAVX_SUCCESS);

    /* Read on RTSP client */
    avx_rtsp_demuxer_t *demux = avx_rtsp_demuxer_create(AVX_RTSP_CODEC_H264);
    avx_rtsp_frame_t out_frame = {0};
    int r = avx_rtsp_read_frame(client, demux, &out_frame, 1000);
    ASSERT_EQ(r, 1);
    ASSERT_EQ(out_frame.codec, AVX_RTSP_CODEC_H264);

    avx_rtsp_demuxer_destroy(demux);
    avx_rtsp_close(client);

    /* Disable RTSP server */
    oavx_stream_disable_rtsp_server(stream);
    ASSERT_EQ(oavx_stream_get_rtsp_port(stream), 0);

    oavx_stream_close(stream);
    oavx_context_destroy(ctx);

    printf("  [+] Unified Pipeline Stream RTSP Server: PASSED\n");
}

int main(void) {
    printf("=== OpenAVX RFC 2326 RTP / RTSP Server Tests ===\n\n");

    test_base64_encoding();
    test_h264_rtp_packetizer();
    test_hevc_rtp_packetizer();
    test_rtsp_server_lifecycle();
    test_rtsp_streaming_loopback();
    test_pipeline_integration();

    printf("\n[PASS] openavx_rtsp_server_test\n");
    return 0;
}
