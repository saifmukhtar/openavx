/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/oavx_async_fd_test.c - Unit test for oavx_status_t and Async Socket Event-Loop Hooks
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "openavx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <unistd.h>
#else
    #include <poll.h>
    #include <unistd.h>
#endif

int main(void) {
    printf("=== OpenAVX Async Socket Hooks & Status Domain Tests ===\n\n");

    /* --------------------------------------------------------------------- */
    /* 1. Status Domain & Stringifier                                        */
    /* --------------------------------------------------------------------- */
    printf("[*] 1. Testing oavx_status_t and oavx_status_to_string()...\n");
    assert(strcmp(oavx_status_to_string(OAVX_SUCCESS), "Success") == 0);
    assert(strcmp(oavx_status_to_string(OAVX_ERR_INVALID_ARG), "Invalid argument or null pointer") == 0);
    assert(strcmp(oavx_status_to_string(OAVX_ERR_NOT_CONNECTED), "Transport not connected") == 0);
    assert(strcmp(oavx_status_to_string((oavx_status_t)999), "Unknown error code") == 0);
    printf("  [+] Status domain descriptions verified: SUCCESS=0, ERR < 0\n");

    /* --------------------------------------------------------------------- */
    /* 2. Error Code Returns on Invalid Invocations                          */
    /* --------------------------------------------------------------------- */
    printf("[*] 2. Testing error returns on null/invalid parameters...\n");
    assert(oavx_stream_send_video(NULL, NULL) == OAVX_ERR_INVALID_ARG);
    assert(oavx_stream_send_audio(NULL, NULL) == OAVX_ERR_INVALID_ARG);
    assert(oavx_stream_send_metadata(NULL, NULL) == OAVX_ERR_INVALID_ARG);
    assert(oavx_stream_get_fd(NULL) == -1);
    assert(oavx_discovery_get_fd(NULL) == -1);
    printf("  [+] Input validation and error codes verified!\n");

    /* --------------------------------------------------------------------- */
    /* 3. Initializing Context & Streams                                     */
    /* --------------------------------------------------------------------- */
    printf("[*] 3. Creating context and opening transmit stream on port 7965...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t tx_cfg = {
        .name = "ASYNC_TX_PROD",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *tx_stream = oavx_stream_open(ctx, &tx_cfg);
    assert(tx_stream != NULL);

    int tx_fd = oavx_stream_get_fd(tx_stream);
    const oavx_endpoint_t *tx_ep = oavx_stream_get_endpoint(tx_stream);
    assert(tx_ep != NULL);
    assert(tx_ep->address != NULL && tx_ep->address[0] != '\0');
    printf("  [+] Transmitter stream socket file descriptor: %d (endpoint: %s)\n", tx_fd, tx_ep->address);
    assert(tx_fd >= 0);

    /* --------------------------------------------------------------------- */
    /* 4. Receiver Socket Hook & Async poll() Event Loop Wakeup              */
    /* --------------------------------------------------------------------- */
    printf("[*] 4. Opening receiver stream connecting to %s...\n", tx_ep->address);
    oavx_stream_config_t rx_cfg = {
        .name = "ASYNC_RX_PROD",
        .endpoint = tx_ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST
    };
    oavx_stream_t *rx_stream = oavx_stream_open(ctx, &rx_cfg);
    assert(rx_stream != NULL);

    /* Wait briefly for TCP connection and handshake */
    usleep(50000);

    int rx_fd = oavx_stream_get_fd(rx_stream);
    printf("  [+] Receiver stream socket file descriptor: %d\n", rx_fd);
    assert(rx_fd >= 0);

#if !defined(_WIN32)
    /* Test non-blocking event-loop poll */
    struct pollfd pfd = {
        .fd = rx_fd,
        .events = POLLIN,
        .revents = 0
    };

    /* Initial poll: buffer is empty, should timeout immediately */
    int poll_init = poll(&pfd, 1, 0);
    assert(poll_init == 0);
    printf("  [+] Initial poll() verified: socket empty (0 events)\n");

    /* Transmit a video frame */
    uint8_t dummy_video[640 * 360 * 2];
    memset(dummy_video, 0x55, sizeof(dummy_video));
    oavx_video_frame_t tx_frame = {
        .width = 640,
        .height = 360,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { dummy_video, NULL, NULL, NULL },
        .strides = { 640 * 2, 0, 0, 0 },
        .framerate = { 60, 1 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 5000000000LL
    };

    oavx_status_t send_st = oavx_stream_send_video(tx_stream, &tx_frame);
    assert(send_st == OAVX_SUCCESS);
    printf("  [+] Transmitted video frame via oavx_stream_send_video: status = %s\n", oavx_status_to_string(send_st));

    /* Poll receiver socket: should wake up with POLLIN */
    int poll_active = poll(&pfd, 1, 1000);
    assert(poll_active > 0);
    assert(pfd.revents & POLLIN);
    printf("  [+] Async event loop woke up on POLLIN event (revents = 0x%X)!\n", pfd.revents);

    /* Non-blocking / low-timeout receive to consume ready frame */
    oavx_video_frame_t rx_frame;
    memset(&rx_frame, 0, sizeof(rx_frame));
    oavx_frame_type_t ftype = oavx_stream_receive(rx_stream, &rx_frame, NULL, NULL, 100);
    assert(ftype == OAVX_FRAME_TYPE_VIDEO);
    assert(rx_frame.width == 640 && rx_frame.height == 360);
    assert(rx_frame.pts_ns == 5000000000LL);
    printf("  [+] Successfully captured frame without blocking (640x360 @ PTS: %ld ns)\n", (long)rx_frame.pts_ns);

    oavx_stream_release_video(rx_stream, &rx_frame);
#endif

    /* --------------------------------------------------------------------- */
    /* 5. Discovery Socket Hook                                              */
    /* --------------------------------------------------------------------- */
    printf("[*] 5. Testing oavx_discovery_get_fd()...\n");
    oavx_discovery_t *disc = oavx_discovery_create(ctx, NULL);
    assert(disc != NULL);

    int disc_fd = oavx_discovery_get_fd(disc);
    printf("  [+] Discovery mDNS socket file descriptor: %d\n", disc_fd);
    assert(disc_fd >= 0);

    oavx_discovery_destroy(disc);

    /* --------------------------------------------------------------------- */
    /* 6. Clean Teardown                                                     */
    /* --------------------------------------------------------------------- */
    printf("[*] 6. Tearing down streams and destroying context...\n");
    oavx_stream_close(rx_stream);
    oavx_stream_close(tx_stream);
    oavx_context_destroy(ctx);

    printf("\n[PASS] oavx_async_fd_test\n");
    return 0;
}
