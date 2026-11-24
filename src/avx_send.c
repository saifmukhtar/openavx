/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_send.c - Stream transmitter, session coordinator, and media ingest
 */

#include "avx_send_internal.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <stdio.h>
#include <time.h>

/*
 * Allocates transmission session slot, sets up multi-client transmit queues,
 * discovers bind IP / network interfaces, and starts background mDNS announce.
 */
OPENAVX_API avx_tx_instance_t avx_tx_create(const avx_tx_config_t* p_create_settings) {
    avx_mutex_lock(&g_avx_state.global_lock);
    if (!g_avx_state.initialized) {
        avx_mutex_unlock(&g_avx_state.global_lock);
        avx_engine_initialize();
        avx_mutex_lock(&g_avx_state.global_lock);
    }

    /* Initialize PCAP dump if requested via environment */
    const char* pcap_env = getenv("OPENAVX_DUMP_PCAP");
    if (pcap_env) {
        avx_pcap_init(pcap_env);
    }

    avx_send_ctx_t* slot = NULL;
    for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
        if (!g_avx_state.send_slots[i].in_use) {
            slot = &g_avx_state.send_slots[i];
            memset(slot, 0, sizeof(*slot));
            slot->in_use = true;
            break;
        }
    }
    avx_mutex_unlock(&g_avx_state.global_lock);

    if (!slot) return NULL;

    avx_mutex_init(&slot->lock);
    avx_cond_init(&slot->tally_cond);
    slot->tcp_listen_fd = AVX_INVALID_SOCKET;
    slot->tcp_client_fd = AVX_INVALID_SOCKET;

    /* Initialize multi-client slots */
    for (int c = 0; c < AVX_MAX_CLIENTS; ++c) {
        slot->clients[c].fd = AVX_INVALID_SOCKET;
        slot->clients[c].connected_time_ns = 0;
        slot->clients[c].dropped_frames = 0;
        slot->clients[c].tally.flags = OAVX_TALLY_NONE;
        slot->clients[c].parent_ctx = slot;
        slot->clients[c].client_index = c;
        slot->clients[c].tx_thread = 0;
        slot->clients[c].tx_thread_active = false;
        slot->clients[c].queue_head = 0;
        slot->clients[c].queue_tail = 0;
        slot->clients[c].queue_count = 0;
        slot->clients[c].video_queued = 0;
        slot->clients[c].congestion_level = 0;
        slot->clients[c].frame_seq = 0;
        slot->clients[c].audio_packets_sent = 0;
        slot->clients[c].video_packets_sent = 0;
        slot->clients[c].tcp_bytes_submitted = 0;
        slot->clients[c].last_acked_bytes = 0;
        slot->clients[c].rate_window_start_acked = 0;
        slot->clients[c].rate_window_start_sub = 0;
        slot->clients[c].rate_window_start_ns = 0;
        slot->clients[c].rate_window_throttled = false;
        slot->clients[c].tx_bytes_per_sec = -1.0f;
        slot->clients[c].last_rtt_ms = 0.0f;
        slot->clients[c].last_rtt_sample_ns = 0;
        slot->clients[c].tx_running = false;
        avx_mutex_init(&slot->clients[c].tx_lock);
        avx_cond_init(&slot->clients[c].tx_cond);
    }
    slot->active_clients = 0;
    slot->video_pool_next = 0;
    for (int v = 0; v < AVX_TX_VIDEO_POOL_SIZE; ++v) {
        memset(&slot->video_pool[v], 0, sizeof(avx_tx_video_slot_t));
    }

    const char *group_env = getenv("OPENAVX_GROUPS");
    const avx_config_t *cfg = avx_config_get();

    /* Resolve configurable max_clients (default 32, ceiling 64) */
    int max_clients = AVX_DEFAULT_MAX_CLIENTS;
    if (cfg && cfg->max_clients > 0) {
        max_clients = cfg->max_clients;
    }
    const char *env_max = getenv("OPENAVX_MAX_CLIENTS");
    if (env_max && atoi(env_max) > 0) {
        max_clients = atoi(env_max);
    }
    if (p_create_settings && p_create_settings->max_connections > 0) {
        max_clients = (int)p_create_settings->max_connections;
    }
    if (max_clients < 1) max_clients = 1;
    if (max_clients > AVX_MAX_CLIENTS) max_clients = AVX_MAX_CLIENTS;
    slot->max_clients = max_clients;

    const oavx_wire_profile_t *wp = (p_create_settings && p_create_settings->wire_profile)
        ? p_create_settings->wire_profile : avx_wire_get_active_profile();
    slot->wire_profile = *wp;
    slot->port = wp->base_port ? wp->base_port : AVX_DEFAULT_PORT;

    slot->running = true;
    slot->client_rx_running = false;
    slot->client_rx_thread = 0;
    slot->num_conn_metadata = 0;
    slot->has_new_metadata = false;

    if (p_create_settings && p_create_settings->name) {
        snprintf(slot->name, sizeof(slot->name), "%s", p_create_settings->name);
    } else {
        snprintf(slot->name, sizeof(slot->name), "OpenAVX (Output)");
    }

    if (p_create_settings && p_create_settings->groups && p_create_settings->groups[0] != '\0') {
        snprintf(slot->groups, sizeof(slot->groups), "%s", p_create_settings->groups);
    } else if (group_env && group_env[0] != '\0') {
        snprintf(slot->groups, sizeof(slot->groups), "%s", group_env);
    } else if (cfg && cfg->channels_publish[0] != '\0') {
        snprintf(slot->groups, sizeof(slot->groups), "%s", cfg->channels_publish);
    } else {
        snprintf(slot->groups, sizeof(slot->groups), "public");
    }

    /* Determine configured interface or bind IP according to 4-tier precedence:
     * 1. Explicit API setting (p_create_settings->bind_ip)
     * 2. Environment variable (OPENAVX_BIND_IP / OPENAVX_NIC)
     * 3. Config file setting (cfg->bind_ip)
     * 4. Automatic local IP detection
     */
    const char *bind_target = NULL;
    if (p_create_settings && p_create_settings->bind_ip && p_create_settings->bind_ip[0] != '\0') {
        bind_target = p_create_settings->bind_ip;
    } else {
        const char *env_bind = getenv("OPENAVX_BIND_IP");
        if (!env_bind || env_bind[0] == '\0') {
            env_bind = getenv("OPENAVX_NIC");
        }
        if (env_bind && env_bind[0] != '\0') {
            bind_target = env_bind;
        } else if (cfg && cfg->bind_ip[0] != '\0') {
            bind_target = cfg->bind_ip;
        }
    }

    slot->bind_ip[0] = '\0';
    slot->advertised_ip[0] = '\0';

    if (bind_target && bind_target[0] != '\0') {
        if (avx_net_resolve_interface_or_ip(bind_target, slot->bind_ip, sizeof(slot->bind_ip))) {
            memcpy(slot->advertised_ip, slot->bind_ip, sizeof(slot->advertised_ip));
            slot->advertised_ip[sizeof(slot->advertised_ip) - 1] = '\0';
        } else {
            oavx_log_write(OAVX_LOG_WARN, "tx",
                           "Could not resolve network interface/IP '%s', falling back to default interface",
                           bind_target);
            slot->bind_ip[0] = '\0';
        }
    }

    if (slot->advertised_ip[0] == '\0') {
        avx_net_get_default_ip(slot->advertised_ip, sizeof(slot->advertised_ip));
    }

    slot->clock_video = p_create_settings ? p_create_settings->clock_video : true;
    slot->clock_audio = p_create_settings ? p_create_settings->clock_audio : true;

    slot->transport = p_create_settings ? p_create_settings->transport : OAVX_TRANSPORT_TCP;
    slot->udp_sock = AVX_INVALID_SOCKET;
    slot->udp_port = 0;
    slot->rudp_sender = NULL;
    slot->nack_thread = 0;
    slot->nack_thread_active = false;
    slot->nack_thread_running = false;
    slot->rate_ctl = avx_rate_controller_create(NULL);

    slot->audio_comp_mode = OAVX_AUDIO_COMPRESSION_OFF;
    slot->audio_target_bitrate_kbps = 0;
    slot->audio_encoder = NULL;
    slot->audio_comp_active = false;
    slot->audio_enc_channels = 0;
    slot->audio_enc_sample_rate = 0;

    slot->video_encoder = NULL;
    slot->video_enc_fourcc = 0;
    slot->video_enc_width = 0;
    slot->video_enc_height = 0;
    slot->video_quality = 0;
    slot->video_codec_fourcc = p_create_settings ? p_create_settings->video_codec_fourcc : 0;
    slot->video_bitrate_kbps = p_create_settings ? p_create_settings->video_bitrate_kbps : 0;
    slot->keyframe_interval  = p_create_settings ? p_create_settings->keyframe_interval : 0;

    slot->wan_client_count = 0;
    slot->wan_codec_fourcc = 0;
    slot->wan_encoder = NULL;
    slot->wan_enc_fourcc = 0;
    slot->wan_enc_width = 0;
    slot->wan_enc_height = 0;
    slot->wan_enc_bitrate_kbps = 0;

    slot->rtsp_server = NULL;
    slot->web_server = NULL;

    if (slot->transport == OAVX_TRANSPORT_RUDP || slot->transport == OAVX_TRANSPORT_UDP) {
        uint16_t udp_p = slot->port + 1;
        const char *bind_addr = (slot->bind_ip[0] != '\0') ? slot->bind_ip : NULL;
        slot->udp_sock = avx_udp_create_socket_bind(bind_addr, udp_p, true);
        if (slot->udp_sock == AVX_INVALID_SOCKET) {
            for (uint16_t alt = slot->port + 2; alt <= slot->port + 30; ++alt) {
                slot->udp_sock = avx_udp_create_socket_bind(bind_addr, alt, true);
                if (slot->udp_sock != AVX_INVALID_SOCKET) {
                    udp_p = alt;
                    break;
                }
            }
        }
        if (slot->udp_sock != AVX_INVALID_SOCKET) {
            slot->udp_port = udp_p;
            if (slot->transport == OAVX_TRANSPORT_RUDP) {
                slot->rudp_sender = avx_udp_rudp_sender_create(slot->udp_sock, 64);
                if (slot->rudp_sender) {
                    if (slot->rate_ctl) {
                        avx_rate_controller_apply_to_sender(slot->rate_ctl, slot->rudp_sender);
                    }
                    if (p_create_settings && p_create_settings->passphrase) {
                        avx_udp_rudp_sender_set_passphrase((avx_udp_rudp_sender_t*)slot->rudp_sender,
                                                           p_create_settings->passphrase,
                                                           p_create_settings->name);
                    }
                    slot->nack_thread_running = true;
                    if (avx_thread_create(&slot->nack_thread, avx_send_nack_service_worker, slot) == 0) {
                        slot->nack_thread_active = true;
                    }
                }
            }
        }
    }

    /* Start TCP Server */
    avx_send_start_listener(slot);

    if (strchr(slot->advertised_ip, ':')) {
        snprintf(slot->url, sizeof(slot->url), "[%s]:%u", slot->advertised_ip, slot->port);
    } else {
        snprintf(slot->url, sizeof(slot->url), "%s:%u", slot->advertised_ip, slot->port);
    }
    slot->source_info.name = slot->name;
    slot->source_info.address = slot->url;
    slot->source_info.port = slot->port;
    slot->source_info.group = slot->groups;

    /* Broadcast source via mDNS (UDP 5353) with specific egress binding */
    slot->last_mdns_broadcast_ns = avx_time_ns();
    avx_mdns_broadcast_source_ex(slot->name, slot->port, slot->advertised_ip);

    /* Register with Discovery Server(s) if configured */
    slot->discovery_server_sock = AVX_INVALID_SOCKET;
    slot->num_discovery_server_socks = 0;
    for (int s = 0; s < 8; ++s) {
        slot->discovery_server_socks[s] = AVX_INVALID_SOCKET;
    }
    const char *ds_env = getenv("OPENAVX_DISCOVERY_SERVER");
    const char *ds_targets = (ds_env && ds_env[0] != '\0') ? ds_env : (cfg ? cfg->discovery_servers : "");

    if (ds_targets && ds_targets[0] != '\0') {
        char my_url[256];
        if (strchr(slot->advertised_ip, ':')) {
            snprintf(my_url, sizeof(my_url), "[%s]:%u", slot->advertised_ip, slot->port);
        } else {
            snprintf(my_url, sizeof(my_url), "%s:%u", slot->advertised_ip, slot->port);
        }

        char targets_copy[512];
        snprintf(targets_copy, sizeof(targets_copy), "%s", ds_targets);

        char *saveptr = NULL;
        char *token = strtok_r(targets_copy, ",; \t", &saveptr);
        while (token && slot->num_discovery_server_socks < (int)(sizeof(slot->discovery_server_socks)/sizeof(slot->discovery_server_socks[0]))) {
            char srv_ip[64] = "127.0.0.1";
            uint16_t srv_port = AVX_DISCOVERY_SERVER_DEFAULT_PORT;
            char *colon = strchr(token, ':');
            if (colon) {
                *colon = '\0';
                srv_port = (uint16_t)atoi(colon + 1);
            }
            snprintf(srv_ip, sizeof(srv_ip), "%s", token);

            int fd = avx_discovery_client_register(srv_ip, srv_port, slot->name, my_url, slot->groups);
            if (fd >= 0) {
                slot->discovery_server_socks[slot->num_discovery_server_socks++] = (avx_socket_t)fd;
            }
            token = strtok_r(NULL, ",; \t", &saveptr);
        }
        if (slot->num_discovery_server_socks > 0) {
            slot->discovery_server_sock = slot->discovery_server_socks[0];
        }
    }

    return (avx_tx_instance_t)slot;
}

