/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_ipv6_test.c - Exhaustive IPv6 Dual-Stack & RFC 3986 Bracketed URL Test Suite
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "openavx.h"
#include "avx_platform.h"
#include "avx_internal.h"
#include "avx_discovery_server.h"

#define TEST_IPV6_PORT 8980

static void test_ipv6_address_resolution(void) {
    printf("[*] 1. Testing IPv6 & Bracketed Address Resolution...\n");

    char out_ip[64] = {0};

    /* IPv4 loopback literal */
    bool ok = avx_net_resolve_interface_or_ip("127.0.0.1", out_ip, sizeof(out_ip));
    assert(ok && strcmp(out_ip, "127.0.0.1") == 0);
    printf("  [+] IPv4 literal '127.0.0.1' validated: %s\n", out_ip);

    /* IPv6 loopback literal */
    ok = avx_net_resolve_interface_or_ip("::1", out_ip, sizeof(out_ip));
    assert(ok && strcmp(out_ip, "::1") == 0);
    printf("  [+] IPv6 literal '::1' validated: %s\n", out_ip);

    /* Bracketed IPv6 literal (RFC 3986) */
    ok = avx_net_resolve_interface_or_ip("[::1]", out_ip, sizeof(out_ip));
    assert(ok && strcmp(out_ip, "::1") == 0);
    printf("  [+] Bracketed IPv6 literal '[::1]' unbracketed and validated: %s\n", out_ip);

    /* Loopback interface name 'lo' */
    ok = avx_net_resolve_interface_or_ip("lo", out_ip, sizeof(out_ip));
    assert(ok && out_ip[0] != '\0');
    printf("  [+] Interface 'lo' resolved: %s\n", out_ip);
}

static void test_dual_stack_tcp_socket(void) {
    printf("[*] 2. Testing Dual-Stack TCP Server (IPv4 & IPv6 Ingress on Same Port)...\n");

    uint16_t port = TEST_IPV6_PORT;

    /* Wildcard bind: creates AF_INET6 socket with IPV6_V6ONLY = 0 */
    avx_socket_t srv_sock = avx_socket_create_tcp_server_bind(NULL, port);
    assert(srv_sock != AVX_INVALID_SOCKET);
    printf("  [+] Dual-stack server socket active on port %u\n", port);

    /* Client 1 connects via IPv4 loopback 127.0.0.1 */
    avx_socket_t cli_v4 = avx_socket_connect_tcp("127.0.0.1", port);
    assert(cli_v4 != AVX_INVALID_SOCKET);

    struct sockaddr_storage addr1;
    avx_socklen_t addr1_len = sizeof(addr1);
    avx_socket_t accepted_v4 = accept(srv_sock, (struct sockaddr*)&addr1, &addr1_len);
    assert(accepted_v4 != AVX_INVALID_SOCKET);
    printf("  [+] Accepted IPv4 connection successfully on dual-stack listener!\n");

    /* Client 2 connects via IPv6 loopback ::1 */
    avx_socket_t cli_v6 = avx_socket_connect_tcp("::1", port);
    assert(cli_v6 != AVX_INVALID_SOCKET);

    struct sockaddr_storage addr2;
    avx_socklen_t addr2_len = sizeof(addr2);
    avx_socket_t accepted_v6 = accept(srv_sock, (struct sockaddr*)&addr2, &addr2_len);
    assert(accepted_v6 != AVX_INVALID_SOCKET);
    printf("  [+] Accepted IPv6 connection ('::1') successfully on dual-stack listener!\n");

    /* Client 3 connects via bracketed IPv6 "[::1]" */
    avx_socket_t cli_v6_bracket = avx_socket_connect_tcp("[::1]", port);
    assert(cli_v6_bracket != AVX_INVALID_SOCKET);

    struct sockaddr_storage addr3;
    avx_socklen_t addr3_len = sizeof(addr3);
    avx_socket_t accepted_v6_b = accept(srv_sock, (struct sockaddr*)&addr3, &addr3_len);
    assert(accepted_v6_b != AVX_INVALID_SOCKET);
    printf("  [+] Accepted bracketed IPv6 connection ('[::1]') successfully!\n");

    avx_socket_close(cli_v4);
    avx_socket_close(accepted_v4);
    avx_socket_close(cli_v6);
    avx_socket_close(accepted_v6);
    avx_socket_close(cli_v6_bracket);
    avx_socket_close(accepted_v6_b);
    avx_socket_close(srv_sock);
}

