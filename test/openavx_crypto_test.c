/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_crypto_test.c - Native End-to-End Encryption (ChaCha20-Poly1305 / Monocypher) Verification
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "avx_platform.h"
#include "openavx.h"
#include "avx_crypto.h"
#include "avx_udp.h"
#include "avx_discovery_server.h"

#define TEST_CRYPTO_TCP_PORT   17971
#define TEST_CRYPTO_RELAY_PORT 17972

static void test_crypto_key_derivation(void)
{
    printf("[*] Test 1: Key Derivation & Domain Separation (BLAKE2b)...\n");

    avx_crypto_ctx_t ctx1, ctx2, ctx3;

    /* 1.1 Deterministic key derivation: same passphrase + stream name produces identical key */
    assert(avx_crypto_init(&ctx1, "SuperSecret123", "StreamAlpha") == 0);
    assert(ctx1.enabled == true);
    assert(avx_crypto_init(&ctx2, "SuperSecret123", "StreamAlpha") == 0);
    assert(ctx2.enabled == true);

    assert(memcmp(ctx1.key, ctx2.key, AVX_CRYPTO_KEY_SIZE) == 0);

    /* 1.2 Domain separation: different stream name produces completely different key */
    assert(avx_crypto_init(&ctx3, "SuperSecret123", "StreamBeta") == 0);
    assert(ctx3.enabled == true);
    assert(memcmp(ctx1.key, ctx3.key, AVX_CRYPTO_KEY_SIZE) != 0);

    /* 1.3 Different passphrase produces completely different key */
    avx_crypto_ctx_t ctx4;
    assert(avx_crypto_init(&ctx4, "DifferentSecret456", "StreamAlpha") == 0);
    assert(memcmp(ctx1.key, ctx4.key, AVX_CRYPTO_KEY_SIZE) != 0);

    /* 1.4 Empty or NULL passphrase disables crypto */
    avx_crypto_ctx_t ctx_disabled;
    assert(avx_crypto_init(&ctx_disabled, NULL, "StreamAlpha") == 0);
    assert(ctx_disabled.enabled == false);
    assert(avx_crypto_init(&ctx_disabled, "", "StreamAlpha") == 0);
    assert(ctx_disabled.enabled == false);

    /* 1.5 Wipe zeroes memory securely */
    avx_crypto_wipe(&ctx1);
    assert(ctx1.enabled == false);
    uint8_t zero_buf[AVX_CRYPTO_KEY_SIZE] = {0};
    assert(memcmp(ctx1.key, zero_buf, AVX_CRYPTO_KEY_SIZE) == 0);

    avx_crypto_wipe(&ctx2);
    avx_crypto_wipe(&ctx3);
    avx_crypto_wipe(&ctx4);

    printf("    [PASS] Test 1: Key derivation and domain separation verified.\n");
}

