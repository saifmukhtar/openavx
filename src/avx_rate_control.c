/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_rate_control.c - Adaptive Bitrate & Dynamic FEC Controller (GCC / BBR Hybrid)
 */

#include "avx_rate_control.h"
#include <stdlib.h>
#include <string.h>

struct avx_rate_controller_t {
    avx_rate_controller_config_t cfg;
    avx_mutex_t lock;

    /* Live link telemetry tracking */
    uint32_t srtt_ms;
    uint32_t rttvar_ms;
    uint32_t min_rtt_ms;
    int64_t  min_rtt_ts_ms;

    /* Previous telemetry snapshot for differential calculation */
    uint64_t prev_packets_sent;
    uint64_t prev_packets_lost;
    uint64_t prev_packets_retrans;
    int64_t  last_update_ms;

    /* Filtered metrics */
    float    loss_ratio;         /* Smoothed packet loss ratio (0.0 .. 1.0) */
    float    queuing_delay_ms;   /* srtt - min_rtt */

    /* Output recommendations */
    uint32_t current_bitrate_bps;
    uint8_t  current_quality;
    uint32_t cadence;
    avx_wan_congestion_state_t congestion_state;
    avx_fec_config_t current_fec;
};

/*
 * Initializes hybrid GCC/BBR rate controller tracking minimum RTT, queuing delay
 * gradients, and packet loss ratio to compute optimal bitrate, quality, and FEC.
 */
avx_rate_controller_t* avx_rate_controller_create(const avx_rate_controller_config_t* cfg) {
    avx_rate_controller_t* c = (avx_rate_controller_t*)calloc(1, sizeof(avx_rate_controller_t));
    if (!c) return NULL;

    if (cfg) {
        c->cfg = *cfg;
    } else {
        c->cfg.min_bitrate_bps = 2000000;    /* 2 Mbps */
        c->cfg.max_bitrate_bps = 150000000;  /* 150 Mbps */
        c->cfg.start_bitrate_bps = 50000000; /* 50 Mbps */
        c->cfg.target_fps = 60.0f;
        c->cfg.min_quality = 30;
        c->cfg.max_quality = 96;
        c->cfg.fec_auto_tune = true;
    }

    if (c->cfg.min_bitrate_bps == 0) c->cfg.min_bitrate_bps = 1000000;
    if (c->cfg.max_bitrate_bps < c->cfg.min_bitrate_bps) c->cfg.max_bitrate_bps = c->cfg.min_bitrate_bps * 2;
    if (c->cfg.start_bitrate_bps < c->cfg.min_bitrate_bps) c->cfg.start_bitrate_bps = c->cfg.min_bitrate_bps;
    if (c->cfg.start_bitrate_bps > c->cfg.max_bitrate_bps) c->cfg.start_bitrate_bps = c->cfg.max_bitrate_bps;
    if (c->cfg.target_fps <= 0.0f) c->cfg.target_fps = 60.0f;
    if (c->cfg.min_quality == 0) c->cfg.min_quality = 20;
    if (c->cfg.max_quality < c->cfg.min_quality) c->cfg.max_quality = 96;

    avx_mutex_init(&c->lock);
    avx_rate_controller_reset(c);
    return c;
}

void avx_rate_controller_destroy(avx_rate_controller_t* c) {
    if (!c) return;
    avx_mutex_destroy(&c->lock);
    free(c);
}

void avx_rate_controller_reset(avx_rate_controller_t* c) {
    if (!c) return;
    avx_mutex_lock(&c->lock);
    c->srtt_ms = 20;
    c->rttvar_ms = 10;
    c->min_rtt_ms = 0;
    c->min_rtt_ts_ms = 0;
    c->prev_packets_sent = 0;
    c->prev_packets_lost = 0;
    c->prev_packets_retrans = 0;
    c->last_update_ms = 0;
    c->loss_ratio = 0.0f;
    c->queuing_delay_ms = 0.0f;

    c->current_bitrate_bps = c->cfg.start_bitrate_bps;
    float rho = (float)(c->current_bitrate_bps - c->cfg.min_bitrate_bps) /
                (float)(c->cfg.max_bitrate_bps - c->cfg.min_bitrate_bps);
    if (rho < 0.0f) rho = 0.0f;
    if (rho > 1.0f) rho = 1.0f;
    c->current_quality = (uint8_t)(c->cfg.min_quality + (uint8_t)(rho * (float)(c->cfg.max_quality - c->cfg.min_quality)));
    c->cadence = 1;
    c->congestion_state = AVX_WAN_CONG_NORMAL;

    c->current_fec.enabled = true;
    c->current_fec.matrix_d = 10;
    c->current_fec.matrix_l = 5;
    c->current_fec.enable_row_fec = false;
    avx_mutex_unlock(&c->lock);
}

