/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_api_v3_test.c - Native OpenAVX Modern API Verification Suite
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "openavx.h"

int main(void) {
    printf("\n=== OpenAVX Public API Tests ===\n\n");

    /* 1. Initialize Subsystem */
    printf("[*] 1. Initializing OpenAVX Context...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 2. Verify Version and CPU Support */
    printf("[*] 2. Verifying oavx_version() and oavx_is_supported_cpu()...\n");
    const char* ver = oavx_version();
    assert(ver != NULL);
    assert(oavx_is_supported_cpu());
    printf("    [+] OpenAVX version: %s (CPU supported)\n", ver);

    /* 3. Create Sender & Verify Source Information */
    printf("[*] 3. Creating Sender and verifying oavx_stream_get_endpoint()...\n");
    oavx_stream_config_t send_settings = {
        .name = "API_V3_TEST_SOURCE",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_settings);
    assert(p_send != NULL);

    const oavx_endpoint_t* p_src_info = oavx_stream_get_endpoint(p_send);
    assert(p_src_info != NULL);
    assert(p_src_info->name != NULL);
    assert(strstr(p_src_info->name, "API_V3_TEST_SOURCE") != NULL);
    assert(p_src_info->address != NULL);
    printf("    [+] Sender source info: \"%s\" @ %s\n", p_src_info->name, p_src_info->address);

    /* 4. Create Receiver & Connect */
    printf("[*] 4. Connecting Receiver to %s...\n", p_src_info->address);
    oavx_stream_config_t recv_settings = {
        .name = "TestReceiver_v3",
        .endpoint = p_src_info->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_settings);
    assert(p_recv != NULL);

    /* Allow connection establishment */
    usleep(100000); /* 100ms */
    int conns = oavx_stream_get_connection_count(p_send, 200);
    assert(conns >= 1);
    printf("    [+] Receiver connected (sender active conns: %d)\n", conns);

    /* 5. Transmit & Receive Audio Frame */
    printf("[*] 5. Transmitting Audio Frame via oavx_stream_send_audio()...\n");
    float audio_samples[2048];
    for (int i = 0; i < 2048; ++i) {
        audio_samples[i] = 0.75f;
    }

    oavx_audio_frame_t tx_audio = {
        .sample_rate = 48000,
        .channels = 2,
        .frames = 1024,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .planes = { audio_samples, audio_samples + 1024, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = 1024 * sizeof(float),
        .pts_ns = 12345678,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    oavx_stream_send_audio(p_send, &tx_audio);

    /* Capture via oavx_stream_receive */
    printf("[*] 6. Capturing Audio Frame via oavx_stream_receive()...\n");
    oavx_video_frame_t rx_video = {0};
    oavx_audio_frame_t rx_audio = {0};
    oavx_metadata_frame_t rx_meta = {0};

    bool got_audio = false;
    for (int attempts = 0; attempts < 20 && !got_audio; ++attempts) {
        oavx_frame_type_t ft = oavx_stream_receive(p_recv, &rx_video, &rx_audio, &rx_meta, 100);
        if (ft == OAVX_FRAME_TYPE_AUDIO) {
            got_audio = true;
            assert(rx_audio.sample_rate == 48000);
            assert(rx_audio.channels == 2);
            assert(rx_audio.frames == 1024);
            assert(rx_audio.planes[0] != NULL);

            float* samples = (float*)rx_audio.planes[0];
            assert(samples[0] == 0.75f);
            assert(samples[1023] == 0.75f);
            printf("    [+] Audio captured: 48kHz, 2ch, 1024 samples (verified!)\n");

            oavx_stream_release_audio(p_recv, &rx_audio);
        }
    }
    assert(got_audio);

    /* 7. Test Upstream Metadata & oavx_stream_capture_metadata() */
    printf("[*] 7. Testing Upstream Metadata via oavx_stream_capture_metadata()...\n");
    oavx_metadata_frame_t upstream_msg = {
        .length = 0,
        .pts_ns = 9999,
        .data = "<openavx_feedback status=\"online\" quality=\"100%\"/>"
    };
    int sent_meta = oavx_stream_send_metadata(p_recv, &upstream_msg);
    assert(sent_meta == 0);

    oavx_metadata_frame_t captured_upstream = {0};
    oavx_frame_type_t cap_ft = oavx_stream_capture_metadata(p_send, &captured_upstream, 500);
    assert(cap_ft == OAVX_FRAME_TYPE_METADATA);
    assert(captured_upstream.data != NULL);
    assert(strstr(captured_upstream.data, "quality=\"100%\"") != NULL);
    printf("    [+] Upstream metadata captured by sender: %s\n", captured_upstream.data);

    oavx_stream_release_metadata(p_send, &captured_upstream);

    /* 8. Verify oavx_stream_get_web_control() */
    printf("[*] 8. Verifying oavx_stream_get_web_control()...\n");
    const char* web_ctrl = oavx_stream_get_web_control(p_recv);
    (void)web_ctrl;
    printf("    [+] oavx_stream_get_web_control() executed cleanly!\n");

    /* 9. Clean up */
    printf("[*] 9. Cleaning up instances...\n");
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_api_v3_test\n\n");
    return 0;
}