OPENAVX_API void avx_tx_destroy(avx_tx_instance_t p_instance) {
    if (!p_instance) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;

    avx_send_stop_listener(slot);
    avx_mutex_destroy(&slot->lock);
    avx_cond_destroy(&slot->tally_cond);

    for (int c = 0; c < AVX_MAX_CLIENTS; ++c) {
        avx_mutex_destroy(&slot->clients[c].tx_lock);
        avx_cond_destroy(&slot->clients[c].tx_cond);
    }

    for (int v = 0; v < AVX_TX_VIDEO_POOL_SIZE; ++v) {
        if (slot->video_pool[v].payload) {
            free(slot->video_pool[v].payload);
            slot->video_pool[v].payload = NULL;
        }
        if (slot->video_pool[v].metadata_buf) {
            free(slot->video_pool[v].metadata_buf);
            slot->video_pool[v].metadata_buf = NULL;
        }
    }

    for (int i = 0; i < slot->num_discovery_server_socks; ++i) {
        if (slot->discovery_server_socks[i] != AVX_INVALID_SOCKET) {
            avx_socket_close(slot->discovery_server_socks[i]);
            slot->discovery_server_socks[i] = AVX_INVALID_SOCKET;
        }
    }
    slot->num_discovery_server_socks = 0;

    if (slot->discovery_server_sock != AVX_INVALID_SOCKET) {
        avx_socket_close(slot->discovery_server_sock);
        slot->discovery_server_sock = AVX_INVALID_SOCKET;
    }

    if (slot->compressed_buf) {
        free(slot->compressed_buf);
        slot->compressed_buf = NULL;
        slot->compressed_buf_size = 0;
    }

    if (slot->nack_thread_active) {
        slot->nack_thread_running = false;
        avx_thread_join(slot->nack_thread);
        slot->nack_thread_active = false;
    }
    if (slot->rudp_sender) {
        avx_udp_rudp_sender_destroy((avx_udp_rudp_sender_t*)slot->rudp_sender);
        slot->rudp_sender = NULL;
    }
    if (slot->udp_sock != AVX_INVALID_SOCKET) {
        avx_socket_close(slot->udp_sock);
        slot->udp_sock = AVX_INVALID_SOCKET;
    }

    if (slot->audio_encoder) {
        avx_audio_encoder_destroy(slot->audio_encoder);
        slot->audio_encoder = NULL;
    }

    if (slot->video_encoder) {
        avx_video_encoder_destroy(slot->video_encoder);
        slot->video_encoder = NULL;
    }

    if (slot->wan_encoder) {
        avx_video_encoder_destroy(slot->wan_encoder);
        slot->wan_encoder = NULL;
    }

    if (slot->rate_ctl) {
        avx_rate_controller_destroy(slot->rate_ctl);
        slot->rate_ctl = NULL;
    }

    if (slot->rtsp_server) {
        avx_rtsp_server_destroy((avx_rtsp_server_t*)slot->rtsp_server);
        slot->rtsp_server = NULL;
    }

    if (slot->web_server) {
        avx_web_server_destroy((avx_web_server_t*)slot->web_server);
        slot->web_server = NULL;
    }

    avx_mutex_lock(&g_avx_state.global_lock);
    slot->in_use = false;
    avx_mutex_unlock(&g_avx_state.global_lock);
}

