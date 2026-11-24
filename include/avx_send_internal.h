/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_send_internal.h - Internal declarations for OpenAVX transmission engine
 */

#ifndef AVX_SEND_INTERNAL_H
#define AVX_SEND_INTERNAL_H

#include "avx_internal.h"
#include "avx_audio.h"
#include "avx_speedhq.h"
#include "avx_discovery_server.h"
#include "avx_wire.h"
#include "avx_rtsp.h"
#include "avx_web.h"
#include "avx_rate_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Networking & Client Helpers */
bool avx_socket_send_all(avx_socket_t fd, const void* buf, size_t len);
void avx_send_recalculate_tally_locked(avx_send_ctx_t* slot);
void avx_send_remove_client_locked(avx_send_ctx_t* slot, int index);
void avx_client_queue_drain(avx_send_ctx_t* slot, avx_client_slot_t* client);
bool avx_send_video_datagram(avx_send_ctx_t *slot, avx_client_slot_t *client, const avx_tx_video_slot_t *vslot, bool use_rudp);

/* Listener & Worker Lifecycle */
bool avx_send_start_listener(avx_send_ctx_t* send_ctx);
OPENAVX_API void avx_send_stop_listener(avx_send_ctx_t* send_ctx);
void* avx_send_client_tx_worker(void* arg);
void* avx_send_client_rx_worker(void* arg);
void* avx_send_listen_worker(void* arg);
void* avx_send_nack_service_worker(void* arg);

/* Auxiliary RTSP & Web Preview Server helpers */
bool avx_send_detect_hx_keyframe(uint32_t fourcc, const uint8_t *data, size_t size);
void avx_send_feed_aux_servers(void *rtsp_srv, void *web_srv, uint32_t fourcc, const uint8_t *payload, size_t payload_size, int64_t pts_ns);

/* Profiles & Rate Control Evaluators */
uint32_t avx_send_evaluate_client_congestion(
    avx_client_slot_t *client,
    int64_t now_ns,
    size_t wire_bytes,
    float target_fps,
    uint32_t *out_cadence
);
void avx_send_profiles_tick(avx_send_ctx_t *slot, int64_t now_ms);
OPENAVX_API bool avx_is_lan_sockaddr(const struct sockaddr_storage *addr);
OPENAVX_API uint32_t avx_negotiate_best_codec(const char *codecs_str);

#ifdef __cplusplus
}
#endif

#endif /* AVX_SEND_INTERNAL_H */
