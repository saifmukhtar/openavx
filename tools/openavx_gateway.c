/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_gateway.c - HTTP/fMP4 and RTSP gateway CLI
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "avx_platform.h"
#include "openavx.h"
#include "openavx_cli.h"

static volatile bool g_running = true;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = false;
}

static void print_usage(const char* prog) {
    printf("OpenAVX Web & RTSP Gateway (Stream Ingress & Bridge)\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -s, --source <SOURCE>   Upstream source name or IP:port to ingest\n");
    printf("  -W, --web [PORT]        Enable embedded HTTP/fMP4 Web preview (default: 8080)\n");
    printf("  -R, --rtsp [PORT]       Enable native RTSP 1.0 streaming server (default: 8554)\n");
    printf("  -p, --path <PATH>       RTSP mount point path (default: /live)\n");
    printf("  -n, --name <NAME>       Name of gateway re-stream output (default: \"OpenAVX Gateway\")\n");
    printf("  -g, --groups <GROUPS>   Broadcast / discovery groups (default: public)\n");
    printf("  -q, --quiet             Suppress info banners and live progress logs\n");
    printf("  -h, --help              Display this help and exit\n\n");
    printf("Examples:\n");
    printf("  %s                                     # Auto-discover source & serve Web on 8080, RTSP on 8554\n", prog);
    printf("  %s --source \"STUDIO_CAM_1\"             # Ingest specific camera and bridge to Web & RTSP\n", prog);
    printf("  %s --source 127.0.0.1:7960 --web 8088  # Direct connect to port 7960, serve Web on 8088\n", prog);
    printf("  %s --rtsp 8554 --path /camera1         # Custom RTSP port and mount path\n", prog);
}