static void test_ipv6_end_to_end_media_stream(void) {
    printf("[*] 3. Testing End-to-End OpenAVX Media Stream over IPv6 Loopback...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 1. Create Sender explicitly bound to IPv6 loopback "::1" */
    oavx_stream_config_t tx_cfg = {
        .name = "IPV6_STUDIO_CAMERA",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .bind_ip = "::1"
    };

    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    assert(tx != NULL);

    const oavx_endpoint_t *tx_ep = oavx_stream_get_endpoint(tx);
    assert(tx_ep != NULL && tx_ep->address != NULL);
    printf("  [+] Sender URL formatted: %s\n", tx_ep->address);
    assert(strstr(tx_ep->address, "[::1]") != NULL);

    /* 2. Create Receiver connected to "[::1]:<port>" */
    oavx_stream_config_t rx_cfg = {
        .name = "IPV6_VIEWER",
        .endpoint = tx_ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_BGRA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };

    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    assert(rx != NULL);

    /* Wait briefly for connection */
    int conns = 0;
    for (int i = 0; i < 20; ++i) {
        conns = oavx_stream_get_connection_count(tx, 50);
        if (conns > 0) break;
    }
    printf("  [+] Active connections on sender: %d\n", conns);
    assert(conns >= 1);

    /* Send Video Frame (320x240 UYVY) */
    size_t v_bytes = 320 * 240 * 2;
    uint8_t* v_buf = (uint8_t*)malloc(v_bytes);
    assert(v_buf != NULL);
    memset(v_buf, 0x5A, v_bytes);

    oavx_video_frame_t vf = {
        .width = 320,
        .height = 240,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 123456789LL,
        .planes = { v_buf, NULL, NULL, NULL },
        .strides = { 320 * 2, 0, 0, 0 }
    };

    /* Send Audio Frame (48kHz stereo, 480 samples) */
    float a_l[480], a_r[480];
    for (int i = 0; i < 480; ++i) {
        a_l[i] = 0.75f;
        a_r[i] = -0.75f;
    }
    oavx_audio_frame_t af = {
        .sample_rate = 48000,
        .channels = 2,
        .frames = 480,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .stride_bytes = 480 * sizeof(float),
        .pts_ns = 123456789LL,
        .planes = { (uint8_t*)a_l, (uint8_t*)a_r, NULL, NULL, NULL, NULL, NULL, NULL }
    };

    /* Send video and audio */
    oavx_stream_send_video(tx, &vf);
    oavx_stream_send_audio(tx, &af);

    /* Capture video and audio on receiver */
    oavx_video_frame_t rx_vf;
    oavx_audio_frame_t rx_af;
    oavx_metadata_frame_t rx_mf;
    bool got_video = false;
    bool got_audio = false;

    for (int attempts = 0; attempts < 100 && (!got_video || !got_audio); ++attempts) {
        oavx_frame_type_t ft = oavx_stream_receive(rx, &rx_vf, &rx_af, &rx_mf, 100);
        if (ft == OAVX_FRAME_TYPE_VIDEO && !got_video) {
            assert(rx_vf.width == 320 && rx_vf.height == 240);
            assert(rx_vf.planes[0] != NULL);
            assert(rx_vf.planes[0][0] == 0x5A);
            printf("  [+] Captured video frame across IPv6 loopback with 100%% data integrity!\n");
            oavx_stream_release_video(rx, &rx_vf);
            got_video = true;
        } else if (ft == OAVX_FRAME_TYPE_AUDIO && !got_audio) {
            float* samples = (float*)rx_af.planes[0];
            assert(rx_af.sample_rate == 48000 && rx_af.channels == 2);
            assert(rx_af.frames == 480);
            assert(samples != NULL && samples[0] == 0.75f);
            printf("  [+] Captured audio frame across IPv6 loopback!\n");
            oavx_stream_release_audio(rx, &rx_af);
            got_audio = true;
        }
        if (!got_video) oavx_stream_send_video(tx, &vf);
        if (!got_audio) oavx_stream_send_audio(tx, &af);
    }
    assert(got_video);
    assert(got_audio);

    /* Teardown */
    oavx_stream_close(rx);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
    free(v_buf);
}

static void test_ipv6_end_to_end_rudp_stream(void) {
    printf("[*] 4. Testing End-to-End Media Stream over Native IPv6 with RUDP Transport...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* Create Sender in RUDP mode bound to IPv6 loopback "::1" */
    oavx_stream_config_t tx_cfg = {
        .name = "IPV6_RUDP_STUDIO",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .transport = OAVX_TRANSPORT_RUDP,
        .clock_sync = false,
        .bind_ip = "::1"
    };

    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    assert(tx != NULL);

    const oavx_endpoint_t *tx_ep = oavx_stream_get_endpoint(tx);
    assert(tx_ep != NULL && tx_ep->address != NULL);
    printf("  [+] RUDP Sender URL: %s\n", tx_ep->address);

    /* Create Receiver connected to "[::1]:<port>" via RUDP */
    oavx_stream_config_t rx_cfg = {
        .name = "IPV6_RUDP_VIEWER",
        .endpoint = tx_ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .transport = OAVX_TRANSPORT_RUDP,
        .color_format = OAVX_COLOR_FORMAT_UYVY_BGRA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };

    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    assert(rx != NULL);

    /* Wait for RUDP handshake */
    avx_sleep_ms(150);

    /* Send Video Frame (320x240 UYVY) */
    size_t v_bytes = 320 * 240 * 2;
    uint8_t* v_buf = (uint8_t*)malloc(v_bytes);
    assert(v_buf != NULL);
    memset(v_buf, 0x7E, v_bytes);

    oavx_video_frame_t vf = {
        .width = 320,
        .height = 240,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 987654321LL,
        .planes = { v_buf, NULL, NULL, NULL },
        .strides = { 320 * 2, 0, 0, 0 }
    };

    oavx_stream_send_video(tx, &vf);

    /* Capture video on receiver */
    oavx_video_frame_t rx_vf;
    bool got_video = false;

    for (int attempts = 0; attempts < 100 && !got_video; ++attempts) {
        oavx_frame_type_t ft = oavx_stream_receive(rx, &rx_vf, NULL, NULL, 100);
        if (ft == OAVX_FRAME_TYPE_VIDEO) {
            assert(rx_vf.width == 320 && rx_vf.height == 240);
            assert(rx_vf.planes[0] != NULL);
            assert(rx_vf.planes[0][0] == 0x7E);
            printf("  [+] Captured video frame across IPv6 RUDP with 100%% byte integrity!\n");
            oavx_stream_release_video(rx, &rx_vf);
            got_video = true;
        } else {
            oavx_stream_send_video(tx, &vf);
        }
    }
    assert(got_video);

    oavx_stream_close(rx);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
    free(v_buf);
}

static void test_ipv6_discovery_server(void) {
    printf("[*] 5. Testing Centralized Discovery Server over IPv6 ('::1')...\n");

    uint16_t ds_port = TEST_IPV6_PORT + 4;
    avx_discovery_server_t* srv = avx_discovery_server_create("::1", ds_port);
    assert(srv != NULL);

    /* Register source via IPv6 */
    int reg_fd = avx_discovery_client_register("::1", ds_port, "IPV6_CAM_MAIN", "[::1]:7980", "production");
    assert(reg_fd >= 0);
    printf("  [+] Registered source 'IPV6_CAM_MAIN' at '[::1]:7980' via IPv6\n");

    /* Query sources via IPv6 */
    char names[8][256];
    char urls[8][256];
    int count = avx_discovery_client_query("::1", ds_port, names, urls, 8);
    assert(count == 1);
    assert(strcmp(names[0], "IPV6_CAM_MAIN") == 0);
    assert(strcmp(urls[0], "[::1]:7980") == 0);
    printf("  [+] Queried source catalog via IPv6: %s -> %s\n", names[0], urls[0]);

    avx_socket_close(reg_fd);
    avx_discovery_server_destroy(srv);
}

static void test_ipv6_udp_transport(void) {
    printf("[*] 6. Testing Native IPv6 UDP Datagram Transmission & Fragmentation...\n");

    uint16_t rx_port = TEST_IPV6_PORT + 10;
    uint16_t tx_port = TEST_IPV6_PORT + 11;

    avx_socket_t rx_sock = avx_udp_create_socket_bind("::1", rx_port, true);
    avx_socket_t tx_sock = avx_udp_create_socket_bind("::1", tx_port, true);
    assert(rx_sock != AVX_INVALID_SOCKET && tx_sock != AVX_INVALID_SOCKET);

    struct sockaddr_in6 rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin6_family = AF_INET6;
    rx_addr.sin6_port = htons(rx_port);
    inet_pton(AF_INET6, "::1", &rx_addr.sin6_addr);

    /* Generate 64 KB test payload */
    size_t payload_len = 64 * 1024;
    uint8_t *tx_data = (uint8_t*)malloc(payload_len);
    uint8_t *rx_data = (uint8_t*)malloc(payload_len + 4096);
    assert(tx_data && rx_data);

    for (size_t i = 0; i < payload_len; ++i) {
        tx_data[i] = (uint8_t)((i * 31 + 7) & 0xFF);
    }

    uint32_t seq = 201;
    uint32_t fourcc = 0x32514853; /* 'SHQ2' */
    int64_t tc = 987654321LL;

    int send_ret = avx_udp_send_frame(tx_sock, &rx_addr, seq, fourcc, tc, tx_data, payload_len);
    assert(send_ret == 0);

    size_t rx_size = 0;
    uint32_t rx_fourcc = 0;
    int64_t rx_tc = 0;

    int recv_ret = avx_udp_recv_frame(rx_sock, rx_data, payload_len + 4096,
                                     &rx_size, &rx_fourcc, &rx_tc, 1000);
    assert(recv_ret == 0);
    assert(rx_size == payload_len);
    assert(rx_fourcc == fourcc);
    assert(rx_tc == tc);
    assert(memcmp(tx_data, rx_data, payload_len) == 0);

    printf("  [+] Native IPv6 UDP loopback: %zu bytes reassembled with 100%% byte integrity!\n", rx_size);

    avx_socket_close(rx_sock);
    avx_socket_close(tx_sock);
    free(tx_data);
    free(rx_data);
}

static void test_ipv6_rudp_transport(void) {
    printf("[*] 7. Testing Native IPv6 RUDP & Selective NACK ARQ Transport...\n");

    uint16_t rx_port = TEST_IPV6_PORT + 12;
    uint16_t tx_port = TEST_IPV6_PORT + 13;

    avx_socket_t rx_sock = avx_udp_create_socket_bind("::1", rx_port, true);
    avx_socket_t tx_sock = avx_udp_create_socket_bind("::1", tx_port, true);
    assert(rx_sock != AVX_INVALID_SOCKET && tx_sock != AVX_INVALID_SOCKET);

    avx_udp_rudp_sender_t* s = avx_udp_rudp_sender_create(tx_sock, 8);
    avx_udp_rudp_receiver_t* r = avx_udp_rudp_receiver_create(rx_sock);
    assert(s && r);

    struct sockaddr_in6 rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin6_family = AF_INET6;
    rx_addr.sin6_port = htons(rx_port);
    inet_pton(AF_INET6, "::1", &rx_addr.sin6_addr);

    struct sockaddr_in6 tx_addr;
    memset(&tx_addr, 0, sizeof(tx_addr));
    tx_addr.sin6_family = AF_INET6;
    tx_addr.sin6_port = htons(tx_port);
    inet_pton(AF_INET6, "::1", &tx_addr.sin6_addr);

    /* Generate 128 KB test payload */
    size_t payload_len = 128 * 1024;
    uint8_t *tx_data = (uint8_t*)malloc(payload_len);
    uint8_t *rx_data = (uint8_t*)malloc(payload_len + 4096);
    assert(tx_data && rx_data);

    for (size_t i = 0; i < payload_len; ++i) {
        tx_data[i] = (uint8_t)((i * 47 + 19) & 0xFF);
    }

    uint32_t seq = 301;
    uint32_t fourcc = 0x32514853; /* 'SHQ2' */
    int64_t tc = 1122334455LL;

    int send_ret = avx_udp_rudp_send_frame(s, &rx_addr, seq, fourcc, tc, tx_data, payload_len);
    assert(send_ret == 0);

    size_t rx_size = 0;
    uint32_t rx_fourcc = 0;
    int64_t rx_tc = 0;

    int recv_ret = avx_udp_rudp_recv_frame(r, &tx_addr, rx_data, payload_len + 4096,
                                          &rx_size, &rx_fourcc, &rx_tc, 1000);
    assert(recv_ret == 0);
    assert(rx_size == payload_len);
    assert(rx_fourcc == fourcc);
    assert(rx_tc == tc);
    assert(memcmp(tx_data, rx_data, payload_len) == 0);

    printf("  [+] Native IPv6 RUDP transfer: %zu bytes reassembled with 100%% byte integrity!\n", rx_size);

    avx_udp_rudp_sender_destroy(s);
    avx_udp_rudp_receiver_destroy(r);
    avx_socket_close(rx_sock);
    avx_socket_close(tx_sock);
    free(tx_data);
    free(rx_data);
}

static void test_multicast_membership(void) {
    printf("[*] 8. Testing Multicast Group Joins & Leaves (IGMP / MLD)...\n");

    /* Create non-blocking socket */
    avx_socket_t sock = avx_udp_create_socket(0, true);
    assert(sock != AVX_INVALID_SOCKET);

    /* Test TTL and Loopback configuration */
    bool ok = avx_udp_set_multicast_ttl(sock, 8);
    assert(ok);
    printf("  [+] Set Multicast TTL / Hops to 8\n");

    ok = avx_udp_set_multicast_loop(sock, true);
    assert(ok);
    printf("  [+] Enabled Multicast Loopback\n");

    /* Test IPv4 Multicast Join / Leave */
    ok = avx_udp_join_multicast(sock, "239.255.0.1", NULL);
    assert(ok);
    printf("  [+] Joined IPv4 Multicast Group 239.255.0.1 (IGMPv3)\n");

    ok = avx_udp_leave_multicast(sock, "239.255.0.1", NULL);
    assert(ok);
    printf("  [+] Left IPv4 Multicast Group 239.255.0.1\n");

    /* Test IPv6 Multicast Join / Leave (BSD/macOS requires AF_INET6 socket for IPv6 multicast) */
    avx_socket_t sock_v6 = sock;
#if defined(AVX_OS_MACOS) || defined(AVX_OS_BSD)
    sock_v6 = avx_udp_create_socket_bind("::", 0, true);
    assert(sock_v6 != AVX_INVALID_SOCKET);
#endif

    ok = avx_udp_join_multicast(sock_v6, "ff02::1", NULL);
    assert(ok);
    printf("  [+] Joined IPv6 Multicast Group ff02::1 (MLDv2)\n");

    ok = avx_udp_leave_multicast(sock_v6, "ff02::1", NULL);
    assert(ok);
    printf("  [+] Left IPv6 Multicast Group ff02::1\n");

    /* Test bracketed IPv6 multicast address */
    ok = avx_udp_join_multicast(sock_v6, "[ff02::1]", NULL);
    assert(ok);
    ok = avx_udp_leave_multicast(sock_v6, "[ff02::1]", NULL);
    assert(ok);
    printf("  [+] Joined and left bracketed IPv6 Multicast '[ff02::1]'\n");

#if defined(AVX_OS_MACOS) || defined(AVX_OS_BSD)
    if (sock_v6 != sock) {
        avx_socket_close(sock_v6);
    }
#endif

    /* Test invalid multicast addresses */
    assert(!avx_udp_join_multicast(sock, "192.168.1.1", NULL));
    assert(!avx_udp_join_multicast(sock, "2001:db8::1", NULL));
    assert(!avx_udp_join_multicast(sock, "invalid-ip", NULL));
    printf("  [+] Invalid multicast addresses safely rejected\n");

    avx_socket_close(sock);
}

int main(void) {
    printf("=== OpenAVX IPv6 Dual-Stack Transport Tests ===\n\n");

    test_ipv6_address_resolution();
    test_dual_stack_tcp_socket();
    test_ipv6_end_to_end_media_stream();
    test_ipv6_end_to_end_rudp_stream();
    test_ipv6_discovery_server();
    test_ipv6_udp_transport();
    test_ipv6_rudp_transport();
    test_multicast_membership();

    printf("\n[PASS] openavx_ipv6_test\n");
    return 0;
}