static void test_crypto_aead_encrypt_decrypt(void)
{
    printf("[*] Test 2: Payload AEAD Encryption / Decryption Verification...\n");

    avx_crypto_ctx_t sender_ctx, receiver_ctx;
    const char *pass = "MasterProductionKey2026!";
    const char *stream = "Cam1_Main";

    assert(avx_crypto_init(&sender_ctx, pass, stream) == 0);
    assert(avx_crypto_init(&receiver_ctx, pass, stream) == 0);

    /* Construct a synthetic 32-byte UDP header as Additional Data (AD) */
    avx_udp_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = OAVX_UDP_MAGIC;
    hdr.frame_seq = 42;
    hdr.frag_idx = 3;
    hdr.frag_count = 10;
    hdr.fourcc = 0x32315659; /* YV12 */
    hdr.timecode = 123456789LL;
    hdr.payload_len = 1000;
    hdr.reserved = AVX_UDP_FLAG_ENCRYPTED;

    /* Create sample payload */
    uint8_t plain[1000];
    for (size_t i = 0; i < sizeof(plain); ++i) {
        plain[i] = (uint8_t)((i * 31 + 7) & 0xFF);
    }

    /* Buffer for encrypted output: [salt (8B)] [cipher (1000B)] [mac (16B)] */
    uint8_t enc_buf[1000 + AVX_CRYPTO_OVERHEAD];
    size_t enc_len = sizeof(enc_buf);

    int ret = avx_crypto_encrypt_payload(&sender_ctx, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                         &hdr, sizeof(hdr),
                                         plain, sizeof(plain),
                                         enc_buf, &enc_len);
    assert(ret == 0);
    assert(enc_len == sizeof(plain) + AVX_CRYPTO_OVERHEAD);

    /* Verify ciphertext is not equal to plaintext */
    assert(memcmp(enc_buf + AVX_CRYPTO_SALT_SIZE, plain, sizeof(plain)) != 0);

    /* Decrypt using receiver context */
    uint8_t dec_buf[1000];
    size_t dec_len = sizeof(dec_buf);

    ret = avx_crypto_decrypt_payload(&receiver_ctx, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                     &hdr, sizeof(hdr),
                                     enc_buf, enc_len,
                                     dec_buf, &dec_len);
    assert(ret == 0);
    assert(dec_len == sizeof(plain));
    assert(memcmp(dec_buf, plain, sizeof(plain)) == 0);

    avx_crypto_wipe(&sender_ctx);
    avx_crypto_wipe(&receiver_ctx);

    printf("    [PASS] Test 2: AEAD encryption and bit-exact decryption verified.\n");
}

static void test_crypto_tampering_and_mac_failure(void)
{
    printf("[*] Test 3: Bit-flip Tampering & Poly1305 MAC Failure Rejection...\n");

    avx_crypto_ctx_t sender_ctx, receiver_ctx;
    assert(avx_crypto_init(&sender_ctx, "TamperProofTestPass", "VideoFeed") == 0);
    assert(avx_crypto_init(&receiver_ctx, "TamperProofTestPass", "VideoFeed") == 0);

    avx_udp_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = OAVX_UDP_MAGIC;
    hdr.frame_seq = 100;
    hdr.frag_idx = 1;
    hdr.frag_count = 5;
    hdr.payload_len = 512;
    hdr.reserved = AVX_UDP_FLAG_ENCRYPTED;

    uint8_t plain[512];
    memset(plain, 0xAB, sizeof(plain));

    uint8_t enc_buf[512 + AVX_CRYPTO_OVERHEAD];
    size_t enc_len = sizeof(enc_buf);

    assert(avx_crypto_encrypt_payload(&sender_ctx, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                      &hdr, sizeof(hdr),
                                      plain, sizeof(plain),
                                      enc_buf, &enc_len) == 0);

    uint8_t dec_buf[512];
    size_t dec_len = sizeof(dec_buf);

    /* 3.1 Tamper: Flip 1 bit in ciphertext */
    uint8_t tampered[512 + AVX_CRYPTO_OVERHEAD];
    memcpy(tampered, enc_buf, enc_len);
    tampered[AVX_CRYPTO_SALT_SIZE + 10] ^= 0x01; /* Bit flip in ciphertext */

    int ret = avx_crypto_decrypt_payload(&receiver_ctx, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                         &hdr, sizeof(hdr),
                                         tampered, enc_len,
                                         dec_buf, &dec_len);
    assert(ret != 0); /* MUST fail */

    /* 3.2 Tamper: Flip 1 bit in MAC tag */
    memcpy(tampered, enc_buf, enc_len);
    tampered[enc_len - 1] ^= 0x80; /* Bit flip in authentication tag */
    ret = avx_crypto_decrypt_payload(&receiver_ctx, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                     &hdr, sizeof(hdr),
                                     tampered, enc_len,
                                     dec_buf, &dec_len);
    assert(ret != 0); /* MUST fail */

    /* 3.3 Tamper: Modify Additional Data (header sequence spoofing) */
    avx_udp_header_t forged_hdr = hdr;
    forged_hdr.frame_seq = 101; /* Tampered sequence number */
    ret = avx_crypto_decrypt_payload(&receiver_ctx, forged_hdr.frame_seq, forged_hdr.frag_idx, forged_hdr.frag_count,
                                     &forged_hdr, sizeof(forged_hdr),
                                     enc_buf, enc_len,
                                     dec_buf, &dec_len);
    assert(ret != 0); /* MUST fail due to header AD mismatch */

    /* 3.4 Tamper: Truncate packet below minimum overhead */
    ret = avx_crypto_decrypt_payload(&receiver_ctx, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                     &hdr, sizeof(hdr),
                                     enc_buf, AVX_CRYPTO_OVERHEAD - 1,
                                     dec_buf, &dec_len);
    assert(ret != 0);

    avx_crypto_wipe(&sender_ctx);
    avx_crypto_wipe(&receiver_ctx);

    printf("    [PASS] Test 3: All tampering attempts rejected by Poly1305 MAC.\n");
}

