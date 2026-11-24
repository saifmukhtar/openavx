/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_rudp_test.c - Exhaustive Reliable UDP (RUDP) & Selective NACK ARQ Verification
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "openavx.h"
#include "avx_udp.h"
#include "avx_platform.h"

#define TEST_PORT_BASE 8970

/* Helper thread structure for concurrent sender NACK servicing */
typedef struct {
    avx_udp_rudp_sender_t* sender;
    volatile bool running;
} sender_nack_worker_t;

static void* sender_nack_thread_func(void* arg) {
    sender_nack_worker_t* w = (sender_nack_worker_t*)arg;
    while (w->running) {
        avx_udp_rudp_sender_service_nacks(w->sender);
        avx_sleep_ms(1);
    }
    return NULL;
}

static void test_rudp_clean_transfer(void) {
    printf("[*] 1. Testing Clean RUDP Delivery (No Packet Loss)...\n");

    uint16_t rx_port = TEST_PORT_BASE;
    uint16_t tx_port = TEST_PORT_BASE + 1;

    avx_socket_t rx_sock = avx_udp_create_socket(rx_port, true);
    avx_socket_t tx_sock = avx_udp_create_socket(tx_port, true);
    assert(rx_sock != AVX_INVALID_SOCKET && tx_sock != AVX_INVALID_SOCKET);

    avx_udp_rudp_sender_t* s = avx_udp_rudp_sender_create(tx_sock, 8);
    avx_udp_rudp_receiver_t* r = avx_udp_rudp_receiver_create(rx_sock);
    assert(s && r);

    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_port = htons(rx_port);
    inet_pton(AF_INET, "127.0.0.1", &rx_addr.sin_addr);

    struct sockaddr_in tx_addr;
    memset(&tx_addr, 0, sizeof(tx_addr));
    tx_addr.sin_family = AF_INET;
    tx_addr.sin_port = htons(tx_port);
    inet_pton(AF_INET, "127.0.0.1", &tx_addr.sin_addr);

    /* Generate 256 KB test payload */
    size_t payload_len = 256 * 1024;
    uint8_t* tx_data = (uint8_t*)malloc(payload_len);
    uint8_t* rx_data = (uint8_t*)malloc(payload_len + 4096);
    assert(tx_data && rx_data);

    for (size_t i = 0; i < payload_len; ++i) {
        tx_data[i] = (uint8_t)((i * 37 + 13) & 0xFF);
    }

    uint32_t seq = 101;
    uint32_t fourcc = 0x32514853; /* 'SHQ2' */
    int64_t tc = 123456789LL;

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

    printf("  [+] Clean RUDP transfer: %zu bytes reassembled with 100%% bitwise match!\n", rx_size);

    avx_udp_rudp_sender_destroy(s);
    avx_udp_rudp_receiver_destroy(r);
    avx_socket_close(rx_sock);
    avx_socket_close(tx_sock);
    free(tx_data);
    free(rx_data);
}

