/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_hdr_test.c - OpenAVX P216 16-bit, and HDR Metadata Test Suite
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "avx_platform.h"

#include "openavx.h"
#include "avx_hdr.h"
#include "avx_color.h"

#define TEST_WIDTH 320
#define TEST_HEIGHT 240
#define TEST_PORT 7960

typedef struct {
    oavx_stream_t* p_send;
    volatile bool running;
    uint8_t* p216_data;
    const char* hdr_metadata;
} sender_thread_arg_t;

static void* sender_thread_func(void* arg) {
    sender_thread_arg_t* s = (sender_thread_arg_t*)arg;

    oavx_video_frame_t frame = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_P216,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { s->p216_data, NULL, NULL, NULL },
        .strides = { (size_t)TEST_WIDTH * 2, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { (uint32_t)TEST_WIDTH, (uint32_t)TEST_HEIGHT },
        .pts_ns = 55667788,
        .dts_ns = 0,
        .metadata = s->hdr_metadata,
        .metadata_len = s->hdr_metadata ? strlen(s->hdr_metadata) : 0,
        .user_ctx = NULL
    };

    while (s->running) {
        struct timespec ts = { 0, 15000000L }; /* ~60 fps */
        nanosleep(&ts, NULL);
        oavx_stream_send_video(s->p_send, &frame);
    }
    return NULL;
}

