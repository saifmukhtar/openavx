/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_wire_packet.c - libFuzzer Target for Wire Protocol & UDP Datagram Framing
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "openavx.h"
#include "avx_internal.h"
#include "avx_wire.h"
#include "avx_udp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size == 0) return 0;

    /* 1. Live UDP fragment reassembly and RX pipeline test over socketpair */
    int udpsv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, udpsv) == 0) {
        /* Test direct low-level UDP fragment reception */
        (void)send(udpsv[0], data, size > 2048 ? 2048 : size, 0);

        uint8_t out_buf[4096];
        size_t out_size = 0;
        uint32_t out_fourcc = 0;
        int64_t out_timecode = 0;
        (void)avx_udp_recv_frame(udpsv[1], out_buf, sizeof(out_buf),
                                 &out_size, &out_fourcc, &out_timecode, 1);

        /* Test full UDP receiver ingestion pipeline */
        (void)send(udpsv[0], data, size > 2048 ? 2048 : size, 0);

        avx_rx_config_t ucfg;
        memset(&ucfg, 0, sizeof(ucfg));
        ucfg.transport = OAVX_TRANSPORT_UDP;
        ucfg.color_format = (oavx_color_format_e)(data[0] % 7);

        avx_rx_instance_t urx = avx_rx_create(&ucfg);
        if (urx) {
            avx_recv_ctx_t *uslot = (avx_recv_ctx_t*)urx;
            avx_mutex_lock(&uslot->lock);
            if (uslot->udp_sock != AVX_INVALID_SOCKET) {
                avx_socket_close(uslot->udp_sock);
            }
            uslot->udp_sock = udpsv[1];
            uslot->connected = true;
            uslot->offline_mode = true;
            avx_mutex_unlock(&uslot->lock);

            oavx_video_frame_t uvframe = {0};
            oavx_audio_frame_t uaframe = {0};
            oavx_metadata_frame_t umframe = {0};
            oavx_frame_type_t uft = avx_rx_receive(urx, &uvframe, &uaframe, &umframe, 1);
            if (uft == OAVX_FRAME_TYPE_VIDEO) {
                avx_rx_free_video(urx, &uvframe);
            } else if (uft == OAVX_FRAME_TYPE_AUDIO) {
                avx_rx_free_audio(urx, &uaframe);
            } else if (uft == OAVX_FRAME_TYPE_METADATA) {
                avx_rx_free_metadata(urx, &umframe);
            }

            avx_rx_destroy(urx); /* Closes udpsv[1] */
        } else {
            close(udpsv[1]);
        }

        close(udpsv[0]);
    }

    /* 2. RUDP NACK parser and frame reassembly test over socketpair */
    int rudpsv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, rudpsv) == 0) {
        avx_socket_set_nonblocking(rudpsv[1], true);

        struct sockaddr_in peer_addr;
        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        peer_addr.sin_port = htons(5960);

        avx_udp_rudp_sender_t *rsender = avx_udp_rudp_sender_create(rudpsv[1], 4);
        if (rsender) {
            uint8_t dummy_payload[2048];
            size_t dlen = size > sizeof(dummy_payload) ? sizeof(dummy_payload) : size;
            memcpy(dummy_payload, data, dlen);

            (void)avx_udp_rudp_send_frame(rsender, &peer_addr, 1, 0x32595655u, 0, dummy_payload, dlen);
            if (size >= 8) {
                uint32_t target_seq = 0;
                memcpy(&target_seq, data + 4, sizeof(target_seq));
                (void)avx_udp_rudp_send_frame(rsender, &peer_addr, target_seq, 0x32595655u, 0, dummy_payload, dlen);
            }

            (void)send(rudpsv[0], data, size > 2048 ? 2048 : size, 0);
            (void)avx_udp_rudp_sender_service_nacks(rsender);
            avx_udp_rudp_sender_destroy(rsender);
        }

        avx_udp_rudp_receiver_t *rrecv = avx_udp_rudp_receiver_create(rudpsv[1]);
        if (rrecv) {
            (void)send(rudpsv[0], data, size > 2048 ? 2048 : size, 0);
            uint8_t rudp_out[4096];
            size_t rudp_size = 0;
            uint32_t rudp_fourcc = 0;
            int64_t rudp_tc = 0;
            (void)avx_udp_rudp_recv_frame(rrecv, &peer_addr, rudp_out, sizeof(rudp_out),
                                          &rudp_size, &rudp_fourcc, &rudp_tc, 1);
            avx_udp_rudp_receiver_destroy(rrecv);
        }

        close(rudpsv[1]);
        close(rudpsv[0]);
    }

    /* 3. Live TCP wire frame ingestion test over socketpair */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        /* Write fuzzer data to sender socket, clamp to 64KB */
        size_t send_len = size > 65536 ? 65536 : size;
        (void)send(sv[0], data, send_len, 0);
        close(sv[0]); /* Signal EOF to receiver */

        avx_rx_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.color_format = (oavx_color_format_e)(data[0] % 7);

        avx_rx_instance_t rx = avx_rx_create(&cfg);
        if (rx) {
            avx_recv_ctx_t *slot = (avx_recv_ctx_t*)rx;
            avx_mutex_lock(&slot->lock);
            if (slot->tcp_conn_fd != AVX_INVALID_SOCKET) {
                avx_socket_close(slot->tcp_conn_fd);
            }
            slot->tcp_conn_fd = sv[1];
            slot->connected = true;
            slot->offline_mode = true;
            avx_mutex_unlock(&slot->lock);

            oavx_video_frame_t vframe = {0};
            oavx_audio_frame_t aframe = {0};
            oavx_metadata_frame_t mframe = {0};

            /* Receive up to 2 frames or EOF */
            for (int i = 0; i < 2; ++i) {
                oavx_frame_type_t ft = avx_rx_receive(rx, &vframe, &aframe, &mframe, 2);
                if (ft <= 0) break;
                if (ft == OAVX_FRAME_TYPE_VIDEO) {
                    avx_rx_free_video(rx, &vframe);
                } else if (ft == OAVX_FRAME_TYPE_AUDIO) {
                    avx_rx_free_audio(rx, &aframe);
                } else if (ft == OAVX_FRAME_TYPE_METADATA) {
                    avx_rx_free_metadata(rx, &mframe);
                }
            }

            avx_rx_destroy(rx); /* Closes sv[1] */
        } else {
            close(sv[1]);
        }
    }

    return 0;
}
