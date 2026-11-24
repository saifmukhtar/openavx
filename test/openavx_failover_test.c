/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_failover_test.c - Automatic Failover Source Switching Verification Suite
 *
 * Verifies:
 *   1. Instant Socket Disconnect Failover (0 ms trigger on remote RST/FIN).
 *   2. Stream Stall / Frame Timeout Failover (hang/deadlock detection).
 *   3. Dynamic Sender-Advised Failover (metadata pushed to downstream receivers).
 *   4. Latching Non-Reverting Mode vs. Debounced Auto-Revert (Flap Prevention).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include "openavx.h"
#include "avx_platform.h"

static void fill_test_video(oavx_video_frame_t* vframe, uint8_t fill_val) {
    memset(vframe, 0, sizeof(*vframe));
    vframe->width = 1920;
    vframe->height = 1080;
    vframe->fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY;
    vframe->framerate.num = 60000;
    vframe->framerate.den = 1000;
    vframe->aspect_ratio.num = 16;
    vframe->aspect_ratio.den = 9;
    vframe->scan_type = OAVX_SCAN_PROGRESSIVE;
    vframe->pts_ns = avx_time_ns();
    vframe->strides[0] = 1920 * 2;

    size_t sz = 1920 * 1080 * 2;
    vframe->planes[0] = (uint8_t*)malloc(sz);
    memset(vframe->planes[0], fill_val, sz);
}

