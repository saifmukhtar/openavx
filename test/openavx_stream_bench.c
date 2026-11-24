/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_stream_bench.c - End-to-End Streaming, Codec, WAN & Fanout Benchmark
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/resource.h>

#include "openavx.h"
#include "avx_speedhq.h"

#define BENCH_RING_1080P 16
#define BENCH_RING_4K     8
#define BENCH_MAX_CLIENTS 8

typedef struct {
    int64_t  wall_ns;
    uint64_t frames_sent;
    uint64_t frames_recv;
    uint64_t dropped_tx;
    double   cpu_pct;
    double   fps;
    double   mbps;
    double   jitter_ms;
    float    rtt_ms;
    uint32_t tcp_retrans;
    uint32_t congestion;
    double   p50_ms;
    double   p95_ms;
    double   p99_ms;
    double   max_ms;
} bench_metrics_t;

typedef struct {
    oavx_stream_t *rx;
    volatile bool  stop;
    bool           stalled;
    int64_t        bench_start_ns;
    int64_t        last_rx_ns;
    int64_t        audio_frames;
    double        *lat_ms;
    size_t         lat_cap;
    size_t         lat_count;
} rx_worker_t;

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static double proc_cpu_sec(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0.0;
    double u = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6;
    double s = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;
    return u + s;
}

static void sleep_until_ns(int64_t target_ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(target_ns / 1000000000LL);
    ts.tv_nsec = (long)(target_ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

static int cmp_double_asc(const void *a, const void *b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    return (da > db) - (da < db);
}

static void summarize_latencies(double *samples, size_t n,
                                double *p50, double *p95, double *p99, double *maxv) {
    *p50 = *p95 = *p99 = *maxv = 0.0;
    if (!samples || n == 0) return;
    qsort(samples, n, sizeof(double), cmp_double_asc);
    *p50 = samples[(size_t)((n - 1) * 0.50)];
    *p95 = samples[(size_t)((n - 1) * 0.95)];
    *p99 = samples[(size_t)((n - 1) * 0.99)];
    *maxv = samples[n - 1];
}

static void fill_uyvy_frame(uint8_t *buf, int w, int h, uint32_t seed) {
    size_t total = (size_t)w * (size_t)h * 2;
    uint32_t rng = seed ? seed : 0x13579BDFu;
    for (int y = 0; y < h; ++y) {
        uint8_t *row = buf + (size_t)y * (size_t)w * 2;
        for (int x = 0; x < w; x += 2) {
            rng = rng * 1664525u + 1013904223u;
            uint8_t noise = (uint8_t)(rng >> 28); /* mild texture + gradient */
            uint8_t base_y = (uint8_t)(((x * 220) / (w > 1 ? w : 1)) + ((y + (int)seed) & 0x1F));
            row[x * 2 + 0] = (uint8_t)(64 + (((x + (int)seed * 7) * 128) / (w > 1 ? w : 1)));
            row[x * 2 + 1] = (uint8_t)(base_y ^ noise);
            row[x * 2 + 2] = (uint8_t)(192 - (((y + (int)seed * 3) * 128) / (h > 1 ? h : 1)));
            row[x * 2 + 3] = (uint8_t)((base_y + 1) ^ (noise >> 1));
        }
    }
    (void)total;
}

static void* rx_worker_main(void *arg) {
    rx_worker_t *w = (rx_worker_t*)arg;
    while (!w->stop) {
        if (w->stalled) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 20000000L };
            nanosleep(&ts, NULL);
            continue;
        }
        oavx_video_frame_t v = {0};
        oavx_audio_frame_t a = {0};
        oavx_frame_type_t ft = oavx_stream_receive(w->rx, &v, &a, NULL, 25);
        int64_t t_rx = mono_ns();
        if (ft == OAVX_FRAME_TYPE_VIDEO) {
            w->last_rx_ns = t_rx;
            if (v.pts_ns >= w->bench_start_ns && t_rx >= v.pts_ns && w->lat_count < w->lat_cap) {
                w->lat_ms[w->lat_count++] = (double)(t_rx - v.pts_ns) / 1e6;
            } else if (w->lat_count < w->lat_cap) {
                w->lat_ms[w->lat_count++] = 0.0;
            }
            oavx_stream_release_video(w->rx, &v);
        } else if (ft == OAVX_FRAME_TYPE_AUDIO) {
            w->audio_frames++;
            oavx_stream_release_audio(w->rx, &a);
        }
    }

    if (!w->stalled) {
        /* Drain any trailing queued frames after sender stops */
        for (int idle = 0; idle < 6; ) {
            oavx_video_frame_t v = {0};
            oavx_audio_frame_t a = {0};
            oavx_frame_type_t ft = oavx_stream_receive(w->rx, &v, &a, NULL, 20);
            int64_t t_rx = mono_ns();
            if (ft == OAVX_FRAME_TYPE_VIDEO) {
                idle = 0;
                w->last_rx_ns = t_rx;
                if (v.pts_ns >= w->bench_start_ns && t_rx >= v.pts_ns && w->lat_count < w->lat_cap) {
                    w->lat_ms[w->lat_count++] = (double)(t_rx - v.pts_ns) / 1e6;
                }
                oavx_stream_release_video(w->rx, &v);
            } else if (ft == OAVX_FRAME_TYPE_AUDIO) {
                idle = 0;
                w->audio_frames++;
                oavx_stream_release_audio(w->rx, &a);
            } else {
                idle++;
            }
        }
    }
    return NULL;
}

