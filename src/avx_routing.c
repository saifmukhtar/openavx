/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_routing.c - Virtual matrix routing subsystem
 */

#define _GNU_SOURCE
#include "avx_platform.h"
#include "avx_internal.h"
#include "avx_routing.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/*
 * Background routing worker pumping video, audio, and metadata frames from the routed
 * source receiver to transmitter while forwarding downstream tally state upstream.
 */
static void* avx_routing_worker(void* arg) {
    avx_routing_ctx_t* slot = (avx_routing_ctx_t*)arg;

    while (1) {
        avx_mutex_lock(&slot->lock);
        while (!slot->stop_requested && !slot->has_source) {
            avx_cond_wait(&slot->source_cond, &slot->lock);
        }
        if (slot->stop_requested) {
            avx_mutex_unlock(&slot->lock);
            break;
        }

        avx_rx_instance_t rx = slot->p_recv;
        avx_tx_instance_t tx = slot->p_send;
        avx_mutex_unlock(&slot->lock);

        if (!rx || !tx) {
            avx_sleep_ms(10);
            continue;
        }

        /* Forward any downstream tally changes upstream to active camera */
        oavx_tally_t cur_tally;
        if (avx_tx_get_tally(tx, &cur_tally, 0)) {
            avx_mutex_lock(&slot->lock);
            if (cur_tally.flags != slot->last_tally.flags) {
                slot->last_tally = cur_tally;
                avx_mutex_unlock(&slot->lock);
                avx_rx_set_tally(rx, &cur_tally);
            } else {
                avx_mutex_unlock(&slot->lock);
            }
        }

        oavx_video_frame_t video;
        oavx_audio_frame_t audio;
        oavx_metadata_frame_t meta;

        oavx_frame_type_t ftype = avx_rx_receive(rx, &video, &audio, &meta, 15);

        avx_mutex_lock(&slot->lock);
        bool still_valid = (!slot->stop_requested && slot->has_source && slot->p_recv == rx);
        avx_mutex_unlock(&slot->lock);

        if (!still_valid) {
            if (ftype == OAVX_FRAME_TYPE_VIDEO) avx_rx_free_video(rx, &video);
            else if (ftype == OAVX_FRAME_TYPE_AUDIO) avx_rx_free_audio(rx, &audio);
            else if (ftype == OAVX_FRAME_TYPE_METADATA) avx_rx_free_metadata(rx, &meta);
            continue;
        }

        if (ftype == OAVX_FRAME_TYPE_VIDEO) {
            avx_tx_send_video(tx, &video);
            avx_rx_free_video(rx, &video);
        } else if (ftype == OAVX_FRAME_TYPE_AUDIO) {
            avx_tx_send_audio(tx, &audio);
            avx_rx_free_audio(rx, &audio);
        } else if (ftype == OAVX_FRAME_TYPE_METADATA) {
            avx_tx_send_metadata(tx, &meta);
            avx_rx_free_metadata(rx, &meta);
        }
    }
    return NULL;
}

avx_router_instance_t avx_router_create(const avx_router_config_t* p_create_settings) {
    avx_mutex_lock(&g_avx_state.global_lock);
    if (!g_avx_state.initialized) {
        avx_mutex_unlock(&g_avx_state.global_lock);
        avx_engine_initialize();
        avx_mutex_lock(&g_avx_state.global_lock);
    }

    avx_routing_ctx_t* slot = NULL;
    for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
        if (!g_avx_state.routing_slots[i].in_use) {
            slot = &g_avx_state.routing_slots[i];
            memset(slot, 0, sizeof(*slot));
            slot->in_use = true;
            break;
        }
    }
    avx_mutex_unlock(&g_avx_state.global_lock);

    if (!slot) return NULL;

    avx_mutex_init(&slot->lock);
    avx_cond_init(&slot->source_cond);
    slot->p_recv = NULL;
    slot->has_source = false;
    slot->stop_requested = false;
    slot->worker_running = false;
    slot->last_tally.flags = OAVX_TALLY_NONE;

    avx_tx_config_t default_settings = {
        .name = "OpenAVX (Routing)",
        .groups = NULL,
        .clock_video = true,
        .clock_audio = true
    };
    avx_tx_config_t send_settings;
    memset(&send_settings, 0, sizeof(send_settings));
    if (p_create_settings) {
        send_settings.name = p_create_settings->name ? p_create_settings->name : default_settings.name;
        send_settings.groups = p_create_settings->groups;
        send_settings.clock_video = true;
        send_settings.clock_audio = true;
    } else {
        send_settings = default_settings;
    }
    slot->p_send = avx_tx_create(&send_settings);
    if (!slot->p_send) {
        avx_mutex_destroy(&slot->lock);
        avx_cond_destroy(&slot->source_cond);
        avx_mutex_lock(&g_avx_state.global_lock);
        slot->in_use = false;
        avx_mutex_unlock(&g_avx_state.global_lock);
        return NULL;
    }

    slot->worker_running = true;
    if (avx_thread_create(&slot->worker_thread, avx_routing_worker, slot) != 0) {
        slot->worker_running = false;
        avx_tx_destroy(slot->p_send);
        slot->p_send = NULL;
        avx_mutex_destroy(&slot->lock);
        avx_cond_destroy(&slot->source_cond);
        avx_mutex_lock(&g_avx_state.global_lock);
        slot->in_use = false;
        avx_mutex_unlock(&g_avx_state.global_lock);
        return NULL;
    }

    return (avx_router_instance_t)slot;
}

