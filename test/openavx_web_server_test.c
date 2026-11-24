/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_web_server_test.c - Unit & Integration Test Suite for Embedded Web Preview Server
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "openavx.h"
#include "avx_web.h"
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

/* Helper: read exact number of bytes from a TCP socket with timeout */
static bool socket_read_exact(int fd, uint8_t *buf, size_t len, int timeout_ms) {
    size_t received = 0;
    while (received < len) {
        avx_pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = avx_poll(&pfd, 1, timeout_ms);
        if (pr <= 0) return false;

        ssize_t n = recv(fd, buf + received, len - received, 0);
        if (n <= 0) return false;
        received += (size_t)n;
    }
    return true;
}

/* ========================================================================= */
/* TEST 1: ISOBMFF INIT SEGMENT GENERATOR (ftyp + moov)                      */
/* ========================================================================= */

static void test_fmp4_init_segment(void) {
    printf("[*] Testing fMP4 Init Segment Generator (ftyp + moov)...\n");

    /* Synthetic H.264 SPS & PPS */
    uint8_t sps[] = { 0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0x84 };
    uint8_t pps[] = { 0x68, 0xEB, 0xEF, 0x20 };

    uint8_t init_buf[4096];
    size_t len = avx_fmp4_generate_init_segment(
        AVX_RTSP_CODEC_H264,
        1920, 1080,
        sps, sizeof(sps),
        pps, sizeof(pps),
        NULL, 0,
        init_buf, sizeof(init_buf)
    );

    ASSERT_TRUE(len > 100);
    ASSERT_TRUE(len < sizeof(init_buf));

    /* Verify ftyp Box */
    uint32_t ftyp_size = (init_buf[0] << 24) | (init_buf[1] << 16) | (init_buf[2] << 8) | init_buf[3];
    ASSERT_EQ(ftyp_size, 32);
    ASSERT_TRUE(memcmp(init_buf + 4, "ftyp", 4) == 0);
    ASSERT_TRUE(memcmp(init_buf + 8, "isom", 4) == 0);

    /* Verify moov Box follows ftyp */
    const uint8_t *moov = init_buf + ftyp_size;
    uint32_t moov_size = (moov[0] << 24) | (moov[1] << 16) | (moov[2] << 8) | moov[3];
    ASSERT_TRUE(memcmp(moov + 4, "moov", 4) == 0);
    ASSERT_EQ(ftyp_size + moov_size, len);

    /* Test HEVC init segment */
    uint8_t vps_hevc[] = { 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00 };
    uint8_t sps_hevc[] = { 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00 };
    uint8_t pps_hevc[] = { 0x44, 0x01, 0xC0, 0xF7, 0xC0 };

    size_t hevc_len = avx_fmp4_generate_init_segment(
        AVX_RTSP_CODEC_HEVC,
        3840, 2160,
        sps_hevc, sizeof(sps_hevc),
        pps_hevc, sizeof(pps_hevc),
        vps_hevc, sizeof(vps_hevc),
        init_buf, sizeof(init_buf)
    );
    ASSERT_TRUE(hevc_len > 100);

    /* Test AV1 init segment */
    uint8_t seq_obu[] = { 0x0A, 0x0E, 0x00, 0x00, 0x00, 0x4A, 0xAB, 0xBF, 0xC3, 0x70, 0x0B, 0x80, 0x20, 0x1D, 0x80 };
    size_t av1_len = avx_fmp4_generate_init_segment(
        AVX_RTSP_CODEC_AV1,
        1280, 720,
        seq_obu, sizeof(seq_obu),
        NULL, 0,
        NULL, 0,
        init_buf, sizeof(init_buf)
    );
    ASSERT_TRUE(av1_len > 100);

    /* Test bounds safety with undersized buffer */
    size_t fail_len = avx_fmp4_generate_init_segment(
        AVX_RTSP_CODEC_H264,
        1920, 1080,
        sps, sizeof(sps),
        pps, sizeof(pps),
        NULL, 0,
        init_buf, 64
    );
    ASSERT_EQ(fail_len, 0);

    printf("    [+] fMP4 Init Segments (H.264, HEVC, AV1) generated and verified!\n");
}

/* ========================================================================= */
/* TEST 2: ISOBMFF MEDIA FRAGMENT GENERATOR (moof + mdat)                   */
/* ========================================================================= */