static void test_rudp_simulated_loss_and_nack_recovery(void) {
    printf("[*] 2. Testing Selective NACK ARQ Packet Loss Recovery...\n");

    uint16_t rx_port = TEST_PORT_BASE + 2;
    uint16_t tx_port = TEST_PORT_BASE + 3;

    avx_socket_t rx_sock = avx_udp_create_socket(rx_port, true);
    avx_socket_t tx_sock = avx_udp_create_socket(tx_port, true);
    assert(rx_sock != AVX_INVALID_SOCKET && tx_sock != AVX_INVALID_SOCKET);

    avx_udp_rudp_sender_t* s = avx_udp_rudp_sender_create(tx_sock, 8);
    avx_udp_rudp_receiver_t* r = avx_udp_rudp_receiver_create(rx_sock);
    assert(s && r);

    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_port = htons(rx_port);
    inet_pton(AF_INET, "127.0.0.1", &rx_addr.sin_addr);

    struct sockaddr_in tx_addr;
    memset(&tx_addr, 0, sizeof(tx_addr));
    tx_addr.sin_family = AF_INET;
    tx_addr.sin_port = htons(tx_port);
    inet_pton(AF_INET, "127.0.0.1", &tx_addr.sin_addr);

    /* Start sender background thread to automatically answer NACKs */
    sender_nack_worker_t worker;
    worker.sender = s;
    worker.running = true;
    pthread_t thread_id;
    int tr = pthread_create(&thread_id, NULL, sender_nack_thread_func, &worker);
    assert(tr == 0);

    /* Generate 200 KB payload (~147 fragments) */
    size_t payload_len = 200 * 1024;
    uint8_t* tx_data = (uint8_t*)malloc(payload_len);
    uint8_t* rx_data = (uint8_t*)malloc(payload_len + 4096);
    assert(tx_data && rx_data);

    for (size_t i = 0; i < payload_len; ++i) {
        tx_data[i] = (uint8_t)((i * 19 + 7) & 0xFF);
    }

    uint32_t seq = 202;
    uint32_t fourcc = 0x59565955; /* 'UYVY' */
    int64_t tc = 987654321LL;

    /* Archive frame in sender retransmission cache without blasting over wire yet */
    /* Then manually send fragments with INTENTIONAL PACKET LOSS */
    uint16_t frag_count = (uint16_t)((payload_len + AVX_UDP_MAX_FRAG_SIZE - 1) / AVX_UDP_MAX_FRAG_SIZE);

    /* Use rudp_send_frame to register in sender cache */
    avx_udp_rudp_send_frame(s, &rx_addr, seq, fourcc, tc, tx_data, payload_len);

    /* Now drop packets: We simulate severe burst network drops by corrupting/dropping:
     * Fragments 10..15 (6 packets burst loss)
     * Fragment 42 (single packet loss)
     * Fragments 80..85 (6 packets burst loss)
     */
    printf("  [~] Simulating severe network packet loss (13 dropped fragments across 3 burst gaps)...\n");

    /* Drain initial packets from kernel socket buffer first */
    char drain_buf[2048];
    while (recv(rx_sock, drain_buf, sizeof(drain_buf), MSG_DONTWAIT) > 0) {}

    /* To test recovery cleanly, we create fresh receiver */
    avx_udp_rudp_receiver_destroy(r);
    r = avx_udp_rudp_receiver_create(rx_sock);

    /* Send fragments 0..frag_count-1 with intentional drops */
    int dropped_count = 0;
    for (uint16_t f = 0; f < frag_count; ++f) {
        bool drop = (f >= 10 && f <= 15) || (f == 42) || (f >= 80 && f <= 85);
        if (drop) {
            dropped_count++;
            continue; /* DROP THIS PACKET */
        }

        size_t offset = (size_t)f * AVX_UDP_MAX_FRAG_SIZE;
        size_t chunk_len = payload_len - offset;
        if (chunk_len > AVX_UDP_MAX_FRAG_SIZE) chunk_len = AVX_UDP_MAX_FRAG_SIZE;

        avx_udp_header_t hdr = {
            .magic = OAVX_UDP_MAGIC,
            .frame_seq = seq,
            .frag_idx = f,
            .frag_count = frag_count,
            .fourcc = fourcc,
            .timecode = tc,
            .payload_len = (uint16_t)chunk_len,
            .reserved = 0
        };

        uint8_t pkt[sizeof(hdr) + AVX_UDP_MAX_FRAG_SIZE];
        memcpy(pkt, &hdr, sizeof(hdr));
        memcpy(pkt + sizeof(hdr), tx_data + offset, chunk_len);

        sendto(tx_sock, (const char*)pkt, sizeof(hdr) + chunk_len, 0,
               (const struct sockaddr*)&rx_addr, sizeof(rx_addr));
    }
    assert(dropped_count == 13);
    printf("  [+] Transmitted %u fragments with exactly %d dropped packets\n", frag_count, dropped_count);

    /* Receiver must detect the 3 distinct gaps, fire Selective NACKs to tx_sock,
     * sender thread must retransmit the 13 missing slices, and receiver must reassemble!
     */
    size_t rx_size = 0;
    uint32_t rx_fourcc = 0;
    int64_t rx_tc = 0;

    int recv_ret = avx_udp_rudp_recv_frame(r, &tx_addr, rx_data, payload_len + 4096,
                                          &rx_size, &rx_fourcc, &rx_tc, 2000);
    assert(recv_ret == 0);
    assert(rx_size == payload_len);
    assert(rx_fourcc == fourcc);
    assert(rx_tc == tc);
    assert(memcmp(tx_data, rx_data, payload_len) == 0);

    printf("  [+] Selective NACK ARQ SUCCESS: All 13 dropped packets recovered!\n");
    printf("  [+] Reassembled payload (%zu bytes) verified with 100%% bitwise integrity!\n", rx_size);

    /* Verify RUDP Telemetry stats on loss recovery */
    avx_rudp_telemetry_t s_telem, r_telem;
    assert(avx_udp_rudp_sender_get_telemetry(s, &s_telem));
    assert(avx_udp_rudp_receiver_get_telemetry(r, &r_telem));
    assert(s_telem.packets_retransmitted >= 13);
    assert(r_telem.packets_recovered == 13);
    assert(s_telem.srtt_ms > 0);
    printf("  [+] RUDP Telemetry verified: SRTT=%u ms, RTTVAR=%u ms, Retransmitted=%lu, Recovered=%lu\n",
           s_telem.srtt_ms, s_telem.rttvar_ms,
           (unsigned long)s_telem.packets_retransmitted, (unsigned long)r_telem.packets_recovered);

    /* Cleanup worker and sessions */
    worker.running = false;
    pthread_join(thread_id, NULL);

    avx_udp_rudp_sender_destroy(s);
    avx_udp_rudp_receiver_destroy(r);
    avx_socket_close(rx_sock);
    avx_socket_close(tx_sock);
    free(tx_data);
    free(rx_data);
}