static void avx_pack_video_frame(const oavx_video_frame_t *src, uint8_t *dst, size_t dst_len) {
    if (!src || !dst || src->width == 0 || src->height == 0 || dst_len == 0) return;
    uint32_t w = src->width;
    uint32_t h = src->height;
    size_t s0 = src->strides[0];
    size_t s1 = src->strides[1];
    size_t s2 = src->strides[2];
    const uint8_t *p0 = src->planes[0];
    const uint8_t *p1 = src->planes[1];
    const uint8_t *p2 = src->planes[2];

    if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 ||
        src->fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC ||
        src->fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01 ||
        src->fourcc == 0x47504A4D ||
        src->fourcc == 0x4A50474D) {
        size_t copy_sz = (s0 > 0 && s0 <= dst_len) ? s0 : dst_len;
        memcpy(dst, p0, copy_sz);
        return;
    }

    if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
        size_t row_bytes = (size_t)w * 2;
        if (s0 == 0 || s0 == row_bytes) {
            size_t copy_sz = row_bytes * h;
            if (copy_sz > dst_len) copy_sz = dst_len;
            memcpy(dst, p0, copy_sz);
        } else {
            for (uint32_t y = 0; y < h; ++y) {
                memcpy(dst + (size_t)y * row_bytes, p0 + y * s0, row_bytes);
            }
        }
        return;
    }

    if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
        size_t yuv_row = (size_t)w * 2;
        size_t src_yuv_stride = (s0 > 0) ? s0 : yuv_row;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(dst + (size_t)y * yuv_row, p0 + y * src_yuv_stride, yuv_row);
        }
        uint8_t *alpha_dst = dst + (size_t)w * h * 2;
        const uint8_t *alpha_src = p1 ? p1 : (p0 + (size_t)h * src_yuv_stride);
        size_t src_alpha_stride = (s1 > 0) ? s1 : (size_t)w;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(alpha_dst + (size_t)y * w, alpha_src + y * src_alpha_stride, w);
        }
    } else if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
        size_t row_bytes = (size_t)w * 2;
        size_t src_y_stride = (s0 > 0) ? s0 : row_bytes;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(dst + (size_t)y * row_bytes, p0 + y * src_y_stride, row_bytes);
        }
        uint8_t *uv_dst = dst + (size_t)w * h * 2;
        const uint8_t *uv_src = p1 ? p1 : (p0 + (size_t)h * src_y_stride);
        size_t src_uv_stride = (s1 > 0) ? s1 : row_bytes;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(uv_dst + (size_t)y * row_bytes, uv_src + y * src_uv_stride, row_bytes);
        }
    } else if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_PA16) {
        size_t row_bytes = (size_t)w * 2;
        size_t src_y_stride = (s0 > 0) ? s0 : row_bytes;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(dst + (size_t)y * row_bytes, p0 + y * src_y_stride, row_bytes);
        }
        uint8_t *uv_dst = dst + (size_t)w * h * 2;
        const uint8_t *uv_src = p1 ? p1 : (p0 + (size_t)h * src_y_stride);
        size_t src_uv_stride = (s1 > 0) ? s1 : row_bytes;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(uv_dst + (size_t)y * row_bytes, uv_src + y * src_uv_stride, row_bytes);
        }
        uint8_t *a_dst = dst + (size_t)w * h * 4;
        const uint8_t *a_src = p2 ? p2 : (uv_src + (size_t)h * src_uv_stride);
        size_t src_a_stride = (s2 > 0) ? s2 : row_bytes;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(a_dst + (size_t)y * row_bytes, a_src + y * src_a_stride, row_bytes);
        }
    } else if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
        size_t y_row = (size_t)w;
        size_t src_y_stride = (s0 > 0) ? s0 : y_row;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(dst + (size_t)y * y_row, p0 + y * src_y_stride, y_row);
        }
        uint8_t *uv_dst = dst + (size_t)w * h;
        const uint8_t *uv_src = p1 ? p1 : (p0 + (size_t)h * src_y_stride);
        size_t src_uv_stride = (s1 > 0) ? s1 : y_row;
        uint32_t h2 = (h + 1) / 2;
        for (uint32_t y = 0; y < h2; ++y) {
            memcpy(uv_dst + (size_t)y * y_row, uv_src + y * src_uv_stride, y_row);
        }
    } else if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420 || src->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
        size_t y_row = (size_t)w;
        size_t src_y_stride = (s0 > 0) ? s0 : y_row;
        for (uint32_t y = 0; y < h; ++y) {
            memcpy(dst + (size_t)y * y_row, p0 + y * src_y_stride, y_row);
        }
        uint32_t w2 = (w + 1) / 2;
        uint32_t h2 = (h + 1) / 2;
        uint8_t *u_dst = dst + (size_t)w * h;
        uint8_t *v_dst = u_dst + (size_t)w2 * h2;
        const uint8_t *u_src = p1 ? p1 : (p0 + (size_t)h * src_y_stride);
        size_t src_u_stride = (s1 > 0) ? s1 : w2;
        const uint8_t *v_src = p2 ? p2 : (u_src + (size_t)h2 * src_u_stride);
        size_t src_v_stride = (s2 > 0) ? s2 : w2;
        if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            uint8_t *tmp = u_dst; u_dst = v_dst; v_dst = tmp;
        }
        for (uint32_t y = 0; y < h2; ++y) {
            memcpy(u_dst + (size_t)y * w2, u_src + y * src_u_stride, w2);
            memcpy(v_dst + (size_t)y * w2, v_src + y * src_v_stride, w2);
        }
    } else {
        size_t bpp = 2;
        if (src->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || src->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA ||
            src->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX || src->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
            bpp = 4;
        }
        size_t row_bytes = (size_t)w * bpp;
        if (s0 == 0 || s0 == row_bytes) {
            size_t copy_sz = row_bytes * h;
            if (copy_sz > dst_len) copy_sz = dst_len;
            memcpy(dst, p0, copy_sz);
        } else {
            for (uint32_t y = 0; y < h; ++y) {
                memcpy(dst + (size_t)y * row_bytes, p0 + y * s0, row_bytes);
            }
        }
    }
}

/*
 * Acquires a slot from circulating video pool (AVX_TX_VIDEO_POOL_SIZE). Encodes
 * frame once (primary codec and optional WAN simulcast), then distributes
 * references to active client queues. Drops frame if all pool slots are in-flight.
 */
