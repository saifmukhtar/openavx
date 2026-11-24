/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_ptz_compat_test.c
 * Comprehensive test suite for PTZ & Recording extensions
 */

#include "openavx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

static void test_ptz_and_recording_extensions(void) {
    printf("[*] 1. Testing Receiver PTZ, Remote Recording & Web Control Extensions...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* Create Sender (representing a PTZ camera) */
    oavx_stream_config_t send_create = {
        .name = "Studio_PTZ_Camera",
        .groups = NULL,
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);

    /* Create Receiver connected to sender */
    oavx_stream_config_t recv_create = {
        .name = "PTZ_Controller_Recv",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    /* Wait for connection handshake */
    for (int wait = 0; wait < 30; ++wait) {
        if (oavx_stream_get_connection_count(p_send, 50) > 0) break;
        sleep_ms(20);
    }
    assert(oavx_stream_get_connection_count(p_send, 0) > 0);

    /* Check Web Control URL */
    const char* web_url = oavx_stream_get_web_control(p_recv);
    assert(web_url != NULL);
    assert(strstr(web_url, "http://127.0.0.1") != NULL);
    printf("  [+] Web Control URL: %s\n", web_url);

    /* 1.1 Test PTZ Movement Commands */
    oavx_ptz_state_t ptz = {
        .pan = 0.35f,
        .tilt = -0.20f,
        .zoom = 0.75f,
        .focus = 0.82f
    };
    assert(oavx_stream_set_ptz(p_recv, &ptz));
    printf("  [+] PTZ Pan/Tilt and Zoom commands transmitted successfully\n");

    /* 1.2 Test Remote Recording Commands & State */
    bool is_rec = false;
    assert(oavx_stream_record_status(p_recv, &is_rec, NULL, NULL));
    assert(!is_rec);

    assert(oavx_stream_record_trigger(p_recv, "start", "take_01_scene_04.mov"));
    const char* active_file = NULL;
    assert(oavx_stream_record_status(p_recv, &is_rec, &active_file, NULL));
    assert(is_rec);
    assert(active_file != NULL);
    assert(strcmp(active_file, "take_01_scene_04.mov") == 0);
    printf("  [+] Remote Recording started: '%s'\n", active_file);

    assert(oavx_stream_record_set_audio_level(p_recv, -6.0f));
    assert(oavx_stream_record_trigger(p_recv, "stop", NULL));
    assert(oavx_stream_record_status(p_recv, &is_rec, NULL, NULL));
    assert(!is_rec);
    printf("  [+] Remote Recording stopped successfully\n");

    /* Allow reverse socket pipeline to flush */
    sleep_ms(50);

    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);
}

int main(void) {
    printf("=== OpenAVX PTZ & Recording Extension Tests ===\n");

    test_ptz_and_recording_extensions();

    printf("\n[PASS] openavx_ptz_compat_test\n");
    return 0;
}
