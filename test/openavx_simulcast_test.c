/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_simulcast_test.c - Lazy On-Demand Hardware Simulcast & Codec Negotiation Test Suite
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "openavx.h"
#include "avx_internal.h"
#include "avx_send_internal.h"
#include "avx_video_codec.h"

/* -------------------------------------------------------------------------- */
/* Test 1: Network Address Classification (LAN vs WAN)                        */
/* -------------------------------------------------------------------------- */
static void test_address_classification(void) {
    printf("[*] 1. Testing LAN vs WAN Network Address Classification...\n");

    struct sockaddr_storage ss;

    /* Loopback 127.0.0.1 -> LAN */
    struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
    memset(&ss, 0, sizeof(ss));
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sin->sin_addr);
    assert(avx_is_lan_sockaddr(&ss) == true);

    /* 10.1.2.3 -> LAN (RFC 1918) */
    inet_pton(AF_INET, "10.1.2.3", &sin->sin_addr);
    assert(avx_is_lan_sockaddr(&ss) == true);

    /* 192.168.1.100 -> LAN (RFC 1918) */
    inet_pton(AF_INET, "192.168.1.100", &sin->sin_addr);
    assert(avx_is_lan_sockaddr(&ss) == true);

    /* 172.20.0.5 -> LAN (RFC 1918) */
    inet_pton(AF_INET, "172.20.0.5", &sin->sin_addr);
    assert(avx_is_lan_sockaddr(&ss) == true);

    /* 169.254.10.20 -> LAN (Link-Local) */
    inet_pton(AF_INET, "169.254.10.20", &sin->sin_addr);
    assert(avx_is_lan_sockaddr(&ss) == true);

    /* 8.8.8.8 -> WAN (Public Internet) */
    inet_pton(AF_INET, "8.8.8.8", &sin->sin_addr);
    assert(avx_is_lan_sockaddr(&ss) == false);

    /* 1.1.1.1 -> WAN (Public Internet) */
    inet_pton(AF_INET, "1.1.1.1", &sin->sin_addr);
    assert(avx_is_lan_sockaddr(&ss) == false);

    /* IPv6 ::1 -> LAN */
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
    memset(&ss, 0, sizeof(ss));
    sin6->sin6_family = AF_INET6;
    inet_pton(AF_INET6, "::1", &sin6->sin6_addr);
    assert(avx_is_lan_sockaddr(&ss) == true);

    /* IPv6 ULA fd00::1 -> LAN */
    inet_pton(AF_INET6, "fd00::1", &sin6->sin6_addr);
    assert(avx_is_lan_sockaddr(&ss) == true);

    /* IPv6 Public 2001:4860:4860::8888 -> WAN */
    inet_pton(AF_INET6, "2001:4860:4860::8888", &sin6->sin6_addr);
    assert(avx_is_lan_sockaddr(&ss) == false);

    /* IPv4-mapped IPv6 ::ffff:127.0.0.1 -> LAN */
    inet_pton(AF_INET6, "::ffff:127.0.0.1", &sin6->sin6_addr);
    assert(avx_is_lan_sockaddr(&ss) == true);

    /* IPv4-mapped IPv6 ::ffff:8.8.8.8 -> WAN */
    inet_pton(AF_INET6, "::ffff:8.8.8.8", &sin6->sin6_addr);
    assert(avx_is_lan_sockaddr(&ss) == false);

    printf("    -> RFC 1918, RFC 3927, and RFC 4193 classification verified.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: Codec Capability Auto-Negotiation                                  */
/* -------------------------------------------------------------------------- */
static void test_codec_negotiation(void) {
    printf("[*] 2. Testing Receiver Codec Capability Auto-Negotiation...\n");

    /* Default negotiation with no receiver preference */
    uint32_t def_codec = avx_negotiate_best_codec(NULL);
    assert(def_codec != 0);
    printf("    -> Default negotiated codec: 0x%08X (%.4s)\n", def_codec, (const char *)&def_codec);

    /* Explicit H.264 preference */
    uint32_t h264_codec = avx_negotiate_best_codec("h264");
    assert(h264_codec == OAVX_FOURCC_VIDEO_TYPE_H264);

    /* Multi-codec preference order */
    uint32_t multi_codec = avx_negotiate_best_codec("av1,hevc,h264");
    assert(multi_codec == OAVX_FOURCC_VIDEO_TYPE_AV01 || multi_codec == OAVX_FOURCC_VIDEO_TYPE_HEVC ||
           multi_codec == OAVX_FOURCC_VIDEO_TYPE_H264);
    printf("    -> Multi-codec matched preference: 0x%08X (%.4s)\n", multi_codec, (const char *)&multi_codec);

    printf("    -> Codec capability auto-negotiation verified.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: Lazy On-Demand Hardware Simulcast Lifecycle                        */
/* -------------------------------------------------------------------------- */
static void test_lazy_simulcast_lifecycle(void) {
    printf("[*] 3. Testing Lazy On-Demand Hardware Simulcast Lifecycle...\n");

    avx_tx_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.name = "SimulcastTestStream";
    cfg.transport = OAVX_TRANSPORT_TCP;

    avx_tx_instance_t tx = avx_tx_create(&cfg);
    assert(tx != NULL);

    /* Initially: 0 WAN clients, 0 hardware simulcast encoders running */
    assert(avx_tx_get_wan_client_count(tx) == 0);
    assert(avx_tx_is_wan_encoder_active(tx) == false);

    /* Prepare dummy test video frame (1280x720 UYVY) */
    size_t frame_bytes = 1280 * 720 * 2;
    uint8_t *frame_buf = (uint8_t *)calloc(1, frame_bytes);
    assert(frame_buf != NULL);

    oavx_video_frame_t vf = {.width = 1280,
                             .height = 720,
                             .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
                             .scan_type = OAVX_SCAN_PROGRESSIVE,
                             .planes = {frame_buf, NULL, NULL, NULL},
                             .strides = {1280 * 2, 0, 0, 0},
                             .framerate = {60, 1},
                             .aspect_ratio = {16, 9},
                             .pts_ns = 1000000000LL,
                             .dts_ns = 1000000000LL,
                             .metadata = NULL,
                             .metadata_len = 0,
                             .user_ctx = NULL};

    /* Send frame with no clients: hardware encoder MUST NOT be allocated */
    avx_tx_send_video(tx, &vf);
    assert(avx_tx_is_wan_encoder_active(tx) == false);

    /* Get transmitter port from endpoint info */
    const oavx_endpoint_t *ep = avx_tx_get_source(tx);
    assert(ep != NULL);
    uint16_t port = ep->port;
    assert(port > 0);

    /* Connect a test client socket to transmitter */
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_fd >= 0);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

    int conn_rc = connect(client_fd, (struct sockaddr *)&sin, sizeof(sin));
    assert(conn_rc == 0);

    /* Read initial transmitter handshake XML */
    uint32_t xml_len = 0;
    ssize_t r1 = recv(client_fd, &xml_len, sizeof(xml_len), MSG_WAITALL);
    assert(r1 == (ssize_t)sizeof(xml_len));
    assert(xml_len > 0 && xml_len < 4096);

    char *xml_buf = (char *)malloc(xml_len + 1);
    assert(xml_buf != NULL);
    ssize_t r2 = recv(client_fd, xml_buf, xml_len, MSG_WAITALL);
    assert(r2 == (ssize_t)xml_len);
    xml_buf[xml_len] = '\0';
    free(xml_buf);

    /* Send reverse XML requesting WAN profile with multi-codec capabilities */
    char rev_xml[256];
    int rev_len =
        snprintf(rev_xml, sizeof(rev_xml), "<transport mode=\"tcp\" codecs=\"h264,hevc,av1\" profile=\"wan\"/>\n");

    AVX_PACKED_START
    struct {
        uint32_t magic;
        int64_t timecode;
        uint32_t payload_len;
    } AVX_PACKED mhdr;
    AVX_PACKED_END
    mhdr.magic = OAVX_WIRE_MAGIC_METADATA;
    mhdr.timecode = 0;
    mhdr.payload_len = (uint32_t)rev_len;

    send(client_fd, &mhdr, sizeof(mhdr), 0);
    send(client_fd, rev_xml, rev_len, 0);

    /* Allow reverse receiver worker thread to process client XML */
    avx_sleep_ms(80);

    /* WAN client count should now be 1 */
    int wan_clients = avx_tx_get_wan_client_count(tx);
    printf("    -> WAN clients registered: %d\n", wan_clients);
    assert(wan_clients == 1);

    /* Send frame: hardware encoder MUST now be lazily spawned! */
    avx_tx_send_video(tx, &vf);

    bool enc_active = avx_tx_is_wan_encoder_active(tx);
    uint32_t wan_codec = avx_tx_get_wan_codec_fourcc(tx);
    printf("    -> Lazy hardware simulcast encoder active: %s (codec: %.4s)\n", enc_active ? "YES" : "NO",
           (const char *)&wan_codec);
    assert(wan_codec != 0);
    if (avx_video_encoder_has_backend(wan_codec)) {
        assert(enc_active == true);
    } else {
        printf("    -> (Notice: No hardware/software encoder backend for %.4s available; skipping active assertion)\n",
               (const char *)&wan_codec);
    }

    /* Now disconnect the client */
    close(client_fd);

    /* Allow listener/worker to detect disconnect */
    avx_sleep_ms(150);

    /* Send frame to trigger refcount and lifecycle check */
    avx_tx_send_video(tx, &vf);

    /* WAN client count should return to 0 and hardware encoder MUST be destroyed */
    int remaining_wan = avx_tx_get_wan_client_count(tx);
    bool remaining_enc = avx_tx_is_wan_encoder_active(tx);
    printf("    -> Post-disconnect WAN clients: %d, encoder active: %s\n", remaining_wan, remaining_enc ? "YES" : "NO");
    assert(remaining_wan == 0);
    assert(remaining_enc == false);

    free(frame_buf);
    avx_tx_destroy(tx);
    printf("    -> Lazy hardware simulcast lifecycle verified successfully.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: Dynamic Re-Negotiation & Seamless In-Flight Migration              */
/* -------------------------------------------------------------------------- */
static void test_inflight_profile_migration(void) {
    printf("[*] 4. Testing Dynamic Re-Negotiation & Seamless In-Flight Migration...\n");

    avx_tx_config_t tx_cfg;
    memset(&tx_cfg, 0, sizeof(tx_cfg));
    tx_cfg.name = "MigrationTestStream";
    tx_cfg.transport = OAVX_TRANSPORT_TCP;

    avx_tx_instance_t tx = avx_tx_create(&tx_cfg);
    assert(tx != NULL);

    const oavx_endpoint_t *ep = avx_tx_get_source(tx);
    assert(ep != NULL);

    /* Spawn receiver using public avx_rx API */
    avx_rx_config_t rx_cfg;
    memset(&rx_cfg, 0, sizeof(rx_cfg));
    rx_cfg.source_to_connect_to = *ep;
    rx_cfg.color_format = OAVX_COLOR_FORMAT_FASTEST;

    avx_rx_instance_t rx = avx_rx_create(&rx_cfg);
    assert(rx != NULL);

    /* Allow receiver to connect */
    avx_sleep_ms(100);

    /* Send test frame */
    size_t frame_bytes = 1280 * 720 * 2;
    uint8_t *frame_buf = (uint8_t *)calloc(1, frame_bytes);
    assert(frame_buf != NULL);

    oavx_video_frame_t vf = {
        .width = 1280,
        .height = 720,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = {frame_buf, NULL, NULL, NULL},
        .strides = {1280 * 2, 0, 0, 0},
        .framerate = {60, 1},
        .aspect_ratio = {16, 9},
        .pts_ns = 2000000000LL,
        .dts_ns = 2000000000LL,
    };
    avx_tx_send_video(tx, &vf);

    /* Initial state: Localhost is private LAN -> 0 WAN clients, 0 WAN encoder */
    assert(avx_tx_get_wan_client_count(tx) == 0);
    assert(avx_tx_is_wan_encoder_active(tx) == false);

    /* Now receiver dynamically requests profile migration to WAN */
    bool req_ok = avx_rx_request_profile(rx, "wan");
    assert(req_ok == true);

    /* Allow reverse worker to process request */
    avx_sleep_ms(80);

    /* WAN client count should now be 1 */
    assert(avx_tx_get_wan_client_count(tx) == 1);

    /* Send video frame: lazy WAN hardware encoder activates */
    avx_tx_send_video(tx, &vf);
    uint32_t wan_codec = avx_tx_get_wan_codec_fourcc(tx);
    if (avx_video_encoder_has_backend(wan_codec)) {
        assert(avx_tx_is_wan_encoder_active(tx) == true);
    } else {
        printf("    -> (Notice: No hardware/software encoder backend for %.4s available; skipping active assertion)\n",
               (const char *)&wan_codec);
    }
    printf("    -> In-flight promotion to WAN: encoder active (codec: %.4s)\n", (const char *)&wan_codec);

    /* Now receiver dynamically switches back to LAN */
    req_ok = avx_rx_request_profile(rx, "lan");
    assert(req_ok == true);

    avx_sleep_ms(80);

    /* WAN client count should now be 0 */
    assert(avx_tx_get_wan_client_count(tx) == 0);
    assert(avx_tx_is_wan_encoder_active(tx) == false);
    printf("    -> In-flight demotion to LAN: encoder destroyed cleanly\n");

    /* Also test sender-side explicit profile migration API: avx_tx_set_client_profile */
    bool set_ok = avx_tx_set_client_profile(tx, 0, "wan");
    assert(set_ok == true);
    assert(avx_tx_get_wan_client_count(tx) == 1);

    avx_tx_send_video(tx, &vf);
    if (avx_video_encoder_has_backend(wan_codec)) {
        assert(avx_tx_is_wan_encoder_active(tx) == true);
    }

    set_ok = avx_tx_set_client_profile(tx, 0, "lan");
    assert(set_ok == true);
    assert(avx_tx_get_wan_client_count(tx) == 0);
    assert(avx_tx_is_wan_encoder_active(tx) == false);
    printf("    -> Sender-side avx_tx_set_client_profile verified\n");

    free(frame_buf);
    avx_rx_destroy(rx);
    avx_tx_destroy(tx);
    printf("    -> Dynamic re-negotiation & seamless migration verified successfully.\n");
}

int main(void) {
    printf("===================================================================\n");
    printf("  OpenAVX Lazy On-Demand Hardware Simulcast Test Suite             \n");
    printf("===================================================================\n");

    test_address_classification();
    test_codec_negotiation();
    test_lazy_simulcast_lifecycle();
    test_inflight_profile_migration();

    printf("\n>>> ALL SIMULCAST TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
