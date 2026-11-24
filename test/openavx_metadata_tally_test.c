/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_metadata_tally_test.c - Automated Bidirectional Metadata & Tally Verification (oavx_*)
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
    printf(" OpenAVX Bidirectional Metadata & Real-Time Tally Test (oavx_*)\n");
    printf("=======================================================\n\n");

    /* 1. Initialize openavx context */
    printf("[*] 1. Initializing OpenAVX Context Engine...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 2. Create Sender */
    printf("[*] 2. Spawning Sender \"STUDIO_CAMERA_TALLY_1\" on port 7960...\n");
    oavx_stream_config_t send_create = {
        .name = "STUDIO_CAMERA_TALLY_1",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);

    /* 3. Add Connection Metadata on Sender */
    printf("[*] 3. Registering Sender Connection Metadata...\n");
    oavx_metadata_frame_t conn_meta = {
        .length = 0,
        .pts_ns = 100,
        .data = "<openavx_device_info vendor=\"openavx\" model=\"StudioCam4K\" serial=\"SN12345\"/>\n"
    };
    oavx_stream_add_connection_metadata(p_send, &conn_meta);

    /* 4. Connect Receiver */
    printf("[*] 4. Connecting Receiver to 127.0.0.1:7960...\n");
    oavx_stream_config_t recv_create = {
        .name = "Switcher_Monitor_1",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    /* 5. Capture Connection Metadata on Receiver */
    printf("[*] 5. Capturing Connection Metadata on Receiver via oavx_stream_receive...\n");
    oavx_video_frame_t v_frame = {0};
    oavx_audio_frame_t a_frame = {0};
    oavx_metadata_frame_t m_frame = {0};

    bool meta_received = false;
    for (int attempts = 0; attempts < 20 && !meta_received; ++attempts) {
        oavx_frame_type_t ftype = oavx_stream_receive(p_recv, &v_frame, &a_frame, &m_frame, 200);
        if (ftype == OAVX_FRAME_TYPE_METADATA) {
            meta_received = true;
            printf("    [+] Captured Metadata Frame (%zu bytes):\n", m_frame.length);
            printf("        XML: %s", m_frame.data);
            assert(strstr(m_frame.data, "StudioCam4K") != NULL);
            oavx_stream_release_metadata(p_recv, &m_frame);
        }
    }
    assert(meta_received);

    /* 6. Test Tally State: Set Program = TRUE, Preview = FALSE (Camera is Live on Air) */
    printf("[*] 6. Switcher sets Tally: PROGRAM = TRUE, PREVIEW = FALSE (Live On-Air)...\n");
    oavx_tally_t tally_live = {
        .flags = OAVX_TALLY_PROGRAM
    };
    bool tally_set_ok = oavx_stream_set_tally(p_recv, &tally_live);
    assert(tally_set_ok);

    /* Allow network message propagation */
    usleep(50000);

    oavx_tally_t query_tally = {0};
    bool query_ok = oavx_stream_get_tally(p_send, &query_tally, 200);
    assert(query_ok);
    printf("    [+] Sender queried tally status: flags=0x%x (program=%d, preview=%d)\n",
           query_tally.flags, (query_tally.flags & OAVX_TALLY_PROGRAM) != 0, (query_tally.flags & OAVX_TALLY_PREVIEW) != 0);
    assert((query_tally.flags & OAVX_TALLY_PROGRAM) != 0);
    assert((query_tally.flags & OAVX_TALLY_PREVIEW) == 0);

    /* 7. Test Tally State: Set Program = FALSE, Preview = TRUE (Camera on Preview Bus) */
    printf("[*] 7. Switcher sets Tally: PROGRAM = FALSE, PREVIEW = TRUE (Camera Preview)...\n");
    oavx_tally_t tally_prev = {
        .flags = OAVX_TALLY_PREVIEW
    };
    tally_set_ok = oavx_stream_set_tally(p_recv, &tally_prev);
    assert(tally_set_ok);

    usleep(50000);

    query_ok = oavx_stream_get_tally(p_send, &query_tally, 200);
    assert(query_ok);
    printf("    [+] Sender queried tally status: flags=0x%x (program=%d, preview=%d)\n",
           query_tally.flags, (query_tally.flags & OAVX_TALLY_PROGRAM) != 0, (query_tally.flags & OAVX_TALLY_PREVIEW) != 0);
    assert((query_tally.flags & OAVX_TALLY_PROGRAM) == 0);
    assert((query_tally.flags & OAVX_TALLY_PREVIEW) != 0);

    /* 7b. Test Recording & Dual State: Set Program = TRUE, Recording = TRUE */
    printf("[*] 7b. Switcher sets Tally: PROGRAM = TRUE, RECORDING = TRUE (Live On-Air + ISO Recording)...\n");
    oavx_tally_t tally_prog_rec = {
        .flags = OAVX_TALLY_PROGRAM | OAVX_TALLY_RECORDING
    };
    tally_set_ok = oavx_stream_set_tally(p_recv, &tally_prog_rec);
    assert(tally_set_ok);

    usleep(50000);

    query_ok = oavx_stream_get_tally(p_send, &query_tally, 200);
    assert(query_ok);
    printf("    [+] Sender queried tally status: flags=0x%x (program=%d, recording=%d)\n",
           query_tally.flags, (query_tally.flags & OAVX_TALLY_PROGRAM) != 0, (query_tally.flags & OAVX_TALLY_RECORDING) != 0);
    assert((query_tally.flags & OAVX_TALLY_PROGRAM) != 0);
    assert((query_tally.flags & OAVX_TALLY_PREVIEW) == 0);
    assert((query_tally.flags & OAVX_TALLY_RECORDING) != 0);

    /* 8. Test Upstream Metadata: Receiver sends PTZ control command to camera */
    printf("[*] 8. Receiver sends PTZ Camera Zoom command upstream to Sender...\n");
    oavx_metadata_frame_t ptz_cmd = {
        .length = 0,
        .pts_ns = 200,
        .data = "<openavx_ptz_zoom speed=\"0.85\"/>\n"
    };
    int ptz_sent = oavx_stream_send_metadata(p_recv, &ptz_cmd);
    assert(ptz_sent >= 0);
    printf("    [+] Upstream PTZ command transmitted!\n");

    /* 9. Test Downstream Metadata: Sender sends Lens status to Receiver */
    printf("[*] 9. Sender sends Lens & Shading metadata downstream to Receiver...\n");
    oavx_metadata_frame_t lens_status = {
        .length = 0,
        .pts_ns = 300,
        .data = "<openavx_lens_status iris=\"f2.8\" focus=\"auto\" zoom=\"3.2x\"/>\n"
    };
    int lens_sent = oavx_stream_send_metadata(p_send, &lens_status);
    assert(lens_sent >= 0);

    bool lens_received = false;
    for (int attempts = 0; attempts < 20 && !lens_received; ++attempts) {
        oavx_frame_type_t ftype = oavx_stream_receive(p_recv, &v_frame, &a_frame, &m_frame, 200);
        if (ftype == OAVX_FRAME_TYPE_METADATA) {
            lens_received = true;
            printf("    [+] Captured Downstream Lens Metadata (%zu bytes):\n", m_frame.length);
            printf("        XML: %s", m_frame.data);
            assert(strstr(m_frame.data, "iris=\"f2.8\"") != NULL);
            assert(strstr(m_frame.data, "zoom=\"3.2x\"") != NULL);
            oavx_stream_release_metadata(p_recv, &m_frame);
        }
    }
    assert(lens_received);

    /* 10. Test Tally Reset (All Off) */
    printf("[*] 10. Switcher resets Tally: PROGRAM = FALSE, PREVIEW = FALSE (Off)...\n");
    oavx_tally_t tally_off = { .flags = OAVX_TALLY_NONE };
    assert(oavx_stream_set_tally(p_recv, &tally_off));

    usleep(50000);
    assert(oavx_stream_get_tally(p_send, &query_tally, 200));
    assert(query_tally.flags == OAVX_TALLY_NONE);
    printf("    [+] Tally successfully cleared (flags=0)\n");

    /* 11. Cleanup */
    printf("[*] 11. Tearing down sessions...\n");
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_metadata_tally_test\n\n");
    return 0;
}
