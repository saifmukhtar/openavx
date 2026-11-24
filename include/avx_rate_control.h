/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_rate_control.h - Adaptive Bitrate & Dynamic FEC Controller (GCC / BBR Hybrid)
 */

#ifndef AVX_RATE_CONTROL_H
#define AVX_RATE_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "openavx.h"
#include "avx_platform.h"
#include "avx_fec.h"
#include "avx_udp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AVX_WAN_CONG_NORMAL   = 0,  /* Network clear: nominal queuing delay and low packet loss */
    AVX_WAN_CONG_PRESSURE = 1,  /* Queuing delay rising towards buffer limit */
    AVX_WAN_CONG_OVERUSE  = 2   /* Bottleneck saturated: severe queuing delay or packet loss */
} avx_wan_congestion_state_t;

typedef struct {
    uint32_t min_bitrate_bps;    /* Minimum allowable bitrate (default: 2,000,000 = 2 Mbps) */
    uint32_t max_bitrate_bps;    /* Maximum allowable bitrate (default: 150,000,000 = 150 Mbps) */
    uint32_t start_bitrate_bps;  /* Initial starting bitrate (default: 50,000,000 = 50 Mbps) */
    float    target_fps;         /* Target framerate (default: 60.0f) */
    uint8_t  min_quality;        /* Lowest SpeedHQ quality level (default: 30) */
    uint8_t  max_quality;        /* Highest SpeedHQ quality level (default: 96) */
    bool     fec_auto_tune;      /* Automatically tune FEC matrix density (default: true) */
} avx_rate_controller_config_t;

typedef struct avx_rate_controller_t avx_rate_controller_t;

/**
 * Creates an Adaptive Bitrate & FEC Controller instance.
 * If cfg is NULL, recommended defaults are applied.
 */
OPENAVX_API avx_rate_controller_t* avx_rate_controller_create(const avx_rate_controller_config_t* cfg);

/**
 * Destroys the rate controller instance and releases resources.
 */
OPENAVX_API void avx_rate_controller_destroy(avx_rate_controller_t* c);

/**
 * Resets controller internal state (filters, minimum RTT, and telemetry history).
 */
OPENAVX_API void avx_rate_controller_reset(avx_rate_controller_t* c);

/**
 * Ingests live RUDP telemetry and updates the congestion model, target bitrate,
 * recommended SpeedHQ quality, and FEC matrix density.
 *
 * @param c Rate controller instance.
 * @param telem Pointer to current RUDP telemetry snapshot.
 * @param now_ms Monotonic timestamp in milliseconds.
 */
OPENAVX_API void avx_rate_controller_update_telemetry(
    avx_rate_controller_t* c,
    const avx_rudp_telemetry_t* telem,
    int64_t now_ms
);

/**
 * Retrieves the recommended video bitrate target in bits per second.
 */
OPENAVX_API uint32_t avx_rate_controller_get_target_bitrate(const avx_rate_controller_t* c);

/**
 * Retrieves the recommended SpeedHQ quality level (range: min_quality .. max_quality).
 */
OPENAVX_API uint8_t avx_rate_controller_get_quality(const avx_rate_controller_t* c);

/**
 * Retrieves the dynamically tuned FEC matrix configuration.
 */
OPENAVX_API void avx_rate_controller_get_fec_config(const avx_rate_controller_t* c, avx_fec_config_t* out_fec);

/**
 * Retrieves the recommended temporal decimation cadence (1 = full fps, 2 = half fps, etc.).
 */
OPENAVX_API uint32_t avx_rate_controller_get_cadence(const avx_rate_controller_t* c);

/**
 * Retrieves the current WAN congestion assessment state.
 */
OPENAVX_API avx_wan_congestion_state_t avx_rate_controller_get_congestion_state(const avx_rate_controller_t* c);

/**
 * Applies the current dynamically tuned FEC parameters directly to an RUDP sender session.
 */
OPENAVX_API void avx_rate_controller_apply_to_sender(const avx_rate_controller_t* c, avx_udp_rudp_sender_t* s);

#ifdef __cplusplus
}
#endif

#endif /* AVX_RATE_CONTROL_H */
