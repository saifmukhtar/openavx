/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_discovery_udp_test.c - Automated Discovery and UDP Transport verification
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "avx_platform.h"

#include "openavx.h"
#include "avx_udp.h"
#include "avx_wire.h"

#define DISCOVERY_WIDTH 1920
#define DISCOVERY_HEIGHT 1080

typedef struct {
    oavx_stream_t *p_send;
    volatile bool running;
    uint8_t* frame_buf;
} sender_bg_arg_t;

static void* sender_bg_worker(void* arg) {
    sender_bg_arg_t* s_arg = (sender_bg_arg_t*)arg;

    oavx_video_frame_t video_frame = {
        .width = DISCOVERY_WIDTH,
        .height = DISCOVERY_HEIGHT,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_SHQ2,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { s_arg->frame_buf, NULL, NULL, NULL },
        .strides = { (size_t)DISCOVERY_WIDTH * 2, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 11223344,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    while (s_arg->running) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 15000000L };
        nanosleep(&ts, NULL);
        oavx_stream_send_video(s_arg->p_send, &video_frame);
    }
    return NULL;
}

int main(void) {
    printf("\n=======================================================\n");
    printf(" OpenAVX Discovery & UDP Transport Test\n");
    printf("=======================================================\n\n");

    /* 1. Initialize openavx */
    printf("[*] 1. Initializing OpenAVX Context...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 2. Create Sender */
    printf("[*] 2. Spawning Sender \"STUDIO_CAMERA_1\"...\n");
    oavx_stream_config_t send_create = {
        .name = "STUDIO_CAMERA_1",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);

    size_t raw_size = (size_t)DISCOVERY_WIDTH * DISCOVERY_HEIGHT * 2;
    uint8_t* raw_uyvy = malloc(raw_size);
    assert(raw_uyvy != NULL);
    memset(raw_uyvy, 0x55, raw_size);

    sender_bg_arg_t s_arg = {
        .p_send = p_send,
        .running = true,
        .frame_buf = raw_uyvy
    };
    pthread_t s_thread;
    pthread_create(&s_thread, NULL, sender_bg_worker, &s_arg);

    /* 3. Test Dynamic Discovery */
    printf("[*] 3. Testing OpenAVX Finder Auto-Discovery...\n");
    oavx_discovery_t *p_find = oavx_discovery_create(ctx, "public");
    assert(p_find != NULL);

    bool found = oavx_discovery_wait(p_find, 1000);
    assert(found);

    oavx_endpoint_t sources[16];
    size_t num_sources = oavx_discovery_get_sources(p_find, sources, 16);
    assert(num_sources >= 1);

    printf("    [+] Discovered %zu Active OpenAVX Source(s):\n", num_sources);
    int match_idx = -1;
    for (size_t i = 0; i < num_sources; ++i) {
        printf("        [%zu] Name: \"%s\" | URL: \"%s\"\n",
               i, sources[i].name, sources[i].address);
        if (strcmp(sources[i].name, "STUDIO_CAMERA_1") == 0) {
            match_idx = (int)i;
        }
    }
    assert(match_idx >= 0);

    /* 4. Connect Receiver directly to discovered URL */
    printf("[*] 4. Connecting Receiver to Discovered Source (\"%s\")...\n",
           sources[match_idx].address);
    oavx_stream_config_t recv_create = {
        .name = "Auto_Discovered_Client",
        .endpoint = sources[match_idx].address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    oavx_video_frame_t v_frame = {0};
    oavx_audio_frame_t a_frame = {0};
    oavx_metadata_frame_t m_frame = {0};

    bool video_captured = false;
    for (int attempts = 0; attempts < 50 && !video_captured; ++attempts) {
        oavx_frame_type_t ftype = oavx_stream_receive(p_recv, &v_frame, &a_frame, &m_frame, 200);
        if (ftype == OAVX_FRAME_TYPE_VIDEO) {
            video_captured = true;
            printf("    [+] Live Stream successfully captured via Auto-Discovery!\n");
            printf("        - Resolution: %u x %u | FourCC: 0x%08X\n",
                   v_frame.width, v_frame.height, v_frame.fourcc);
            assert(v_frame.width == DISCOVERY_WIDTH);
            assert(v_frame.height == DISCOVERY_HEIGHT);
            oavx_stream_release_video(p_recv, &v_frame);
        }
    }
    assert(video_captured);

    /* Clean up discovery / streaming sessions */
    s_arg.running = false;
    pthread_join(s_thread, NULL);
    oavx_stream_close(p_recv);
    oavx_discovery_destroy(p_find);
    oavx_stream_close(p_send);

    /* 5. Test High-Throughput UDP Datagram Fragmenter & Reassembler */
    printf("[*] 5. Testing UDP Datagram Transport Engine...\n");
    const uint16_t udp_port = 6970;
    int rx_sock = avx_udp_create_socket(udp_port, true);
    assert(rx_sock >= 0);

    int tx_sock = avx_udp_create_socket(0, false);
    assert(tx_sock >= 0);

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(udp_port);
    inet_pton(AF_INET, "127.0.0.1", &dest_addr.sin_addr);

    /* Generate a 250 KB packet stream (fragmented into ~180 datagrams) */
    const size_t test_udp_payload_len = 256 * 1024;
    uint8_t* tx_payload = malloc(test_udp_payload_len);
    uint8_t* rx_payload = malloc(test_udp_payload_len + 4096);
    assert(tx_payload && rx_payload);

    for (size_t i = 0; i < test_udp_payload_len; ++i) {
        tx_payload[i] = (uint8_t)(i & 0xFF);
    }

    uint32_t test_seq = 42;
    uint32_t test_fourcc = 0x32514853; /* 'SHQ2' */
    int64_t test_timecode = 9988776655LL;

    int send_ret = avx_udp_send_frame(tx_sock, &dest_addr, test_seq, test_fourcc, test_timecode,
                                      tx_payload, test_udp_payload_len);
    assert(send_ret == 0);
    printf("    [+] Fragmented and sent %zu bytes over UDP datagram bursts...\n", test_udp_payload_len);

    size_t rx_assembled_size = 0;
    uint32_t rx_fourcc = 0;
    int64_t rx_timecode = 0;

    int recv_ret = avx_udp_recv_frame(rx_sock, rx_payload, test_udp_payload_len + 4096,
                                      &rx_assembled_size, &rx_fourcc, &rx_timecode, 1000);
    assert(recv_ret == 0);
    assert(rx_assembled_size == test_udp_payload_len);
    assert(rx_fourcc == test_fourcc);
    assert(rx_timecode == test_timecode);
    assert(memcmp(tx_payload, rx_payload, test_udp_payload_len) == 0);

    printf("    [+] Reassembled %zu bytes with 100%% bitwise integrity!\n", rx_assembled_size);
    printf("    [+] FourCC (0x%08X) and Timecode (%lld) matched!\n", rx_fourcc, (long long)rx_timecode);

    /* Also verify custom wire profile UDP datagram magic configuration */
    oavx_wire_profile_t custom_prof = *avx_wire_get_default_profile();
    custom_prof.magic_udp = 0x54534554; /* 'TEST' */
    avx_wire_set_active_profile(&custom_prof);

    uint8_t custom_pkt[sizeof(avx_udp_header_t) + 64];
    avx_udp_header_t *c_hdr = (avx_udp_header_t*)custom_pkt;
    c_hdr->magic = 0x54534554;
    c_hdr->frame_seq = 43;
    c_hdr->frag_idx = 0;
    c_hdr->frag_count = 1;
    c_hdr->fourcc = 0x30514853;
    c_hdr->timecode = 12345LL;
    c_hdr->payload_len = 64;
    c_hdr->reserved = 0;
    memset(custom_pkt + sizeof(avx_udp_header_t), 0xAA, 64);
    sendto(tx_sock, (const char*)custom_pkt, sizeof(custom_pkt), 0, (const struct sockaddr*)&dest_addr, sizeof(dest_addr));

    uint8_t custom_rx[512];
    recv_ret = avx_udp_recv_frame(rx_sock, custom_rx, sizeof(custom_rx), &rx_assembled_size, &rx_fourcc, &rx_timecode, 1000);
    assert(recv_ret == 0);
    assert(rx_assembled_size == 64);
    assert(custom_rx[0] == 0xAA);
    printf("    [+] Wire profile extensibility verified: custom UDP magic ('TEST') dynamically accepted!\n");

    /* Reset to default profile */
    avx_wire_set_active_profile(avx_wire_get_default_profile());

    close(rx_sock);
    close(tx_sock);
    free(tx_payload);
    free(rx_payload);
    free(raw_uyvy);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_discovery_udp_test\n\n");
    return 0;
}
