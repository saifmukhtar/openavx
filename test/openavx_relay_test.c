/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_relay_test.c - WAN Discovery, STUN Reflection & UDP Relay Verification
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "avx_platform.h"
#include "openavx.h"
#include "avx_discovery_server.h"
#include "avx_udp.h"
#include "avx_fec.h"
#include "avx_rate_control.h"

#define TEST_TCP_PORT   17959
#define TEST_RELAY_PORT 17960

static void test_stun_reflection(void)
{
    printf("[*] Test 1: STUN Reflexive Address Discovery across UDP...\n");

    avx_discovery_server_config_t cfg = {
        .bind_ip = "127.0.0.1",
        .tcp_port = TEST_TCP_PORT,
        .relay_udp_port = TEST_RELAY_PORT,
        .enable_relay = true,
        .public_ip = "127.0.0.1"
    };

    avx_discovery_server_t *srv = avx_discovery_server_create_ex(&cfg);
    assert(srv != NULL);

    /* 1.1 Discovery via existing socket */
    avx_socket_t sock = avx_udp_create_socket(0, false);
    assert(sock != AVX_INVALID_SOCKET);

    char out_ip[64] = {0};
    uint16_t out_port = 0;
    int ret = avx_discovery_client_get_reflexive_addr_sock(sock, "127.0.0.1", TEST_RELAY_PORT, out_ip, &out_port);
    assert(ret == 0);
    assert(strcmp(out_ip, "127.0.0.1") == 0);
    assert(out_port > 0);
    printf("    [+] Reflexive socket address discovered: %s:%u\n", out_ip, out_port);
    avx_socket_close(sock);

    /* 1.2 Discovery via transient helper socket */
    memset(out_ip, 0, sizeof(out_ip));
    out_port = 0;
    ret = avx_discovery_client_get_reflexive_addr("127.0.0.1", TEST_RELAY_PORT, out_ip, &out_port);
    assert(ret == 0);
    assert(strcmp(out_ip, "127.0.0.1") == 0);
    assert(out_port > 0);
    printf("    [+] Transient reflexive address discovered: %s:%u\n", out_ip, out_port);

    avx_discovery_server_destroy(srv);
    printf("    [PASS] Test 1: STUN Reflexive Address Discovery passed.\n");
}