OPENAVX_API void avx_tx_send_video(avx_tx_instance_t p_instance, const oavx_video_frame_t* p_video_data) {
    if (!p_instance || !p_video_data) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    slot->video_frames_sent++;

    int rtsp_clients = slot->rtsp_server ? avx_rtsp_server_get_client_count((avx_rtsp_server_t*)slot->rtsp_server) : 0;
    int web_clients  = slot->web_server  ? avx_web_server_get_client_count((avx_web_server_t*)slot->web_server) : 0;

    if ((slot->active_clients > 0 || rtsp_clients > 0 || web_clients > 0) && p_video_data->planes[0]) {
        /* Find free slot in shared circulating video pool */
        int pool_idx = -1;
        for (int i = 0; i < AVX_TX_VIDEO_POOL_SIZE; ++i) {
            int candidate = (slot->video_pool_next + i) % AVX_TX_VIDEO_POOL_SIZE;
            if (slot->video_pool[candidate].refcount == 0 && !slot->video_pool[candidate].in_flight) {
                pool_idx = candidate;
                slot->video_pool[candidate].in_flight = true;
                slot->video_pool_next = (candidate + 1) % AVX_TX_VIDEO_POOL_SIZE;
                break;
            }
        }

        if (pool_idx < 0) {
            /* All pool slots are referenced in active queues; drop frame to prevent stalling caller */
            for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
                if (slot->clients[i].fd != AVX_INVALID_SOCKET) {
                    slot->clients[i].dropped_frames++;
                }
            }
            avx_mutex_unlock(&slot->lock);
            return;
        }

        /* Check whether input is already compressed passthrough */
        bool is_compressed_passthrough = (p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 ||
                                          p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC ||
                                          p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01 ||
                                          p_video_data->fourcc == 0x47504A4D ||
                                          p_video_data->fourcc == 0x4A50474D);

        uint32_t target_comp_fourcc = 0;
        if (!is_compressed_passthrough) {
            if (slot->video_codec_fourcc != 0) {
                target_comp_fourcc = slot->video_codec_fourcc;
            } else if (avx_is_speedhq_fourcc((uint32_t)p_video_data->fourcc)) {
                target_comp_fourcc = (uint32_t)p_video_data->fourcc;
            }
        }

        /* Snapshot settings and release slot->lock during CPU-intensive video compression
         * so concurrent audio packets (48kHz) are enqueued without lock contention spikes */
        avx_video_encoder_t *venc = NULL;
        if (target_comp_fourcc != 0) {
            uint32_t req_fourcc = target_comp_fourcc;
            uint32_t req_w = p_video_data->width;
            uint32_t req_h = p_video_data->height;
            if (!slot->video_encoder ||
                slot->video_enc_fourcc != req_fourcc ||
                slot->video_enc_width != req_w ||
                slot->video_enc_height != req_h) {
                if (slot->video_encoder) {
                    avx_video_encoder_destroy(slot->video_encoder);
                    slot->video_encoder = NULL;
                }
                avx_video_enc_cfg_t vcfg;
                memset(&vcfg, 0, sizeof(vcfg));
                vcfg.fourcc = req_fourcc;
                vcfg.width = req_w;
                vcfg.height = req_h;
                uint8_t eff_quality = slot->video_quality;
                if (eff_quality == 0 && slot->rate_ctl) {
                    eff_quality = avx_rate_controller_get_quality(slot->rate_ctl);
                }
                vcfg.quality = eff_quality;
                vcfg.bitrate_kbps = slot->video_bitrate_kbps;
                vcfg.keyframe_interval = slot->keyframe_interval;
                vcfg.fps_num = (int32_t)p_video_data->framerate.num;
                vcfg.fps_den = (int32_t)p_video_data->framerate.den;
                slot->video_encoder = avx_video_encoder_create(&vcfg);
                slot->video_enc_fourcc = slot->video_encoder ? req_fourcc : 0;
                slot->video_enc_width = slot->video_encoder ? req_w : 0;
                slot->video_enc_height = slot->video_encoder ? req_h : 0;
            }
            venc = slot->video_encoder;
        }

        /* Secondary On-Demand Hardware WAN Simulcast Encoder */
        avx_video_encoder_t *wan_enc = NULL;
        uint32_t wan_fourcc = 0;
        int wan_clients = slot->wan_client_count;
        if (wan_clients > 0 && !is_compressed_passthrough) {
            wan_fourcc = slot->wan_codec_fourcc ? slot->wan_codec_fourcc : OAVX_FOURCC_VIDEO_TYPE_H264;
            uint32_t req_w = p_video_data->width;
            uint32_t req_h = p_video_data->height;
            uint32_t wan_br_kbps = 4000;
            if (slot->rate_ctl) {
                wan_br_kbps = avx_rate_controller_get_target_bitrate(slot->rate_ctl) / 1000;
                if (wan_br_kbps < 500) wan_br_kbps = 500;
                if (wan_br_kbps > 50000) wan_br_kbps = 50000;
            }
            if (!slot->wan_encoder ||
                slot->wan_enc_fourcc != wan_fourcc ||
                slot->wan_enc_width != req_w ||
                slot->wan_enc_height != req_h) {
                if (slot->wan_encoder) {
                    avx_video_encoder_destroy(slot->wan_encoder);
                    slot->wan_encoder = NULL;
                }
                avx_video_enc_cfg_t wcfg;
                memset(&wcfg, 0, sizeof(wcfg));
                wcfg.fourcc = wan_fourcc;
                wcfg.width = req_w;
                wcfg.height = req_h;
                wcfg.quality = 0;
                wcfg.bitrate_kbps = wan_br_kbps;
                wcfg.keyframe_interval = slot->keyframe_interval ? slot->keyframe_interval : 60;
                wcfg.fps_num = (int32_t)p_video_data->framerate.num;
                wcfg.fps_den = (int32_t)p_video_data->framerate.den;
                slot->wan_encoder = avx_video_encoder_create(&wcfg);
                slot->wan_enc_fourcc = slot->wan_encoder ? wan_fourcc : 0;
                slot->wan_enc_width = slot->wan_encoder ? req_w : 0;
                slot->wan_enc_height = slot->wan_encoder ? req_h : 0;
                slot->wan_enc_bitrate_kbps = wan_br_kbps;
            }
            wan_enc = slot->wan_encoder;
        }

        uint32_t magic_video = slot->wire_profile.magic_video;
        uint32_t magic_metadata = slot->wire_profile.magic_metadata;
        void *rtsp_srv = slot->rtsp_server;
        void *web_srv = slot->web_server;
        avx_mutex_unlock(&slot->lock);

        avx_tx_video_slot_t *vslot = &slot->video_pool[pool_idx];

        AVX_PACKED_START
        struct {
            uint32_t magic;
            uint32_t xres;
            uint32_t yres;
            uint32_t fourcc;
            int32_t  frame_rate_n;
            int32_t  frame_rate_d;
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED hdr;
        AVX_PACKED_END

        size_t frame_bytes = 0;
        uint32_t w = p_video_data->width;
        uint32_t h = p_video_data->height;
        size_t s0 = p_video_data->strides[0];

        if (is_compressed_passthrough) {
            frame_bytes = s0;
        } else if (p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
            frame_bytes = (size_t)w * h * 4;
        } else if (p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_PA16) {
            frame_bytes = (size_t)w * h * 6;
        } else if (p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
            frame_bytes = (size_t)w * h * 3;
        } else if (p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12 ||
                   p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420 ||
                   p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            frame_bytes = (size_t)w * h * 3 / 2;
        } else if (p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA ||
                   p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX ||
                   p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA ||
                   p_video_data->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
            frame_bytes = (size_t)w * h * 4;
        } else if (w > 0 && h > 0) {
            frame_bytes = (size_t)w * h * 2; /* UYVY default */
        } else if (s0 > 0) {
            frame_bytes = s0;
        }
        uint32_t fourcc = (uint32_t)p_video_data->fourcc;

        /* Prepare per-frame metadata header if present */
        bool has_metadata = (p_video_data->metadata && p_video_data->metadata[0] != '\0');
        size_t mlen = 0;
        AVX_PACKED_START
        struct {
            uint32_t magic;
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED mhdr;
        AVX_PACKED_END
        if (has_metadata) {
            mlen = p_video_data->metadata_len > 0 ? p_video_data->metadata_len : strlen(p_video_data->metadata);
            mhdr.magic = magic_metadata;
            mhdr.timecode = p_video_data->dts_ns != 0 ? p_video_data->dts_ns : p_video_data->pts_ns;
            mhdr.payload_len = (uint32_t)mlen;

            memcpy(vslot->metadata_hdr, &mhdr, sizeof(mhdr));
            vslot->metadata_hdr_len = sizeof(mhdr);

            if (vslot->metadata_buf) free(vslot->metadata_buf);
            vslot->metadata_buf = (uint8_t*)malloc(mlen + 1);
            if (vslot->metadata_buf) {
                memcpy(vslot->metadata_buf, p_video_data->metadata, mlen);
                vslot->metadata_buf[mlen] = '\0';
                vslot->metadata_len = mlen;
            } else {
                vslot->metadata_len = 0;
                vslot->metadata_hdr_len = 0;
            }
        } else {
            if (vslot->metadata_buf) {
                free(vslot->metadata_buf);
                vslot->metadata_buf = NULL;
            }
            vslot->metadata_len = 0;
            vslot->metadata_hdr_len = 0;
        }

        /* If compression (H.264, HEVC, SpeedHQ) is active */
        if (target_comp_fourcc != 0) {
            size_t max_out = (size_t)p_video_data->width * p_video_data->height * 4;
            if (max_out < 1048576) max_out = 1048576;
            if (vslot->payload_capacity < max_out) {
                uint8_t *nb = (uint8_t*)realloc(vslot->payload, max_out);
                if (nb) {
                    vslot->payload = nb;
                    vslot->payload_capacity = max_out;
                }
            }
            if (vslot->payload) {
                size_t actual_out = 0;
                int enc_ret = -1;
                if (venc) {
                    bool is_kf = false;
                    enc_ret = avx_video_encoder_encode(venc, p_video_data,
                                                       vslot->payload, vslot->payload_capacity,
                                                       &actual_out, &is_kf);
                }
                if (enc_ret == 0 && actual_out > 0) {
                    frame_bytes = actual_out;
                    fourcc = target_comp_fourcc;
                    vslot->payload_size = actual_out;
                } else {
                    /* Compression failed or encoder unavailable: fallback to raw packing */
                    fourcc = (uint32_t)p_video_data->fourcc;
                    if (avx_is_speedhq_fourcc(fourcc)) {
                        const uint8_t *alpha_plane = p_video_data->planes[1];
                        if (!alpha_plane && (fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ3 ||
                                             fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ1 ||
                                             fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ5 ||
                                             fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ7 ||
                                             fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ9)) {
                            alpha_plane = p_video_data->planes[0] + (size_t)p_video_data->width * p_video_data->height * 2;
                        }
                        if (alpha_plane) {
                            fourcc = (uint32_t)OAVX_FOURCC_VIDEO_TYPE_UYVA;
                            frame_bytes = (size_t)w * h * 3;
                        } else {
                            fourcc = (uint32_t)OAVX_FOURCC_VIDEO_TYPE_UYVY;
                            frame_bytes = (size_t)w * h * 2;
                        }
                    }
                    if (vslot->payload_capacity < frame_bytes) {
                        uint8_t *nb = (uint8_t*)realloc(vslot->payload, frame_bytes);
                        if (nb) {
                            vslot->payload = nb;
                            vslot->payload_capacity = frame_bytes;
                        }
                    }
                    if (vslot->payload) {
                        oavx_video_frame_t raw_frame = *p_video_data;
                        raw_frame.fourcc = (oavx_fourcc_video_type_e)fourcc;
                        avx_pack_video_frame(&raw_frame, vslot->payload, frame_bytes);
                        vslot->payload_size = frame_bytes;
                    }
                }
            }
        } else {
            /* Raw video copy into pool slot */
            if (vslot->payload_capacity < frame_bytes) {
                uint8_t *nb = (uint8_t*)realloc(vslot->payload, frame_bytes);
                if (nb) {
                    vslot->payload = nb;
                    vslot->payload_capacity = frame_bytes;
                }
            }
            if (vslot->payload) {
                avx_pack_video_frame(p_video_data, vslot->payload, frame_bytes);
                vslot->payload_size = frame_bytes;
            }
        }

        hdr.magic = magic_video;
        hdr.xres = (uint32_t)p_video_data->width;
        hdr.yres = (uint32_t)p_video_data->height;
        hdr.fourcc = fourcc;
        hdr.frame_rate_n = (int32_t)p_video_data->framerate.num;
        hdr.frame_rate_d = (int32_t)p_video_data->framerate.den;
        hdr.timecode = p_video_data->dts_ns != 0 ? p_video_data->dts_ns : p_video_data->pts_ns;
        hdr.payload_len = (uint32_t)frame_bytes;

        memcpy(vslot->header, &hdr, sizeof(hdr));
        vslot->header_len = sizeof(hdr);
        vslot->pts_ns = p_video_data->pts_ns;

        /* Feed auxiliary RTSP and Web preview servers */
        avx_send_feed_aux_servers(rtsp_srv, web_srv, fourcc, vslot->payload, vslot->payload_size, p_video_data->pts_ns);

        /* Encode secondary compressed simulcast frame for WAN clients if active */
        avx_tx_shared_pkt_t *wan_shared_pkt = NULL;
        if (wan_clients > 0 && wan_enc) {
            size_t max_wan_out = (size_t)p_video_data->width * p_video_data->height;
            if (max_wan_out < 1048576) max_wan_out = 1048576;
            uint8_t *wan_buf = (uint8_t*)malloc(sizeof(hdr) + max_wan_out);
            if (wan_buf) {
                size_t actual_wan = 0;
                bool is_kf = false;
                int enc_ret = avx_video_encoder_encode(wan_enc, p_video_data,
                                                       wan_buf + sizeof(hdr), max_wan_out,
                                                       &actual_wan, &is_kf);
                if (enc_ret == 0 && actual_wan > 0) {
                    AVX_PACKED_START
                    struct {
                        uint32_t magic;
                        uint32_t xres;
                        uint32_t yres;
                        uint32_t fourcc;
                        int32_t  frame_rate_n;
                        int32_t  frame_rate_d;
                        int64_t  timecode;
                        uint32_t payload_len;
                    } AVX_PACKED wan_hdr;
                    AVX_PACKED_END
                    wan_hdr.magic = magic_video;
                    wan_hdr.xres = (uint32_t)p_video_data->width;
                    wan_hdr.yres = (uint32_t)p_video_data->height;
                    wan_hdr.fourcc = wan_fourcc;
                    wan_hdr.frame_rate_n = (int32_t)p_video_data->framerate.num;
                    wan_hdr.frame_rate_d = (int32_t)p_video_data->framerate.den;
                    wan_hdr.timecode = p_video_data->dts_ns != 0 ? p_video_data->dts_ns : p_video_data->pts_ns;
                    wan_hdr.payload_len = (uint32_t)actual_wan;
                    memcpy(wan_buf, &wan_hdr, sizeof(wan_hdr));

                    wan_shared_pkt = (avx_tx_shared_pkt_t*)calloc(1, sizeof(avx_tx_shared_pkt_t));
                    if (wan_shared_pkt) {
                        wan_shared_pkt->data = wan_buf;
                        wan_shared_pkt->size = sizeof(wan_hdr) + actual_wan;
                        wan_shared_pkt->refcount = 0;
                    } else {
                        free(wan_buf);
                    }
                } else {
                    free(wan_buf);
                }
            }
        }

        /* Re-acquire slot->lock to safely enqueue to clients and update counters */
        avx_mutex_lock(&slot->lock);
        vslot->in_flight = false;
        vslot->refcount = 0;
        int enqueued_count = 0;
        size_t wire_bytes = sizeof(hdr) + frame_bytes + (has_metadata ? (sizeof(mhdr) + mlen) : 0);
        int64_t now_ns = avx_time_ns();
        float target_fps = (p_video_data->framerate.den > 0 && p_video_data->framerate.num > 0)
                           ? ((float)p_video_data->framerate.num / (float)p_video_data->framerate.den)
                           : 60.0f;
        if (target_fps < 1.0f) target_fps = 1.0f;
        if (target_fps > 240.0f) target_fps = 240.0f;

        for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
            if (slot->clients[i].fd == AVX_INVALID_SOCKET) continue;

            avx_mutex_lock(&slot->clients[i].tx_lock);
            if (!slot->clients[i].tx_running) {
                avx_mutex_unlock(&slot->clients[i].tx_lock);
                continue;
            }

            slot->clients[i].frame_seq++;

            /* 1. Reserved Slot Headroom for Audio:
             * Video is strictly prohibited from occupying the final 4 slots of the queue.
             * This guarantees that audio and metadata frames always have space available. */
            if (slot->clients[i].queue_count >= (AVX_CLIENT_QUEUE_DEPTH - 4)) {
                slot->clients[i].dropped_frames++;
                avx_mutex_unlock(&slot->clients[i].tx_lock);
                continue;
            }

            bool client_is_wan = slot->clients[i].is_wan_client && (wan_shared_pkt != NULL);
            size_t client_wire_bytes = client_is_wan ? wan_shared_pkt->size : wire_bytes;

            /* 2. Congestion Detection & Adaptive Temporal Decimation */
            uint32_t cadence = 1;
            slot->clients[i].congestion_level = avx_send_evaluate_client_congestion(
                &slot->clients[i], now_ns, client_wire_bytes, target_fps, &cadence);

            if ((slot->clients[i].frame_seq % cadence) != 0) {
                /* Decimate: skip this frame cleanly without enqueuing */
                slot->clients[i].dropped_frames++;
                avx_mutex_unlock(&slot->clients[i].tx_lock);
                continue;
            }

            int tail = slot->clients[i].queue_tail;
            slot->clients[i].queue[tail].type = AVX_QUEUE_ITEM_VIDEO;
            if (client_is_wan) {
                slot->clients[i].queue[tail].video_pool_idx = -1;
                slot->clients[i].queue[tail].shared_pkt = wan_shared_pkt;
                wan_shared_pkt->refcount++;
            } else {
                slot->clients[i].queue[tail].video_pool_idx = pool_idx;
                slot->clients[i].queue[tail].shared_pkt = NULL;
                vslot->refcount++;
            }
            slot->clients[i].queue_tail = (tail + 1) % AVX_CLIENT_QUEUE_DEPTH;
            slot->clients[i].queue_count++;
            slot->clients[i].video_queued++;
            slot->clients[i].video_packets_sent++;

            enqueued_count++;

            avx_cond_signal(&slot->clients[i].tx_cond);
            avx_mutex_unlock(&slot->clients[i].tx_lock);
        }

        if (wan_shared_pkt && wan_shared_pkt->refcount == 0) {
            if (wan_shared_pkt->data) free(wan_shared_pkt->data);
            free(wan_shared_pkt);
        }

        if (enqueued_count > 0) {
            /* Telemetry tracking for video */
            slot->telem.total_bytes += (wire_bytes * enqueued_count);
            slot->telem.window_bytes += (wire_bytes * enqueued_count);
            slot->telem.window_frames++;
            slot->telem.last_fourcc = p_video_data->fourcc;
            slot->telem.last_width = p_video_data->width;
            slot->telem.last_height = p_video_data->height;

            if (slot->telem.last_window_time_ns == 0) {
                slot->telem.last_window_time_ns = now_ns;
            } else {
                int64_t elapsed_ns = now_ns - slot->telem.last_window_time_ns;
                if (elapsed_ns >= 500000000LL) { /* 500 ms sliding window */
                    float elapsed_sec = (float)elapsed_ns / 1000000000.0f;
                    slot->telem.instant_fps = (float)slot->telem.window_frames / elapsed_sec;
                    slot->telem.instant_mbps = ((float)slot->telem.window_bytes * 8.0f) / (elapsed_sec * 1000000.0f);
                    slot->telem.window_bytes = 0;
                    slot->telem.window_frames = 0;
                    slot->telem.last_window_time_ns = now_ns;
                    avx_send_profiles_tick(slot, now_ns / 1000000LL);
                }
            }

            avx_trace_packet("[TX]", "AVX VIDEO FRAME", &hdr, sizeof(hdr));
            size_t sample_cap = hdr.payload_len > 1024 ? 1024 : hdr.payload_len;
            size_t snap_len = sizeof(hdr) + sample_cap;
            uint8_t *snap = (uint8_t*)malloc(snap_len);
            if (snap) {
                memcpy(snap, &hdr, sizeof(hdr));
                memcpy(snap + sizeof(hdr), vslot->payload, sample_cap);
                avx_pcap_write_tcp(htonl(0x7F000001), slot->port, htonl(0x7F000001), 54321, 100, 1, 0x18, snap, snap_len);
                free(snap);
            }
        }
    } else {
        slot->telem.last_fourcc = p_video_data->fourcc;
        slot->telem.last_width = p_video_data->width;
        slot->telem.last_height = p_video_data->height;
    }
    avx_mutex_unlock(&slot->lock);
}

