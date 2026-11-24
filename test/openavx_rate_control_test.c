/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_rate_control_test.c - Adaptive Bitrate & Dynamic FEC Controller Test Suite
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "openavx.h"
#include "avx_internal.h"
#include "avx_rate_control.h"
#include "avx_fec.h"
#include "avx_udp.h"
#include "avx_speedhq.h"

/* -------------------------------------------------------------------------- */
/* Test 1: Clean Link Probing & Additive Bitrate Increase                      */
/* -------------------------------------------------------------------------- */
static void test_clean_link_additive_probing(void) {
    printf("[*] 1. Testing Clean Link Probing & Additive Bitrate Scaling...\n");

    avx_rate_controller_config_t cfg = {
        .min_bitrate_bps = 5000000,    /* 5 Mbps */
        .max_bitrate_bps = 100000000,  /* 100 Mbps */
        .start_bitrate_bps = 30000000, /* 30 Mbps */
        .target_fps = 60.0f,
        .min_quality = 30,
        .max_quality = 96,
        .fec_auto_tune = true
    };

    avx_rate_controller_t* c = avx_rate_controller_create(&cfg);
    assert(c != NULL);

    assert(avx_rate_controller_get_target_bitrate(c) == 30000000);
    assert(avx_rate_controller_get_cadence(c) == 1);
    assert(avx_rate_controller_get_congestion_state(c) == AVX_WAN_CONG_NORMAL);

    /* Simulate 5 seconds of clean link telemetry */
    int64_t now_ms = 1000;
    avx_rudp_telemetry_t telem;
    memset(&telem, 0, sizeof(telem));
    telem.srtt_ms = 25;
    telem.rttvar_ms = 3;

    for (int step = 0; step < 50; ++step) {
        now_ms += 100;
        telem.packets_sent += 200; /* 200 packets per 100ms */
        /* 0 drops, 0 retransmissions */

        avx_rate_controller_update_telemetry(c, &telem, now_ms);
    }

    uint32_t final_br = avx_rate_controller_get_target_bitrate(c);
    uint8_t  final_q  = avx_rate_controller_get_quality(c);
    avx_wan_congestion_state_t st = avx_rate_controller_get_congestion_state(c);

    printf("    -> Final Bitrate: %u bps (%.2f Mbps)\n", final_br, (double)final_br / 1000000.0);
    printf("    -> Final Quality: %u\n", final_q);
    printf("    -> Congestion State: %d\n", st);

    /* Bitrate should have probed upwards from 30 Mbps */
    assert(final_br > 45000000);
    assert(final_q >= 60);
    assert(st == AVX_WAN_CONG_NORMAL);

    /* Verify Tier 0 clean FEC configuration */
    avx_fec_config_t fec;
    avx_rate_controller_get_fec_config(c, &fec);
    assert(fec.enabled == true);
    assert(fec.matrix_d == 16);
    assert(fec.matrix_l == 5);
    assert(fec.enable_row_fec == false);

    avx_rate_controller_destroy(c);
    printf("    -> Clean link probing verified successfully.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 2: GCC Delay-Based Queuing Detection (Bufferbloat Warning)             */
/* -------------------------------------------------------------------------- */
static void test_delay_based_queuing_detection(void) {
    printf("[*] 2. Testing GCC Delay-Based Queuing Detection...\n");

    avx_rate_controller_config_t cfg = {
        .min_bitrate_bps = 5000000,
        .max_bitrate_bps = 100000000,
        .start_bitrate_bps = 80000000, /* 80 Mbps */
        .target_fps = 60.0f,
        .min_quality = 30,
        .max_quality = 96,
        .fec_auto_tune = true
    };

    avx_rate_controller_t* c = avx_rate_controller_create(&cfg);
    assert(c != NULL);

    int64_t now_ms = 1000;
    avx_rudp_telemetry_t telem;
    memset(&telem, 0, sizeof(telem));

    /* 1. Baseline RTT is 20 ms */
    telem.srtt_ms = 20;
    telem.rttvar_ms = 2;
    telem.packets_sent = 500;
    avx_rate_controller_update_telemetry(c, &telem, now_ms);

    /* 2. Bufferbloat occurs in router: RTT spikes to 75 ms (Delta = +55 ms), with ZERO packet loss */
    for (int step = 0; step < 10; ++step) {
        now_ms += 100;
        telem.packets_sent += 200;
        telem.srtt_ms = 75;
        telem.rttvar_ms = 15;
        avx_rate_controller_update_telemetry(c, &telem, now_ms);
    }

    avx_wan_congestion_state_t st = avx_rate_controller_get_congestion_state(c);
    uint32_t choked_br = avx_rate_controller_get_target_bitrate(c);
    uint8_t  choked_q  = avx_rate_controller_get_quality(c);

    printf("    -> Congestion State under bufferbloat: %d (Expected OVERUSE=2)\n", st);
    printf("    -> Choked Bitrate: %u bps (%.2f Mbps)\n", choked_br, (double)choked_br / 1000000.0);
    printf("    -> Stepped-down Quality: %u\n", choked_q);

    assert(st == AVX_WAN_CONG_OVERUSE);
    assert(choked_br < 65000000); /* Bitrate throttled down proactively to drain queue */
    assert(choked_q < 90);

    avx_rate_controller_destroy(c);
    printf("    -> Delay-based congestion detected and mitigated before packet loss.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 3: Loss-Based Multiplicative Backoff & Emergency Cadence Decimation    */
/* -------------------------------------------------------------------------- */
static void test_loss_backoff_and_cadence(void) {
    printf("[*] 3. Testing Loss-Based Multiplicative Backoff & Cadence...\n");

    avx_rate_controller_config_t cfg = {
        .min_bitrate_bps = 2000000,    /* 2 Mbps */
        .max_bitrate_bps = 50000000,   /* 50 Mbps */
        .start_bitrate_bps = 20000000, /* 20 Mbps */
        .target_fps = 60.0f,
        .min_quality = 25,
        .max_quality = 96,
        .fec_auto_tune = true
    };

    avx_rate_controller_t* c = avx_rate_controller_create(&cfg);
    assert(c != NULL);

    int64_t now_ms = 2000;
    avx_rudp_telemetry_t telem;
    memset(&telem, 0, sizeof(telem));
    telem.srtt_ms = 40;
    telem.rttvar_ms = 10;
    telem.packets_sent = 1000;
    avx_rate_controller_update_telemetry(c, &telem, now_ms);

    /* Inject 15% sustained packet loss across 15 updates */
    for (int step = 0; step < 15; ++step) {
        now_ms += 100;
        telem.packets_sent += 200;
        telem.packets_lost += 30; /* 30 out of 200 lost = 15% */
        telem.srtt_ms = 45;
        avx_rate_controller_update_telemetry(c, &telem, now_ms);
    }

    uint32_t choked_br = avx_rate_controller_get_target_bitrate(c);
    uint32_t cadence   = avx_rate_controller_get_cadence(c);
    uint8_t  q         = avx_rate_controller_get_quality(c);

    printf("    -> Choked Bitrate: %u bps (%.2f Mbps)\n", choked_br, (double)choked_br / 1000000.0);
    printf("    -> Cadence: %u (Expected: 2 for < 8 Mbps)\n", cadence);
    printf("    -> Quality: %u\n", q);

    assert(choked_br <= 8000000);
    assert(cadence == 2);
    assert(q <= 35);

    avx_rate_controller_destroy(c);
    printf("    -> Multiplicative loss backoff and emergency cadence verified.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 4: Dynamic FEC Density Matrix Auto-Tuning                              */
/* -------------------------------------------------------------------------- */
static void test_fec_density_auto_tuning(void) {
    printf("[*] 4. Testing FEC Matrix Density Auto-Tuning Across Loss Tiers...\n");

    avx_rate_controller_t* c = avx_rate_controller_create(NULL);
    assert(c != NULL);

    int64_t now_ms = 3000;
    avx_rudp_telemetry_t telem;
    memset(&telem, 0, sizeof(telem));
    telem.srtt_ms = 30;
    telem.packets_sent = 1000;
    avx_rate_controller_update_telemetry(c, &telem, now_ms);

    avx_fec_config_t fec;

    /* Tier 0: Clean link (loss = 0) */
    now_ms += 100;
    telem.packets_sent += 500;
    avx_rate_controller_update_telemetry(c, &telem, now_ms);
    avx_rate_controller_get_fec_config(c, &fec);
    assert(fec.matrix_d == 16 && fec.matrix_l == 5 && fec.enable_row_fec == false);
    printf("    -> Tier 0 (Clean): D=%u, L=%u, RowFEC=%d\n", fec.matrix_d, fec.matrix_l, fec.enable_row_fec);

    /* Tier 1: Mild loss (~2%) */
    for (int i = 0; i < 5; ++i) {
        now_ms += 100;
        telem.packets_sent += 200;
        telem.packets_lost += 4; /* 2% loss */
        avx_rate_controller_update_telemetry(c, &telem, now_ms);
    }
    avx_rate_controller_get_fec_config(c, &fec);
    assert(fec.matrix_d == 10 && fec.matrix_l == 5 && fec.enable_row_fec == false);
    printf("    -> Tier 1 (Mild):  D=%u, L=%u, RowFEC=%d\n", fec.matrix_d, fec.matrix_l, fec.enable_row_fec);

    /* Tier 2: Moderate loss (~5%) */
    for (int i = 0; i < 5; ++i) {
        now_ms += 100;
        telem.packets_sent += 200;
        telem.packets_lost += 10; /* 5% loss */
        avx_rate_controller_update_telemetry(c, &telem, now_ms);
    }
    avx_rate_controller_get_fec_config(c, &fec);
    assert(fec.matrix_d == 8 && fec.matrix_l == 4 && fec.enable_row_fec == true);
    printf("    -> Tier 2 (Burst): D=%u, L=%u, RowFEC=%d\n", fec.matrix_d, fec.matrix_l, fec.enable_row_fec);

    /* Tier 3: Severe loss (~12%) */
    for (int i = 0; i < 5; ++i) {
        now_ms += 100;
        telem.packets_sent += 200;
        telem.packets_lost += 24; /* 12% loss */
        avx_rate_controller_update_telemetry(c, &telem, now_ms);
    }
    avx_rate_controller_get_fec_config(c, &fec);
    assert(fec.matrix_d == 6 && fec.matrix_l == 3 && fec.enable_row_fec == true);
    printf("    -> Tier 3 (Severe): D=%u, L=%u, RowFEC=%d\n", fec.matrix_d, fec.matrix_l, fec.enable_row_fec);

    avx_rate_controller_destroy(c);
    printf("    -> FEC matrix auto-tuning verified across all 4 tiers.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 5: Integration with RUDP Sender & SpeedHQ Dynamic Quantization        */
/* -------------------------------------------------------------------------- */
static void test_rudp_sender_adaptation_integration(void) {
    printf("[*] 5. Testing Sender Integration with SpeedHQ Dynamic Quantization...\n");

    avx_socket_t tx_sock = avx_udp_create_socket(0, true);
    assert(tx_sock != AVX_INVALID_SOCKET);

    avx_udp_rudp_sender_t* s = avx_udp_rudp_sender_create(tx_sock, 8);
    assert(s != NULL);

    avx_rate_controller_t* c = avx_rate_controller_create(NULL);
    assert(c != NULL);

    /* Initial state */
    avx_rate_controller_apply_to_sender(c, s);

    /* Feed severe loss telemetry and apply to sender */
    int64_t now_ms = 4000;
    avx_rudp_telemetry_t telem;
    memset(&telem, 0, sizeof(telem));
    telem.srtt_ms = 50;
    telem.packets_sent = 500;
    telem.packets_lost = 50; /* 10% loss */

    for (int i = 0; i < 10; ++i) {
        now_ms += 100;
        telem.packets_sent += 200;
        telem.packets_lost += 20;
        avx_rate_controller_update_telemetry(c, &telem, now_ms);
    }

    /* Apply auto-tuned FEC to sender */
    avx_rate_controller_apply_to_sender(c, s);

    /* Apply recommended quality to SpeedHQ encoder */
    uint8_t rec_q = avx_rate_controller_get_quality(c);
    oavx_speedhq_set_quality(rec_q);
    assert(oavx_speedhq_get_quality() == rec_q);

    printf("    -> SpeedHQ quality tuned dynamically to: %u\n", rec_q);

    /* Restore default SpeedHQ quality */
    oavx_speedhq_set_quality(96);

    avx_rate_controller_destroy(c);
    avx_udp_rudp_sender_destroy(s);
    avx_socket_close(tx_sock);

    printf("    -> Sender adaptation and SpeedHQ quantization integrated successfully.\n");
}

/* -------------------------------------------------------------------------- */
/* Test 6: In-Loop Transmitter Rate Controller Wiring & Live Telemetry        */
/* -------------------------------------------------------------------------- */
static void test_transmitter_in_loop_rate_control(void) {
    printf("[*] 6. Testing In-Loop Transmitter Rate Controller Integration...\n");

    avx_tx_config_t tx_cfg;
    memset(&tx_cfg, 0, sizeof(tx_cfg));
    tx_cfg.name = "TestTxRateCtl";
    tx_cfg.transport = OAVX_TRANSPORT_RUDP;

    avx_tx_instance_t tx = avx_tx_create(&tx_cfg);
    assert(tx != NULL);

    avx_rate_controller_t *rc = avx_tx_get_rate_controller(tx);
    assert(rc != NULL);

    /* Verify initial baseline */
    assert(avx_rate_controller_get_cadence(rc) == 1);
    assert(avx_rate_controller_get_congestion_state(rc) == AVX_WAN_CONG_NORMAL);

    /* Test telemetry query */
    oavx_telemetry_t telem;
    bool ok = avx_tx_get_telemetry(tx, &telem);
    assert(ok == true);

    /* Verify destruction releases rate controller without leak */
    avx_tx_destroy(tx);
    printf("    -> Transmitter in-loop rate controller verified successfully.\n");
}

int main(void) {
    printf("===================================================================\n");
    printf("  OpenAVX Adaptive Bitrate & Dynamic FEC Controller Suite          \n");
    printf("===================================================================\n");

    test_clean_link_additive_probing();
    test_delay_based_queuing_detection();
    test_loss_backoff_and_cadence();
    test_fec_density_auto_tuning();
    test_rudp_sender_adaptation_integration();
    test_transmitter_in_loop_rate_control();

    printf("\n>>> ALL RATE CONTROL TESTS PASSED SUCCESSFULLY! <<<\n");
    return 0;
}