static void test_fmp4_media_fragment(void) {
    printf("[*] Testing fMP4 Media Fragment Generator (moof + mdat)...\n");

    /* Synthetic AVCC sample payload (4-byte length + NALU) */
    uint8_t sample_avcc[] = {
        0x00, 0x00, 0x00, 0x05, /* 5 bytes NALU length */
        0x65, 0x88, 0x84, 0x00, 0x10 /* IDR slice NALU */
    };

    uint8_t frag_buf[1024];
    size_t frag_len = avx_fmp4_generate_media_fragment(
        1,              /* sequence_number */
        90000,          /* base_decode_time (1.0s at 90kHz) */
        1500,           /* duration (60fps) */
        true,           /* is_keyframe */
        sample_avcc,
        sizeof(sample_avcc),
        frag_buf,
        sizeof(frag_buf)
    );

    ASSERT_TRUE(frag_len > sizeof(sample_avcc));
    ASSERT_TRUE(frag_len < sizeof(frag_buf));

    /* Verify moof Box */
    uint32_t moof_size = (frag_buf[0] << 24) | (frag_buf[1] << 16) | (frag_buf[2] << 8) | frag_buf[3];
    ASSERT_TRUE(memcmp(frag_buf + 4, "moof", 4) == 0);

    /* Verify mdat Box follows moof */
    const uint8_t *mdat = frag_buf + moof_size;
    uint32_t mdat_size = (mdat[0] << 24) | (mdat[1] << 16) | (mdat[2] << 8) | mdat[3];
    ASSERT_TRUE(memcmp(mdat + 4, "mdat", 4) == 0);
    ASSERT_EQ(mdat_size, 8 + sizeof(sample_avcc));
    ASSERT_TRUE(memcmp(mdat + 8, sample_avcc, sizeof(sample_avcc)) == 0);
    ASSERT_EQ(moof_size + mdat_size, frag_len);

    /* Verify trun data_offset matches moof_size + 8 */
    uint32_t expected_data_offset = moof_size + 8;
    /* In trun, data_offset is at offset: moof(8) + mfhd(16) + traf(8) + tfhd(16) + tfdt(20) + trun_hdr(12) + sample_cnt(4) = 84 */
    const uint8_t *p_trun_do = frag_buf + 84;
    uint32_t trun_do = (p_trun_do[0] << 24) | (p_trun_do[1] << 16) | (p_trun_do[2] << 8) | p_trun_do[3];
    ASSERT_EQ(trun_do, expected_data_offset);

    /* Test bounds safety */
    size_t fail_len = avx_fmp4_generate_media_fragment(
        1, 90000, 1500, true, sample_avcc, sizeof(sample_avcc), frag_buf, 32
    );
    ASSERT_EQ(fail_len, 0);

    printf("    [+] fMP4 Media Fragments generated and box offsets verified!\n");
}

/* ========================================================================= */
/* TEST 3: WEB SERVER LIFECYCLE & HTTP ENDPOINTS                            */
/* ========================================================================= */

static void test_web_server_endpoints(void) {
    printf("[*] Testing Web Server Lifecycle & HTTP Endpoints (GET /, GET /status)...\n");

    avx_web_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0; /* Auto-assign ephemeral port */
    cfg.stream_name = "Studio-Cam-1";
    cfg.codec = AVX_RTSP_CODEC_H264;

    avx_web_server_t *srv = avx_web_server_create(&cfg);
    ASSERT_TRUE(srv != NULL);

    uint16_t port = avx_web_server_get_port(srv);
    ASSERT_TRUE(port > 1024);
    ASSERT_EQ(avx_web_server_get_client_count(srv), 0);

    /* 1. Test GET / (HTML Player) */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(port);

    int cr = connect(fd, (struct sockaddr*)&sin, sizeof(sin));
    ASSERT_EQ(cr, 0);

    const char *get_root = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(fd, get_root, strlen(get_root), 0);

    char resp[4096] = {0};
    size_t total = 0;
    while (total < sizeof(resp) - 1) {
        ssize_t n = recv(fd, resp + total, sizeof(resp) - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        if (strstr(resp, "</html>")) break;
    }
    ASSERT_TRUE(total > 0);
    ASSERT_TRUE(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(resp, "Content-Type: text/html") != NULL);
    ASSERT_TRUE(strstr(resp, "MediaSource") != NULL);
    ASSERT_TRUE(strstr(resp, "<video") != NULL);
    close(fd);

    /* 2. Test GET /status (JSON Stats) */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    cr = connect(fd, (struct sockaddr*)&sin, sizeof(sin));
    ASSERT_EQ(cr, 0);

    const char *get_status = "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(fd, get_status, strlen(get_status), 0);

    memset(resp, 0, sizeof(resp));
    ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(resp, "Content-Type: application/json") != NULL);
    ASSERT_TRUE(strstr(resp, "\"status\":\"online\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"Studio-Cam-1\"") != NULL);
    close(fd);

    /* 3. Test OPTIONS /live.mp4 (CORS Pre-Flight) */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);
    cr = connect(fd, (struct sockaddr*)&sin, sizeof(sin));
    ASSERT_EQ(cr, 0);

    const char *opt_req = "OPTIONS /live.mp4 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(fd, opt_req, strlen(opt_req), 0);

    memset(resp, 0, sizeof(resp));
    n = recv(fd, resp, sizeof(resp) - 1, 0);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(resp, "HTTP/1.1 204 No Content") != NULL);
    ASSERT_TRUE(strstr(resp, "Access-Control-Allow-Origin: *") != NULL);
    close(fd);

    avx_web_server_destroy(srv);
    printf("    [+] Web server lifecycle and HTTP responses verified!\n");
}

/* ========================================================================= */
/* TEST 4: LIVE fMP4 STREAMING (GET /live.mp4 + Feed Frame)                  */
/* ========================================================================= */

static volatile bool g_keyframe_callback_called = false;
static void on_keyframe_req(void *user_ctx) {
    (void)user_ctx;
    g_keyframe_callback_called = true;
}