int main(void) {
    printf("=== OpenAVX Automatic Failover & Source Switching Tests ===\n\n");

    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        fprintf(stderr, "[-] Failed to initialize openavx context\n");
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* 1. Instant Socket Disconnect Failover                               */
    /* ------------------------------------------------------------------ */
    printf("[*] 1. Testing Instant Socket Disconnect Failover (0 ms reaction)...\n");
    {
        oavx_stream_config_t pri_tx_cfg = {
            .direction = OAVX_DIR_TRANSMIT,
            .name = "CAM_PRIMARY_1",
            .bind_ip = "127.0.0.1"
        };
        oavx_stream_t *pri_tx = oavx_stream_open(ctx, &pri_tx_cfg);

        oavx_stream_config_t bak_tx_cfg = {
            .direction = OAVX_DIR_TRANSMIT,
            .name = "CAM_BACKUP_1",
            .bind_ip = "127.0.0.1"
        };
        oavx_stream_t *bak_tx = oavx_stream_open(ctx, &bak_tx_cfg);

        if (!pri_tx || !bak_tx) {
            fprintf(stderr, "[-] Failed to create primary or backup transmitters\n");
            return 1;
        }

        const oavx_endpoint_t *pri_ep = oavx_stream_get_endpoint(pri_tx);
        const oavx_endpoint_t *bak_ep = oavx_stream_get_endpoint(bak_tx);

        /* Receiver connecting to primary, with backup failover endpoint specified */
        oavx_stream_config_t rx_cfg = {
            .direction = OAVX_DIR_RECEIVE,
            .name = "STUDIO_VIEWER_1",
            .endpoint = pri_ep->address,
            .color_format = OAVX_COLOR_FORMAT_FASTEST
        };
        oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
        if (!rx) {
            fprintf(stderr, "[-] Failed to create receiver\n");
            return 1;
        }

        oavx_stream_set_failover(rx, bak_ep);

        /* Wait for primary connection */
        int conns = 0;
        for (int i = 0; i < 20; ++i) {
            conns = oavx_stream_get_connection_count(pri_tx, 50);
            if (conns > 0) break;
            avx_sleep_ms(20);
        }
        if (conns == 0) {
            fprintf(stderr, "[-] Receiver failed to connect to primary\n");
            return 1;
        }

        /* Transmit video frame from primary */
        oavx_video_frame_t v_pri;
        fill_test_video(&v_pri, 0x11);
        oavx_stream_send_video(pri_tx, &v_pri);

        oavx_video_frame_t v_recv;
        oavx_audio_frame_t a_recv;
        oavx_frame_type_t ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 500);
        if (ft != OAVX_FRAME_TYPE_VIDEO || v_recv.planes[0][0] != 0x11) {
            fprintf(stderr, "[-] Failed to receive primary video frame\n");
            return 1;
        }
        oavx_stream_release_video(rx, &v_recv);
        free(v_pri.planes[0]);

        if (oavx_stream_is_on_failover(rx)) {
            fprintf(stderr, "[-] Receiver erroneously reported on failover before primary drop\n");
            return 1;
        }
        printf("  [+] Captured primary frame (fill=0x11), is_on_failover=false\n");

        /* Abruptly kill primary transmitter (simulating crash/unplug) */
        printf("  [~] Killing primary transmitter to simulate sudden process crash...\n");
        oavx_stream_close(pri_tx);

        /* Transmit backup video frame */
        oavx_video_frame_t v_bak;
        fill_test_video(&v_bak, 0x22);

        /* Receiver should detect disconnect, execute failover, and connect to backup */
        bool received_bak = false;
        for (int retry = 0; retry < 60; ++retry) {
            oavx_stream_send_video(bak_tx, &v_bak);
            ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 50);
            if (ft == OAVX_FRAME_TYPE_VIDEO && v_recv.planes[0][0] == 0x22) {
                received_bak = true;
                oavx_stream_release_video(rx, &v_recv);
                break;
            }
            avx_sleep_ms(20);
        }

        if (!received_bak) {
            fprintf(stderr, "[-] Receiver failed to receive backup frame after primary disconnect\n");
            return 1;
        }

        if (!oavx_stream_is_on_failover(rx)) {
            fprintf(stderr, "[-] Receiver did not mark is_on_failover=true after cutover\n");
            return 1;
        }
        printf("  [+] Socket disconnect failover SUCCESS: Receiver cut to backup (fill=0x22, is_on_failover=true)!\n");

        free(v_bak.planes[0]);
        oavx_stream_close(rx);
        oavx_stream_close(bak_tx);
    }

    /* ------------------------------------------------------------------ */
    /* 2. Stream Stall Failover (Frame Timeout Threshold)                  */
    /* ------------------------------------------------------------------ */
    printf("\n[*] 2. Testing Stream Stall Failover (Encoder Hang with Active Socket)...\n");
    {
        oavx_stream_config_t pri_tx_cfg = {
            .direction = OAVX_DIR_TRANSMIT,
            .name = "CAM_PRIMARY_2",
            .bind_ip = "127.0.0.1"
        };
        oavx_stream_t *pri_tx = oavx_stream_open(ctx, &pri_tx_cfg);

        oavx_stream_config_t bak_tx_cfg = {
            .direction = OAVX_DIR_TRANSMIT,
            .name = "CAM_BACKUP_2",
            .bind_ip = "127.0.0.1"
        };
        oavx_stream_t *bak_tx = oavx_stream_open(ctx, &bak_tx_cfg);

        const oavx_endpoint_t *pri_ep = oavx_stream_get_endpoint(pri_tx);
        const oavx_endpoint_t *bak_ep = oavx_stream_get_endpoint(bak_tx);

        oavx_stream_config_t rx_cfg = {
            .direction = OAVX_DIR_RECEIVE,
            .name = "STUDIO_VIEWER_2",
            .endpoint = pri_ep->address,
            .color_format = OAVX_COLOR_FORMAT_FASTEST
        };
        oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);

        oavx_stream_set_failover(rx, bak_ep);
        /* Set sensitive 200 ms stall threshold for fast testing */
        oavx_stream_set_failover_timeout(rx, 200);

        /* Wait for primary connection */
        for (int i = 0; i < 20; ++i) {
            if (oavx_stream_get_connection_count(pri_tx, 50) > 0) break;
            avx_sleep_ms(20);
        }

        /* Prime stream with initial frame */
        oavx_video_frame_t v_pri;
        fill_test_video(&v_pri, 0x33);
        oavx_video_frame_t v_recv;
        oavx_audio_frame_t a_recv;
        oavx_frame_type_t ft = OAVX_FRAME_TYPE_NONE;
        for (int i = 0; i < 20; ++i) {
            oavx_stream_send_video(pri_tx, &v_pri);
            ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 100);
            if (ft == OAVX_FRAME_TYPE_VIDEO) {
                oavx_stream_release_video(rx, &v_recv);
                break;
            }
            avx_sleep_ms(20);
        }
        assert(ft == OAVX_FRAME_TYPE_VIDEO);
        free(v_pri.planes[0]);

        /* Now: Keep primary TCP socket open, but stop sending frames (stream stall) */
        printf("  [~] Freezing primary encoder (socket remains connected, no frames sent)...\n");
        avx_sleep_ms(350); /* Exceeds 200 ms stall timeout with ample safety buffer */

        /* Transmit frame on backup stream */
        oavx_video_frame_t v_bak;
        fill_test_video(&v_bak, 0x44);

        bool received_bak = false;
        for (int retry = 0; retry < 60; ++retry) {
            oavx_stream_send_video(bak_tx, &v_bak);
            ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 50);
            if (ft == OAVX_FRAME_TYPE_VIDEO && v_recv.planes[0][0] == 0x44) {
                received_bak = true;
                oavx_stream_release_video(rx, &v_recv);
                break;
            }
            avx_sleep_ms(20);
        }

        if (!received_bak) {
            fprintf(stderr, "[-] Receiver failed to trigger failover on stream stall (is_on_failover=%d, ft=%d)\n",
                    oavx_stream_is_on_failover(rx), (int)ft);
            return 1;
        }

        if (!oavx_stream_is_on_failover(rx)) {
            fprintf(stderr, "[-] Receiver did not transition to is_on_failover=true after stall\n");
            return 1;
        }
        printf("  [+] Stream stall failover SUCCESS: Receiver triggered failover after 200 ms timeout!\n");

        free(v_bak.planes[0]);
        oavx_stream_close(rx);
        oavx_stream_close(pri_tx);
        oavx_stream_close(bak_tx);
    }

    /* ------------------------------------------------------------------ */
    /* 3. Sender-Advised Dynamic Failover (Connection Metadata XML)        */
    /* ------------------------------------------------------------------ */
    printf("\n[*] 3. Testing Dynamic Sender-Advised Failover via Connection Metadata...\n");
    {
        oavx_stream_config_t pri_tx_cfg = {
            .direction = OAVX_DIR_TRANSMIT,
            .name = "CAM_PRIMARY_3",
            .bind_ip = "127.0.0.1"
        };
        oavx_stream_t *pri_tx = oavx_stream_open(ctx, &pri_tx_cfg);

        oavx_stream_config_t bak_tx_cfg = {
            .direction = OAVX_DIR_TRANSMIT,
            .name = "CAM_BACKUP_3",
            .bind_ip = "127.0.0.1"
        };
        oavx_stream_t *bak_tx = oavx_stream_open(ctx, &bak_tx_cfg);

        const oavx_endpoint_t *pri_ep = oavx_stream_get_endpoint(pri_tx);
        const oavx_endpoint_t *bak_ep = oavx_stream_get_endpoint(bak_tx);

        /* Transmitter advertises backup endpoint downstream to receivers */
        oavx_stream_set_failover(pri_tx, bak_ep);

        /* Receiver creates connection WITHOUT explicitly knowing backup URL */
        oavx_stream_config_t rx_cfg = {
            .direction = OAVX_DIR_RECEIVE,
            .name = "STUDIO_VIEWER_3",
            .endpoint = pri_ep->address,
            .color_format = OAVX_COLOR_FORMAT_FASTEST
        };
        oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);

        /* Wait for connection and metadata propagation */
        for (int i = 0; i < 20; ++i) {
            if (oavx_stream_get_connection_count(pri_tx, 50) > 0) break;
            avx_sleep_ms(20);
        }

        /* Prime stream: frame from primary */
        oavx_video_frame_t v_pri;
        fill_test_video(&v_pri, 0x55);
        oavx_video_frame_t v_recv;
        oavx_audio_frame_t a_recv;
        oavx_frame_type_t ft = OAVX_FRAME_TYPE_NONE;
        for (int i = 0; i < 20; ++i) {
            oavx_stream_send_video(pri_tx, &v_pri);
            ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 100);
            if (ft == OAVX_FRAME_TYPE_VIDEO) {
                oavx_stream_release_video(rx, &v_recv);
                break;
            }
            avx_sleep_ms(20);
        }
        assert(ft == OAVX_FRAME_TYPE_VIDEO);
        free(v_pri.planes[0]);

        /* Terminate primary stream */
        printf("  [~] Killing sender with advised failover...\n");
        oavx_stream_close(pri_tx);

        /* Stream on backup */
        oavx_video_frame_t v_bak;
        fill_test_video(&v_bak, 0x66);

        bool received_bak = false;
        for (int retry = 0; retry < 60; ++retry) {
            oavx_stream_send_video(bak_tx, &v_bak);
            ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 50);
            if (ft == OAVX_FRAME_TYPE_VIDEO && v_recv.planes[0][0] == 0x66) {
                received_bak = true;
                oavx_stream_release_video(rx, &v_recv);
                break;
            }
            avx_sleep_ms(20);
        }

        if (!received_bak) {
            fprintf(stderr, "[-] Receiver failed to failover using sender-advised metadata\n");
            return 1;
        }
        printf("  [+] Sender-advised failover SUCCESS: Receiver auto-learned failover target via XML!\n");

        free(v_bak.planes[0]);
        oavx_stream_close(rx);
        oavx_stream_close(bak_tx);
    }

    /* ------------------------------------------------------------------ */
    /* 4. Latching Policy vs. Debounced Auto-Revert                        */
    /* ------------------------------------------------------------------ */
    printf("\n[*] 4. Testing Latching Policy & Debounced Auto-Revert (Flap Prevention)...\n");
    {
        oavx_stream_config_t pri_tx_cfg = {
            .direction = OAVX_DIR_TRANSMIT,
            .name = "CAM_PRIMARY_4",
            .bind_ip = "127.0.0.1"
        };
        oavx_stream_t *pri_tx = oavx_stream_open(ctx, &pri_tx_cfg);

        oavx_stream_config_t bak_tx_cfg = {
            .direction = OAVX_DIR_TRANSMIT,
            .name = "CAM_BACKUP_4",
            .bind_ip = "127.0.0.1"
        };
        oavx_stream_t *bak_tx = oavx_stream_open(ctx, &bak_tx_cfg);

        const oavx_endpoint_t *pri_ep = oavx_stream_get_endpoint(pri_tx);
        const oavx_endpoint_t *bak_ep = oavx_stream_get_endpoint(bak_tx);

        char saved_pri_url[64];
        strncpy(saved_pri_url, pri_ep->address, sizeof(saved_pri_url) - 1);
        saved_pri_url[sizeof(saved_pri_url) - 1] = '\0';

        oavx_stream_config_t rx_cfg = {
            .direction = OAVX_DIR_RECEIVE,
            .name = "STUDIO_VIEWER_4",
            .endpoint = pri_ep->address,
            .color_format = OAVX_COLOR_FORMAT_FASTEST
        };
        oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);

        oavx_stream_set_failover(rx, bak_ep);

        /* Wait for initial primary connection */
        for (int i = 0; i < 20; ++i) {
            if (oavx_stream_get_connection_count(pri_tx, 50) > 0) break;
            avx_sleep_ms(20);
        }

        /* Initial frame from primary */
        oavx_video_frame_t v_pri;
        fill_test_video(&v_pri, 0x77);
        oavx_stream_send_video(pri_tx, &v_pri);

        oavx_video_frame_t v_recv;
        oavx_audio_frame_t a_recv;
        oavx_frame_type_t ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 500);
        if (ft == OAVX_FRAME_TYPE_VIDEO) oavx_stream_release_video(rx, &v_recv);
        free(v_pri.planes[0]);

        /* Trigger failover by closing primary */
        oavx_stream_close(pri_tx);

        oavx_video_frame_t v_bak;
        fill_test_video(&v_bak, 0x88);

        for (int retry = 0; retry < 60; ++retry) {
            oavx_stream_send_video(bak_tx, &v_bak);
            ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 50);
            if (ft == OAVX_FRAME_TYPE_VIDEO) {
                oavx_stream_release_video(rx, &v_recv);
                break;
            }
            avx_sleep_ms(20);
        }

        if (!oavx_stream_is_on_failover(rx)) {
            fprintf(stderr, "[-] Expected receiver to be on failover\n");
            return 1;
        }

        /* 4A. Verify LATCHING: Re-open Primary transmitter on the same port/endpoint, verify receiver stays on backup */
        pri_tx = oavx_stream_open(ctx, &pri_tx_cfg);
        fill_test_video(&v_pri, 0x99);
        oavx_stream_send_video(pri_tx, &v_pri);
        oavx_stream_send_video(bak_tx, &v_bak);

        ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 100);
        if (ft == OAVX_FRAME_TYPE_VIDEO) oavx_stream_release_video(rx, &v_recv);

        if (!oavx_stream_is_on_failover(rx)) {
            fprintf(stderr, "[-] Latching mode failed: receiver switched prematurely without auto_revert enabled\n");
            return 1;
        }
        printf("  [+] Latching Policy verified: Receiver remains locked on backup despite primary recovery.\n");

        /* 4B. Enable Auto-Revert with short 200 ms debounce window */
        printf("  [~] Enabling auto-revert (200 ms hysteresis hold window)...\n");
        oavx_stream_set_auto_revert(rx, true, 200);

        /* Wait for debounce hold window to elapse with stable primary */
        bool reverted = false;
        for (int i = 0; i < 40; ++i) {
            oavx_stream_send_video(pri_tx, &v_pri);
            oavx_stream_send_video(bak_tx, &v_bak);
            ft = oavx_stream_receive(rx, &v_recv, &a_recv, NULL, 50);
            if (ft == OAVX_FRAME_TYPE_VIDEO) {
                if (!oavx_stream_is_on_failover(rx)) {
                    reverted = true;
                    oavx_stream_release_video(rx, &v_recv);
                    break;
                }
                oavx_stream_release_video(rx, &v_recv);
            }
            avx_sleep_ms(50);
        }

        if (!reverted) {
            fprintf(stderr, "[-] Auto-revert failed to cut back to primary after debounce window\n");
            return 1;
        }
        printf("  [+] Debounced Auto-Revert SUCCESS: Receiver returned cleanly to primary after stability hold!\n");

        free(v_pri.planes[0]);
        free(v_bak.planes[0]);
        oavx_stream_close(rx);
        oavx_stream_close(pri_tx);
        oavx_stream_close(bak_tx);
    }

    oavx_context_destroy(ctx);
    printf("\n[PASS] openavx_failover_test\n");
    return 0;
}