int openavx_cmd_gateway(int argc, char** argv) {
    optind = 1;
    g_running = true;

    char source_arg[256] = "";
    char gateway_name[256] = "OpenAVX Gateway";
    char groups[256] = "public";
    char rtsp_path[128] = "/live";
    uint16_t web_port = 8080;
    uint16_t rtsp_port = 8554;
    bool enable_web = false;
    bool enable_rtsp = false;
    bool web_explicit = false;
    bool rtsp_explicit = false;
    bool quiet = false;

    static struct option long_opts[] = {
        { "source",   required_argument, 0, 's' },
        { "web",      optional_argument, 0, 'W' },
        { "rtsp",     optional_argument, 0, 'R' },
        { "path",     required_argument, 0, 'p' },
        { "name",     required_argument, 0, 'n' },
        { "groups",   required_argument, 0, 'g' },
        { "quiet",    no_argument,       0, 'q' },
        { "help",     no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:W::R::p:n:g:qh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            snprintf(source_arg, sizeof(source_arg), "%s", optarg);
            break;
        case 'W':
            enable_web = true;
            web_explicit = true;
            if (!optarg && optind < argc && argv[optind][0] != '-') {
                optarg = argv[optind++];
            }
            if (optarg) {
                int p = atoi(optarg);
                if (p > 0 && p <= 65535) web_port = (uint16_t)p;
            }
            break;
        case 'R':
            enable_rtsp = true;
            rtsp_explicit = true;
            if (!optarg && optind < argc && argv[optind][0] != '-') {
                optarg = argv[optind++];
            }
            if (optarg) {
                int p = atoi(optarg);
                if (p > 0 && p <= 65535) rtsp_port = (uint16_t)p;
            }
            break;
        case 'p':
            snprintf(rtsp_path, sizeof(rtsp_path), "%s", optarg);
            break;
        case 'n':
            snprintf(gateway_name, sizeof(gateway_name), "%s", optarg);
            break;
        case 'g':
            snprintf(groups, sizeof(groups), "%s", optarg);
            break;
        case 'q':
            quiet = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    openavx_cli_set_quiet(quiet);

    /* If neither --web nor --rtsp was explicitly passed, enable both by default */
    if (!web_explicit && !rtsp_explicit) {
        enable_web = true;
        enable_rtsp = true;
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        oavx_log_write(OAVX_LOG_ERROR, "gateway", "Failed to initialize OpenAVX context engine");
        return 1;
    }

    char target_name[256] = "";
    char target_url[256] = "";

    if (source_arg[0] != '\0') {
        if (strchr(source_arg, ':') && !strchr(source_arg, ' ')) {
            snprintf(target_url, sizeof(target_url), "%s", source_arg);
        } else {
            snprintf(target_name, sizeof(target_name), "%s", source_arg);
        }
    } else {
        oavx_log_write(OAVX_LOG_INFO, "gateway", "No upstream source specified; discovering active sources...");
        oavx_discovery_t *p_find = oavx_discovery_create(ctx, groups);
        if (!p_find) {
            oavx_log_write(OAVX_LOG_ERROR, "gateway", "Failed to initialize network discovery");
            oavx_context_destroy(ctx);
            return 1;
        }
        oavx_discovery_wait(p_find, 1500);
        oavx_endpoint_t sources[64];
        size_t count = oavx_discovery_get_sources(p_find, sources, 64);
        if (count == 0) {
            oavx_log_write(OAVX_LOG_ERROR, "gateway",
                           "No active OpenAVX sources found on groups '%s' (pass --source <NAME|IP:PORT>)",
                           groups);
            oavx_discovery_destroy(p_find);
            oavx_context_destroy(ctx);
            return 1;
        }

        oavx_log_write(OAVX_LOG_INFO, "gateway", "Found %zu active source(s):", count);
        for (size_t i = 0; i < count; ++i) {
            oavx_log_write(OAVX_LOG_INFO, "gateway", "  [%zu] %s (%s)", i,
                           sources[i].name ? sources[i].name : "<unnamed>",
                           sources[i].address ? sources[i].address : "mDNS");
        }
        if (sources[0].name) snprintf(target_name, sizeof(target_name), "%s", sources[0].name);
        if (sources[0].address) snprintf(target_url, sizeof(target_url), "%s", sources[0].address);
        oavx_log_write(OAVX_LOG_INFO, "gateway", "Auto-selecting source: %s (%s)",
                       target_name, target_url[0] ? target_url : "mDNS");
        oavx_discovery_destroy(p_find);
    }

    /* Configure and open receiver for upstream stream */
    oavx_stream_config_t rx_cfg = {
        .name = target_name[0] ? target_name : "OpenAVX Gateway Target",
        .endpoint = target_url[0] ? target_url : NULL,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_FASTEST,
        .groups = groups
    };

    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    if (!rx) {
        oavx_log_write(OAVX_LOG_ERROR, "gateway", "Failed to connect receiver to upstream source");
        oavx_context_destroy(ctx);
        return 1;
    }

    /* Configure and open transmitter for gateway bridge output */
    oavx_stream_config_t tx_cfg = {
        .name = gateway_name,
        .groups = groups,
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };

    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    if (!tx) {
        oavx_log_write(OAVX_LOG_ERROR, "gateway", "Failed to open gateway transmitter '%s'", gateway_name);
        oavx_stream_close(rx);
        oavx_context_destroy(ctx);
        return 1;
    }

    /* Enable Web preview server if configured */
    char web_url[256] = "";
    if (enable_web) {
        if (oavx_stream_enable_web_preview(tx, web_port) == OAVX_SUCCESS) {
            web_port = oavx_stream_get_web_preview_port(tx);
            oavx_stream_get_web_preview_url(tx, web_url, sizeof(web_url));
        } else {
            oavx_log_write(OAVX_LOG_ERROR, "gateway", "Failed to activate Web Preview server on port %u", web_port);
            enable_web = false;
        }
    }

    /* Enable RTSP server if configured */
    if (enable_rtsp) {
        if (oavx_stream_enable_rtsp_server(tx, rtsp_port, rtsp_path) == OAVX_SUCCESS) {
            rtsp_port = oavx_stream_get_rtsp_port(tx);
        } else {
            oavx_log_write(OAVX_LOG_ERROR, "gateway", "Failed to activate RTSP server on port %u", rtsp_port);
            enable_rtsp = false;
        }
    }

    openavx_cli_header("gateway");
    openavx_cli_kv("upstream", "%s%s%s",
                   target_name[0] ? target_name : target_url,
                   (target_name[0] && target_url[0]) ? " @ " : "",
                   (target_name[0] && target_url[0]) ? target_url : "");
    openavx_cli_kv("stream", "%s", gateway_name);
    openavx_cli_kv("groups", "%s", groups);
    if (enable_web) {
        openavx_cli_kv("web", "%s", web_url);
    }
    if (enable_rtsp) {
        openavx_cli_kv("rtsp", "rtsp://127.0.0.1:%u%s", rtsp_port, rtsp_path);
    }
    oavx_log_write(OAVX_LOG_INFO, "gateway", "Gateway running (Ctrl+C to stop)");

    bool warned_codec = false;
    uint64_t v_count = 0, a_count = 0;
    int64_t last_stat_time = avx_time_ms();

    while (g_running) {
        oavx_video_frame_t vf;
        oavx_audio_frame_t af;
        oavx_metadata_frame_t mf;

        oavx_frame_type_t ft = oavx_stream_receive(rx, &vf, &af, &mf, 100);
        if (ft == OAVX_FRAME_TYPE_VIDEO) {
            if (!warned_codec && vf.fourcc != OAVX_FOURCC_VIDEO_TYPE_H264 && vf.fourcc != OAVX_FOURCC_VIDEO_TYPE_HEVC) {
                openavx_cli_status_end();
                oavx_log_write(OAVX_LOG_WARN, "gateway",
                               "Upstream video is not H.264/HEVC; browser preview requires H.264/AVC (RTSP/OpenAVX still active)");
                warned_codec = true;
            }
            oavx_stream_send_video(tx, &vf);
            oavx_stream_release_video(rx, &vf);
            v_count++;
        } else if (ft == OAVX_FRAME_TYPE_AUDIO) {
            oavx_stream_send_audio(tx, &af);
            oavx_stream_release_audio(rx, &af);
            a_count++;
        } else if (ft == OAVX_FRAME_TYPE_METADATA) {
            oavx_stream_send_metadata(tx, &mf);
            oavx_stream_release_metadata(rx, &mf);
        }

        if (oavx_get_log_level() >= OAVX_LOG_INFO) {
            int64_t now = avx_time_ms();
            if (now - last_stat_time >= 1000) {
                int rtsp_clients = enable_rtsp ? oavx_stream_get_rtsp_client_count(tx) : 0;
                int web_clients = enable_web ? oavx_stream_get_web_client_count(tx) : 0;
                openavx_cli_status("gateway", "video=%lu audio=%lu web_clients=%d rtsp_clients=%d",
                                   (unsigned long)v_count, (unsigned long)a_count, web_clients, rtsp_clients);
                last_stat_time = now;
            }
        }
    }

    openavx_cli_status_end();
    oavx_log_write(OAVX_LOG_INFO, "gateway", "Shutting down gateway...");
    if (enable_web) oavx_stream_disable_web_preview(tx);
    if (enable_rtsp) oavx_stream_disable_rtsp_server(tx);
    oavx_stream_close(tx);
    oavx_stream_close(rx);
    oavx_context_destroy(ctx);
    oavx_log_write(OAVX_LOG_INFO, "gateway", "Gateway stopped cleanly");

    return 0;
}
