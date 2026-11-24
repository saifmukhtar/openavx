/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_send_aux.c - Auxiliary streaming servers (Native RTSP 1.0 & HTTP/fMP4 Web Preview)
 */

#include "avx_send_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Inspects compressed NAL unit headers (H.264 IDR, HEVC CRA/IDR, AV1 keyframe) in elementary stream. */
bool avx_send_detect_hx_keyframe(uint32_t fourcc, const uint8_t *data, size_t size)
{
    if (!data || size == 0) return false;

    if (fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
        return avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, data, size);
    }

    avx_rtsp_codec_e codec;
    if (fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 || fourcc == 0x34363268) {
        codec = AVX_RTSP_CODEC_H264;
    } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || fourcc == 0x43564568) {
        codec = AVX_RTSP_CODEC_HEVC;
    } else {
        return false;
    }

    bool saw_start_code = false;
    size_t pos = 0;
    while (pos + 3 <= size) {
        size_t sc_len = 0;
        if (data[pos] == 0 && data[pos + 1] == 0) {
            if (data[pos + 2] == 1) sc_len = 3;
            else if (pos + 4 <= size && data[pos + 2] == 0 && data[pos + 3] == 1) sc_len = 4;
        }
        if (sc_len > 0) {
            saw_start_code = true;
            size_t nalu_start = pos + sc_len;
            size_t next_pos = nalu_start;
            while (next_pos + 3 <= size) {
                if (data[next_pos] == 0 && data[next_pos + 1] == 0 &&
                    (data[next_pos + 2] == 1 ||
                     (next_pos + 4 <= size && data[next_pos + 2] == 0 && data[next_pos + 3] == 1))) {
                    break;
                }
                next_pos++;
            }
            if (next_pos + 3 > size) next_pos = size;
            if (next_pos > nalu_start && avx_rtsp_is_keyframe(codec, data + nalu_start, next_pos - nalu_start)) {
                return true;
            }
            pos = next_pos;
        } else {
            pos++;
        }
    }

    return !saw_start_code && avx_rtsp_is_keyframe(codec, data, size);
}

/* Dispatches compressed video frames to active auxiliary RTSP and WebSocket preview servers. */
void avx_send_feed_aux_servers(void *rtsp_srv, void *web_srv, uint32_t fourcc, const uint8_t *payload, size_t payload_size, int64_t pts_ns) {
    if ((rtsp_srv || web_srv) && payload && payload_size > 0) {
        bool is_compressed = (fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 ||
                              fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC ||
                              fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01);
        if (is_compressed) {
            bool is_kf = avx_send_detect_hx_keyframe(fourcc, payload, payload_size);
            if (rtsp_srv) {
                avx_rtsp_server_feed_frame((avx_rtsp_server_t*)rtsp_srv,
                                           payload,
                                           payload_size,
                                           pts_ns,
                                           is_kf);
            }
            if (web_srv) {
                avx_web_server_feed_frame((avx_web_server_t*)web_srv,
                                          payload,
                                          payload_size,
                                          pts_ns,
                                          is_kf);
            }
        }
    }
}

/* Initializes and launches embedded RTSP server for RTP/AVP H.264 streaming. */
bool avx_tx_enable_rtsp_server(avx_tx_instance_t instance, uint16_t port, const char *path) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot) return false;

    avx_mutex_lock(&slot->lock);
    if (slot->rtsp_server) {
        avx_mutex_unlock(&slot->lock);
        return true;
    }

    avx_rtsp_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = port;
    cfg.path = path ? path : "/live";
    cfg.codec = AVX_RTSP_CODEC_H264;
    cfg.user_ctx = slot;

    slot->rtsp_server = avx_rtsp_server_create(&cfg);
    bool ok = (slot->rtsp_server != NULL);
    avx_mutex_unlock(&slot->lock);
    return ok;
}

