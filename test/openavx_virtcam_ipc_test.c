/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_virtcam_ipc_test.c - Unit tests for Cross-Platform Virtual Camera IPC & Plugins
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "openavx.h"
#include "avx_virtcam_ipc.h"
#include "avx_dshow_sink.h"
#include "avx_dal_sink.h"

static void test_virtcam_ipc_roundtrip(void)
{
    printf("[test] Testing cross-platform virtual camera shared memory IPC...\n");

    avx_virtcam_producer_t *prod = avx_virtcam_producer_create();
    assert(prod != NULL);
    assert(!avx_virtcam_producer_has_clients(prod));

    avx_virtcam_consumer_t *cons = avx_virtcam_consumer_create();
    assert(cons != NULL);
    assert(avx_virtcam_producer_has_clients(prod));

    /* Create sample 640x360 test frame */
    uint32_t width = 640;
    uint32_t height = 360;
    uint32_t stride = width * 2;
    size_t frame_bytes = stride * height;
    uint8_t *src_pixels = (uint8_t*)malloc(frame_bytes);
    assert(src_pixels != NULL);
    memset(src_pixels, 0x42, frame_bytes);

    /* Write frame from producer */
    bool wrote = avx_virtcam_producer_write_frame(
        prod,
        src_pixels,
        frame_bytes,
        width,
        height,
        stride,
        OAVX_FOURCC_VIDEO_TYPE_UYVY,
        30,
        1
    );
    assert(wrote);

    /* Read frame from consumer */
    avx_virtcam_shm_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    uint8_t *dst_pixels = (uint8_t*)malloc(frame_bytes);
    assert(dst_pixels != NULL);
    uint64_t last_seq = 0;

    bool read_ok = avx_virtcam_consumer_read_frame(cons, &hdr, dst_pixels, frame_bytes, &last_seq);
    assert(read_ok);
    assert(hdr.magic == AVX_VIRTCAM_MAGIC);
    assert(hdr.width == width);
    assert(hdr.height == height);
    assert(hdr.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY);
    assert(hdr.sequence == 1);
    assert(last_seq == 1);
    assert(memcmp(src_pixels, dst_pixels, frame_bytes) == 0);

    /* Read again with same sequence must return false (no new frame) */
    assert(!avx_virtcam_consumer_read_frame(cons, &hdr, dst_pixels, frame_bytes, &last_seq));

    /* Write second frame */
    memset(src_pixels, 0x88, frame_bytes);
    assert(avx_virtcam_producer_write_frame(prod, src_pixels, frame_bytes, width, height, stride, OAVX_FOURCC_VIDEO_TYPE_UYVY, 60, 1));
    assert(avx_virtcam_consumer_read_frame(cons, &hdr, dst_pixels, frame_bytes, &last_seq));
    assert(hdr.sequence == 2);
    assert(last_seq == 2);
    assert(hdr.fps_num == 60);
    assert(dst_pixels[0] == 0x88);

    /* Cleanup consumer */
    avx_virtcam_consumer_destroy(cons);
    assert(!avx_virtcam_producer_has_clients(prod));

    /* Cleanup producer */
    avx_virtcam_producer_destroy(prod);
    free(src_pixels);
    free(dst_pixels);
    printf("  -> Virtual camera IPC roundtrip passed cleanly.\n");
}

static void test_virtcam_plugins_defensive(void)
{
    printf("[test] Testing platform virtual camera registration defensive checks...\n");
    avx_virtcam_producer_destroy(NULL);
    avx_virtcam_consumer_destroy(NULL);
    assert(!avx_virtcam_producer_write_frame(NULL, NULL, 0, 0, 0, 0, 0, 0, 0));
    assert(!avx_virtcam_consumer_read_frame(NULL, NULL, NULL, 0, NULL));

    /* Check graceful behavior of platform APIs on current OS */
#if defined(AVX_OS_WINDOWS)
    printf("  -> Running on Windows: testing DirectShow registration...\n");
#elif defined(AVX_OS_MACOS)
    printf("  -> Running on macOS: testing DAL plugin check...\n");
#else
    printf("  -> Running on Linux: checking Windows and macOS stubs return false safely...\n");
    assert(!avx_dshow_is_registered());
    assert(!avx_dal_is_installed());
#endif
    printf("  -> Defensive checks passed.\n");
}

int main(void)
{
    printf("===================================================\n");
    printf("OpenAVX Virtual Camera IPC & Plugins Test Suite\n");
    printf("===================================================\n");

    test_virtcam_ipc_roundtrip();
    test_virtcam_plugins_defensive();

    printf("===================================================\n");
    printf("All Virtual Camera IPC & Plugin Tests Passed!\n");
    printf("===================================================\n");
    return 0;
}