static void test_relay_fanout_and_feedback(void)
{
    printf("[*] Test 2: Symmetric NAT Simulated Relay Forwarding & Multiplexing...\n");

    avx_discovery_server_config_t cfg = {
        .bind_ip = "127.0.0.1",
        .tcp_port = TEST_TCP_PORT,
        .relay_udp_port = TEST_RELAY_PORT,
        .enable_relay = true,
        .public_ip = "127.0.0.1"
    };

    avx_discovery_server_t *srv = avx_discovery_server_create_ex(&cfg);
    assert(srv != NULL);

    avx_socket_t pub_sock = avx_udp_create_socket(0, false);
    avx_socket_t sub1_sock = avx_udp_create_socket(0, false);
    avx_socket_t sub2_sock = avx_udp_create_socket(0, false);
    assert(pub_sock != AVX_INVALID_SOCKET);
    assert(sub1_sock != AVX_INVALID_SOCKET);
    assert(sub2_sock != AVX_INVALID_SOCKET);

    const uint32_t session_id = 101;

    /* Bind endpoints to session 101 */
    int r1 = avx_relay_client_bind_session(pub_sock, "127.0.0.1", TEST_RELAY_PORT, session_id, true);
    assert(r1 == 0);
    int r2 = avx_relay_client_bind_session(sub1_sock, "127.0.0.1", TEST_RELAY_PORT, session_id, false);
    assert(r2 == 0);
    int r3 = avx_relay_client_bind_session(sub2_sock, "127.0.0.1", TEST_RELAY_PORT, session_id, false);
    assert(r3 == 0);

    /* Prepare relay server sockaddr */
    struct sockaddr_in relay_sa;
    memset(&relay_sa, 0, sizeof(relay_sa));
    relay_sa.sin_family = AF_INET;
    relay_sa.sin_port = htons(TEST_RELAY_PORT);
    inet_pton(AF_INET, "127.0.0.1", &relay_sa.sin_addr);

    /* 2.1 Publisher sends 5 distinct media datagrams */
    for (int i = 0; i < 5; ++i) {
        char payload[128];
        snprintf(payload, sizeof(payload), "MEDIA_DATAGRAM_SEQ_%04d_PAYLOAD_DATA", i);

        int sent = avx_relay_send_packet(pub_sock, (struct sockaddr*)&relay_sa, sizeof(relay_sa),
                                         session_id, AVX_RELAY_CHANNEL_MEDIA, payload, strlen(payload) + 1);
        assert(sent > 0);

        /* Verify Subscriber 1 receives payload bit-exact */
        char buf1[256] = {0};
        struct sockaddr_storage from1;
        avx_socklen_t flen1 = sizeof(from1);
        ssize_t n1 = recvfrom(sub1_sock, buf1, sizeof(buf1), 0, (struct sockaddr*)&from1, &flen1);
        assert(n1 > 0);
        assert(strcmp(buf1, payload) == 0);

        /* Verify Subscriber 2 receives payload bit-exact */
        char buf2[256] = {0};
        struct sockaddr_storage from2;
        avx_socklen_t flen2 = sizeof(from2);
        ssize_t n2 = recvfrom(sub2_sock, buf2, sizeof(buf2), 0, (struct sockaddr*)&from2, &flen2);
        assert(n2 > 0);
        assert(strcmp(buf2, payload) == 0);
    }
    printf("    [+] 5 media datagrams successfully relayed bit-exact to 2 subscribers!\n");

    /* 2.2 Verify stats */
    avx_relay_stats_t stats;
    bool got_stats = avx_relay_server_get_stats(srv, &stats);
    assert(got_stats);
    assert(stats.active_sessions >= 1);
    assert(stats.active_subscribers >= 2);
    assert(stats.packets_relayed >= 10);
    printf("    [+] Server stats: %u sessions, %u subscribers, %llu relayed pkts\n",
           stats.active_sessions, stats.active_subscribers, (unsigned long long)stats.packets_relayed);

    /* 2.3 Reverse path feedback: Subscriber 1 sends NACK/Feedback packet to publisher */
    printf("[*] Test 3: Reverse Path Control Feedback (Subscriber -> Relay -> Publisher)...\n");
    const char feedback_msg[] = "FEEDBACK_NACK_REQUEST_FOR_FRAME_42";
    int fbsent = avx_relay_send_packet(sub1_sock, (struct sockaddr*)&relay_sa, sizeof(relay_sa),
                                       session_id, AVX_RELAY_CHANNEL_CONTROL, feedback_msg, sizeof(feedback_msg));
    assert(fbsent > 0);

    char pub_buf[256] = {0};
    struct sockaddr_storage pub_from;
    avx_socklen_t pub_flen = sizeof(pub_from);
    ssize_t pn = recvfrom(pub_sock, pub_buf, sizeof(pub_buf), 0, (struct sockaddr*)&pub_from, &pub_flen);
    assert(pn > 0);
    assert(strcmp(pub_buf, feedback_msg) == 0);
    printf("    [+] Reverse path feedback delivered bit-exact to publisher: \"%s\"\n", pub_buf);

    avx_socket_close(pub_sock);
    avx_socket_close(sub1_sock);
    avx_socket_close(sub2_sock);
    avx_discovery_server_destroy(srv);
    printf("    [PASS] Test 2 & 3: Relay fanout and feedback passed.\n");
}

static void test_transparent_relay(void)
{
    printf("[*] Test 4: Transparent Raw Datagram Forwarding (Zero-Header Routing)...\n");

    avx_discovery_server_config_t cfg = {
        .bind_ip = "127.0.0.1",
        .tcp_port = TEST_TCP_PORT,
        .relay_udp_port = TEST_RELAY_PORT,
        .enable_relay = true,
        .public_ip = "127.0.0.1"
    };

    avx_discovery_server_t *srv = avx_discovery_server_create_ex(&cfg);
    assert(srv != NULL);

    avx_socket_t pub_sock = avx_udp_create_socket(0, false);
    avx_socket_t sub_sock = avx_udp_create_socket(0, false);
    assert(pub_sock != AVX_INVALID_SOCKET);
    assert(sub_sock != AVX_INVALID_SOCKET);

    const uint32_t session_id = 202;

    /* Bind endpoints to session 202 */
    assert(avx_relay_client_bind_session(pub_sock, "127.0.0.1", TEST_RELAY_PORT, session_id, true) == 0);
    assert(avx_relay_client_bind_session(sub_sock, "127.0.0.1", TEST_RELAY_PORT, session_id, false) == 0);

    struct sockaddr_in relay_sa;
    memset(&relay_sa, 0, sizeof(relay_sa));
    relay_sa.sin_family = AF_INET;
    relay_sa.sin_port = htons(TEST_RELAY_PORT);
    inet_pton(AF_INET, "127.0.0.1", &relay_sa.sin_addr);

    /* Send raw packet from publisher directly to relay port without relay header */
    const char raw_pub_msg[] = "RAW_OPENAVX_MEDIA_PACKET_DIRECT";
    sendto(pub_sock, raw_pub_msg, sizeof(raw_pub_msg), 0, (struct sockaddr*)&relay_sa, sizeof(relay_sa));

    char rx_buf[128] = {0};
    struct sockaddr_storage from;
    avx_socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(sub_sock, rx_buf, sizeof(rx_buf), 0, (struct sockaddr*)&from, &flen);
    assert(n > 0);
    assert(strcmp(rx_buf, raw_pub_msg) == 0);
    printf("    [+] Raw forward path received bit-exact: \"%s\"\n", rx_buf);

    /* Send raw response from subscriber directly to relay port without relay header */
    const char raw_sub_msg[] = "RAW_OPENAVX_FEEDBACK_DIRECT";
    sendto(sub_sock, raw_sub_msg, sizeof(raw_sub_msg), 0, (struct sockaddr*)&relay_sa, sizeof(relay_sa));

    char pub_buf[128] = {0};
    flen = sizeof(from);
    ssize_t pn = recvfrom(pub_sock, pub_buf, sizeof(pub_buf), 0, (struct sockaddr*)&from, &flen);
    assert(pn > 0);
    assert(strcmp(pub_buf, raw_sub_msg) == 0);
    printf("    [+] Raw reverse path received bit-exact: \"%s\"\n", pub_buf);

    avx_socket_close(pub_sock);
    avx_socket_close(sub_sock);
    avx_discovery_server_destroy(srv);
    printf("    [PASS] Test 4: Transparent zero-header routing passed.\n");
}