/* ========================================================================= */
/* LAYER 1: Full-Frame SpeedHQ Codec Benchmark (Cold Buffer Ring)            */
/* ========================================================================= */

static void bench_codec_case(int w, int h, int quality, int ring_n, int total_iters) {
    size_t raw_bytes = (size_t)w * (size_t)h * 2;
    size_t max_comp = raw_bytes + 4096;

    uint8_t **in_ring = (uint8_t**)calloc((size_t)ring_n, sizeof(uint8_t*));
    uint8_t **comp_ring = (uint8_t**)calloc((size_t)ring_n, sizeof(uint8_t*));
    size_t   *comp_sizes = (size_t*)calloc((size_t)ring_n, sizeof(size_t));
    uint8_t  *dec_out = (uint8_t*)malloc(raw_bytes);

    if (!in_ring || !comp_ring || !comp_sizes || !dec_out) {
        fprintf(stderr, "OOM allocating Layer 1 ring (%dx%d)\n", w, h);
        exit(1);
    }
    for (int i = 0; i < ring_n; ++i) {
        in_ring[i] = (uint8_t*)malloc(raw_bytes);
        comp_ring[i] = (uint8_t*)malloc(max_comp);
        if (!in_ring[i] || !comp_ring[i]) exit(1);
        fill_uyvy_frame(in_ring[i], w, h, (uint32_t)(i + 1) * 97u);
    }

    oavx_speedhq_set_quality((uint8_t)quality);

    /* Warmup */
    size_t warm_sz = 0;
    avx_speedhq_encode_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ2, w, h, in_ring[0], NULL, comp_ring[0], max_comp, &warm_sz);
    bool has_alpha = false;
    avx_speedhq_decode_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ2, w, h, comp_ring[0], warm_sz, dec_out, NULL, &has_alpha);

    double enc_cpu0 = proc_cpu_sec();
    int64_t enc_t0 = mono_ns();
    size_t total_comp_bytes = 0;
    for (int i = 0; i < total_iters; ++i) {
        int idx = i % ring_n;
        size_t out_sz = 0;
        if (avx_speedhq_encode_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ2, w, h,
                                  in_ring[idx], NULL, comp_ring[idx], max_comp, &out_sz) != 0 || out_sz == 0) {
            fprintf(stderr, "SpeedHQ encode failed at %dx%d\n", w, h);
            exit(1);
        }
        comp_sizes[idx] = out_sz;
        total_comp_bytes += out_sz;
    }
    double enc_wall = (double)(mono_ns() - enc_t0) / 1e9;
    double enc_cpu = proc_cpu_sec() - enc_cpu0;

    double dec_cpu0 = proc_cpu_sec();
    int64_t dec_t0 = mono_ns();
    for (int i = 0; i < total_iters; ++i) {
        int idx = i % ring_n;
        if (avx_speedhq_decode_ex(OAVX_FOURCC_VIDEO_TYPE_SHQ2, w, h,
                                  comp_ring[idx], comp_sizes[idx], dec_out, NULL, &has_alpha) != 0) {
            fprintf(stderr, "SpeedHQ decode failed at %dx%d\n", w, h);
            exit(1);
        }
    }
    double dec_wall = (double)(mono_ns() - dec_t0) / 1e9;
    double dec_cpu = proc_cpu_sec() - dec_cpu0;

    double avg_kb = ((double)total_comp_bytes / (double)total_iters) / 1024.0;
    double ratio = (double)raw_bytes / ((double)total_comp_bytes / (double)total_iters);
    double mbps_at_60 = ((double)total_comp_bytes / (double)total_iters) * 8.0 * 60.0 / 1e6;

    printf("  %-11s q=%-2d | Enc: %6.1f FPS (%5.2f ms, %5.0f%% CPU) | Dec: %6.1f FPS (%5.2f ms, %5.0f%% CPU) | %6.1f KB (%4.1f:1, %6.1f Mbps@60)\n",
           (w >= 3840) ? "4K UYVY" : "1080p UYVY", quality,
           total_iters / enc_wall, (enc_wall / total_iters) * 1000.0, (enc_cpu / enc_wall) * 100.0,
           total_iters / dec_wall, (dec_wall / total_iters) * 1000.0, (dec_cpu / dec_wall) * 100.0,
           avg_kb, ratio, mbps_at_60);

    for (int i = 0; i < ring_n; ++i) {
        free(in_ring[i]);
        free(comp_ring[i]);
    }
    free(in_ring);
    free(comp_ring);
    free(comp_sizes);
    free(dec_out);
}