/* Analyzes round-trip link feedback to adapt video bitrate, SpeedHQ quality, and FEC matrix dimensions. */
void avx_rate_controller_update_telemetry(
    avx_rate_controller_t* c,
    const avx_rudp_telemetry_t* telem,
    int64_t now_ms
) {
    if (!c || !telem) return;

    avx_mutex_lock(&c->lock);

    c->srtt_ms = telem->srtt_ms;
    c->rttvar_ms = telem->rttvar_ms;

    /* 1. Track minimum baseline RTT (physical propagation delay without queue bufferbloat) */
    if (telem->srtt_ms > 0) {
        if (c->min_rtt_ms == 0 || telem->srtt_ms < c->min_rtt_ms ||
            (c->min_rtt_ts_ms > 0 && (now_ms - c->min_rtt_ts_ms) > 10000)) {
            c->min_rtt_ms = telem->srtt_ms;
            c->min_rtt_ts_ms = now_ms;
        }
    } else if (c->min_rtt_ms == 0) {
        c->min_rtt_ms = 20;
    }

    /* 2. Compute queuing delay (gradient of RTT relative to minimum baseline) */
    float q_delay = (float)telem->srtt_ms - (float)c->min_rtt_ms;
    if (q_delay < 0.0f) q_delay = 0.0f;
    c->queuing_delay_ms = 0.7f * c->queuing_delay_ms + 0.3f * q_delay;

    /* 3. Compute differential packet loss ratio over elapsed window */
    uint64_t delta_sent = (telem->packets_sent > c->prev_packets_sent)
                        ? (telem->packets_sent - c->prev_packets_sent) : 0;
    uint64_t delta_lost = ((telem->packets_lost > c->prev_packets_lost)
                        ? (telem->packets_lost - c->prev_packets_lost) : 0) +
                          ((telem->packets_retransmitted > c->prev_packets_retrans)
                        ? (telem->packets_retransmitted - c->prev_packets_retrans) : 0);

    c->prev_packets_sent = telem->packets_sent;
    c->prev_packets_lost = telem->packets_lost;
    c->prev_packets_retrans = telem->packets_retransmitted;

    float sample_loss = 0.0f;
    if (delta_sent + delta_lost > 0) {
        sample_loss = (float)delta_lost / (float)(delta_sent + delta_lost);
    }
    c->loss_ratio = 0.6f * c->loss_ratio + 0.4f * sample_loss;

    /* 4. GCC Congestion Assessment: delay threshold gamma adaptive to RTT */
    float gamma = 0.20f * (float)c->min_rtt_ms;
    if (gamma < 15.0f) gamma = 15.0f;
    if (gamma > 50.0f) gamma = 50.0f;

    if (c->loss_ratio >= 0.05f || c->queuing_delay_ms >= gamma) {
        c->congestion_state = AVX_WAN_CONG_OVERUSE;
    } else if (c->loss_ratio >= 0.02f || c->queuing_delay_ms >= (0.6f * gamma)) {
        c->congestion_state = AVX_WAN_CONG_PRESSURE;
    } else {
        c->congestion_state = AVX_WAN_CONG_NORMAL;
    }

    /* 5. Target Bitrate Adaptation */
    int64_t elapsed_ms = (c->last_update_ms > 0 && now_ms > c->last_update_ms)
                       ? (now_ms - c->last_update_ms) : 100;
    if (elapsed_ms > 1000) elapsed_ms = 1000;
    if (elapsed_ms < 20) elapsed_ms = 20;

    if (c->congestion_state == AVX_WAN_CONG_OVERUSE) {
        /* Multiplicative decrease */
        float backoff = (c->loss_ratio > 0.10f) ? 0.65f : 0.80f;
        c->current_bitrate_bps = (uint32_t)((float)c->current_bitrate_bps * backoff);
    } else if (c->congestion_state == AVX_WAN_CONG_PRESSURE) {
        /* Mild throttle */
        c->current_bitrate_bps = (uint32_t)((float)c->current_bitrate_bps * 0.95f);
    } else {
        /* Additive increase: probe upward when link is clean */
        uint32_t add_step = (uint32_t)((float)c->current_bitrate_bps * 0.05f * ((float)elapsed_ms / 1000.0f));
        if (add_step < 500000) add_step = 500000; /* Minimum 500 kbps probe */
        c->current_bitrate_bps += add_step;
    }

    if (c->current_bitrate_bps < c->cfg.min_bitrate_bps) c->current_bitrate_bps = c->cfg.min_bitrate_bps;
    if (c->current_bitrate_bps > c->cfg.max_bitrate_bps) c->current_bitrate_bps = c->cfg.max_bitrate_bps;
    c->last_update_ms = now_ms;

    /* 6. SpeedHQ Quality & Cadence Mapping */
    float rho = (float)(c->current_bitrate_bps - c->cfg.min_bitrate_bps) /
                (float)(c->cfg.max_bitrate_bps - c->cfg.min_bitrate_bps);
    if (rho < 0.0f) rho = 0.0f;
    if (rho > 1.0f) rho = 1.0f;
    c->current_quality = (uint8_t)(c->cfg.min_quality + (uint8_t)(rho * (float)(c->cfg.max_quality - c->cfg.min_quality)));

    if (c->current_bitrate_bps < 8000000) {
        c->cadence = 2; /* Half-fps under extreme bottleneck */
    } else {
        c->cadence = 1; /* Full fps */
    }

    /* 7. Dynamic FEC Density Auto-Tuning */
    if (c->cfg.fec_auto_tune) {
        c->current_fec.enabled = true;
        if (c->loss_ratio < 0.005f) {
            /* Tier 0: Clean Link (Sparse 1D Column Parity: ~6.25% overhead) */
            c->current_fec.matrix_d = 16;
            c->current_fec.matrix_l = 5;
            c->current_fec.enable_row_fec = false;
        } else if (c->loss_ratio < 0.03f) {
            /* Tier 1: Mild Loss (Standard 1D Column Parity: ~10% overhead) */
            c->current_fec.matrix_d = 10;
            c->current_fec.matrix_l = 5;
            c->current_fec.enable_row_fec = false;
        } else if (c->loss_ratio < 0.08f) {
            /* Tier 2: Moderate / Burst Loss (2D Column + Row Parity: ~25% overhead) */
            c->current_fec.matrix_d = 8;
            c->current_fec.matrix_l = 4;
            c->current_fec.enable_row_fec = true;
        } else {
            /* Tier 3: Severe Loss (Dense 2D Matrix Parity: ~50% overhead) */
            c->current_fec.matrix_d = 6;
            c->current_fec.matrix_l = 3;
            c->current_fec.enable_row_fec = true;
        }
    }

    avx_mutex_unlock(&c->lock);
}