static void test_crypto_wrong_passphrase(void)
{
    printf("[*] Test 4: Wrong Passphrase Rejection...\n");

    avx_crypto_ctx_t sender_ctx, wrong_receiver_ctx;
    assert(avx_crypto_init(&sender_ctx, "CorrectPassword999", "LiveStream") == 0);
    assert(avx_crypto_init(&wrong_receiver_ctx, "WrongPassword888", "LiveStream") == 0);

    avx_udp_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = OAVX_UDP_MAGIC;
    hdr.frame_seq = 7;
    hdr.frag_idx = 0;
    hdr.frag_count = 1;
    hdr.payload_len = 256;
    hdr.reserved = AVX_UDP_FLAG_ENCRYPTED;

    uint8_t plain[256];
    memset(plain, 0x42, sizeof(plain));

    uint8_t enc_buf[256 + AVX_CRYPTO_OVERHEAD];
    size_t enc_len = sizeof(enc_buf);

    assert(avx_crypto_encrypt_payload(&sender_ctx, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                      &hdr, sizeof(hdr),
                                      plain, sizeof(plain),
                                      enc_buf, &enc_len) == 0);

    uint8_t dec_buf[256];
    size_t dec_len = sizeof(dec_buf);

    int ret = avx_crypto_decrypt_payload(&wrong_receiver_ctx, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                         &hdr, sizeof(hdr),
                                         enc_buf, enc_len,
                                         dec_buf, &dec_len);
    assert(ret != 0); /* MUST fail */

    avx_crypto_wipe(&sender_ctx);
    avx_crypto_wipe(&wrong_receiver_ctx);

    printf("    [PASS] Test 4: Wrong passphrase correctly rejected without leaking plaintext.\n");
}