static void run_layer1_codec(bool quick) {
    printf("\n=====================================================================================================================\n");
    printf("  LAYER 1: Full-Frame SpeedHQ-2 Codec Benchmark (Cold Multi-Frame Buffer Ring > L3 Cache)\n");
    printf("=====================================================================================================================\n");
    avx_speedhq_init();
    int n1080 = quick ? 30 : 80;
    int n4k   = quick ? 10 : 24;
    bench_codec_case(1920, 1080, 96, BENCH_RING_1080P, n1080);
    bench_codec_case(1920, 1080, 50, BENCH_RING_1080P, n1080);
    bench_codec_case(3840, 2160, 96, BENCH_RING_4K,    n4k);
    bench_codec_case(3840, 2160, 50, BENCH_RING_4K,    n4k);
    oavx_speedhq_set_quality(96);
}

/* ========================================================================= */
/* LAYERS 2, 3, 4: Real Socket Pipeline Stream Benchmark (TCP & RUDP)        */
/* ========================================================================= */

static void run_stream_scenario(const char *label,
                                int w, int h,
                                oavx_fourcc_video_type_e tx_fourcc,
                                oavx_color_format_e rx_fmt,
                                oavx_transport_e transport,
                                int target_fps,
                                int num_frames,
                                int num_clients,
                                bool stall_last_client) {
    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        fprintf(stderr, "oavx_context_create failed\n");
        return;
    }

    oavx_stream_config_t tx_cfg = {
        .name = label,
        .groups = "bench",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = (target_fps > 0),
        .max_connections = (uint32_t)(num_clients + 4),
        .bind_ip = "127.0.0.1",
        .transport = transport
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    if (!tx) {
        fprintf(stderr, "Failed to open TX for %s\n", label);
        oavx_context_destroy(ctx);
        return;
    }
    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);

    oavx_stream_t *rxs[BENCH_MAX_CLIENTS] = {0};
    rx_worker_t    workers[BENCH_MAX_CLIENTS] = {0};
    pthread_t      threads[BENCH_MAX_CLIENTS] = {0};

    int64_t bench_start = mono_ns();
    for (int c = 0; c < num_clients; ++c) {
        char cname[64];
        snprintf(cname, sizeof(cname), "BENCH_RX_%d", c);
        oavx_stream_config_t rx_cfg = {
            .name = cname,
            .endpoint = ep->address,
            .direction = OAVX_DIR_RECEIVE,
            .color_format = rx_fmt,
            .bandwidth = OAVX_BANDWIDTH_HIGHEST,
            .clock_sync = (target_fps > 0),
            .transport = transport
        };
        rxs[c] = oavx_stream_open(ctx, &rx_cfg);
        workers[c].rx = rxs[c];
        workers[c].stop = false;
        workers[c].stalled = (stall_last_client && c == num_clients - 1);
        workers[c].bench_start_ns = bench_start;
        workers[c].lat_cap = (size_t)(num_frames + 32);
        workers[c].lat_ms = (double*)calloc(workers[c].lat_cap, sizeof(double));
    }

    for (int c = 0; c < num_clients; ++c) {
        if (rxs[c]) pthread_create(&threads[c], NULL, rx_worker_main, &workers[c]);
    }

    /* Wait for all clients to finish TCP handshake (including SYN retry on lossy WAN) */
    int connected_clients = 0;
    for (int wait = 0; wait < 120; ++wait) {
        connected_clients = oavx_stream_get_connection_count(tx, 25);
        if (connected_clients >= num_clients) break;
    }
    if (connected_clients < num_clients) {
        fprintf(stderr, "  [WARN] %s: only %d/%d clients connected before handshake deadline\n",
                label, connected_clients, num_clients);
    }
    struct timespec hs_ts = { .tv_sec = 0, .tv_nsec = 120000000L };
    nanosleep(&hs_ts, NULL);

    /* Prepare distinct UYVY frames + 48 kHz stereo audio chunk */
    int ring_n = 4;
    size_t raw_bytes = (size_t)w * (size_t)h * 2;
    uint8_t *ring[4] = {0};
    for (int i = 0; i < ring_n; ++i) {
        ring[i] = (uint8_t*)malloc(raw_bytes);
        fill_uyvy_frame(ring[i], w, h, (uint32_t)(i + 1) * 31u);
    }

    float audio_l[800], audio_r[800];
    for (int i = 0; i < 800; ++i) {
        audio_l[i] = 0.25f;
        audio_r[i] = -0.25f;
    }

    oavx_video_frame_t vf = {0};
    vf.width = (uint32_t)w;
    vf.height = (uint32_t)h;
    vf.fourcc = tx_fourcc;
    vf.scan_type = OAVX_SCAN_PROGRESSIVE;
    vf.strides[0] = (size_t)w * 2;
    vf.framerate.num = (uint32_t)(target_fps > 0 ? target_fps * 1000 : 60000);
    vf.framerate.den = 1000;
    vf.aspect_ratio.num = 16;
    vf.aspect_ratio.den = 9;

    oavx_audio_frame_t af = {0};
    af.sample_rate = 48000;
    af.channels = 2;
    af.frames = 800; /* 16.67 ms at 48 kHz */
    af.format = OAVX_AUDIO_FMT_FLT_PLANAR;
    af.stride_bytes = sizeof(audio_l);
    af.planes[0] = audio_l;
    af.planes[1] = audio_r;

    double cpu0 = proc_cpu_sec();
    int64_t t0 = mono_ns();
    int64_t period_ns = (target_fps > 0) ? (1000000000LL / target_fps) : 0;

    for (int f = 0; f < num_frames; ++f) {
        int64_t now = mono_ns();
        vf.planes[0] = ring[f % ring_n];
        vf.pts_ns = now;
        oavx_stream_send_video(tx, &vf);

        af.pts_ns = now;
        oavx_stream_send_audio(tx, &af);

        if (period_ns > 0) {
            int64_t next_tick = t0 + (int64_t)(f + 1) * period_ns;
            if (mono_ns() < next_tick) sleep_until_ns(next_tick);
        }
    }

    /* Allow trailing packets / WAN NACKs in flight to land cleanly */
    long tail_ns = (transport == OAVX_TRANSPORT_RUDP) ? 350000000L : 150000000L;
    struct timespec tail_ts = { .tv_sec = 0, .tv_nsec = tail_ns };
    nanosleep(&tail_ts, NULL);
    int64_t t_end = mono_ns();
    double cpu_used = proc_cpu_sec() - cpu0;

    for (int c = 0; c < num_clients; ++c) workers[c].stop = true;
    for (int c = 0; c < num_clients; ++c) {
        if (rxs[c]) pthread_join(threads[c], NULL);
    }

    int64_t active_end_ns = t_end - (int64_t)tail_ns;
    int active_clients = stall_last_client ? (num_clients - 1) : num_clients;
    if (active_clients < 1) active_clients = 1;
    int64_t max_rx_ns = 0;
    for (int c = 0; c < active_clients; ++c) {
        if (workers[c].last_rx_ns > max_rx_ns) max_rx_ns = workers[c].last_rx_ns;
    }
    if (max_rx_ns > t0) active_end_ns = max_rx_ns;
    double wall_sec = (double)(active_end_ns - t0) / 1e9;
    if (wall_sec <= 0.001) wall_sec = 0.001;

    oavx_telemetry_t tx_telem = {0};
    oavx_stream_get_telemetry(tx, &tx_telem);

    uint64_t total_rx_frames = 0;
    uint64_t total_rx_wire = 0;
    double sum_jitter = 0.0;
    float max_rtt = 0.0f;
    uint32_t max_retrans = 0;
    size_t combined_lat_n = 0;

    for (int c = 0; c < active_clients; ++c) {
        oavx_telemetry_t rx_telem = {0};
        if (rxs[c]) oavx_stream_get_telemetry(rxs[c], &rx_telem);
        total_rx_frames += (uint64_t)workers[c].lat_count;
        total_rx_wire += rx_telem.total_wire_bytes;
        sum_jitter += rx_telem.inter_frame_jitter_ms;
        if (rx_telem.kernel_rtt_ms > max_rtt) max_rtt = rx_telem.kernel_rtt_ms;
        if (rx_telem.tcp_retransmissions > max_retrans) max_retrans = rx_telem.tcp_retransmissions;
        combined_lat_n += workers[c].lat_count;
    }

    double *all_lat = combined_lat_n ? (double*)malloc(combined_lat_n * sizeof(double)) : NULL;
    size_t pos = 0;
    for (int c = 0; c < active_clients; ++c) {
        if (all_lat && workers[c].lat_count > 0) {
            memcpy(all_lat + pos, workers[c].lat_ms, workers[c].lat_count * sizeof(double));
            pos += workers[c].lat_count;
        }
    }

    double p50 = 0, p95 = 0, p99 = 0, max_l = 0;
    summarize_latencies(all_lat, pos, &p50, &p95, &p99, &max_l);
    free(all_lat);

    double avg_rx_fps = ((double)total_rx_frames / (double)active_clients) / wall_sec;
    double wire_mbps = ((double)tx_telem.total_wire_bytes * 8.0) / (wall_sec * 1e6);
    if (wire_mbps == 0.0 && total_rx_wire > 0) {
        wire_mbps = ((double)total_rx_wire * 8.0) / (wall_sec * 1e6);
    }
    double avg_jitter = sum_jitter / (double)active_clients;

    printf("  %-34s | RX:%5.1f/%-3d (%3llu/%-3d) | %7.1f Mbps | Drop:%4lld | CPU:%5.0f%% | Jit:%4.2fms | p50:%5.2f p95:%5.2f p99:%5.2f max:%6.2f ms | rtt:%.2f retx:%u cg:%u\n",
           label,
           avg_rx_fps, target_fps > 0 ? target_fps : 0,
           (unsigned long long)(total_rx_frames / (uint64_t)active_clients), num_frames,
           wire_mbps,
           (long long)tx_telem.dropped_frames_total,
           (cpu_used / wall_sec) * 100.0,
           avg_jitter,
           p50, p95, p99, max_l,
           (double)max_rtt > 0 ? max_rtt : tx_telem.kernel_rtt_ms,
           max_retrans > 0 ? max_retrans : tx_telem.tcp_retransmissions,
           tx_telem.congestion_level);

    for (int c = 0; c < num_clients; ++c) {
        free(workers[c].lat_ms);
        if (rxs[c]) oavx_stream_close(rxs[c]);
    }
    for (int i = 0; i < ring_n; ++i) free(ring[i]);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
}

