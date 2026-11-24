/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_fec_test.c - SMPTE ST 2022-1 2D XOR FEC & Zero-RTT WAN Recovery Suite
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
#include "avx_fec.h"
#include "avx_udp.h"
#include "avx_platform.h"

#define FEC_TEST_PORT_BASE 8980

/* -------------------------------------------------------------------------- */
/* Test 1: Vectorized SIMD XOR Kernel Accuracy across Alignment Boundaries     */
/* -------------------------------------------------------------------------- */
static void test_fec_xor_kernel(void) {
    printf("[*] 1. Testing SIMD Bitwise XOR Kernel (AVX2 / NEON / Scalar tail)...\n");

    const size_t test_sizes[] = { 0, 1, 7, 8, 15, 16, 24, 31, 32, 63, 64, 127, 128, 1400, 4096 };
    size_t num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (size_t s = 0; s < num_sizes; ++s) {
        size_t len = test_sizes[s];
        if (len == 0) {
            avx_fec_xor_block(NULL, NULL, 0);
            continue;
        }

        uint8_t *orig = (uint8_t*)malloc(len);
        uint8_t *mask = (uint8_t*)malloc(len);
        uint8_t *dst  = (uint8_t*)malloc(len);
        assert(orig && mask && dst);

        for (size_t i = 0; i < len; ++i) {
            orig[i] = (uint8_t)((i * 31 + 17) & 0xFF);
            mask[i] = (uint8_t)((i * 59 + 83) & 0xFF);
            dst[i]  = orig[i];
        }

        /* dst ^= mask */
        avx_fec_xor_block(dst, mask, len);
        for (size_t i = 0; i < len; ++i) {
            assert(dst[i] == (uint8_t)(orig[i] ^ mask[i]));
        }

        /* (dst ^ mask) ^ mask == orig */
        avx_fec_xor_block(dst, mask, len);
        assert(memcmp(dst, orig, len) == 0);

        free(orig);
        free(mask);
        free(dst);
    }

    printf("    -> Vectorized XOR Kernel verified across all byte alignments.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: 1D Column Parity 10-Packet Burst Loss Zero-RTT Recovery             */
/* -------------------------------------------------------------------------- */
static void test_fec_1d_column_burst_recovery(void) {
    printf("[*] 2. Testing 1D Column Parity Burst Drop Recovery (10 Packets)...\n");

    uint16_t D = 10;
    uint16_t L = 5;
    uint16_t block_frags = 50;
    size_t frag_size = 1400;

    uint8_t *orig_data[50];
    uint8_t *rx_data[50];
    avx_fec_slice_t media_slices[50];
    bool is_present[50];

    for (uint16_t k = 0; k < block_frags; ++k) {
        orig_data[k] = (uint8_t*)malloc(frag_size);
        rx_data[k]   = (uint8_t*)malloc(frag_size);
        assert(orig_data[k] && rx_data[k]);

        for (size_t b = 0; b < frag_size; ++b) {
            orig_data[k][b] = (uint8_t)((k * 73 + b * 19 + 5) & 0xFF);
        }
        memcpy(rx_data[k], orig_data[k], frag_size);

        media_slices[k].data = orig_data[k];
        media_slices[k].len = frag_size;
        is_present[k] = true;
    }

    /* Generate Column Parity */
    uint8_t col_buf[AVX_FEC_MAX_COLS][1400];
    avx_fec_slice_t col_slices[AVX_FEC_MAX_COLS];
    bool col_present[AVX_FEC_MAX_COLS];

    for (uint16_t j = 0; j < D; ++j) {
        col_slices[j].data = col_buf[j];
        col_slices[j].len = 0;
        col_present[j] = true;
    }

    int gen_ret = avx_fec_generate_block(media_slices, block_frags, D, L, false, col_slices, NULL);
    assert(gen_ret == 0);

    for (uint16_t j = 0; j < D; ++j) {
        assert(col_slices[j].len == frag_size);
    }

    /* Simulate 10-Packet continuous burst loss on the WAN: Fragments 10 through 19 (Row 1) */
    for (uint16_t k = 10; k < 20; ++k) {
        memset(rx_data[k], 0, frag_size);
        is_present[k] = false;
    }

    /* Prepare receiver slices */
    avx_fec_slice_t rx_slices[50];
    for (uint16_t k = 0; k < block_frags; ++k) {
        rx_slices[k].data = rx_data[k];
        rx_slices[k].len = is_present[k] ? frag_size : 0;
    }

    int recovered = avx_fec_reconstruct_block(
        rx_slices,
        is_present,
        block_frags,
        D,
        L,
        col_slices,
        col_present,
        NULL,
        NULL
    );

    assert(recovered == 10);

    /* Verify all 10 recovered fragments are 100% bit-exact */
    for (uint16_t k = 0; k < block_frags; ++k) {
        assert(is_present[k] == true);
        assert(rx_slices[k].len == frag_size);
        assert(memcmp(rx_data[k], orig_data[k], frag_size) == 0);
        free(orig_data[k]);
        free(rx_data[k]);
    }

    printf("    -> 10-Packet burst loss recovered 100%% bit-exact in 1 pass.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: 2D Matrix Iterative Recovery with Multi-Packet Loss Pattern         */
/* -------------------------------------------------------------------------- */
static void test_fec_2d_matrix_multi_drop_recovery(void) {
    printf("[*] 3. Testing 2D Matrix Iterative Multi-Loss Recovery...\n");

    uint16_t D = 10;
    uint16_t L = 5;
    uint16_t block_frags = 50;
    size_t frag_size = 1400;

    uint8_t *orig_data[50];
    uint8_t *rx_data[50];
    avx_fec_slice_t media_slices[50];
    bool is_present[50];

    for (uint16_t k = 0; k < block_frags; ++k) {
        orig_data[k] = (uint8_t*)malloc(frag_size);
        rx_data[k]   = (uint8_t*)malloc(frag_size);
        assert(orig_data[k] && rx_data[k]);

        for (size_t b = 0; b < frag_size; ++b) {
            orig_data[k][b] = (uint8_t)((k * 41 + b * 7 + 11) & 0xFF);
        }
        memcpy(rx_data[k], orig_data[k], frag_size);

        media_slices[k].data = orig_data[k];
        media_slices[k].len = frag_size;
        is_present[k] = true;
    }

    /* Generate 2D Column & Row Parity */
    uint8_t col_buf[AVX_FEC_MAX_COLS][1400];
    avx_fec_slice_t col_slices[AVX_FEC_MAX_COLS];
    bool col_present[AVX_FEC_MAX_COLS];
    for (uint16_t j = 0; j < D; ++j) {
        col_slices[j].data = col_buf[j];
        col_slices[j].len = 0;
        col_present[j] = true;
    }

    uint8_t row_buf[AVX_FEC_MAX_ROWS][1400];
    avx_fec_slice_t row_slices[AVX_FEC_MAX_ROWS];
    bool row_present[AVX_FEC_MAX_ROWS];
    for (uint16_t i = 0; i < L; ++i) {
        row_slices[i].data = row_buf[i];
        row_slices[i].len = 0;
        row_present[i] = true;
    }

    int gen_ret = avx_fec_generate_block(media_slices, block_frags, D, L, true, col_slices, row_slices);
    assert(gen_ret == 0);

    /*
     * Drop pattern:
     * - Fragment 0 (row 0, col 0)
     * - Fragment 1 (row 0, col 1)
     * - Fragment 10 (row 1, col 0)
     * - And Column 0 Parity!
     *
     * In iteration 1:
     *   Column 0 parity is lost.
     *   Row 0 has frags 0 & 1 missing (2 missing -> cannot solve yet).
     *   Column 1 has frag 1 missing (1 missing) -> Column 1 parity recovers frag 1!
     * In iteration 2:
     *   Row 0 now has ONLY frag 0 missing (1 missing) -> Row 0 parity recovers frag 0!
     * In iteration 3:
     *   Row 1 has ONLY frag 10 missing (1 missing) -> Row 1 parity recovers frag 10!
     * Result: all 3 fragments recovered!
     */
    is_present[0] = false; memset(rx_data[0], 0, frag_size);
    is_present[1] = false; memset(rx_data[1], 0, frag_size);
    is_present[10] = false; memset(rx_data[10], 0, frag_size);
    col_present[0] = false; /* Column 0 parity lost */

    avx_fec_slice_t rx_slices[50];
    for (uint16_t k = 0; k < block_frags; ++k) {
        rx_slices[k].data = rx_data[k];
        rx_slices[k].len = is_present[k] ? frag_size : 0;
    }

    int recovered = avx_fec_reconstruct_block(
        rx_slices,
        is_present,
        block_frags,
        D,
        L,
        col_slices,
        col_present,
        row_slices,
        row_present
    );

    assert(recovered == 3);
    assert(is_present[0] && is_present[1] && is_present[10]);
    assert(memcmp(rx_data[0], orig_data[0], frag_size) == 0);
    assert(memcmp(rx_data[1], orig_data[1], frag_size) == 0);
    assert(memcmp(rx_data[10], orig_data[10], frag_size) == 0);

    for (uint16_t k = 0; k < block_frags; ++k) {
        free(orig_data[k]);
        free(rx_data[k]);
    }

    printf("    -> 2D Iterative matrix solver recovered multi-packet drop pattern.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: Variable-Length Last Fragment Zero-RTT Recovery                     */
/* -------------------------------------------------------------------------- */
static void test_fec_variable_length_recovery(void) {
    printf("[*] 4. Testing Variable-Length Tail Fragment Recovery...\n");

    uint16_t D = 5;
    uint16_t L = 2;
    uint16_t block_frags = 10;
    size_t standard_size = 1400;
    size_t tail_size = 573; /* Non-aligned short tail fragment */

    uint8_t *orig_data[10];
    uint8_t *rx_data[10];
    avx_fec_slice_t media_slices[10];
    bool is_present[10];

    for (uint16_t k = 0; k < block_frags; ++k) {
        size_t sz = (k == block_frags - 1) ? tail_size : standard_size;
        orig_data[k] = (uint8_t*)malloc(standard_size);
        rx_data[k]   = (uint8_t*)malloc(standard_size);
        assert(orig_data[k] && rx_data[k]);

        for (size_t b = 0; b < sz; ++b) {
            orig_data[k][b] = (uint8_t)((k * 13 + b * 23) & 0xFF);
        }
        memcpy(rx_data[k], orig_data[k], sz);

        media_slices[k].data = orig_data[k];
        media_slices[k].len = sz;
        is_present[k] = true;
    }

    uint8_t col_buf[AVX_FEC_MAX_COLS][1400];
    avx_fec_slice_t col_slices[AVX_FEC_MAX_COLS];
    bool col_present[AVX_FEC_MAX_COLS];
    for (uint16_t j = 0; j < D; ++j) {
        col_slices[j].data = col_buf[j];
        col_slices[j].len = 0;
        col_present[j] = true;
    }

    int gen_ret = avx_fec_generate_block(media_slices, block_frags, D, L, false, col_slices, NULL);
    assert(gen_ret == 0);

    /* Drop the short tail fragment (frag 9) */
    is_present[9] = false;
    memset(rx_data[9], 0, standard_size);

    avx_fec_slice_t rx_slices[10];
    for (uint16_t k = 0; k < block_frags; ++k) {
        rx_slices[k].data = rx_data[k];
        rx_slices[k].len = is_present[k] ? ((k == 9) ? tail_size : standard_size) : 0;
    }

    int recovered = avx_fec_reconstruct_block(
        rx_slices,
        is_present,
        block_frags,
        D,
        L,
        col_slices,
        col_present,
        NULL,
        NULL
    );

    assert(recovered == 1);
    assert(is_present[9]);
    assert(memcmp(rx_data[9], orig_data[9], tail_size) == 0);

    for (uint16_t k = 0; k < block_frags; ++k) {
        free(orig_data[k]);
        free(rx_data[k]);
    }

    printf("    -> Variable-length tail fragment recovered bit-exact without corrupting padding.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 5: End-to-End RUDP HARQ Stream with Zero-RTT WAN Recovery             */
/* -------------------------------------------------------------------------- */
static void test_fec_rudp_zero_nack_wan_recovery(void) {
    printf("[*] 5. Testing Live RUDP Pipeline with FEC Burst Recovery (0 NACK Retransmissions)...\n");

    uint16_t sender_port   = FEC_TEST_PORT_BASE;
    uint16_t inter_port    = FEC_TEST_PORT_BASE + 1;
    uint16_t receiver_port = FEC_TEST_PORT_BASE + 2;

    avx_socket_t tx_sock    = avx_udp_create_socket(sender_port, true);
    avx_socket_t inter_sock = avx_udp_create_socket(inter_port, true);
    avx_socket_t rx_sock    = avx_udp_create_socket(receiver_port, true);
    assert(tx_sock != AVX_INVALID_SOCKET && inter_sock != AVX_INVALID_SOCKET && rx_sock != AVX_INVALID_SOCKET);

    avx_udp_rudp_sender_t* s = avx_udp_rudp_sender_create(tx_sock, 8);
    avx_udp_rudp_receiver_t* r = avx_udp_rudp_receiver_create(rx_sock);
    assert(s && r);

    /* Configure FEC: D=10, L=5 */
    avx_fec_config_t fec_cfg = {
        .enabled = true,
        .matrix_d = 10,
        .matrix_l = 5,
        .enable_row_fec = true
    };
    avx_udp_rudp_sender_set_fec(s, &fec_cfg);
    avx_udp_rudp_receiver_set_fec(r, &fec_cfg);

    struct sockaddr_in inter_addr;
    memset(&inter_addr, 0, sizeof(inter_addr));
    inter_addr.sin_family = AF_INET;
    inter_addr.sin_port = htons(inter_port);
    inet_pton(AF_INET, "127.0.0.1", &inter_addr.sin_addr);

    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_port = htons(receiver_port);
    inet_pton(AF_INET, "127.0.0.1", &rx_addr.sin_addr);

    struct sockaddr_in tx_addr;
    memset(&tx_addr, 0, sizeof(tx_addr));
    tx_addr.sin_family = AF_INET;
    tx_addr.sin_port = htons(sender_port);
    inet_pton(AF_INET, "127.0.0.1", &tx_addr.sin_addr);

    /* Generate a 35 KB payload (~26 fragments) */
    size_t payload_len = 35 * 1024;
    uint8_t *tx_data = (uint8_t*)malloc(payload_len);
    uint8_t *rx_data = (uint8_t*)malloc(payload_len + 4096);
    assert(tx_data && rx_data);

    for (size_t i = 0; i < payload_len; ++i) {
        tx_data[i] = (uint8_t)((i * 47 + 29) & 0xFF);
    }

    uint32_t seq = 2026;
    uint32_t fourcc = 0x30514853; /* 'SHQ0' */
    int64_t tc = 987654321LL;

    /* Transmit frame from sender towards intermediary */
    int send_ret = avx_udp_rudp_send_frame(s, &inter_addr, seq, fourcc, tc, tx_data, payload_len);
    assert(send_ret == 0);

    /*
     * Intermediary: reads packets sent from sender, and forwards to receiver,
     * BUT intentionally drops media fragments 3, 4, 13 (a multi-drop loss pattern).
     */
    int dropped_count = 0;
    int forwarded_count = 0;
    while (1) {
        avx_pollfd_t pfd = { .fd = inter_sock, .events = POLLIN, .revents = 0 };
        if (avx_poll(&pfd, 1, 50) <= 0 || !(pfd.revents & POLLIN)) {
            break;
        }

        uint8_t pkt[2048];
        ssize_t n = recv(inter_sock, (char*)pkt, sizeof(pkt), 0);
        if (n <= 0) break;

        if (n >= (ssize_t)sizeof(avx_udp_header_t)) {
            avx_udp_header_t hdr;
            memcpy(&hdr, pkt, sizeof(hdr));

            /* If it's a media packet with index 3, 4, or 13 -> simulate WAN packet loss drop! */
            if (hdr.fourcc == fourcc && (hdr.frag_idx == 3 || hdr.frag_idx == 4 || hdr.frag_idx == 13)) {
                dropped_count++;
                continue; /* Drop it! */
            }
        }

        /* Forward to receiver */
        ssize_t fw = sendto(inter_sock, (const char*)pkt, (size_t)n, 0, (struct sockaddr*)&rx_addr, sizeof(rx_addr));
        if (fw > 0) forwarded_count++;
    }

    printf("    -> Intermediary: dropped %d, forwarded %d packets\n", dropped_count, forwarded_count);

    /* Receiver reassembles the frame with FEC */
    size_t rx_size = 0;
    uint32_t rx_fourcc = 0;
    int64_t rx_timecode = 0;

    int recv_ret = avx_udp_rudp_recv_frame(r, &inter_addr, rx_data, payload_len + 4096,
                                           &rx_size, &rx_fourcc, &rx_timecode, 500);
    assert(recv_ret == 0);
    assert(rx_size == payload_len);
    assert(rx_fourcc == fourcc);
    assert(rx_timecode == tc);
    assert(memcmp(rx_data, tx_data, payload_len) == 0);

    /* Check Telemetry */
    avx_rudp_telemetry_t rx_telem, tx_telem;
    assert(avx_udp_rudp_receiver_get_telemetry(r, &rx_telem));
    assert(avx_udp_rudp_sender_get_telemetry(s, &tx_telem));

    printf("    -> Dropped packets: %d\n", dropped_count);
    printf("    -> Receiver recovered packets: %llu\n", (unsigned long long)rx_telem.packets_recovered);
    printf("    -> Sender retransmitted packets: %llu\n", (unsigned long long)tx_telem.packets_retransmitted);

    /* The critical invariant of zero-latency FEC: */
    assert(rx_telem.packets_recovered >= 3);
    assert(tx_telem.packets_retransmitted == 0);

    printf("    -> 100%% Bit-exact frame delivery with ZERO NACK retransmissions!\n");

    free(tx_data);
    free(rx_data);
    avx_udp_rudp_sender_destroy(s);
    avx_udp_rudp_receiver_destroy(r);
    avx_socket_close(tx_sock);
    avx_socket_close(inter_sock);
    avx_socket_close(rx_sock);
}

int main(void) {
    printf("===================================================================\n");
    printf("  OpenAVX SMPTE ST 2022-1 2D XOR FEC Test Suite                    \n");
    printf("===================================================================\n");

    test_fec_xor_kernel();
    test_fec_1d_column_burst_recovery();
    test_fec_2d_matrix_multi_drop_recovery();
    test_fec_variable_length_recovery();
    test_fec_rudp_zero_nack_wan_recovery();

    printf("\n>>> ALL FEC TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