static void test_rudp_fec_stream_over_relay(void)
{
    printf("[*] Test 5: End-to-End RUDP + FEC Stream through UDP Relay Server...\n");

    avx_discovery_server_config_t cfg = {
        .bind_ip = "127.0.0.1",
        .tcp_port = TEST_TCP_PORT,
        .relay_udp_port = TEST_RELAY_PORT,
        .enable_relay = true,
        .public_ip = "127.0.0.1"
    };

    avx_discovery_server_t *srv = avx_discovery_server_create_ex(&cfg);
    assert(srv != NULL);

    avx_socket_t tx_sock = avx_udp_create_socket(0, false);
    avx_socket_t rx_sock = avx_udp_create_socket(0, false);
    assert(tx_sock != AVX_INVALID_SOCKET);
    assert(rx_sock != AVX_INVALID_SOCKET);

    const uint32_t session_id = 303;
    assert(avx_relay_client_bind_session(tx_sock, "127.0.0.1", TEST_RELAY_PORT, session_id, true) == 0);
    assert(avx_relay_client_bind_session(rx_sock, "127.0.0.1", TEST_RELAY_PORT, session_id, false) == 0);

    struct sockaddr_in relay_sa;
    memset(&relay_sa, 0, sizeof(relay_sa));
    relay_sa.sin_family = AF_INET;
    relay_sa.sin_port = htons(TEST_RELAY_PORT);
    inet_pton(AF_INET, "127.0.0.1", &relay_sa.sin_addr);

    /* Create RUDP Sender targeting the Relay Server */
    avx_udp_rudp_sender_t *sender = avx_udp_rudp_sender_create(tx_sock, 8);
    assert(sender != NULL);

    /* Create RUDP Receiver with sender target pointing back to Relay Server */
    avx_udp_rudp_receiver_t *receiver = avx_udp_rudp_receiver_create(rx_sock);
    assert(receiver != NULL);

    /* Configure 2D FEC on sender and receiver */
    avx_fec_config_t fec_cfg = {
        .enabled = true,
        .matrix_d = 4,
        .matrix_l = 4,
        .enable_row_fec = true
    };
    avx_udp_rudp_sender_set_fec(sender, &fec_cfg);
    avx_udp_rudp_receiver_set_fec(receiver, &fec_cfg);

    /* Transmit 3 large frames (64 KB each = ~46 fragments per frame) through relay */
    const size_t frame_bytes = 64 * 1024;
    uint8_t *tx_frame = malloc(frame_bytes);
    uint8_t *rx_frame = malloc(frame_bytes);
    assert(tx_frame && rx_frame);

    for (int f = 0; f < 3; ++f) {
        for (size_t b = 0; b < frame_bytes; ++b) {
            tx_frame[b] = (uint8_t)((f * 37 + b) & 0xFF);
        }

        int send_ret = avx_udp_rudp_send_frame(sender, (struct sockaddr*)&relay_sa,
                                               (uint32_t)f, 0x32315659, 1000LL * f,
                                               tx_frame, frame_bytes);
        assert(send_ret == 0);

        uint32_t rx_fourcc = 0;
        int64_t rx_timecode = 0;
        size_t rx_size = 0;
        int recv_ret = -1;

        for (int poll = 0; poll < 100 && recv_ret < 0; ++poll) {
            recv_ret = avx_udp_rudp_recv_frame(receiver, (struct sockaddr*)&relay_sa,
                                               rx_frame, frame_bytes,
                                               &rx_size, &rx_fourcc, &rx_timecode, 15);
        }

        assert(recv_ret == 0);
        assert(rx_size == frame_bytes);
        assert(rx_fourcc == 0x32315659);
        assert(rx_timecode == 1000LL * f);
        assert(memcmp(tx_frame, rx_frame, frame_bytes) == 0);
    }
    printf("    [+] 3 full 64KB video frames successfully transmitted & verified across Relay!\n");

    free(tx_frame);
    free(rx_frame);
    avx_udp_rudp_receiver_destroy(receiver);
    avx_udp_rudp_sender_destroy(sender);
    avx_socket_close(tx_sock);
    avx_socket_close(rx_sock);
    avx_discovery_server_destroy(srv);
    printf("    [PASS] Test 5: End-to-End RUDP + FEC over Relay passed.\n");
}

