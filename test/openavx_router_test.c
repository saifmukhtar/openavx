/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * openavx_router_test.c - Automated Virtual Matrix Router Integration Verification
 */

#include "openavx.h"
#include "avx_routing.h"
#include "avx_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

static inline void sleep_ms(int ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

int main(void) {
    printf("=== OpenAVX Virtual Matrix Router Tests ===\n\n");

    /* 1. Initialize openavx context */
    printf("[*] 1. Initializing OpenAVX Context Engine...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 2. Create Matrix Router */
    printf("[*] 2. Creating Virtual Matrix Router destination 'MATRIX_PGM_ROUTER'...\n");
    oavx_router_t *p_router = oavx_router_create(ctx, "MATRIX_PGM_ROUTER", "public");
    assert(p_router != NULL);

    oavx_endpoint_t router_adv = oavx_router_get_source(p_router);
    printf("    [+] Router online: %s @ %s\n", router_adv.name, router_adv.address);
    assert(router_adv.address != NULL);

    /* 3. Create Upstream Camera Senders */
    printf("[*] 3. Creating upstream feeds: 'CAM_ALPHA' and 'CAM_BETA'...\n");
    oavx_stream_config_t cam1_set = { .name = "CAM_ALPHA", .groups = "public", .direction = OAVX_DIR_TRANSMIT, .clock_sync = true };
    oavx_stream_config_t cam2_set = { .name = "CAM_BETA",  .groups = "public", .direction = OAVX_DIR_TRANSMIT, .clock_sync = true };

    oavx_stream_t *p_cam1 = oavx_stream_open(ctx, &cam1_set);
    oavx_stream_t *p_cam2 = oavx_stream_open(ctx, &cam2_set);
    assert(p_cam1 && p_cam2);

    const oavx_endpoint_t *ep1 = oavx_stream_get_endpoint(p_cam1);
    const oavx_endpoint_t *ep2 = oavx_stream_get_endpoint(p_cam2);

    oavx_endpoint_t src_cam1 = { .name = "CAM_ALPHA", .address = ep1 ? ep1->address : "127.0.0.1:7960" };
    oavx_endpoint_t src_cam2 = { .name = "CAM_BETA",  .address = ep2 ? ep2->address : "127.0.0.1:7961" };

    /* 4. Connect Receiver to Router */
    printf("[*] 4. Connecting Receiver to Router 'MATRIX_PGM_ROUTER'...\n");
    oavx_stream_config_t recv_settings = {
        .name = "Matrix PGM Monitor",
        .endpoint = router_adv.address,
        .color_format = OAVX_COLOR_FORMAT_BEST,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .direction = OAVX_DIR_RECEIVE
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_settings);
    assert(p_recv != NULL);
    sleep_ms(150);

    /* Allocate test frames */
    const int w = 320, h = 180;
    const size_t sz = (size_t)w * h * 2;
    uint8_t* f1 = (uint8_t*)malloc(sz);
    uint8_t* f2 = (uint8_t*)malloc(sz);
    assert(f1 && f2);
    memset(f1, 0x33, sz); /* CAM_ALPHA signature */
    memset(f2, 0x77, sz); /* CAM_BETA signature */

    oavx_video_frame_t vf1 = {
        .width = w, .height = h, .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { f1, NULL, NULL, NULL },
        .strides = { (size_t)w * 2, 0, 0, 0 },
        .framerate = { 30000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 0, .dts_ns = 0,
        .metadata = NULL, .metadata_len = 0,
        .user_ctx = NULL
    };
    oavx_video_frame_t vf2 = vf1;
    vf2.planes[0] = f2;

    /* 5. Route CAM_ALPHA and verify through router */
    printf("[*] 5. Switching matrix route to CAM_ALPHA (0x33 signature)...\n");
    bool route_ok = oavx_router_route(p_router, &src_cam1);
    assert(route_ok);
    for (int wp = 0; wp < 50 && oavx_stream_get_connection_count(p_cam1, 10) == 0; ++wp) sleep_ms(20);

    /* Pump frames into CAM_ALPHA */
    for (int i = 0; i < 10; ++i) {
        oavx_stream_send_video(p_cam1, &vf1);
        sleep_ms(33);
    }

    oavx_video_frame_t cap_v = {0};
    oavx_audio_frame_t cap_a = {0};
    oavx_metadata_frame_t cap_m = {0};

    bool got_cam1 = false;
    for (int t = 0; t < 20 && !got_cam1; ++t) {
        if (oavx_stream_receive(p_recv, &cap_v, &cap_a, &cap_m, 100) == OAVX_FRAME_TYPE_VIDEO) {
            got_cam1 = true;
            assert(cap_v.planes[0][0] == 0x33);
            printf("    [+] Router output verified for CAM_ALPHA (0x33 matched)!\n");
            oavx_stream_release_video(p_recv, &cap_v);
        }
    }
    assert(got_cam1);

    /* 6. Dynamic Switch to CAM_BETA and verify seamless transition */
    printf("[*] 6. Dynamically switching route to CAM_BETA (0x77 signature)...\n");
    route_ok = oavx_router_route(p_router, &src_cam2);
    assert(route_ok);
    for (int wp = 0; wp < 50 && oavx_stream_get_connection_count(p_cam2, 10) == 0; ++wp) sleep_ms(20);

    /* Pump frames into CAM_BETA */
    for (int i = 0; i < 10; ++i) {
        oavx_stream_send_video(p_cam2, &vf2);
        sleep_ms(33);
    }

    bool got_cam2 = false;
    for (int t = 0; t < 20 && !got_cam2; ++t) {
        if (oavx_stream_receive(p_recv, &cap_v, &cap_a, &cap_m, 100) == OAVX_FRAME_TYPE_VIDEO) {
            if (cap_v.planes[0][0] == 0x77) {
                got_cam2 = true;
                printf("    [+] Router dynamically switched to CAM_BETA (0x77 matched)!\n");
            }
            oavx_stream_release_video(p_recv, &cap_v);
        }
    }
    assert(got_cam2);

    /* 7. Clear Route */
    printf("[*] 7. Clearing route (idle state)...\n");
    assert(oavx_router_clear(p_router));

    /* Teardown */
    oavx_stream_close(p_recv);
    oavx_router_destroy(p_router);
    oavx_stream_close(p_cam1);
    oavx_stream_close(p_cam2);
    free(f1);
    free(f2);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_router_test\n");
    return 0;
}