void avx_tx_send_video_async(avx_tx_instance_t p_instance, const oavx_video_frame_t* p_video_data) {
    avx_tx_send_video(p_instance, p_video_data);
}

/*
 * Distributes shared refcounted audio packet to client queues. Preempts and
 * evicts queued video frames if client queue is full to prevent audio dropouts.
 */
static void avx_tx_enqueue_audio_shared_pkt(
    avx_send_ctx_t *slot,
    avx_tx_shared_pkt_t *pkt,
    const void *hdr_ptr,
    size_t hdr_len,
    const void *sample_ptr,
    size_t sample_len
) {
    pkt->refcount = 0;
    int enqueued_count = 0;

    for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
        if (slot->clients[i].fd == AVX_INVALID_SOCKET) continue;

        avx_mutex_lock(&slot->clients[i].tx_lock);
        if (!slot->clients[i].tx_running) {
            avx_mutex_unlock(&slot->clients[i].tx_lock);
            continue;
        }

        /* Priority Preemption: Audio frames must NEVER be dropped if video frames are queued.
         * If the queue is full (queue_count >= AVX_CLIENT_QUEUE_DEPTH), evict the oldest
         * video frame to immediately create a free slot for this audio frame! */
        if (slot->clients[i].queue_count >= AVX_CLIENT_QUEUE_DEPTH) {
            int head = slot->clients[i].queue_head;
            int evict_idx = -1;
            for (int q = 0; q < slot->clients[i].queue_count; ++q) {
                int idx = (head + q) % AVX_CLIENT_QUEUE_DEPTH;
                if (slot->clients[i].queue[idx].type == AVX_QUEUE_ITEM_VIDEO) {
                    evict_idx = idx;
                    break;
                }
            }

            if (evict_idx >= 0) {
                int p_idx = slot->clients[i].queue[evict_idx].video_pool_idx;
                if (p_idx >= 0 && p_idx < AVX_TX_VIDEO_POOL_SIZE) {
                    if (slot->video_pool[p_idx].refcount > 0) {
                        slot->video_pool[p_idx].refcount--;
                    }
                }
                if (slot->clients[i].video_queued > 0) {
                    slot->clients[i].video_queued--;
                }
                slot->clients[i].dropped_frames++;

                /* Shift items between evict_idx and tail - 1 */
                int cur = evict_idx;
                int tail = slot->clients[i].queue_tail;
                int last = (tail - 1 + AVX_CLIENT_QUEUE_DEPTH) % AVX_CLIENT_QUEUE_DEPTH;
                while (cur != last) {
                    int next = (cur + 1) % AVX_CLIENT_QUEUE_DEPTH;
                    slot->clients[i].queue[cur] = slot->clients[i].queue[next];
                    cur = next;
                }
                slot->clients[i].queue[last].type = AVX_QUEUE_ITEM_NONE;
                slot->clients[i].queue[last].shared_pkt = NULL;
                slot->clients[i].queue_tail = last;
                slot->clients[i].queue_count--;
            }
        }

        if (slot->clients[i].queue_count < AVX_CLIENT_QUEUE_DEPTH) {
            int tail = slot->clients[i].queue_tail;
            slot->clients[i].queue[tail].type = AVX_QUEUE_ITEM_AUDIO;
            slot->clients[i].queue[tail].video_pool_idx = -1;
            slot->clients[i].queue[tail].shared_pkt = pkt;
            slot->clients[i].queue_tail = (tail + 1) % AVX_CLIENT_QUEUE_DEPTH;
            slot->clients[i].queue_count++;
            slot->clients[i].audio_packets_sent++;

            pkt->refcount++;
            enqueued_count++;

            avx_cond_signal(&slot->clients[i].tx_cond);
        } else {
            slot->clients[i].dropped_frames++;
        }
        avx_mutex_unlock(&slot->clients[i].tx_lock);
    }

    if (enqueued_count == 0) {
        free(pkt->data);
        free(pkt);
    } else {
        /* Telemetry tracking for audio */
        size_t a_wire = pkt->size * enqueued_count;
        slot->telem.total_bytes += a_wire;
        slot->telem.window_bytes += a_wire;

        if (hdr_ptr && hdr_len > 0) {
            avx_trace_packet("[TX]", "AVX AUDIO FRAME", hdr_ptr, hdr_len);
            size_t sample_cap = sample_len > 512 ? 512 : sample_len;
            size_t snap_len = hdr_len + sample_cap;
            uint8_t *snap = (uint8_t*)malloc(snap_len);
            if (snap) {
                memcpy(snap, hdr_ptr, hdr_len);
                if (sample_ptr && sample_cap > 0) {
                    memcpy(snap + hdr_len, sample_ptr, sample_cap);
                }
                avx_pcap_write_tcp(htonl(0x7F000001), slot->port, htonl(0x7F000001), 54321, 200, 1, 0x18, snap, snap_len);
                free(snap);
            }
        }
    }
}