static void test_tcp_wan_signaling(void)
{
    printf("[*] Test 6: TCP WAN Signaling (RESOLVE and PUNCH_REQ)...\n");

    avx_discovery_server_config_t cfg = {
        .bind_ip = "127.0.0.1",
        .tcp_port = TEST_TCP_PORT,
        .relay_udp_port = TEST_RELAY_PORT,
        .enable_relay = true,
        .public_ip = "127.0.0.1"
    };

    avx_discovery_server_t *srv = avx_discovery_server_create_ex(&cfg);
    assert(srv != NULL);

    /* 6.1 Register a publisher stream with explicit WAN endpoint */
    int pub_sock = avx_discovery_client_register("127.0.0.1", TEST_TCP_PORT,
                                                 "STUDIO_CAMERA_WAN",
                                                 "avx://192.168.1.150:7960",
                                                 "production");
    assert(pub_sock >= 0);

    /* 6.2 Client calls RESOLVE */
    char url[256] = {0};
    char wan_addr[64] = {0};
    int res = avx_discovery_client_resolve("127.0.0.1", TEST_TCP_PORT,
                                          "STUDIO_CAMERA_WAN",
                                          url, sizeof(url),
                                          wan_addr, sizeof(wan_addr));
    assert(res == 0);
    assert(strcmp(url, "avx://192.168.1.150:7960") == 0);
    assert(wan_addr[0] != '\0');
    printf("    [+] Stream resolved: URL=\"%s\", WAN=\"%s\"\n", url, wan_addr);

    /* 6.3 Client calls PUNCH_REQ */
    char peer_candidate[128] = {0};
    int punch_res = avx_discovery_client_request_punch("127.0.0.1", TEST_TCP_PORT,
                                                       "STUDIO_CAMERA_WAN",
                                                       "198.51.100.22:45678",
                                                       peer_candidate, sizeof(peer_candidate));
    assert(punch_res == 0);
    assert(peer_candidate[0] != '\0');
    printf("    [+] Punch candidate returned: \"%s\"\n", peer_candidate);

    /* Verify publisher received PUNCH_PEER notification on its active TCP socket */
    char pub_buf[256] = {0};
    ssize_t n = recv(pub_sock, pub_buf, sizeof(pub_buf) - 1, 0);
    assert(n > 0);
    pub_buf[n] = '\0';
    assert(strncmp(pub_buf, "PUNCH_PEER", 10) == 0);
    printf("    [+] Publisher received punch notification: \"%s\"\n", strtok(pub_buf, "\r\n"));

    /* 6.4 Resolve non-existent stream */
    int not_found = avx_discovery_client_resolve("127.0.0.1", TEST_TCP_PORT,
                                                 "NON_EXISTENT_STREAM",
                                                 url, sizeof(url),
                                                 wan_addr, sizeof(wan_addr));
    assert(not_found == -1);

    avx_socket_close(pub_sock);
    avx_discovery_server_destroy(srv);
    printf("    [PASS] Test 6: TCP WAN signaling passed.\n");
}

int main(void)
{
    printf("\n=======================================================\n");
    printf(" OpenAVX Phase 3: WAN Discovery, STUN & UDP Relay Test\n");
    printf("=======================================================\n\n");

    test_stun_reflection();
    test_relay_fanout_and_feedback();
    test_transparent_relay();
    test_rudp_fec_stream_over_relay();
    test_tcp_wan_signaling();

    printf("\n[PASS] All OpenAVX WAN Discovery & Relay Server tests passed successfully!\n\n");
    return 0;
}