static void run_layer2_single(bool quick) {
    int n1080 = quick ? 90 : 180;
    int n4k   = quick ? 45 : 120;
    printf("\n=====================================================================================================================\n");
    printf("  LAYER 2: Single-Stream End-to-End Pipeline Benchmark (127.0.0.1, TX -> Wire -> RX + 48kHz Audio)\n");
    printf("=====================================================================================================================\n");
    run_stream_scenario("1080p60 UYVY -> FASTEST (raw)",   1920, 1080, OAVX_FOURCC_VIDEO_TYPE_UYVY, OAVX_COLOR_FORMAT_FASTEST,   OAVX_TRANSPORT_TCP, 60, n1080, 1, false);
    run_stream_scenario("1080p60 UYVY -> BGRA (SIMD)",     1920, 1080, OAVX_FOURCC_VIDEO_TYPE_UYVY, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP, 60, n1080, 1, false);
    run_stream_scenario("1080p60 SHQ2 -> BGRA (Codec)",    1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP, 60, n1080, 1, false);
    run_stream_scenario("4K60    UYVY -> FASTEST (raw)",   3840, 2160, OAVX_FOURCC_VIDEO_TYPE_UYVY, OAVX_COLOR_FORMAT_FASTEST,   OAVX_TRANSPORT_TCP, 60, n4k,   1, false);
    run_stream_scenario("4K60    SHQ2 -> BGRA (Codec)",    3840, 2160, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP, 60, n4k,   1, false);
    run_stream_scenario("1080p   UYVY -> FASTEST (burst)", 1920, 1080, OAVX_FOURCC_VIDEO_TYPE_UYVY, OAVX_COLOR_FORMAT_FASTEST,   OAVX_TRANSPORT_TCP,  0, n1080, 1, false);
    run_stream_scenario("1080p   SHQ2 -> BGRA (burst)",    1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP,  0, n1080, 1, false);
}