void avx_router_destroy(avx_router_instance_t p_instance) {
    if (!p_instance) return;
    avx_routing_ctx_t* slot = (avx_routing_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    slot->stop_requested = true;
    avx_cond_broadcast(&slot->source_cond);
    avx_mutex_unlock(&slot->lock);

    if (slot->worker_running) {
        avx_thread_join(slot->worker_thread);
        slot->worker_running = false;
    }

    avx_mutex_lock(&slot->lock);
    avx_rx_instance_t rx = slot->p_recv;
    slot->p_recv = NULL;
    avx_tx_instance_t tx = slot->p_send;
    slot->p_send = NULL;
    avx_mutex_unlock(&slot->lock);

    if (rx) {
        oavx_tally_t off_tally = { .flags = OAVX_TALLY_NONE };
        avx_rx_set_tally(rx, &off_tally);
        avx_rx_destroy(rx);
    }
    if (tx) {
        avx_tx_destroy(tx);
    }

    avx_mutex_destroy(&slot->lock);
    avx_cond_destroy(&slot->source_cond);

    avx_mutex_lock(&g_avx_state.global_lock);
    slot->in_use = false;
    avx_mutex_unlock(&g_avx_state.global_lock);
}

/* Seamlessly switches routed input source without disconnecting downstream transmitter subscribers. */
bool avx_router_route(avx_router_instance_t p_instance, const oavx_endpoint_t* p_source) {
    if (!p_instance) return false;
    avx_routing_ctx_t* slot = (avx_routing_ctx_t*)p_instance;

    if (!p_source || (!p_source->name && !p_source->address)) {
        return avx_router_clear(p_instance);
    }

    /* Check if already routed to this exact source */
    avx_mutex_lock(&slot->lock);
    if (slot->has_source) {
        bool match = true;
        if (p_source->name && strcmp(slot->current_name, p_source->name) != 0) match = false;
        if (p_source->address && strcmp(slot->current_url, p_source->address) != 0) match = false;
        if (match) {
            avx_mutex_unlock(&slot->lock);
            return true;
        }
    }
    avx_mutex_unlock(&slot->lock);

    /* Create new receiver connecting to the new source */
    avx_rx_config_t recv_create = {
        .source_to_connect_to = *p_source,
        .color_format = OAVX_COLOR_FORMAT_FASTEST,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .allow_video_fields = false,
        .name = "avx_router_rx"
    };
    avx_rx_instance_t new_recv = avx_rx_create(&recv_create);
    if (!new_recv) return false;

    /* Seamlessly swap receivers */
    avx_mutex_lock(&slot->lock);
    avx_rx_instance_t old_recv = slot->p_recv;
    slot->p_recv = new_recv;
    slot->has_source = true;
    if (p_source->name) {
        strncpy(slot->current_name, p_source->name, sizeof(slot->current_name) - 1);
        slot->current_name[sizeof(slot->current_name) - 1] = '\0';
    } else {
        slot->current_name[0] = '\0';
    }
    if (p_source->address) {
        strncpy(slot->current_url, p_source->address, sizeof(slot->current_url) - 1);
        slot->current_url[sizeof(slot->current_url) - 1] = '\0';
    } else {
        slot->current_url[0] = '\0';
    }
    slot->current_source.name = slot->current_name;
    slot->current_source.address = slot->current_url;
    oavx_tally_t cur_tally = slot->last_tally;
    avx_cond_broadcast(&slot->source_cond);
    avx_mutex_unlock(&slot->lock);

    /* Disconnect old source cleanly */
    if (old_recv) {
        oavx_tally_t off_tally = { .flags = OAVX_TALLY_NONE };
        avx_rx_set_tally(old_recv, &off_tally);
        avx_rx_destroy(old_recv);
    }

    /* Pass active downstream tally to the newly connected upstream camera */
    if (cur_tally.flags & (OAVX_TALLY_PROGRAM | OAVX_TALLY_PREVIEW)) {
        avx_rx_set_tally(new_recv, &cur_tally);
    }

    return true;
}

/* Clears routed source and transitions router to idle standby awaiting new input selection. */
bool avx_router_clear(avx_router_instance_t p_instance) {
    if (!p_instance) return false;
    avx_routing_ctx_t* slot = (avx_routing_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    avx_rx_instance_t old_recv = slot->p_recv;
    slot->p_recv = NULL;
    slot->has_source = false;
    slot->current_name[0] = '\0';
    slot->current_url[0] = '\0';
    slot->current_source.name = NULL;
    slot->current_source.address = NULL;
    avx_mutex_unlock(&slot->lock);

    if (old_recv) {
        oavx_tally_t off_tally = { .flags = OAVX_TALLY_NONE };
        avx_rx_set_tally(old_recv, &off_tally);
        avx_rx_destroy(old_recv);
    }

    return true;
}

oavx_endpoint_t avx_router_get_source(avx_router_instance_t p_instance) {
    oavx_endpoint_t src = { .name = NULL, .address = NULL, .port = 0, .group = NULL };
    if (!p_instance) return src;
    avx_routing_ctx_t* slot = (avx_routing_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    if (slot->p_send) {
        avx_send_ctx_t* send = (avx_send_ctx_t*)slot->p_send;
        src.name = send->name;
        static AVX_THREAD_LOCAL char url_buf[128];
        snprintf(url_buf, sizeof(url_buf), "127.0.0.1:%u", send->port);
        src.address = url_buf;
        src.port = send->port;
        src.group = send->groups;
    }
    avx_mutex_unlock(&slot->lock);
    return src;
}