int main(void) {
    printf("\n======================================================================\n");
    printf(" openavx HDR & 16-bit Video Verification Test Suite                   \n");
    printf(" Features: P216 16-bit Video, Dynamic Load, and Rec.2100 HDR Metadata\n");
    printf("======================================================================\n\n");

    /* --------------------------------------------------------------------- */
    /* 1. Test Dynamic Loading: dlopen("build/libopenavx.so.1")               */
    /* --------------------------------------------------------------------- */
    printf("[*] 1. Testing dynamic dlopen of libopenavx.so.1 and symbol resolution...\n");
    void* handle = dlopen("build/libopenavx.so.1", RTLD_NOW);
    if (!handle) handle = dlopen("./build/libopenavx.so.1", RTLD_NOW);
    if (!handle) handle = dlopen("libopenavx.so.1", RTLD_NOW);
    if (!handle) handle = dlopen("./libopenavx.so.1", RTLD_NOW);
    if (!handle) handle = dlopen("../build/libopenavx.so.1", RTLD_NOW);
    if (!handle) handle = dlopen("build/libopenavx.so", RTLD_NOW);
    if (!handle) handle = dlopen("./build/libopenavx.so", RTLD_NOW);
    if (!handle) handle = dlopen("libopenavx.so", RTLD_NOW);

    if (handle) {
        typedef const char* (*version_fn)(void);
        version_fn get_ver = (version_fn)dlsym(handle, "oavx_version");
        assert(get_ver != NULL);
        printf("  [+] Dynamic load succeeded: Version '%s'\n", get_ver());
        dlclose(handle);
    } else {
        printf("  [*] Shared library not in search path yet (will verify post-build)\n");
    }

    /* --------------------------------------------------------------------- */
    /* 2. Test HDR Metadata XML Formatting and Parsing                       */
    /* --------------------------------------------------------------------- */
    printf("[*] 2. Testing HDR Metadata XML Formatting & Parsing...\n");

    /* Test A: HLG over BT.2020 */
    avx_color_info_t hlg_info = {
        .transfer = AVX_COLOR_TRANSFER_BT2100_HLG,
        .matrix = AVX_COLOR_MATRIX_BT2020,
        .primaries = AVX_COLOR_PRIMARIES_BT2020,
        .max_cll = 0,
        .max_fall = 0
    };
    char xml_buf[256];
    bool fmt_ok = avx_hdr_format_color_info(&hlg_info, xml_buf, sizeof(xml_buf));
    assert(fmt_ok);
    printf("  [+] Formatted HLG XML: '%s'\n", xml_buf);
    assert(strstr(xml_buf, "transfer=\"bt_2100_hlg\"") != NULL);
    assert(strstr(xml_buf, "matrix=\"bt_2020\"") != NULL);
    assert(strstr(xml_buf, "primaries=\"bt_2020\"") != NULL);

    avx_color_info_t parsed_hlg = {0};
    bool parse_ok = avx_hdr_parse_color_info(xml_buf, &parsed_hlg);
    assert(parse_ok);
    assert(parsed_hlg.transfer == AVX_COLOR_TRANSFER_BT2100_HLG);
    assert(parsed_hlg.matrix == AVX_COLOR_MATRIX_BT2020);
    assert(parsed_hlg.primaries == AVX_COLOR_PRIMARIES_BT2020);
    assert(avx_hdr_is_hdr(&parsed_hlg));

    /* Test B: PQ with mastering display metadata (MaxCLL 1000 nits, MaxFALL 400 nits) */
    avx_color_info_t pq_info = {
        .transfer = AVX_COLOR_TRANSFER_BT2100_PQ,
        .matrix = AVX_COLOR_MATRIX_BT2020,
        .primaries = AVX_COLOR_PRIMARIES_BT2020,
        .max_cll = 1000,
        .max_fall = 400
    };
    fmt_ok = avx_hdr_format_color_info(&pq_info, xml_buf, sizeof(xml_buf));
    assert(fmt_ok);
    printf("  [+] Formatted PQ XML:  '%s'\n", xml_buf);
    assert(strstr(xml_buf, "transfer=\"bt_2100_pq\"") != NULL);
    assert(strstr(xml_buf, "max_cll=\"1000\"") != NULL);
    assert(strstr(xml_buf, "max_fall=\"400\"") != NULL);

    avx_color_info_t parsed_pq = {0};
    parse_ok = avx_hdr_parse_color_info(xml_buf, &parsed_pq);
    assert(parse_ok);
    assert(parsed_pq.transfer == AVX_COLOR_TRANSFER_BT2100_PQ);
    assert(parsed_pq.max_cll == 1000);
    assert(parsed_pq.max_fall == 400);
    assert(avx_hdr_is_hdr(&parsed_pq));

    /* Test C: Frame helper methods */
    oavx_video_frame_t test_vframe = {0};
    char frame_xml[256];
    bool set_ok = avx_hdr_frame_set_color_info(&test_vframe, &hlg_info, frame_xml, sizeof(frame_xml));
    assert(set_ok);
    assert(test_vframe.metadata == frame_xml);

    avx_color_info_t query_info = {0};
    bool get_ok = avx_hdr_frame_get_color_info(&test_vframe, &query_info);
    assert(get_ok);
    assert(query_info.transfer == AVX_COLOR_TRANSFER_BT2100_HLG);
    printf("  [+] Video frame HDR metadata setters & getters: PASSED\n");

    /* --------------------------------------------------------------------- */
    /* 3. Test P216 16-bit Color Conversions                                 */
    /* --------------------------------------------------------------------- */
    printf("[*] 3. Testing 16-bit P216 Semi-Planar Conversions...\n");

    size_t uyvy_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 2;
    size_t p216_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 4;
    size_t rgba_size = (size_t)TEST_WIDTH * TEST_HEIGHT * 4;

    uint8_t* orig_uyvy = (uint8_t*)malloc(uyvy_size);
    uint8_t* p216_buf  = (uint8_t*)malloc(p216_size);
    uint8_t* back_uyvy = (uint8_t*)malloc(uyvy_size);
    uint8_t* rgba_buf  = (uint8_t*)malloc(rgba_size);
    assert(orig_uyvy && p216_buf && back_uyvy && rgba_buf);

    /* Fill original UYVY gradient */
    for (size_t i = 0; i < uyvy_size; ++i) {
        orig_uyvy[i] = (uint8_t)(i & 0xFF);
    }

    /* Convert UYVY -> P216 */
    avx_color_uyvy_to_p216(orig_uyvy, p216_buf, TEST_WIDTH, TEST_HEIGHT);

    /* Verify P216 structure */
    const uint16_t* py = (const uint16_t*)p216_buf;
    const uint16_t* puv = (const uint16_t*)(p216_buf + (size_t)TEST_WIDTH * TEST_HEIGHT * 2);
    /* In orig_uyvy: byte 0 = U, byte 1 = Y0, byte 2 = V, byte 3 = Y1 */
    uint16_t expected_y0 = (uint16_t)((orig_uyvy[1] << 8) | orig_uyvy[1]);
    uint16_t expected_u0 = (uint16_t)((orig_uyvy[0] << 8) | orig_uyvy[0]);
    assert(py[0] == expected_y0);
    assert(puv[0] == expected_u0);

    /* Convert P216 back to UYVY */
    avx_color_p216_to_uyvy(p216_buf, back_uyvy, TEST_WIDTH, TEST_HEIGHT);

    /* Assert 100% roundtrip bitwise parity */
    assert(memcmp(orig_uyvy, back_uyvy, uyvy_size) == 0);
    printf("  [+] UYVY <-> P216 Roundtrip Bitwise Parity: 100%% EXACT\n");

    /* Convert P216 -> RGBA */
    avx_color_p216_to_rgba(p216_buf, rgba_buf, TEST_WIDTH, TEST_HEIGHT);
    /* Verify alpha channel is 255 */
    for (size_t i = 0; i < (size_t)TEST_WIDTH * TEST_HEIGHT; ++i) {
        assert(rgba_buf[4 * i + 3] == 255);
    }
    printf("  [+] P216 -> RGBA Conversion: PASSED\n");

    /* --------------------------------------------------------------------- */
    /* 4. End-to-End P216 & HDR Network Loopback Streaming                   */
    /* --------------------------------------------------------------------- */
    printf("[*] 4. Testing End-to-End P216 & HDR Stream (Sender -> Receiver)...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t send_create = {
        .name = "OPENAVX_HDR_SOURCE",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);

    sender_thread_arg_t s_arg = {
        .p_send = p_send,
        .running = true,
        .p216_data = p216_buf,
        .hdr_metadata = xml_buf
    };
    avx_thread_t s_thread;
    avx_thread_create(&s_thread, sender_thread_func, &s_arg);

    avx_sleep_ms(50);

    /* Mode A: Native P216 ingestion (compressed_v1 / raw) */
    printf("  [*] Connecting Receiver in Native 16-bit P216 Mode...\n");
    oavx_stream_config_t recv_create = {
        .name = "P216_Client_Native",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_COMPRESSED_V1,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    oavx_video_frame_t vf = {0};
    oavx_audio_frame_t af = {0};
    oavx_metadata_frame_t mf = {0};

    bool got_p216 = false;
    int attempts = 0;
    while (attempts++ < 40 && !got_p216) {
        oavx_frame_type_e ftype = oavx_stream_receive(p_recv, &vf, &af, &mf, 200);
        if (ftype == OAVX_FRAME_TYPE_VIDEO) {
            got_p216 = true;
            printf("    [+] Captured P216 Video Frame:\n");
            printf("        - Resolution: %ux%u\n", vf.width, vf.height);
            printf("        - FourCC:     0x%08X (P216: 0x%08X)\n", vf.fourcc, OAVX_FOURCC_VIDEO_TYPE_P216);
            printf("        - Metadata:   '%s'\n", vf.metadata ? vf.metadata : "(none)");

            assert(vf.width == TEST_WIDTH);
            assert(vf.height == TEST_HEIGHT);
            assert(vf.fourcc == OAVX_FOURCC_VIDEO_TYPE_P216);
            assert(vf.metadata != NULL);

            avx_color_info_t rx_info = {0};
            bool parsed_rx = avx_hdr_frame_get_color_info(&vf, &rx_info);
            assert(parsed_rx);
            assert(rx_info.transfer == AVX_COLOR_TRANSFER_BT2100_PQ);
            assert(rx_info.max_cll == 1000);
            printf("    [+] Frame-attached HDR metadata successfully recovered!\n");

            oavx_stream_release_video(p_recv, &vf);
        }
    }
    assert(got_p216);
    oavx_stream_close(p_recv);

    /* Mode B: Auto-downconversion of P216 to standard UYVY for legacy SDR displays */
    printf("  [*] Connecting Receiver in Auto-Conversion Mode (P216 -> UYVY)...\n");
    recv_create.color_format = OAVX_COLOR_FORMAT_UYVY_RGBA;
    recv_create.name = "P216_Client_Converted";
    p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    bool got_uyvy = false;
    attempts = 0;
    while (attempts++ < 40 && !got_uyvy) {
        oavx_frame_type_e ftype = oavx_stream_receive(p_recv, &vf, &af, &mf, 200);
        if (ftype == OAVX_FRAME_TYPE_VIDEO) {
            got_uyvy = true;
            printf("    [+] Captured Auto-Converted UYVY Frame:\n");
            printf("        - FourCC: 0x%08X (UYVY: 0x%08X)\n", vf.fourcc, OAVX_FOURCC_VIDEO_TYPE_UYVY);
            printf("        - Stride: %zu bytes\n", vf.strides[0]);
            assert(vf.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY);
            assert(vf.strides[0] == (size_t)TEST_WIDTH * 2);

            oavx_stream_release_video(p_recv, &vf);
        }
    }
    assert(got_uyvy);

    /* Teardown */
    printf("[*] 5. Tearing down test session...\n");
    s_arg.running = false;
    avx_thread_join(s_thread);
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    free(orig_uyvy);
    free(p216_buf);
    free(back_uyvy);
    free(rgba_buf);

    printf("\n[PASS] openavx_hdr_test\n\n");
    return 0;
}