static void test_rudp_periodic_feedback_and_rtt(void) {
    printf("[*] 3. Testing RUDP Periodic Feedback & Live RTT Tracking (0%% Loss)...\n");

    uint16_t rx_port = TEST_PORT_BASE + 4;
    uint16_t tx_port = TEST_PORT_BASE + 5;

    avx_socket_t rx_sock = avx_udp_create_socket(rx_port, true);
    avx_socket_t tx_sock = avx_udp_create_socket(tx_port, true);
    assert(rx_sock != AVX_INVALID_SOCKET && tx_sock != AVX_INVALID_SOCKET);

    avx_udp_rudp_sender_t* s = avx_udp_rudp_sender_create(tx_sock, 16);
    avx_udp_rudp_receiver_t* r = avx_udp_rudp_receiver_create(rx_sock);
    assert(s && r);

    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_port = htons(rx_port);
    inet_pton(AF_INET, "127.0.0.1", &rx_addr.sin_addr);

    struct sockaddr_in tx_addr;
    memset(&tx_addr, 0, sizeof(tx_addr));
    tx_addr.sin_family = AF_INET;
    tx_addr.sin_port = htons(tx_port);
    inet_pton(AF_INET, "127.0.0.1", &tx_addr.sin_addr);

    sender_nack_worker_t worker;
    worker.sender = s;
    worker.running = true;
    pthread_t thread_id;
    int tr = pthread_create(&thread_id, NULL, sender_nack_thread_func, &worker);
    assert(tr == 0);

    /* Stream 5 sequential frames with 30ms spacing */
    size_t payload_len = 32 * 1024;
    uint8_t* tx_buf = (uint8_t*)malloc(payload_len);
    uint8_t* rx_buf = (uint8_t*)malloc(payload_len + 4096);
    assert(tx_buf && rx_buf);
    memset(tx_buf, 0x5A, payload_len);

    for (uint32_t seq = 1; seq <= 5; ++seq) {
        int sret = avx_udp_rudp_send_frame(s, &rx_addr, seq, 0x32514853, seq * 16666666LL, tx_buf, payload_len);
        assert(sret == 0);

        size_t rx_size = 0;
        uint32_t rx_fourcc = 0;
        int64_t rx_tc = 0;
        int rret = avx_udp_rudp_recv_frame(r, &tx_addr, rx_buf, payload_len + 4096, &rx_size, &rx_fourcc, &rx_tc, 200);
        assert(rret == 0);
        assert(rx_size == payload_len);

        avx_sleep_ms(30);
    }

    /* Wait a moment for periodic feedback packet to be processed */
    avx_sleep_ms(120);
    avx_udp_rudp_sender_service_nacks(s);

    avx_rudp_telemetry_t tx_telem, rx_telem;
    assert(avx_udp_rudp_sender_get_telemetry(s, &tx_telem));
    assert(avx_udp_rudp_receiver_get_telemetry(r, &rx_telem));

    assert(tx_telem.packets_sent > 0);
    assert(rx_telem.packets_recv > 0);
    assert(tx_telem.packets_lost == 0);
    assert(rx_telem.packets_lost == 0);
    assert(tx_telem.srtt_ms > 0 && tx_telem.srtt_ms < 500);

    printf("  [+] Clean WAN stream telemetry: Sent=%lu, Recv=%lu, Loss=0, SRTT=%u ms, RTTVAR=%u ms\n",
           (unsigned long)tx_telem.packets_sent, (unsigned long)rx_telem.packets_recv,
           tx_telem.srtt_ms, tx_telem.rttvar_ms);

    worker.running = false;
    pthread_join(thread_id, NULL);

    avx_udp_rudp_sender_destroy(s);
    avx_udp_rudp_receiver_destroy(r);
    avx_socket_close(rx_sock);
    avx_socket_close(tx_sock);
    free(tx_buf);
    free(rx_buf);
}

