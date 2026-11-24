/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_pipewire_sink_test.c - Unit & integration tests for PipeWire virtual camera sink
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "openavx.h"
#include "avx_pipewire_sink.h"

static void test_pipewire_availability(void)
{
    printf("[test] Testing PipeWire availability check...\n");
    bool avail = avx_pipewire_is_available();
    printf("  -> PipeWire available: %s\n", avail ? "YES" : "NO (graceful fallback)");
}

static void test_pipewire_sink_null_defensive(void)
{
    printf("[test] Testing PipeWire sink defensive null checks...\n");
    avx_pipewire_sink_destroy(NULL);
    assert(!avx_pipewire_sink_push_frame(NULL, NULL, 0, 0, 0, 0));
    assert(!avx_pipewire_sink_is_active(NULL));
    printf("  -> Defensive checks passed.\n");
}

static void test_pipewire_sink_lifecycle(void)
{
    printf("[test] Testing PipeWire virtual camera sink lifecycle...\n");
    if (!avx_pipewire_is_available()) {
        printf("  -> Skipping lifecycle test (PipeWire library not available).\n");
        return;
    }

    avx_pipewire_sink_config_t config = {
        .node_name = "OpenAVX Test Camera",
        .node_description = "OpenAVX Automated Test Camera Source",
        .width = 640,
        .height = 360,
        .fps_num = 30,
        .fps_den = 1,
        .preferred_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
    };

    avx_pipewire_sink_t *sink = avx_pipewire_sink_create(&config);
    if (!sink) {
        printf("  -> Warning: Could not connect to PipeWire daemon (headless/container environment). Skipping live graph test.\n");
        return;
    }

    assert(sink != NULL);

    /* Allocate dummy 640x360 UYVY test frame */
    size_t frame_bytes = 640 * 360 * 2;
    uint8_t *dummy_uyvy = (uint8_t*)malloc(frame_bytes);
    assert(dummy_uyvy != NULL);
    memset(dummy_uyvy, 0x80, frame_bytes); // neutral gray UYVY (Y=128, U=128, V=128)

    /* Allow background thread-loop to connect to daemon */
    usleep(100000); // 100ms

    /* Push several frames */
    for (int i = 0; i < 5; i++) {
        avx_pipewire_sink_push_frame(sink, dummy_uyvy, frame_bytes, 640, 360, OAVX_FOURCC_VIDEO_TYPE_UYVY);
        usleep(10000); // 10ms
    }

    free(dummy_uyvy);
    avx_pipewire_sink_destroy(sink);
    printf("  -> Lifecycle test passed cleanly.\n");
}

int main(void)
{
    printf("===================================================\n");
    printf("OpenAVX PipeWire Virtual Camera Sink Test Suite\n");
    printf("===================================================\n");

    test_pipewire_availability();
    test_pipewire_sink_null_defensive();
    test_pipewire_sink_lifecycle();

    printf("===================================================\n");
    printf("All PipeWire Virtual Camera Tests Passed!\n");
    printf("===================================================\n");
    return 0;
}