/*
 * Ingests planar float audio. Evaluates congestion for adaptive Opus/ADPCM
 * compression; otherwise packages uncompressed 32-bit float PCM.
 */
OPENAVX_API void avx_tx_send_audio(avx_tx_instance_t p_instance, const oavx_audio_frame_t* p_audio_data) {
    if (!p_instance || !p_audio_data) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    slot->audio_frames_sent++;

    if (slot->active_clients > 0 && p_audio_data->planes[0] && p_audio_data->frames > 0) {
        /* Adaptive compression mode evaluation */
        if (slot->audio_comp_mode == OAVX_AUDIO_COMPRESSION_ADAPTIVE) {
            bool congested = false;
            for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
                if (slot->clients[i].fd != AVX_INVALID_SOCKET) {
                    if (slot->clients[i].congestion_level > 0 || slot->clients[i].queue_count > 0) {
                        congested = true;
                        break;
                    }
                }
            }
            if (!congested && slot->audio_comp_active && slot->audio_encoder) {
                /* Link cleared: flush pending compressed accumulator before switching back to uncompressed */
                uint8_t f_buf[65536];
                size_t f_size = 0;
                int f_samples = 0;
                int64_t f_pts = 0;
                uint32_t f_fourcc = 0;
                if (avx_audio_encoder_flush(slot->audio_encoder, f_buf, sizeof(f_buf),
                                            &f_size, &f_samples, &f_pts, &f_fourcc)) {
                    AVX_PACKED_START
                    struct {
                        uint32_t magic;
                        uint32_t codec_fourcc;
                        int32_t  sample_rate;
                        int32_t  channels;
                        int32_t  samples;
                        int64_t  timecode;
                        uint32_t payload_len;
                    } AVX_PACKED fhdr;
                    AVX_PACKED_END

                    fhdr.magic = OAVX_WIRE_MAGIC_AUDIO_COMP;
                    fhdr.codec_fourcc = f_fourcc;
                    fhdr.sample_rate = (int32_t)p_audio_data->sample_rate;
                    fhdr.channels = (int32_t)p_audio_data->channels;
                    fhdr.samples = (int32_t)f_samples;
                    fhdr.timecode = f_pts;
                    fhdr.payload_len = (uint32_t)f_size;

                    avx_tx_shared_pkt_t *fpkt = (avx_tx_shared_pkt_t*)malloc(sizeof(avx_tx_shared_pkt_t));
                    if (fpkt) {
                        fpkt->size = sizeof(fhdr) + f_size;
                        fpkt->data = (uint8_t*)malloc(fpkt->size);
                        if (fpkt->data) {
                            memcpy(fpkt->data, &fhdr, sizeof(fhdr));
                            memcpy(fpkt->data + sizeof(fhdr), f_buf, f_size);
                            avx_tx_enqueue_audio_shared_pkt(slot, fpkt, &fhdr, sizeof(fhdr), f_buf, f_size);
                        } else {
                            free(fpkt);
                        }
                    }
                }
            }
            slot->audio_comp_active = congested;
        }

        bool separate_planes = (p_audio_data->channels > 1 &&
                                p_audio_data->planes[1] != NULL &&
                                p_audio_data->planes[1] != (void*)((float*)p_audio_data->planes[0] + p_audio_data->frames));

        if (slot->audio_comp_active) {
            /* Create or reconfigure encoder instance as necessary */
            if (!slot->audio_encoder || slot->audio_enc_channels != (int)p_audio_data->channels ||
                slot->audio_enc_sample_rate != (int)p_audio_data->sample_rate) {
                if (slot->audio_encoder) {
                    avx_audio_encoder_destroy(slot->audio_encoder);
                }
                avx_audio_codec_type_e codec_pref = (p_audio_data->channels <= 8) ? AVX_AUDIO_CODEC_OPUS : AVX_AUDIO_CODEC_ADPCM;
                slot->audio_encoder = avx_audio_encoder_create((int)p_audio_data->sample_rate,
                                                               (int)p_audio_data->channels,
                                                               codec_pref,
                                                               slot->audio_target_bitrate_kbps);
                slot->audio_enc_channels = (int)p_audio_data->channels;
                slot->audio_enc_sample_rate = (int)p_audio_data->sample_rate;
            }

            if (slot->audio_encoder) {
                const float* in_planes[OAVX_MAX_AUDIO_CHANNELS];
                for (int c = 0; c < (int)p_audio_data->channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
                    if (separate_planes) {
                        in_planes[c] = (const float*)p_audio_data->planes[c];
                    } else {
                        in_planes[c] = (const float*)p_audio_data->planes[0] + (size_t)c * p_audio_data->frames;
                    }
                }
                avx_audio_encoder_feed(slot->audio_encoder, in_planes, (int)p_audio_data->frames, p_audio_data->pts_ns);

                uint8_t comp_buf[65536];
                size_t comp_size = 0;
                int out_samples = 0;
                int64_t out_pts = 0;
                uint32_t out_fourcc = 0;

                while (avx_audio_encoder_pull(slot->audio_encoder, comp_buf, sizeof(comp_buf),
                                              &comp_size, &out_samples, &out_pts, &out_fourcc)) {
                    AVX_PACKED_START
                    struct {
                        uint32_t magic;         /* OAVX_WIRE_MAGIC_AUDIO_COMP ('AVXO') */
                        uint32_t codec_fourcc;  /* 'OPUS' or 'ADPC' */
                        int32_t  sample_rate;
                        int32_t  channels;
                        int32_t  samples;
                        int64_t  timecode;
                        uint32_t payload_len;
                    } AVX_PACKED chdr;
                    AVX_PACKED_END

                    chdr.magic = OAVX_WIRE_MAGIC_AUDIO_COMP;
                    chdr.codec_fourcc = out_fourcc;
                    chdr.sample_rate = (int32_t)p_audio_data->sample_rate;
                    chdr.channels = (int32_t)p_audio_data->channels;
                    chdr.samples = (int32_t)out_samples;
                    chdr.timecode = out_pts;
                    chdr.payload_len = (uint32_t)comp_size;

                    avx_tx_shared_pkt_t *pkt = (avx_tx_shared_pkt_t*)malloc(sizeof(avx_tx_shared_pkt_t));
                    if (!pkt) break;

                    pkt->size = sizeof(chdr) + comp_size;
                    pkt->data = (uint8_t*)malloc(pkt->size);
                    if (!pkt->data) {
                        free(pkt);
                        break;
                    }

                    memcpy(pkt->data, &chdr, sizeof(chdr));
                    memcpy(pkt->data + sizeof(chdr), comp_buf, comp_size);

                    avx_tx_enqueue_audio_shared_pkt(slot, pkt, &chdr, sizeof(chdr), comp_buf, comp_size);
                }

                avx_mutex_unlock(&slot->lock);
                return;
            }
        }

        /* Uncompressed fallback / legacy ('AVXA') */
        AVX_PACKED_START
        struct {
            uint32_t magic;
            int32_t  sample_rate;
            int32_t  channels;
            int32_t  samples;
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED hdr;
        AVX_PACKED_END

        hdr.magic = slot->wire_profile.magic_audio;
        hdr.sample_rate = (int32_t)p_audio_data->sample_rate;
        hdr.channels = (int32_t)p_audio_data->channels;
        hdr.samples = (int32_t)p_audio_data->frames;
        hdr.timecode = p_audio_data->pts_ns;

        size_t audio_bytes = (size_t)p_audio_data->channels * p_audio_data->frames * sizeof(float);
        hdr.payload_len = (uint32_t)audio_bytes;

        avx_tx_shared_pkt_t *pkt = (avx_tx_shared_pkt_t*)malloc(sizeof(avx_tx_shared_pkt_t));
        if (!pkt) {
            avx_mutex_unlock(&slot->lock);
            return;
        }

        pkt->size = sizeof(hdr) + audio_bytes;
        pkt->data = (uint8_t*)malloc(pkt->size);
        if (!pkt->data) {
            free(pkt);
            avx_mutex_unlock(&slot->lock);
            return;
        }

        memcpy(pkt->data, &hdr, sizeof(hdr));
        if (separate_planes) {
            size_t ch_bytes = (size_t)p_audio_data->frames * sizeof(float);
            for (uint16_t c = 0; c < p_audio_data->channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
                if (p_audio_data->planes[c]) {
                    memcpy(pkt->data + sizeof(hdr) + c * ch_bytes, p_audio_data->planes[c], ch_bytes);
                } else {
                    memset(pkt->data + sizeof(hdr) + c * ch_bytes, 0, ch_bytes);
                }
            }
        } else {
            memcpy(pkt->data + sizeof(hdr), p_audio_data->planes[0], audio_bytes);
        }

        avx_tx_enqueue_audio_shared_pkt(slot, pkt, &hdr, sizeof(hdr), p_audio_data->planes[0], audio_bytes);
    }
    avx_mutex_unlock(&slot->lock);
}