static void run_layer3_transport(bool quick, bool wan_active) {
    int n = quick ? 90 : 240;
    printf("\n=====================================================================================================================\n");
    printf("  LAYER 3: Transport Comparison — TCP vs RUDP (%s, 1080p60 SHQ2 -> BGRA)\n",
           wan_active ? "WAN tc netem mode" : "Clean Loopback Baseline");
    printf("=====================================================================================================================\n");
    run_stream_scenario("TCP  1080p60 SHQ2 -> BGRA", 1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP,  60, n, 1, false);
    run_stream_scenario("RUDP 1080p60 SHQ2 -> BGRA", 1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_RUDP, 60, n, 1, false);
}

static void run_layer4_fanout(bool quick) {
    int n = quick ? 60 : 120;
    printf("\n=====================================================================================================================\n");
    printf("  LAYER 4: Multi-Viewer Fanout Scaling & Slow-Client Isolation (1080p60 SHQ2 -> BGRA)\n");
    printf("=====================================================================================================================\n");
    run_stream_scenario("Fanout 1 client  (SHQ2 1080p60)",     1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP, 60, n, 1, false);
    run_stream_scenario("Fanout 2 clients (SHQ2 1080p60)",     1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP, 60, n, 2, false);
    run_stream_scenario("Fanout 4 clients (SHQ2 1080p60)",     1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP, 60, n, 4, false);
    run_stream_scenario("Fanout 8 clients (SHQ2 1080p60)",     1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP, 60, n, 8, false);
    run_stream_scenario("Isolation: 3 fast + 1 stalled rx",    1920, 1080, OAVX_FOURCC_VIDEO_TYPE_SHQ2, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_TRANSPORT_TCP, 60, n, 4, true);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [--all | --layer <1|2|3|4>] [--wan] [--quick]\n", prog);
}

int main(int argc, char **argv) {
    int layer = 0;
    bool wan_active = false;
    bool quick = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--all") == 0) {
            layer = 0;
        } else if (strcmp(argv[i], "--wan") == 0) {
            wan_active = true;
            if (layer == 0) layer = 3;
        } else if (strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (strcmp(argv[i], "--layer") == 0 && i + 1 < argc) {
            layer = atoi(argv[++i]);
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    oavx_set_log_level(OAVX_LOG_ERROR);

    printf("=== OpenAVX End-to-End Full Stream Benchmark (%s) ===\n", oavx_version());
    printf("[*] Mode: %s%s\n",
           quick ? "Quick" : "Standard",
           wan_active ? " | WAN netem profile expected on lo" : "");

    if (layer == 0 || layer == 1) run_layer1_codec(quick);
    if (layer == 0 || layer == 2) run_layer2_single(quick);
    if (layer == 0 || layer == 3) run_layer3_transport(quick, wan_active);
    if (layer == 0 || layer == 4) run_layer4_fanout(quick);

    printf("\n[DONE] openavx_stream_bench\n");
    return 0;
}