static void test_live_fmp4_streaming(void) {
    printf("[*] Testing Live fMP4 Streaming (GET /live.mp4 + Feed Frame)...\n");

    avx_web_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0;
    cfg.stream_name = "Cam-Live";
    cfg.codec = AVX_RTSP_CODEC_H264;
    cfg.on_keyframe_request = on_keyframe_req;

    g_keyframe_callback_called = false;
    avx_web_server_t *srv = avx_web_server_create(&cfg);
    ASSERT_TRUE(srv != NULL);
    uint16_t port = avx_web_server_get_port(srv);

    /* Connect client socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(port);

    int cr = connect(fd, (struct sockaddr*)&sin, sizeof(sin));
    ASSERT_EQ(cr, 0);

    /* Issue GET /live.mp4 */
    const char *get_live = "GET /live.mp4 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(fd, get_live, strlen(get_live), 0);

    /* Give server worker thread a few ms to process GET request */
    avx_sleep_ms(50);
    ASSERT_TRUE(g_keyframe_callback_called);
    ASSERT_EQ(avx_web_server_get_client_count(srv), 1);

    /* Feed an H.264 IDR Frame with SPS + PPS + IDR Slice in Annex-B */
    uint8_t h264_keyframe[] = {
        /* SPS */
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0x84,
        /* PPS */
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEB, 0xEF, 0x20,
        /* IDR Slice */
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x10, 0xFF, 0xEE, 0xDD
    };

    bool feed_ok = avx_web_server_feed_frame(srv, h264_keyframe, sizeof(h264_keyframe), 1000000000ULL, true);
    ASSERT_TRUE(feed_ok);

    /* Read response header from client socket */
    char hdr_buf[1024] = {0};
    size_t hdr_len = 0;
    while (hdr_len < sizeof(hdr_buf) - 1) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        ASSERT_TRUE(n > 0);
        hdr_buf[hdr_len++] = ch;
        if (hdr_len >= 4 && memcmp(hdr_buf + hdr_len - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    ASSERT_TRUE(strstr(hdr_buf, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(hdr_buf, "Content-Type: video/mp4") != NULL);

    /* Next bytes must be the fMP4 init segment: 'ftyp' */
    uint8_t box_hdr[8];
    ASSERT_TRUE(socket_read_exact(fd, box_hdr, 8, 500));
    uint32_t ftyp_size = (box_hdr[0] << 24) | (box_hdr[1] << 16) | (box_hdr[2] << 8) | box_hdr[3];
    ASSERT_EQ(ftyp_size, 32);
    ASSERT_TRUE(memcmp(box_hdr + 4, "ftyp", 4) == 0);

    /* Read remaining ftyp bytes (24 bytes) */
    uint8_t ftyp_rest[24];
    ASSERT_TRUE(socket_read_exact(fd, ftyp_rest, 24, 500));

    /* Next box must be 'moov' */
    ASSERT_TRUE(socket_read_exact(fd, box_hdr, 8, 500));
    uint32_t moov_size = (box_hdr[0] << 24) | (box_hdr[1] << 16) | (box_hdr[2] << 8) | box_hdr[3];
    ASSERT_TRUE(memcmp(box_hdr + 4, "moov", 4) == 0);
    uint8_t *moov_body = (uint8_t*)malloc(moov_size - 8);
    ASSERT_TRUE(socket_read_exact(fd, moov_body, moov_size - 8, 500));
    free(moov_body);

    /* Next box must be 'moof' (media fragment) */
    ASSERT_TRUE(socket_read_exact(fd, box_hdr, 8, 500));
    uint32_t moof_size = (box_hdr[0] << 24) | (box_hdr[1] << 16) | (box_hdr[2] << 8) | box_hdr[3];
    ASSERT_TRUE(memcmp(box_hdr + 4, "moof", 4) == 0);
    uint8_t *moof_body = (uint8_t*)malloc(moof_size - 8);
    ASSERT_TRUE(socket_read_exact(fd, moof_body, moof_size - 8, 500));
    free(moof_body);

    /* Next box must be 'mdat' */
    ASSERT_TRUE(socket_read_exact(fd, box_hdr, 8, 500));
    uint32_t mdat_size = (box_hdr[0] << 24) | (box_hdr[1] << 16) | (box_hdr[2] << 8) | box_hdr[3];
    ASSERT_TRUE(memcmp(box_hdr + 4, "mdat", 4) == 0);
    uint8_t *mdat_body = (uint8_t*)malloc(mdat_size - 8);
    ASSERT_TRUE(socket_read_exact(fd, mdat_body, mdat_size - 8, 500));
    free(mdat_body);

    /* Feed a subsequent P-Frame */
    uint8_t h264_pframe[] = {
        0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x12, 0x34
    };
    feed_ok = avx_web_server_feed_frame(srv, h264_pframe, sizeof(h264_pframe), 1016666666ULL, false);
    ASSERT_TRUE(feed_ok);

    /* Verify next 'moof' box arrives */
    ASSERT_TRUE(socket_read_exact(fd, box_hdr, 8, 500));
    ASSERT_TRUE(memcmp(box_hdr + 4, "moof", 4) == 0);

    close(fd);
    avx_sleep_ms(50);

    /* Check that client count updates after socket close */
    avx_web_server_feed_frame(srv, h264_pframe, sizeof(h264_pframe), 1033333333ULL, false);
    ASSERT_EQ(avx_web_server_get_client_count(srv), 0);

    avx_web_server_destroy(srv);
    printf("    [+] Live fMP4 stream negotiation and fragment delivery verified!\n");
}

/* ========================================================================= */
/* TEST 5: UNIFIED PIPELINE INTEGRATION                                      */
/* ========================================================================= */

static void test_pipeline_web_preview(void) {
    printf("[*] Testing Unified Pipeline Integration (oavx_stream_enable_web_preview)...\n");

    oavx_context_t *ctx = oavx_context_create();
    ASSERT_TRUE(ctx != NULL);

    oavx_stream_config_t config;
    memset(&config, 0, sizeof(config));
    config.name = "Web-Pipeline-Test";
    config.direction = OAVX_DIR_TRANSMIT;
    config.clock_sync = false;

    oavx_stream_t *stream = oavx_stream_open(ctx, &config);
    ASSERT_TRUE(stream != NULL);

    /* Enable Web Preview */
    oavx_status_t status = oavx_stream_enable_web_preview(stream, 0);
    ASSERT_EQ(status, OAVX_SUCCESS);

    uint16_t port = oavx_stream_get_web_preview_port(stream);
    ASSERT_TRUE(port > 1024);

    char url[128] = {0};
    const char *url_ptr = oavx_stream_get_web_preview_url(stream, url, sizeof(url));
    ASSERT_TRUE(url_ptr != NULL);
    ASSERT_TRUE(strstr(url, "http://") != NULL);
    printf("    [+] Web preview URL generated: %s\n", url);

    /* Feed an H.264 frame through the unified pipeline */
    uint8_t h264_frame[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0x84,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEB, 0xEF, 0x20,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x10, 0xAA, 0xBB, 0xCC
    };

    oavx_video_frame_t vf;
    memset(&vf, 0, sizeof(vf));
    vf.fourcc = OAVX_FOURCC_VIDEO_TYPE_H264;
    vf.scan_type = OAVX_SCAN_PROGRESSIVE;
    vf.width = 1920;
    vf.height = 1080;
    vf.planes[0] = h264_frame;
    vf.strides[0] = sizeof(h264_frame);
    vf.framerate.num = 60000;
    vf.framerate.den = 1000;
    vf.pts_ns = 1000000000ULL;

    oavx_status_t send_st = oavx_stream_send_video(stream, &vf);
    ASSERT_EQ(send_st, OAVX_SUCCESS);

    /* Disable Web Preview */
    oavx_stream_disable_web_preview(stream);
    ASSERT_EQ(oavx_stream_get_web_preview_port(stream), 0);

    oavx_stream_close(stream);
    oavx_context_destroy(ctx);

    printf("    [+] Pipeline web preview lifecycle and frame dispatch verified!\n");
}

static void test_web_server_av1_and_hevc_cra_keyframes(void) {
    printf("[*] Testing AV1 & HEVC CRA Auto-Keyframe Detection in Web Preview Server...\n");

    /* 1. HEVC CRA keyframe detection when caller passes is_keyframe=false */
    avx_web_server_config_t hcfg;
    memset(&hcfg, 0, sizeof(hcfg));
    hcfg.port = 0;
    hcfg.stream_name = "HEVC-CRA-Test";
    hcfg.codec = AVX_RTSP_CODEC_HEVC;

    avx_web_server_t *hsrv = avx_web_server_create(&hcfg);
    ASSERT_TRUE(hsrv != NULL);
    uint16_t hport = avx_web_server_get_port(hsrv);

    int hfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(hfd >= 0);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(hport);
    ASSERT_EQ(connect(hfd, (struct sockaddr*)&sin, sizeof(sin)), 0);

    const char *req = "GET /live.mp4 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(hfd, req, strlen(req), 0);
    avx_sleep_ms(50);

    /* HEVC CRA_NUT (type 21 => (21 << 1) = 0x2A, layer/tid byte 0x01) with is_keyframe=false */
    uint8_t hevc_cra[] = {
        0x00, 0x00, 0x00, 0x01, 0x2A, 0x01, 0xDE, 0xAD, 0xBE, 0xEF
    };
    ASSERT_TRUE(avx_web_server_feed_frame(hsrv, hevc_cra, sizeof(hevc_cra), 1000000000ULL, false));

    char hdr_buf[1024] = {0};
    size_t hdr_len = 0;
    while (hdr_len < sizeof(hdr_buf) - 1) {
        char ch;
        ssize_t n = recv(hfd, &ch, 1, 0);
        ASSERT_TRUE(n > 0);
        hdr_buf[hdr_len++] = ch;
        if (hdr_len >= 4 && memcmp(hdr_buf + hdr_len - 4, "\r\n\r\n", 4) == 0) break;
    }
    ASSERT_TRUE(strstr(hdr_buf, "HTTP/1.1 200 OK") != NULL);
    uint8_t box_hdr[8];
    ASSERT_TRUE(socket_read_exact(hfd, box_hdr, 8, 500));
    ASSERT_TRUE(memcmp(box_hdr + 4, "ftyp", 4) == 0);
    close(hfd);
    avx_web_server_destroy(hsrv);

    /* 2. AV1 OBU_FRAME keyframe detection when caller passes is_keyframe=false */
    avx_web_server_config_t acfg;
    memset(&acfg, 0, sizeof(acfg));
    acfg.port = 0;
    acfg.stream_name = "AV1-KF-Test";
    acfg.codec = AVX_RTSP_CODEC_AV1;

    avx_web_server_t *asrv = avx_web_server_create(&acfg);
    ASSERT_TRUE(asrv != NULL);
    uint16_t aport = avx_web_server_get_port(asrv);

    int afd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(afd >= 0);
    sin.sin_port = htons(aport);
    ASSERT_EQ(connect(afd, (struct sockaddr*)&sin, sizeof(sin)), 0);
    send(afd, req, strlen(req), 0);
    avx_sleep_ms(50);

    /* AV1 OBU_FRAME (obu_type=6 => (6<<3)|has_sz = 0x32, len=1, byte0=0x00 => KEY_FRAME) */
    uint8_t av1_kf[] = { 0x32, 0x01, 0x00 };
    ASSERT_TRUE(avx_web_server_feed_frame(asrv, av1_kf, sizeof(av1_kf), 1000000000ULL, false));

    memset(hdr_buf, 0, sizeof(hdr_buf));
    hdr_len = 0;
    while (hdr_len < sizeof(hdr_buf) - 1) {
        char ch;
        ssize_t n = recv(afd, &ch, 1, 0);
        ASSERT_TRUE(n > 0);
        hdr_buf[hdr_len++] = ch;
        if (hdr_len >= 4 && memcmp(hdr_buf + hdr_len - 4, "\r\n\r\n", 4) == 0) break;
    }
    ASSERT_TRUE(strstr(hdr_buf, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(socket_read_exact(afd, box_hdr, 8, 500));
    ASSERT_TRUE(memcmp(box_hdr + 4, "ftyp", 4) == 0);
    close(afd);
    avx_web_server_destroy(asrv);

    printf("    [+] HEVC CRA and AV1 keyframe auto-detection verified!\n");
}

/* ========================================================================= */
/* TEST 7: RFC 3174 SHA-1 & RFC 6455 WEBSOCKET ACCEPT KEY GENERATION        */
/* ========================================================================= */

static void test_sha1_and_websocket_accept(void) {
    printf("[*] Testing SHA-1 & RFC 6455 WebSocket Handshake Cryptography...\n");

    /* Vector 1: SHA-1 of "The quick brown fox jumps over the lazy dog" */
    const char *fox_str = "The quick brown fox jumps over the lazy dog";
    uint8_t fox_digest[20];
    avx_sha1_hash((const uint8_t*)fox_str, strlen(fox_str), fox_digest);
    uint8_t fox_expected[20] = {
        0x2f, 0xd4, 0xe1, 0xc6, 0x7a, 0x2d, 0x28, 0xfc, 0xed, 0x84,
        0x9e, 0xe1, 0xbb, 0x76, 0xe7, 0x39, 0x1b, 0x93, 0xeb, 0x12
    };
    ASSERT_TRUE(memcmp(fox_digest, fox_expected, 20) == 0);

    /* Vector 2: RFC 6455 Section 1.3 Key Accept test vector */
    /* Key: "dGhlIHNhbXBsZSBub25jZQ==" -> Accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" */
    char accept_out[64] = {0};
    size_t alen = avx_websocket_compute_accept("dGhlIHNhbXBsZSBub25jZQ==", accept_out, sizeof(accept_out));
    ASSERT_EQ(alen, 28);
    ASSERT_TRUE(strcmp(accept_out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

    printf("    [+] SHA-1 NIST vector and RFC 6455 Sec-WebSocket-Accept token validated!\n");
}

/* ========================================================================= */
/* TEST 8: SMARTPHONE WEB CAMERA INGRESS & WEBSOCKET PROTOCOL ENGINE         */
/* ========================================================================= */

static volatile int g_ingress_frame_count = 0;
static uint32_t g_ingress_fourcc = 0;
static uint32_t g_ingress_width = 0;
static uint32_t g_ingress_height = 0;
static bool g_ingress_is_keyframe = false;
static uint8_t g_ingress_received_data[256];
static size_t g_ingress_received_size = 0;

static void test_camera_ingress_cb(
    const uint8_t *frame_data,
    size_t frame_size,
    uint32_t fourcc,
    uint32_t width,
    uint32_t height,
    bool is_keyframe,
    void *user_ctx
) {
    (void)user_ctx;
    g_ingress_frame_count++;
    g_ingress_fourcc = fourcc;
    g_ingress_width = width;
    g_ingress_height = height;
    g_ingress_is_keyframe = is_keyframe;
    if (frame_size <= sizeof(g_ingress_received_data)) {
        memcpy(g_ingress_received_data, frame_data, frame_size);
        g_ingress_received_size = frame_size;
    }
}

static void test_camera_page_and_websocket_ingress(void) {
    printf("[*] Testing Smartphone Web Camera Ingress (GET /camera + RFC 6455 WS)...\n");

    g_ingress_frame_count = 0;
    g_ingress_received_size = 0;

    avx_web_server_config_t cfg = {
        .port = 0,
        .stream_name = "Phone Camera Test",
        .codec = AVX_RTSP_CODEC_H264,
        .on_ingress_frame = test_camera_ingress_cb,
        .user_ctx = NULL
    };

    avx_web_server_t *srv = avx_web_server_create(&cfg);
    ASSERT_TRUE(srv != NULL);
    uint16_t port = avx_web_server_get_port(srv);
    ASSERT_TRUE(port > 0);

    /* 1. Test HTTP GET /camera returns 200 OK and HTML5 camera UI */
    int http_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(http_fd >= 0);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    ASSERT_EQ(connect(http_fd, (struct sockaddr*)&sin, sizeof(sin)), 0);

    const char *get_cam_req = "GET /camera HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(http_fd, get_cam_req, strlen(get_cam_req), 0);

    char http_resp[32768] = {0};
    size_t read_bytes = 0;
    while (read_bytes < sizeof(http_resp) - 1) {
        ssize_t n = recv(http_fd, http_resp + read_bytes, sizeof(http_resp) - 1 - read_bytes, 0);
        if (n <= 0) break;
        read_bytes += (size_t)n;
    }
    close(http_fd);

    ASSERT_TRUE(strstr(http_resp, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(http_resp, "Content-Type: text/html") != NULL);
    ASSERT_TRUE(strstr(http_resp, "OpenAVX Mobile Camera") != NULL);
    ASSERT_TRUE(strstr(http_resp, "OPENAVX CAM") != NULL);

    /* 2. Test WebSocket Handshake & Frame Ingress on /camera/ws */
    int ws_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(ws_fd >= 0);
    ASSERT_EQ(connect(ws_fd, (struct sockaddr*)&sin, sizeof(sin)), 0);

    const char *ws_upgrade_req =
        "GET /camera/ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    send(ws_fd, ws_upgrade_req, strlen(ws_upgrade_req), 0);

    char ws_resp[1024] = {0};
    size_t ws_read = 0;
    while (ws_read < sizeof(ws_resp) - 1) {
        ssize_t n = recv(ws_fd, ws_resp + ws_read, sizeof(ws_resp) - 1 - ws_read, 0);
        if (n <= 0) break;
        ws_read += (size_t)n;
        if (strstr(ws_resp, "\r\n\r\n")) break;
    }

    ASSERT_TRUE(strstr(ws_resp, "HTTP/1.1 101 Switching Protocols") != NULL);
    ASSERT_TRUE(strstr(ws_resp, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);

    /* 3. Send RFC 6455 Masked Binary Frame with OAVX packet header */
    uint8_t payload[16 + 64];
    avx_web_frame_header_t *whdr = (avx_web_frame_header_t*)payload;
    whdr->magic = AVX_WEB_FRAME_MAGIC;
    whdr->fourcc = OAVX_FOURCC_VIDEO_TYPE_H264;
    whdr->width = 1920;
    whdr->height = 1080;
    whdr->flags = 1; /* Keyframe */

    /* Fill payload with synthetic video bytes */
    for (size_t i = 0; i < 64; ++i) {
        payload[16 + i] = (uint8_t)(0xA0 + i);
    }

    /* Build RFC 6455 Frame: FIN=1, Opcode=0x2 (Binary), Masked=1 */
    size_t plen = sizeof(payload);
    uint8_t ws_frame[256];
    ws_frame[0] = 0x82; /* 0x80 | 0x02 */
    ws_frame[1] = 0x80 | (uint8_t)plen; /* Masked bit + 80-byte payload */

    uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    memcpy(ws_frame + 2, mask, 4);

    /* Mask payload and copy into ws_frame */
    for (size_t i = 0; i < plen; ++i) {
        ws_frame[6 + i] = payload[i] ^ mask[i & 3];
    }

    send(ws_fd, ws_frame, 6 + plen, 0);

    /* Wait for server worker to process packet */
    for (int retry = 0; retry < 50 && g_ingress_frame_count == 0; ++retry) {
        avx_sleep_ms(10);
    }

    ASSERT_EQ(g_ingress_frame_count, 1);
    ASSERT_EQ(g_ingress_fourcc, OAVX_FOURCC_VIDEO_TYPE_H264);
    ASSERT_EQ(g_ingress_width, 1920);
    ASSERT_EQ(g_ingress_height, 1080);
    ASSERT_TRUE(g_ingress_is_keyframe);
    ASSERT_EQ(g_ingress_received_size, 64);
    ASSERT_TRUE(memcmp(g_ingress_received_data, payload + 16, 64) == 0);

    /* Send WebSocket Close frame (opcode 0x8) */
    uint8_t close_pkt[6] = { 0x88, 0x80, 0x11, 0x22, 0x33, 0x44 };
    send(ws_fd, close_pkt, sizeof(close_pkt), 0);
    close(ws_fd);

    avx_web_server_destroy(srv);
    printf("    [+] /camera endpoint, RFC 6455 WebSocket handshake, and frame ingress verified!\n");
}

/* ========================================================================= */
/* TEST 9: REMOTE CAMERA TALLY REST API & WEBSOCKET BROADCAST                */
/* ========================================================================= */

static void test_camera_tally_broadcast_and_rest_api(void) {
    printf("[*] Testing Camera Tally Broadcast & REST API (/api/tally + WS push)...\n");

    avx_web_server_config_t cfg = {
        .port = 0,
        .stream_name = "Tally-Test-Cam",
        .codec = AVX_RTSP_CODEC_H264
    };

    avx_web_server_t *srv = avx_web_server_create(&cfg);
    ASSERT_TRUE(srv != NULL);
    uint16_t port = avx_web_server_get_port(srv);
    ASSERT_TRUE(port > 0);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

    /* 1. GET /api/tally (Initial default: Off-Air / Program=false, Preview=false, Recording=false) */
    int http_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(http_fd >= 0);
    ASSERT_EQ(connect(http_fd, (struct sockaddr*)&sin, sizeof(sin)), 0);

    const char *get_tally_req = "GET /api/tally HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(http_fd, get_tally_req, strlen(get_tally_req), 0);

    char resp[1024] = {0};
    ssize_t n = recv(http_fd, resp, sizeof(resp) - 1, 0);
    ASSERT_TRUE(n > 0);
    close(http_fd);

    ASSERT_TRUE(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(resp, "\"program\":false") != NULL);
    ASSERT_TRUE(strstr(resp, "\"preview\":false") != NULL);
    ASSERT_TRUE(strstr(resp, "\"recording\":false") != NULL);
    ASSERT_TRUE(strstr(resp, "\"flags\":0") != NULL);

    /* 1b. GET /api/tally/text (Plaintext microcontroller endpoint) */
    http_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(http_fd >= 0);
    ASSERT_EQ(connect(http_fd, (struct sockaddr*)&sin, sizeof(sin)), 0);
    const char *get_txt_req = "GET /api/tally/text HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(http_fd, get_txt_req, strlen(get_txt_req), 0);
    memset(resp, 0, sizeof(resp));
    n = recv(http_fd, resp, sizeof(resp) - 1, 0);
    ASSERT_TRUE(n > 0);
    close(http_fd);
    ASSERT_TRUE(strstr(resp, "Content-Type: text/plain") != NULL);
    ASSERT_TRUE(strstr(resp, "OFF") != NULL);

    /* 2. Broadcast ON AIR (Program: Red) */
    oavx_tally_t t_prog = { .flags = OAVX_TALLY_PROGRAM };
    avx_web_server_broadcast_tally(srv, &t_prog);

    /* 3. GET /tally to verify updated status */
    http_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(http_fd >= 0);
    ASSERT_EQ(connect(http_fd, (struct sockaddr*)&sin, sizeof(sin)), 0);
    const char *get_tally2 = "GET /tally HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(http_fd, get_tally2, strlen(get_tally2), 0);
    memset(resp, 0, sizeof(resp));
    n = recv(http_fd, resp, sizeof(resp) - 1, 0);
    ASSERT_TRUE(n > 0);
    close(http_fd);

    ASSERT_TRUE(strstr(resp, "\"program\":true") != NULL);
    ASSERT_TRUE(strstr(resp, "\"preview\":false") != NULL);
    ASSERT_TRUE(strstr(resp, "\"recording\":false") != NULL);
    ASSERT_TRUE(strstr(resp, "\"flags\":1") != NULL);

    /* 3b. Verify plaintext shows PROGRAM */
    http_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(http_fd >= 0);
    ASSERT_EQ(connect(http_fd, (struct sockaddr*)&sin, sizeof(sin)), 0);
    send(http_fd, get_txt_req, strlen(get_txt_req), 0);
    memset(resp, 0, sizeof(resp));
    n = recv(http_fd, resp, sizeof(resp) - 1, 0);
    ASSERT_TRUE(n > 0);
    close(http_fd);
    ASSERT_TRUE(strstr(resp, "PROGRAM") != NULL);

    /* 4. Connect WebSocket client and verify initial tally push upon upgrade */
    int ws_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(ws_fd >= 0);
    ASSERT_EQ(connect(ws_fd, (struct sockaddr*)&sin, sizeof(sin)), 0);

    const char *ws_upgrade_req =
        "GET /camera/ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    send(ws_fd, ws_upgrade_req, strlen(ws_upgrade_req), 0);

    char ws_hdr[1024] = {0};
    size_t ws_hdr_len = 0;
    while (ws_hdr_len < sizeof(ws_hdr) - 1) {
        char ch;
        ssize_t r = recv(ws_fd, &ch, 1, 0);
        ASSERT_TRUE(r > 0);
        ws_hdr[ws_hdr_len++] = ch;
        if (ws_hdr_len >= 4 && memcmp(ws_hdr + ws_hdr_len - 4, "\r\n\r\n", 4) == 0) break;
    }
    ASSERT_TRUE(strstr(ws_hdr, "HTTP/1.1 101 Switching Protocols") != NULL);

    /* Read initial WS frame from server */
    uint8_t frame_hdr[2];
    ASSERT_TRUE(socket_read_exact(ws_fd, frame_hdr, 2, 1000));
    ASSERT_EQ(frame_hdr[0], 0x81); /* FIN + Text */
    uint8_t payload_len = frame_hdr[1] & 0x7F;
    ASSERT_TRUE(payload_len > 0);

    char ws_payload[256] = {0};
    ASSERT_TRUE(socket_read_exact(ws_fd, (uint8_t*)ws_payload, payload_len, 1000));
    ASSERT_TRUE(strstr(ws_payload, "\"type\":\"tally\"") != NULL);
    ASSERT_TRUE(strstr(ws_payload, "\"program\":true") != NULL);
    ASSERT_TRUE(strstr(ws_payload, "\"recording\":false") != NULL);
    ASSERT_TRUE(strstr(ws_payload, "\"flags\":1") != NULL);

    /* 5. Live broadcast transition to CUED + RECORDING (Preview: Green, Recording: Amber) */
    oavx_tally_t t_prev_rec = { .flags = OAVX_TALLY_PREVIEW | OAVX_TALLY_RECORDING };
    avx_web_server_broadcast_tally(srv, &t_prev_rec);

    /* Read pushed live frame from WebSocket */
    ASSERT_TRUE(socket_read_exact(ws_fd, frame_hdr, 2, 1000));
    ASSERT_EQ(frame_hdr[0], 0x81);
    payload_len = frame_hdr[1] & 0x7F;
    memset(ws_payload, 0, sizeof(ws_payload));
    ASSERT_TRUE(socket_read_exact(ws_fd, (uint8_t*)ws_payload, payload_len, 1000));
    ASSERT_TRUE(strstr(ws_payload, "\"type\":\"tally\"") != NULL);
    ASSERT_TRUE(strstr(ws_payload, "\"program\":false") != NULL);
    ASSERT_TRUE(strstr(ws_payload, "\"preview\":true") != NULL);
    ASSERT_TRUE(strstr(ws_payload, "\"recording\":true") != NULL);
    ASSERT_TRUE(strstr(ws_payload, "\"flags\":6") != NULL);

    /* Close WS */
    uint8_t close_pkt[6] = { 0x88, 0x80, 0x11, 0x22, 0x33, 0x44 };
    send(ws_fd, close_pkt, sizeof(close_pkt), 0);
    close(ws_fd);

    /* 6. Test Server-Sent Events (SSE) Stream: GET /api/tally/sse */
    int sse_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(sse_fd >= 0);
    ASSERT_EQ(connect(sse_fd, (struct sockaddr*)&sin, sizeof(sin)), 0);

    const char *get_sse_req = "GET /api/tally/sse HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(sse_fd, get_sse_req, strlen(get_sse_req), 0);

    char sse_resp[1024] = {0};
    size_t sse_read = 0;
    while (sse_read < sizeof(sse_resp) - 1) {
        char ch;
        ssize_t r = recv(sse_fd, &ch, 1, 0);
        ASSERT_TRUE(r > 0);
        sse_resp[sse_read++] = ch;
        if (strstr(sse_resp, "\n\n")) break;
    }
    ASSERT_TRUE(strstr(sse_resp, "HTTP/1.1 200 OK") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "Content-Type: text/event-stream") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "data: {\"type\":\"tally\"") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "\"recording\":true") != NULL);

    /* 7. Broadcast Program + Recording while SSE client is connected */
    oavx_tally_t t_prog_rec = { .flags = OAVX_TALLY_PROGRAM | OAVX_TALLY_RECORDING };
    avx_web_server_broadcast_tally(srv, &t_prog_rec);

    /* Read pushed live SSE event */
    memset(sse_resp, 0, sizeof(sse_resp));
    sse_read = 0;
    while (sse_read < sizeof(sse_resp) - 1) {
        char ch;
        ssize_t r = recv(sse_fd, &ch, 1, 0);
        ASSERT_TRUE(r > 0);
        sse_resp[sse_read++] = ch;
        if (strstr(sse_resp, "\n\n")) break;
    }
    ASSERT_TRUE(strstr(sse_resp, "data: {\"type\":\"tally\"") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "\"program\":true") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "\"preview\":false") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "\"recording\":true") != NULL);
    ASSERT_TRUE(strstr(sse_resp, "\"flags\":5") != NULL);

    close(sse_fd);

    avx_web_server_destroy(srv);
    printf("    [+] Camera tally broadcast, initial push, and /api/tally validated!\n");
}

/* ========================================================================= */
/* MAIN RUNNER                                                               */
/* ========================================================================= */

int main(void) {
    printf("=== OpenAVX Embedded HTTP / fMP4 Web Preview Tests ===\n\n");

    test_fmp4_init_segment();
    test_fmp4_media_fragment();
    test_web_server_endpoints();
    test_live_fmp4_streaming();
    test_pipeline_web_preview();
    test_web_server_av1_and_hevc_cra_keyframes();
    test_sha1_and_websocket_accept();
    test_camera_page_and_websocket_ingress();
    test_camera_tally_broadcast_and_rest_api();

    printf("\n[PASS] openavx_web_server_test\n");
    return 0;
}