uint32_t avx_rate_controller_get_target_bitrate(const avx_rate_controller_t* c) {
    if (!c) return 0;
    avx_rate_controller_t* mut_c = (avx_rate_controller_t*)c;
    avx_mutex_lock(&mut_c->lock);
    uint32_t br = c->current_bitrate_bps;
    avx_mutex_unlock(&mut_c->lock);
    return br;
}

uint8_t avx_rate_controller_get_quality(const avx_rate_controller_t* c) {
    if (!c) return 96;
    avx_rate_controller_t* mut_c = (avx_rate_controller_t*)c;
    avx_mutex_lock(&mut_c->lock);
    uint8_t q = c->current_quality;
    avx_mutex_unlock(&mut_c->lock);
    return q;
}

void avx_rate_controller_get_fec_config(const avx_rate_controller_t* c, avx_fec_config_t* out_fec) {
    if (!c || !out_fec) return;
    avx_rate_controller_t* mut_c = (avx_rate_controller_t*)c;
    avx_mutex_lock(&mut_c->lock);
    *out_fec = c->current_fec;
    avx_mutex_unlock(&mut_c->lock);
}

uint32_t avx_rate_controller_get_cadence(const avx_rate_controller_t* c) {
    if (!c) return 1;
    avx_rate_controller_t* mut_c = (avx_rate_controller_t*)c;
    avx_mutex_lock(&mut_c->lock);
    uint32_t cad = c->cadence;
    avx_mutex_unlock(&mut_c->lock);
    return cad;
}

avx_wan_congestion_state_t avx_rate_controller_get_congestion_state(const avx_rate_controller_t* c) {
    if (!c) return AVX_WAN_CONG_NORMAL;
    avx_rate_controller_t* mut_c = (avx_rate_controller_t*)c;
    avx_mutex_lock(&mut_c->lock);
    avx_wan_congestion_state_t st = c->congestion_state;
    avx_mutex_unlock(&mut_c->lock);
    return st;
}

/* Pushes updated pacing rate, congestion window, and FEC matrices to active RUDP sender. */
void avx_rate_controller_apply_to_sender(const avx_rate_controller_t* c, avx_udp_rudp_sender_t* s) {
    if (!c || !s) return;
    avx_fec_config_t fec;
    avx_rate_controller_get_fec_config(c, &fec);
    avx_udp_rudp_sender_set_fec(s, &fec);
}