void avx_tx_disable_rtsp_server(avx_tx_instance_t instance) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot) return;

    avx_mutex_lock(&slot->lock);
    if (slot->rtsp_server) {
        avx_rtsp_server_destroy((avx_rtsp_server_t*)slot->rtsp_server);
        slot->rtsp_server = NULL;
    }
    avx_mutex_unlock(&slot->lock);
}

uint16_t avx_tx_get_rtsp_port(avx_tx_instance_t instance) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot) return 0;
    avx_mutex_lock(&slot->lock);
    uint16_t p = slot->rtsp_server ? avx_rtsp_server_get_port((avx_rtsp_server_t*)slot->rtsp_server) : 0;
    avx_mutex_unlock(&slot->lock);
    return p;
}

int avx_tx_get_rtsp_client_count(avx_tx_instance_t instance) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot) return 0;
    avx_mutex_lock(&slot->lock);
    int count = slot->rtsp_server ? avx_rtsp_server_get_client_count((avx_rtsp_server_t*)slot->rtsp_server) : 0;
    avx_mutex_unlock(&slot->lock);
    return count;
}

static void avx_tx_web_on_tally_change(void *user_ctx, const oavx_tally_t *tally) {
    if (!user_ctx || !tally) return;
    avx_tx_set_tally((avx_tx_instance_t)user_ctx, tally);
}

/* Spawns embedded HTTP/WebSocket server streaming compressed video for web browser preview. */
bool avx_tx_enable_web_preview(avx_tx_instance_t instance, uint16_t port) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot) return false;

    avx_mutex_lock(&slot->lock);
    if (slot->web_server) {
        avx_mutex_unlock(&slot->lock);
        return true;
    }

    avx_web_server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = port;
    cfg.stream_name = slot->name;
    cfg.codec = AVX_RTSP_CODEC_H264;
    cfg.user_ctx = slot;
    cfg.on_tally_change = avx_tx_web_on_tally_change;

    slot->web_server = avx_web_server_create(&cfg);
    bool ok = (slot->web_server != NULL);
    if (ok) {
        avx_web_server_broadcast_tally((avx_web_server_t*)slot->web_server, &slot->tally);
    }
    avx_mutex_unlock(&slot->lock);
    return ok;
}

void avx_tx_disable_web_preview(avx_tx_instance_t instance) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot) return;

    avx_mutex_lock(&slot->lock);
    if (slot->web_server) {
        avx_web_server_destroy((avx_web_server_t*)slot->web_server);
        slot->web_server = NULL;
    }
    avx_mutex_unlock(&slot->lock);
}

uint16_t avx_tx_get_web_preview_port(avx_tx_instance_t instance) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot) return 0;
    avx_mutex_lock(&slot->lock);
    uint16_t p = slot->web_server ? avx_web_server_get_port((avx_web_server_t*)slot->web_server) : 0;
    avx_mutex_unlock(&slot->lock);
    return p;
}

const char* avx_tx_get_web_preview_url(avx_tx_instance_t instance, char *out_url, size_t max_len) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot || !out_url || max_len < 16) return NULL;
    avx_mutex_lock(&slot->lock);
    if (!slot->web_server) {
        avx_mutex_unlock(&slot->lock);
        return NULL;
    }
    uint16_t port = avx_web_server_get_port((avx_web_server_t*)slot->web_server);
    const char *ip = slot->advertised_ip[0] ? slot->advertised_ip : "127.0.0.1";
    snprintf(out_url, max_len, "http://%s:%u/", ip, port);
    avx_mutex_unlock(&slot->lock);
    return out_url;
}

int avx_tx_get_web_client_count(avx_tx_instance_t instance) {
    avx_send_ctx_t *slot = (avx_send_ctx_t*)instance;
    if (!slot) return 0;
    avx_mutex_lock(&slot->lock);
    int count = slot->web_server ? avx_web_server_get_client_count((avx_web_server_t*)slot->web_server) : 0;
    avx_mutex_unlock(&slot->lock);
    return count;
}
