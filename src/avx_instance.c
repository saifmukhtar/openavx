/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_instance.c - Library initialization and subsystem lifecycle
 */

#include "avx_internal.h"
#include "avx_video_codec.h"
#include <string.h>
#include <stdio.h>

avx_global_state_t g_avx_state = {
    .global_lock = AVX_MUTEX_INITIALIZER,
    .initialized = false
};

/* Initializes platform socket subsystems and global session slot tables. */
OPENAVX_API bool avx_engine_initialize(void) {
    avx_platform_sockets_init();
    avx_mutex_lock(&g_avx_state.global_lock);
    if (!g_avx_state.initialized) {
        memset(g_avx_state.find_slots, 0, sizeof(g_avx_state.find_slots));
        memset(g_avx_state.send_slots, 0, sizeof(g_avx_state.send_slots));
        memset(g_avx_state.recv_slots, 0, sizeof(g_avx_state.recv_slots));
        memset(g_avx_state.routing_slots, 0, sizeof(g_avx_state.routing_slots));
        g_avx_state.initialized = true;
        avx_config_init();
    }
    avx_mutex_unlock(&g_avx_state.global_lock);
    return true;
}

/* Destroys active sessions outside global_lock to prevent lock inversion deadlocks. */
OPENAVX_API void avx_engine_destroy(void) {
    avx_mutex_lock(&g_avx_state.global_lock);
    if (!g_avx_state.initialized) {
        avx_mutex_unlock(&g_avx_state.global_lock);
        return;
    }
    g_avx_state.initialized = false;

    /* Snapshot active slots under global_lock, then destroy them outside global_lock
     * to eliminate recursive mutex self-deadlocks and lock hierarchy inversions */
    avx_find_ctx_t* find_to_destroy[AVX_MAX_SESSIONS];
    int n_find = 0;
    for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
        if (g_avx_state.find_slots[i].in_use) {
            find_to_destroy[n_find++] = &g_avx_state.find_slots[i];
        }
    }

    avx_send_ctx_t* send_to_destroy[AVX_MAX_SESSIONS];
    int n_send = 0;
    for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
        if (g_avx_state.send_slots[i].in_use) {
            send_to_destroy[n_send++] = &g_avx_state.send_slots[i];
        }
    }

    avx_recv_ctx_t* recv_to_destroy[AVX_MAX_SESSIONS];
    int n_recv = 0;
    for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
        if (g_avx_state.recv_slots[i].in_use) {
            recv_to_destroy[n_recv++] = &g_avx_state.recv_slots[i];
        }
    }

    avx_routing_ctx_t* routing_to_destroy[AVX_MAX_SESSIONS];
    int n_routing = 0;
    for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
        if (g_avx_state.routing_slots[i].in_use) {
            routing_to_destroy[n_routing++] = &g_avx_state.routing_slots[i];
        }
    }

    avx_mutex_unlock(&g_avx_state.global_lock);

    /* Now destroy all active sessions safely with global_lock released */
    for (int i = 0; i < n_routing; ++i) {
        avx_router_destroy(routing_to_destroy[i]);
    }
    for (int i = 0; i < n_recv; ++i) {
        avx_rx_destroy(recv_to_destroy[i]);
    }
    for (int i = 0; i < n_send; ++i) {
        avx_tx_destroy(send_to_destroy[i]);
    }
    for (int i = 0; i < n_find; ++i) {
        avx_find_destroy(find_to_destroy[i]);
    }

    avx_video_codec_destroy();
    avx_config_cleanup();
    avx_platform_sockets_cleanup();
}

const char* avx_engine_version(void) {
    return OPENAVX_VERSION;
}

bool avx_engine_is_supported_cpu(void) {
    /* Standard x86_64 CPU is universally supported */
    return true;
}
