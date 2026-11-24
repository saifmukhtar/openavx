/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_routing_test.c - Automated Virtual Matrix Router Verification
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

#include "openavx.h"

int main(void) {
    printf("\n=======================================================\n");
    printf(" OpenAVX Virtual Matrix Router Subsystem Test\n");
    printf("=======================================================\n\n");

    /* 1. Initialize openavx */
    printf("[*] 1. Initializing OpenAVX...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 2. Create Matrix Router destination */
    printf("[*] 2. Spawning Matrix Router destination \"STUDIO_PGM_ROUTER\"...\n");
    oavx_router_t *p_router = oavx_router_create(ctx, "STUDIO_PGM_ROUTER", "public");
    assert(p_router != NULL);

    oavx_endpoint_t router_source = oavx_router_get_source(p_router);
    printf("    [+] Router listening on URL: %s (Source: %s)\n",
           router_source.address, router_source.name);
    assert(router_source.address != NULL);

    /* 3. Create two independent camera senders */
    printf("[*] 3. Creating upstream sources: Camera 1 and Camera 2...\n");
    oavx_stream_config_t cam1_settings = {
        .name = "STUDIO_CAM_1",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_cam1 = oavx_stream_open(ctx, &cam1_settings);
    assert(p_cam1 != NULL);

    oavx_stream_config_t cam2_settings = {
        .name = "STUDIO_CAM_2",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_cam2 = oavx_stream_open(ctx, &cam2_settings);
    assert(p_cam2 != NULL);

    const oavx_endpoint_t *src_cam1 = oavx_stream_get_endpoint(p_cam1);
    const oavx_endpoint_t *src_cam2 = oavx_stream_get_endpoint(p_cam2);
    assert(src_cam1 != NULL && src_cam2 != NULL);

    printf("    [+] Camera 1 on %s\n", src_cam1->address);
    printf("    [+] Camera 2 on %s\n", src_cam2->address);

    /* Allocate video frame buffers */
    int width = 320, height = 180;
    size_t frame_bytes = (size_t)width * height * 2; /* UYVY */
    uint8_t *cam1_frame_data = (uint8_t*)malloc(frame_bytes);
    uint8_t *cam2_frame_data = (uint8_t*)malloc(frame_bytes);
    assert(cam1_frame_data && cam2_frame_data);
    oavx_frame_type_e ftype = OAVX_FRAME_TYPE_NONE;

    memset(cam1_frame_data, 0x11, frame_bytes);
    memset(cam2_frame_data, 0x22, frame_bytes);

    oavx_video_frame_t vf_cam1 = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { cam1_frame_data, NULL, NULL, NULL },
        .strides = { (size_t)width * 2, 0, 0, 0 },
        .framerate = { 60, 1 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 1001,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    oavx_video_frame_t vf_cam2 = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { cam2_frame_data, NULL, NULL, NULL },
        .strides = { (size_t)width * 2, 0, 0, 0 },
        .framerate = { 60, 1 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 2002,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* 4. Connect downstream receiver to the Matrix Router */
    printf("[*] 4. Connecting downstream receiver to Matrix Router (\"%s\")...\n", router_source.address);
    oavx_stream_config_t recv_create = {
        .endpoint = router_source.address,
        .name = "Master_Control_Monitor",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_FASTEST,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_downstream_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_downstream_recv != NULL);
    usleep(50000); /* 50ms for TCP handshake */

    /* 5. Route Camera 1 through Matrix Router */
    printf("[*] 5. Routing Camera 1 (\"%s\") to Router output...\n", src_cam1->name);
    bool route_ok = oavx_router_route(p_router, src_cam1);
    assert(route_ok);
    for (int w = 0; w < 50 && oavx_stream_get_connection_count(p_cam1, 10) == 0; ++w) usleep(20000);

    /* Capture forwarded frame on downstream receiver */
    oavx_video_frame_t rx_frame;
    bool got_routed_cam1 = false;
    for (int retry = 0; retry < 30 && !got_routed_cam1; ++retry) {
        oavx_stream_send_video(p_cam1, &vf_cam1);
        oavx_frame_type_e ftype_c = oavx_stream_receive(p_downstream_recv, &rx_frame, NULL, NULL, 100);
        if (ftype_c == OAVX_FRAME_TYPE_VIDEO) {
            got_routed_cam1 = true;
            assert(rx_frame.pts_ns == 1001);
            assert(rx_frame.planes[0] != NULL);
            assert(rx_frame.planes[0][0] == 0x11);
            printf("    [+] Downstream receiver captured Camera 1 feed! (PTS: %ld, Signature: 0x%02X)\n",
                   (long)rx_frame.pts_ns, rx_frame.planes[0][0]);
            oavx_stream_release_video(p_downstream_recv, &rx_frame);
        }
    }
    assert(got_routed_cam1);

    /* 6. Verify Bidirectional Tally Migration */
    printf("[*] 6. Testing Bidirectional Tally synchronization through Router...\n");
    oavx_tally_t tally_pgm = { .flags = OAVX_TALLY_PROGRAM };
    oavx_stream_set_tally(p_downstream_recv, &tally_pgm);

    oavx_tally_t cam1_tally = {0}, cam2_tally = {0};
    for (int t = 0; t < 50 && (!(cam1_tally.flags & OAVX_TALLY_PROGRAM) || (cam2_tally.flags & OAVX_TALLY_PROGRAM)); ++t) {
        usleep(20000);
        oavx_stream_get_tally(p_cam1, &cam1_tally, 50);
        oavx_stream_get_tally(p_cam2, &cam2_tally, 50);
    }

    printf("    [+] Camera 1 Tally: flags=0x%x (Expected: 0x1)\n", cam1_tally.flags);
    printf("    [+] Camera 2 Tally: flags=0x%x (Expected: 0x0)\n", cam2_tally.flags);
    assert((cam1_tally.flags & OAVX_TALLY_PROGRAM) != 0);
    assert((cam2_tally.flags & OAVX_TALLY_PROGRAM) == 0);

    /* 7. Seamless Matrix Switch to Camera 2 */
    printf("[*] 7. Matrix switching Router from Camera 1 to Camera 2...\n");
    route_ok = oavx_router_route(p_router, src_cam2);
    assert(route_ok);
    for (int w = 0; w < 50 && oavx_stream_get_connection_count(p_cam2, 10) == 0; ++w) usleep(20000);

    /* Verify Tally automatically migrated from Camera 1 to Camera 2! */
    for (int t = 0; t < 50 && ((cam1_tally.flags & OAVX_TALLY_PROGRAM) || !(cam2_tally.flags & OAVX_TALLY_PROGRAM)); ++t) {
        usleep(20000);
        oavx_stream_get_tally(p_cam1, &cam1_tally, 50);
        oavx_stream_get_tally(p_cam2, &cam2_tally, 50);
    }

    printf("    [+] Tally post-switch: Cam 1 flags=0x%x, Cam 2 flags=0x%x\n",
           cam1_tally.flags, cam2_tally.flags);
    assert((cam1_tally.flags & OAVX_TALLY_PROGRAM) == 0); /* Camera 1 tally turned off! */
    assert((cam2_tally.flags & OAVX_TALLY_PROGRAM) != 0);  /* Camera 2 tally turned on! */

    /* Downstream receiver captures frame WITHOUT reconnecting */
    bool got_routed_cam2 = false;
    for (int retry = 0; retry < 30 && !got_routed_cam2; ++retry) {
        oavx_stream_send_video(p_cam2, &vf_cam2);
        oavx_frame_type_e ftype_c = oavx_stream_receive(p_downstream_recv, &rx_frame, NULL, NULL, 100);
        if (ftype_c == OAVX_FRAME_TYPE_VIDEO) {
            got_routed_cam2 = true;
            assert(rx_frame.pts_ns == 2002);
            assert(rx_frame.planes[0][0] == 0x22);
            printf("    [+] Downstream receiver captured Camera 2 feed seamlessly! (PTS: %ld, Signature: 0x%02X)\n",
                   (long)rx_frame.pts_ns, rx_frame.planes[0][0]);
            oavx_stream_release_video(p_downstream_recv, &rx_frame);
        }
    }
    assert(got_routed_cam2);

    /* 8. Clear Routing */
    printf("[*] 8. Clearing Router output via oavx_router_clear...\n");
    bool clear_ok = oavx_router_clear(p_router);
    assert(clear_ok);
    usleep(50000);

    /* Camera 2 tally should now be off */
    oavx_stream_get_tally(p_cam2, &cam2_tally, 200);
    assert((cam2_tally.flags & OAVX_TALLY_PROGRAM) == 0);
    printf("    [+] Routing cleared, Camera 2 tally light extinguished.\n");

    /* Downstream receiver is still connected (returns none on timeout, not error) */
    ftype = oavx_stream_receive(p_downstream_recv, &rx_frame, NULL, NULL, 100);
    assert(ftype == OAVX_FRAME_TYPE_NONE);
    printf("    [+] Downstream receiver remains connected in idle state.\n");

    /* 9. Teardown */
    printf("[*] 9. Tearing down Matrix Router and instances...\n");
    oavx_stream_close(p_downstream_recv);
    oavx_router_destroy(p_router);
    oavx_stream_close(p_cam1);
    oavx_stream_close(p_cam2);
    free(cam1_frame_data);
    free(cam2_frame_data);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_routing_test\n\n");
    return 0;
}