static void test_rudp_e2ee_loopback(void)
{
    printf("[*] Test 5: End-to-End RUDP Sender -> Receiver with Native E2EE...\n");

    avx_socket_t tx_sock = avx_udp_create_socket_bind("127.0.0.1", 0, false);
    avx_socket_t rx_sock = avx_udp_create_socket_bind("127.0.0.1", 0, false);
    assert(tx_sock != AVX_INVALID_SOCKET);
    assert(rx_sock != AVX_INVALID_SOCKET);

    struct sockaddr_in rx_addr;
    avx_socklen_t rx_len = sizeof(rx_addr);
    assert(getsockname(rx_sock, (struct sockaddr*)&rx_addr, &rx_len) == 0);

    struct sockaddr_in tx_dest;
    memset(&tx_dest, 0, sizeof(tx_dest));
    tx_dest.sin_family = AF_INET;
    tx_dest.sin_port = rx_addr.sin_port;
    inet_pton(AF_INET, "127.0.0.1", &tx_dest.sin_addr);

    struct sockaddr_in tx_addr;
    avx_socklen_t tx_len = sizeof(tx_addr);
    assert(getsockname(tx_sock, (struct sockaddr*)&tx_addr, &tx_len) == 0);

    struct sockaddr_in rx_src;
    memset(&rx_src, 0, sizeof(rx_src));
    rx_src.sin_family = AF_INET;
    rx_src.sin_port = tx_addr.sin_port;
    inet_pton(AF_INET, "127.0.0.1", &rx_src.sin_addr);

    avx_udp_rudp_sender_t *sender = avx_udp_rudp_sender_create(tx_sock, 8);
    avx_udp_rudp_receiver_t *receiver = avx_udp_rudp_receiver_create(rx_sock);
    assert(sender != NULL && receiver != NULL);

    /* Configure identical E2EE passphrase */
    const char *pass = "OpenAVX_Native_ChaCha20_Poly1305_Secret!";
    const char *stream = "TestStream_Loopback";
    assert(avx_udp_rudp_sender_set_passphrase(sender, pass, stream) == 0);
    assert(avx_udp_rudp_receiver_set_passphrase(receiver, pass, stream) == 0);

    /* Send a 50 KB multi-fragment video frame (36 fragments) */
    const size_t frame_bytes = 50 * 1024;
    uint8_t *tx_buf = (uint8_t*)malloc(frame_bytes);
    uint8_t *rx_buf = (uint8_t*)malloc(frame_bytes);
    assert(tx_buf && rx_buf);

    for (size_t i = 0; i < frame_bytes; ++i) {
        tx_buf[i] = (uint8_t)((i * 17 + 5) & 0xFF);
    }

    uint32_t fourcc = 0x32315659; /* YV12 */
    int64_t timecode = 987654321LL;

    int send_ret = avx_udp_rudp_send_frame(sender, (struct sockaddr*)&tx_dest,
                                           1, fourcc, timecode, tx_buf, frame_bytes);
    assert(send_ret == 0);

    uint32_t out_fourcc = 0;
    int64_t out_timecode = 0;
    size_t out_size = 0;
    int recv_ret = -1;

    for (int poll = 0; poll < 100 && recv_ret < 0; ++poll) {
        recv_ret = avx_udp_rudp_recv_frame(receiver, (struct sockaddr*)&rx_src,
                                           rx_buf, frame_bytes,
                                           &out_size, &out_fourcc, &out_timecode, 15);
    }

    assert(recv_ret == 0);
    assert(out_size == frame_bytes);
    assert(out_fourcc == fourcc);
    assert(out_timecode == timecode);
    assert(memcmp(tx_buf, rx_buf, frame_bytes) == 0);

    free(tx_buf);
    free(rx_buf);
    avx_udp_rudp_sender_destroy(sender);
    avx_udp_rudp_receiver_destroy(receiver);
    avx_socket_close(tx_sock);
    avx_socket_close(rx_sock);

    printf("    [PASS] Test 5: End-to-End RUDP with ChaCha20-Poly1305 encryption verified.\n");
}