static void test_rudp_oversized_frame_rejected(void) {
    printf("[*] 4. UDP & RUDP >5.47 MiB Oversized Frame Rejection...\n");

    avx_socket_t tx_sock = avx_udp_create_socket(0, false);
    assert(tx_sock != AVX_INVALID_SOCKET);

    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rx_addr.sin_port = htons(19099);

    avx_udp_rudp_sender_t *s = avx_udp_rudp_sender_create(tx_sock, 16);
    assert(s != NULL);

    size_t max_bytes = (size_t)AVX_UDP_MAX_FRAGS * (size_t)AVX_UDP_MAX_FRAG_SIZE;
    uint8_t dummy = 0;
    assert(avx_udp_send_frame(tx_sock, &rx_addr, 1, OAVX_FOURCC_VIDEO_TYPE_UYVY, 0, &dummy, max_bytes + 1) == -1);
    assert(avx_udp_rudp_send_frame(s, &rx_addr, 1, OAVX_FOURCC_VIDEO_TYPE_UYVY, 0, &dummy, max_bytes + 1) == -1);

    avx_udp_rudp_sender_destroy(s);
    avx_socket_close(tx_sock);
    printf("  [+] Oversized UDP/RUDP payloads (>%zu bytes) cleanly rejected!\n", max_bytes);
}

int main(void) {
    printf("=== OpenAVX Reliable UDP (RUDP) Tests ===\n\n");

    test_rudp_clean_transfer();
    test_rudp_simulated_loss_and_nack_recovery();
    test_rudp_periodic_feedback_and_rtt();
    test_rudp_oversized_frame_rejected();

    printf("\n[PASS] openavx_rudp_test\n");
    return 0;
}