void avx_tx_set_audio_compression(avx_tx_instance_t p_instance, oavx_audio_compression_mode_e mode, int target_bitrate_kbps) {
    if (!p_instance) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    slot->audio_comp_mode = mode;
    slot->audio_target_bitrate_kbps = target_bitrate_kbps;
    if (mode == OAVX_AUDIO_COMPRESSION_MANUAL) {
        slot->audio_comp_active = true;
    } else if (mode == OAVX_AUDIO_COMPRESSION_OFF) {
        slot->audio_comp_active = false;
        if (slot->audio_encoder) {
            avx_audio_encoder_destroy(slot->audio_encoder);
            slot->audio_encoder = NULL;
            slot->audio_enc_channels = 0;
            slot->audio_enc_sample_rate = 0;
        }
    }
    avx_mutex_unlock(&slot->lock);
}

void avx_tx_set_video_quality(avx_tx_instance_t p_instance, uint8_t quality) {
    if (!p_instance) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    if (quality != 0) {
        if (quality < 20) quality = 20;
        if (quality > 98) quality = 98;
    }
    avx_mutex_lock(&slot->lock);
    slot->video_quality = quality;
    if (slot->video_encoder) {
        avx_video_encoder_set_quality(slot->video_encoder, quality);
    }
    avx_mutex_unlock(&slot->lock);
}

uint8_t avx_tx_get_video_quality(avx_tx_instance_t p_instance) {
    if (!p_instance) return 0;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    uint8_t q = slot->video_quality;
    avx_mutex_unlock(&slot->lock);
    return q ? q : avx_speedhq_get_quality();
}

void avx_tx_set_video_compression(avx_tx_instance_t p_instance, uint32_t target_fourcc, uint32_t bitrate_kbps) {
    if (!p_instance) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    slot->video_codec_fourcc = target_fourcc;
    slot->video_bitrate_kbps = bitrate_kbps;
    if (slot->video_encoder) {
        avx_video_encoder_destroy(slot->video_encoder);
        slot->video_encoder = NULL;
        slot->video_enc_fourcc = 0;
        slot->video_enc_width = 0;
        slot->video_enc_height = 0;
    }
    avx_mutex_unlock(&slot->lock);
}

uint32_t avx_tx_get_video_compression(avx_tx_instance_t p_instance) {
    if (!p_instance) return 0;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    uint32_t fcc = slot->video_codec_fourcc;
    avx_mutex_unlock(&slot->lock);
    return fcc;
}

void avx_tx_send_metadata(avx_tx_instance_t p_instance, const oavx_metadata_frame_t* metadata) {
    if (!p_instance || !metadata || !metadata->data) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    if (slot->active_clients > 0) {
        size_t len = metadata->length > 0 ? metadata->length : strlen(metadata->data);

        AVX_PACKED_START
        struct {
            uint32_t magic;
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED hdr;
        AVX_PACKED_END

        hdr.magic = slot->wire_profile.magic_metadata;
        hdr.timecode = metadata->pts_ns;
        hdr.payload_len = (uint32_t)len;

        avx_tx_shared_pkt_t *pkt = (avx_tx_shared_pkt_t*)malloc(sizeof(avx_tx_shared_pkt_t));
        if (!pkt) {
            avx_mutex_unlock(&slot->lock);
            return;
        }

        pkt->size = sizeof(hdr) + len;
        pkt->data = (uint8_t*)malloc(pkt->size);
        if (!pkt->data) {
            free(pkt);
            avx_mutex_unlock(&slot->lock);
            return;
        }

        memcpy(pkt->data, &hdr, sizeof(hdr));
        memcpy(pkt->data + sizeof(hdr), metadata->data, len);

        pkt->refcount = 0;
        int enqueued_count = 0;

        for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
            if (slot->clients[i].fd == AVX_INVALID_SOCKET) continue;

            avx_mutex_lock(&slot->clients[i].tx_lock);
            if (!slot->clients[i].tx_running) {
                avx_mutex_unlock(&slot->clients[i].tx_lock);
                continue;
            }

            if (slot->clients[i].queue_count >= AVX_CLIENT_QUEUE_DEPTH) {
                slot->clients[i].dropped_frames++;
                avx_mutex_unlock(&slot->clients[i].tx_lock);
                continue;
            }

            int tail = slot->clients[i].queue_tail;
            slot->clients[i].queue[tail].type = AVX_QUEUE_ITEM_METADATA;
            slot->clients[i].queue[tail].video_pool_idx = -1;
            slot->clients[i].queue[tail].shared_pkt = pkt;
            slot->clients[i].queue_tail = (tail + 1) % AVX_CLIENT_QUEUE_DEPTH;
            slot->clients[i].queue_count++;

            pkt->refcount++;
            enqueued_count++;

            avx_cond_signal(&slot->clients[i].tx_cond);
            avx_mutex_unlock(&slot->clients[i].tx_lock);
        }

        if (enqueued_count == 0) {
            free(pkt->data);
            free(pkt);
        } else {
            /* Telemetry tracking for metadata */
            slot->metadata_frames_sent++;
            size_t m_wire = (sizeof(hdr) + len) * enqueued_count;
            slot->telem.total_bytes += m_wire;
            slot->telem.window_bytes += m_wire;

            avx_trace_packet("[TX]", "AVX METADATA FRAME", metadata->data, len);

            /* Write to PCAP */
            size_t snap_len = sizeof(hdr) + len;
            uint8_t *snap = (uint8_t*)malloc(snap_len);
            if (snap) {
                memcpy(snap, &hdr, sizeof(hdr));
                memcpy(snap + sizeof(hdr), metadata->data, len);
                avx_pcap_write_tcp(htonl(0x7F000001), slot->port, htonl(0x7F000001), 54321, 300, 1, 0x18, snap, snap_len);
                free(snap);
            }
        }
    }
    avx_mutex_unlock(&slot->lock);
}

bool avx_tx_get_tally(avx_tx_instance_t p_instance, oavx_tally_t* p_tally, uint32_t timeout_in_ms) {
    if (!p_instance || !p_tally) return false;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    if (timeout_in_ms > 0 && !(slot->tally.flags & (OAVX_TALLY_PROGRAM | OAVX_TALLY_PREVIEW | OAVX_TALLY_RECORDING))) {
        avx_cond_timedwait(&slot->tally_cond, &slot->lock, timeout_in_ms);
    }
    *p_tally = slot->tally;
    avx_mutex_unlock(&slot->lock);
    return true;
}

bool avx_tx_set_tally(avx_tx_instance_t p_instance, const oavx_tally_t* p_tally) {
    if (!p_instance || !p_tally) return false;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    slot->tally = *p_tally;
    avx_cond_broadcast(&slot->tally_cond);
    if (slot->web_server) {
        avx_web_server_broadcast_tally((avx_web_server_t*)slot->web_server, &slot->tally);
    }

    char tally_xml[256];
    int len = snprintf(tally_xml, sizeof(tally_xml),
        "<%s on_program=\"%s\" on_preview=\"%s\" on_recording=\"%s\"/>\n",
        slot->wire_profile.tally_tag ? slot->wire_profile.tally_tag : OAVX_WIRE_TALLY_TAG,
        (p_tally->flags & OAVX_TALLY_PROGRAM) ? "true" : "false",
        (p_tally->flags & OAVX_TALLY_PREVIEW) ? "true" : "false",
        (p_tally->flags & OAVX_TALLY_RECORDING) ? "true" : "false");
    avx_mutex_unlock(&slot->lock);

    oavx_metadata_frame_t meta = {
        .length = (size_t)len,
        .pts_ns = 0,
        .data = tally_xml
    };
    avx_tx_send_metadata(p_instance, &meta);
    return true;
}

void avx_tx_clear_connection_metadata(avx_tx_instance_t p_instance) {
    if (!p_instance) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    slot->num_conn_metadata = 0;
    avx_mutex_unlock(&slot->lock);
}

void avx_tx_add_connection_metadata(avx_tx_instance_t p_instance, const oavx_metadata_frame_t* metadata) {
    if (!p_instance || !metadata || !metadata->data) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    if (slot->num_conn_metadata < 16) {
        snprintf(slot->conn_metadata[slot->num_conn_metadata],
                 sizeof(slot->conn_metadata[0]), "%s", metadata->data);
        slot->num_conn_metadata++;
    }
    avx_mutex_unlock(&slot->lock);
}

/*
 * Configures secondary backup endpoint and broadcasts failover XML descriptor
 * to all connected receivers for instant client-side switchover on disconnect.
 */
void avx_tx_set_failover(avx_tx_instance_t p_instance, const oavx_endpoint_t* p_failover_source) {
    if (!p_instance) return;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);

    /* Remove any existing failover metadata from conn_metadata */
    size_t w = 0;
    for (size_t r = 0; r < slot->num_conn_metadata; ++r) {
        if (strstr(slot->conn_metadata[r], "failover") == NULL) {
            if (w != r) {
                snprintf(slot->conn_metadata[w], sizeof(slot->conn_metadata[0]),
                         "%s", slot->conn_metadata[r]);
            }
            w++;
        }
    }
    slot->num_conn_metadata = w;

    if (p_failover_source && (p_failover_source->address != NULL || p_failover_source->name != NULL)) {
        if (p_failover_source->name) {
            snprintf(slot->failover_name, sizeof(slot->failover_name), "%s", p_failover_source->name);
        } else {
            slot->failover_name[0] = '\0';
        }
        if (p_failover_source->address) {
            snprintf(slot->failover_url, sizeof(slot->failover_url), "%s", p_failover_source->address);
        } else {
            slot->failover_url[0] = '\0';
        }
        slot->failover_source.name = slot->failover_name;
        slot->failover_source.address = slot->failover_url;
        slot->has_failover = true;

        /* Build XML failover metadata packet */
        char meta_xml[512];
        snprintf(meta_xml, sizeof(meta_xml),
                 "<openavx_failover name=\"%s\" address=\"%s\"/>\n",
                 slot->failover_name, slot->failover_url);

        /* Add to conn_metadata for future receivers */
        if (slot->num_conn_metadata < 16) {
            snprintf(slot->conn_metadata[slot->num_conn_metadata],
                     sizeof(slot->conn_metadata[0]), "%s", meta_xml);
            slot->num_conn_metadata++;
        }

        /* Broadcast immediately to all currently connected clients */
        size_t mlen = strlen(meta_xml);
        AVX_PACKED_START
        struct {
            uint32_t magic;
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED mhdr;
        AVX_PACKED_END
        mhdr.magic = slot->wire_profile.magic_metadata;
        mhdr.timecode = 0;
        mhdr.payload_len = (uint32_t)mlen;

        for (int i = 0; i < slot->max_clients; ++i) {
            if (slot->clients[i].fd != AVX_INVALID_SOCKET) {
                send(slot->clients[i].fd, (const char*)&mhdr, sizeof(mhdr), MSG_NOSIGNAL);
                send(slot->clients[i].fd, meta_xml, mlen, MSG_NOSIGNAL);
            }
        }
    } else {
        slot->has_failover = false;
        slot->failover_name[0] = '\0';
        slot->failover_url[0] = '\0';
    }

    avx_mutex_unlock(&slot->lock);
}