static void test_rudp_e2ee_over_relay(void)
{
    printf("[*] Test 6: E2EE Stream Over UDP Relay (Zero-Knowledge Relay Verification)...\n");

    avx_discovery_server_config_t cfg = {
        .bind_ip = "127.0.0.1",
        .tcp_port = TEST_CRYPTO_TCP_PORT,
        .relay_udp_port = TEST_CRYPTO_RELAY_PORT,
        .enable_relay = true,
        .public_ip = "127.0.0.1"
    };

    avx_discovery_server_t *srv = avx_discovery_server_create_ex(&cfg);
    assert(srv != NULL);

    avx_socket_t tx_sock = avx_udp_create_socket(0, false);
    avx_socket_t rx_sock = avx_udp_create_socket(0, false);
    assert(tx_sock != AVX_INVALID_SOCKET);
    assert(rx_sock != AVX_INVALID_SOCKET);

    const uint32_t session_id = 707;
    assert(avx_relay_client_bind_session(tx_sock, "127.0.0.1", TEST_CRYPTO_RELAY_PORT, session_id, true) == 0);
    assert(avx_relay_client_bind_session(rx_sock, "127.0.0.1", TEST_CRYPTO_RELAY_PORT, session_id, false) == 0);

    struct sockaddr_in relay_sa;
    memset(&relay_sa, 0, sizeof(relay_sa));
    relay_sa.sin_family = AF_INET;
    relay_sa.sin_port = htons(TEST_CRYPTO_RELAY_PORT);
    inet_pton(AF_INET, "127.0.0.1", &relay_sa.sin_addr);

    avx_udp_rudp_sender_t *sender = avx_udp_rudp_sender_create(tx_sock, 8);
    avx_udp_rudp_receiver_t *receiver = avx_udp_rudp_receiver_create(rx_sock);
    assert(sender != NULL && receiver != NULL);

    /* Configure identical E2EE passphrase on publisher and subscriber */
    const char *pass = "ZeroKnowledgeRelaySecret2026!";
    const char *stream = "RelaySecuredStream";
    assert(avx_udp_rudp_sender_set_passphrase(sender, pass, stream) == 0);
    assert(avx_udp_rudp_receiver_set_passphrase(receiver, pass, stream) == 0);

    /* Configure 2D FEC on both ends */
    avx_fec_config_t fec_cfg = {
        .enabled = true,
        .matrix_d = 4,
        .matrix_l = 4,
        .enable_row_fec = true
    };
    avx_udp_rudp_sender_set_fec(sender, &fec_cfg);
    avx_udp_rudp_receiver_set_fec(receiver, &fec_cfg);

    const size_t frame_bytes = 48 * 1024;
    uint8_t *tx_frame = (uint8_t*)malloc(frame_bytes);
    uint8_t *rx_frame = (uint8_t*)malloc(frame_bytes);
    assert(tx_frame && rx_frame);

    for (size_t i = 0; i < frame_bytes; ++i) {
        tx_frame[i] = (uint8_t)((i * 13 + 3) & 0xFF);
    }

    int send_ret = avx_udp_rudp_send_frame(sender, (struct sockaddr*)&relay_sa,
                                           10, 0x32315659, 55555LL,
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
    assert(rx_timecode == 55555LL);
    assert(memcmp(tx_frame, rx_frame, frame_bytes) == 0);

    /* Verify relay server statistics */
    avx_relay_stats_t stats;
    assert(avx_relay_server_get_stats(srv, &stats) == true);
    assert(stats.packets_relayed > 0);
    assert(stats.bytes_relayed > 0);
    printf("    [+] Zero-Knowledge Relay forwarded %llu encrypted packets (%llu bytes) without possessing keys\n",
           (unsigned long long)stats.packets_relayed, (unsigned long long)stats.bytes_relayed);

    free(tx_frame);
    free(rx_frame);
    avx_udp_rudp_sender_destroy(sender);
    avx_udp_rudp_receiver_destroy(receiver);
    avx_socket_close(tx_sock);
    avx_socket_close(rx_sock);
    avx_discovery_server_destroy(srv);

    printf("    [PASS] Test 6: E2EE stream through Zero-Knowledge relay verified.\n");
}

static void test_rudp_e2ee_mismatch_rejection(void)
{
    printf("[*] Test 7: Encryption Mismatch Rejection (Plaintext vs Encrypted)...\n");

    avx_socket_t tx_sock = avx_udp_create_socket_bind("127.0.0.1", 0, false);
    avx_socket_t rx_sock = avx_udp_create_socket_bind("127.0.0.1", 0, false);
    assert(tx_sock != AVX_INVALID_SOCKET);
    assert(rx_sock != AVX_INVALID_SOCKET);

    struct sockaddr_in rx_addr;
    avx_socklen_t rx_len = sizeof(rx_addr);
    assert(getsockname(rx_sock, (struct sockaddr*)&rx_addr, &rx_len) == 0);

    struct sockaddr_in tx_dest;
    memset(&tx_dest, 0, sizeof(tx_dest));
    tx_dest.sin_family = AF_INET;
    tx_dest.sin_port = rx_addr.sin_port;
    inet_pton(AF_INET, "127.0.0.1", &tx_dest.sin_addr);

    struct sockaddr_in tx_addr;
    avx_socklen_t tx_len = sizeof(tx_addr);
    assert(getsockname(tx_sock, (struct sockaddr*)&tx_addr, &tx_len) == 0);

    struct sockaddr_in rx_src;
    memset(&rx_src, 0, sizeof(rx_src));
    rx_src.sin_family = AF_INET;
    rx_src.sin_port = tx_addr.sin_port;
    inet_pton(AF_INET, "127.0.0.1", &rx_src.sin_addr);

    avx_udp_rudp_sender_t *sender = avx_udp_rudp_sender_create(tx_sock, 8);
    avx_udp_rudp_receiver_t *receiver = avx_udp_rudp_receiver_create(rx_sock);
    assert(sender != NULL && receiver != NULL);

    /* 7.1 Sender is ENCRYPTED, Receiver is PLAINTEXT (no passphrase) */
    assert(avx_udp_rudp_sender_set_passphrase(sender, "EncryptedSecretKey!", "MismatchTest") == 0);

    uint8_t payload[2048];
    memset(payload, 0xEE, sizeof(payload));

    int send_ret = avx_udp_rudp_send_frame(sender, (struct sockaddr*)&tx_dest,
                                           1, 0x32315659, 100LL, payload, sizeof(payload));
    assert(send_ret == 0);

    uint8_t rx_buf[2048];
    uint32_t out_fourcc = 0;
    int64_t out_timecode = 0;
    size_t out_size = 0;

    /* Receiver must NOT deliver encrypted frame when crypto is disabled */
    int recv_ret = avx_udp_rudp_recv_frame(receiver, (struct sockaddr*)&rx_src,
                                          rx_buf, sizeof(rx_buf),
                                          &out_size, &out_fourcc, &out_timecode, 50);
    assert(recv_ret != 0); /* Must fail / timeout */

    /* 7.2 Now Sender is PLAINTEXT (disable crypto), Receiver is ENCRYPTED */
    assert(avx_udp_rudp_sender_set_passphrase(sender, NULL, NULL) == 0);
    assert(avx_udp_rudp_receiver_set_passphrase(receiver, "EncryptedSecretKey!", "MismatchTest") == 0);

    send_ret = avx_udp_rudp_send_frame(sender, (struct sockaddr*)&tx_dest,
                                       2, 0x32315659, 200LL, payload, sizeof(payload));
    assert(send_ret == 0);

    /* Receiver must NOT accept plaintext frame when encryption is enforced */
    recv_ret = avx_udp_rudp_recv_frame(receiver, (struct sockaddr*)&rx_src,
                                       rx_buf, sizeof(rx_buf),
                                       &out_size, &out_fourcc, &out_timecode, 50);
    assert(recv_ret != 0); /* Must fail / timeout */

    avx_udp_rudp_sender_destroy(sender);
    avx_udp_rudp_receiver_destroy(receiver);
    avx_socket_close(tx_sock);
    avx_socket_close(rx_sock);

    printf("    [PASS] Test 7: Encryption state mismatches cleanly rejected.\n");
}

int main(void)
{
    printf("=================================================================\n");
    printf("OpenAVX Native End-to-End Encryption (E2EE) Test Suite\n");
    printf("Cipher: XChaCha20-Poly1305 (Monocypher) with BLAKE2b KDF\n");
    printf("=================================================================\n");

    test_crypto_key_derivation();
    test_crypto_aead_encrypt_decrypt();
    test_crypto_tampering_and_mac_failure();
    test_crypto_wrong_passphrase();
    test_rudp_e2ee_loopback();
    test_rudp_e2ee_over_relay();
    test_rudp_e2ee_mismatch_rejection();

    printf("=================================================================\n");
    printf("All Native E2EE Crypto Tests Passed Successfully (100%%)!\n");
    printf("=================================================================\n");
    return 0;
}