int avx_tx_get_connection_count(avx_tx_instance_t p_instance, uint32_t timeout_in_ms) {
    if (!p_instance) return 0;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    uint32_t elapsed = 0;
    while (1) {
        avx_mutex_lock(&slot->lock);
        int active = slot->active_clients;
        avx_mutex_unlock(&slot->lock);
        if (active > 0 || elapsed >= timeout_in_ms) {
            return active;
        }
        avx_sleep_ms(5);
        elapsed += 5;
    }
}

OPENAVX_API const oavx_endpoint_t* avx_tx_get_source(avx_tx_instance_t p_instance) {
    if (!p_instance) return NULL;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    return &slot->source_info;
}

oavx_frame_type_t avx_tx_capture_metadata(
    avx_tx_instance_t p_instance,
    oavx_metadata_frame_t* metadata,
    uint32_t timeout_in_ms
) {
    if (!p_instance || !metadata) return OAVX_FRAME_TYPE_ERROR;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;

    uint32_t elapsed = 0;
    while (elapsed <= timeout_in_ms) {
        avx_mutex_lock(&slot->lock);
        if (slot->has_new_metadata) {
            size_t len = strlen(slot->last_metadata);
            char* buf = (char*)malloc(len + 1);
            if (buf) {
                memcpy(buf, slot->last_metadata, len + 1);
                metadata->data = buf;
                metadata->length = len;
                metadata->pts_ns = 0;
                slot->has_new_metadata = false;
                avx_mutex_unlock(&slot->lock);
                return OAVX_FRAME_TYPE_METADATA;
            }
        }
        avx_mutex_unlock(&slot->lock);

        if (timeout_in_ms == 0) break;
        avx_sleep_ms(5);
        elapsed += 5;
    }

    return OAVX_FRAME_TYPE_NONE;
}

void avx_tx_free_metadata(
    avx_tx_instance_t p_instance,
    const oavx_metadata_frame_t* metadata
) {
    (void)p_instance;
    if (!metadata) return;
    if (metadata->data) {
        free(metadata->data);
    }
}

OPENAVX_API bool avx_tx_get_telemetry(avx_tx_instance_t p_instance, oavx_telemetry_t *out) {
    if (!p_instance || !out) return false;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    memset(out, 0, sizeof(*out));

    avx_mutex_lock(&slot->lock);

    int64_t now_ns = avx_time_ns();
    float fps = slot->telem.instant_fps;
    float mbps = slot->telem.instant_mbps;

    if (slot->telem.last_window_time_ns > 0 && (now_ns - slot->telem.last_window_time_ns) > 1000000000LL && slot->telem.window_frames == 0) {
        fps = 0.0f;
        mbps = 0.0f;
    } else if (fps == 0.0f && slot->telem.window_frames > 0 && slot->telem.last_window_time_ns > 0) {
        int64_t elapsed_ns = now_ns - slot->telem.last_window_time_ns;
        if (elapsed_ns >= 10000000LL) {
            float elapsed_sec = (float)elapsed_ns / 1000000000.0f;
            fps = (float)slot->telem.window_frames / elapsed_sec;
            mbps = ((float)slot->telem.window_bytes * 8.0f) / (elapsed_sec * 1000000.0f);
        }
    }

    uint64_t total_dropped = 0;
    uint32_t max_congestion = 0;
    for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
        if (slot->clients[i].fd != AVX_INVALID_SOCKET) {
            total_dropped += slot->clients[i].dropped_frames;
            if (slot->clients[i].congestion_level > max_congestion) {
                max_congestion = slot->clients[i].congestion_level;
            }
        }
    }

    out->video_frames_total    = slot->video_frames_sent;
    out->audio_frames_total    = slot->audio_frames_sent;
    out->metadata_frames_total = slot->metadata_frames_sent;
    out->dropped_frames_total  = total_dropped;
    out->fps                   = fps;
    out->bitrate_mbps          = mbps;
    out->total_wire_bytes      = slot->telem.total_bytes;
    out->inter_frame_jitter_ms = 0.0f;
    out->fourcc                = slot->telem.last_fourcc;
    out->width                 = slot->telem.last_width;
    out->height                = slot->telem.last_height;
    out->is_connected          = (slot->active_clients > 0);
    out->congestion_level      = max_congestion;

    if (slot->url[0]) {
        snprintf(out->remote_endpoint, sizeof(out->remote_endpoint), "%.127s", slot->url);
    } else {
        snprintf(out->remote_endpoint, sizeof(out->remote_endpoint), "0.0.0.0:%u", slot->port);
    }

    avx_socket_t client_sock = slot->tcp_client_fd;
    void *rudp_s = slot->rudp_sender;
    oavx_transport_e tx_transport = slot->transport;
    avx_mutex_unlock(&slot->lock);

    if (tx_transport == OAVX_TRANSPORT_RUDP && rudp_s) {
        avx_rudp_telemetry_t r_telem;
        if (avx_udp_rudp_sender_get_telemetry((avx_udp_rudp_sender_t*)rudp_s, &r_telem)) {
            out->kernel_rtt_ms = (float)r_telem.srtt_ms;
            out->tcp_retransmissions = (uint32_t)r_telem.packets_retransmitted;
        }
    } else if (client_sock != AVX_INVALID_SOCKET) {
        avx_socket_get_diagnostics(client_sock, &out->kernel_rtt_ms, &out->tcp_retransmissions,
                                   &out->socket_send_q_bytes, &out->socket_recv_q_bytes);
    }

    return true;
}

OPENAVX_API struct avx_rate_controller_t* avx_tx_get_rate_controller(avx_tx_instance_t p_instance) {
    if (!p_instance) return NULL;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    return slot->rate_ctl;
}

OPENAVX_API int avx_tx_get_wan_client_count(avx_tx_instance_t p_instance) {
    if (!p_instance) return 0;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    int count = slot->wan_client_count;
    avx_mutex_unlock(&slot->lock);
    return count;
}

OPENAVX_API uint32_t avx_tx_get_wan_codec_fourcc(avx_tx_instance_t p_instance) {
    if (!p_instance) return 0;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    uint32_t fourcc = slot->wan_codec_fourcc;
    avx_mutex_unlock(&slot->lock);
    return fourcc;
}

OPENAVX_API bool avx_tx_is_wan_encoder_active(avx_tx_instance_t p_instance) {
    if (!p_instance) return false;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    bool active = (slot->wan_encoder != NULL);
    avx_mutex_unlock(&slot->lock);
    return active;
}

/*
 * Sets transmission profile for a specific client (e.g. "wan" for secondary
 * hardware-compressed simulcast). Spawns or tears down WAN encoder on demand.
 */
OPENAVX_API bool avx_tx_set_client_profile(avx_tx_instance_t p_instance, int client_idx, const char *profile) {
    if (!p_instance || !profile || client_idx < 0 || client_idx >= AVX_MAX_CLIENTS) return false;
    avx_send_ctx_t* slot = (avx_send_ctx_t*)p_instance;
    avx_mutex_lock(&slot->lock);
    if (slot->clients[client_idx].fd == AVX_INVALID_SOCKET) {
        avx_mutex_unlock(&slot->lock);
        return false;
    }

    bool is_wan = (strcasecmp(profile, "wan") == 0);
    if (is_wan && !slot->clients[client_idx].is_wan_registered) {
        slot->clients[client_idx].is_wan_client = true;
        slot->clients[client_idx].is_wan_registered = true;
        slot->wan_client_count++;
        if (slot->wan_codec_fourcc == 0) {
            slot->wan_codec_fourcc = slot->clients[client_idx].negotiated_codec_fourcc
                ? slot->clients[client_idx].negotiated_codec_fourcc
                : OAVX_FOURCC_VIDEO_TYPE_H264;
        }
        if (slot->wan_encoder) {
            avx_video_encoder_request_keyframe(slot->wan_encoder);
        }
    } else if (!is_wan && slot->clients[client_idx].is_wan_registered) {
        slot->clients[client_idx].is_wan_client = false;
        slot->clients[client_idx].is_wan_registered = false;
        if (slot->wan_client_count > 0) slot->wan_client_count--;
        if (slot->wan_client_count == 0 && slot->wan_encoder) {
            avx_video_encoder_destroy(slot->wan_encoder);
            slot->wan_encoder = NULL;
            slot->wan_enc_width = 0;
            slot->wan_enc_height = 0;
            slot->wan_enc_fourcc = 0;
        }
        if (slot->video_encoder) {
            avx_video_encoder_request_keyframe(slot->video_encoder);
        }
    }
    avx_mutex_unlock(&slot->lock);
    return true;
}
