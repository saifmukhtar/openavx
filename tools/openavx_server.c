/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_server.c - Discovery server CLI command
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "avx_platform.h"
#include "avx_discovery_server.h"
#include "avx_web.h"
#include "openavx.h"
#include "openavx_cli.h"

static volatile bool s_keep_running = true;
static avx_web_server_t *s_web_srv = NULL;
static oavx_stream_t *s_tx_stream = NULL;
static int64_t s_last_ingress_log_ms = 0;
static uint64_t s_ingress_frame_count = 0;
static uint64_t s_ingress_byte_count = 0;

static void on_relay_ingress_frame(
    const uint8_t *frame_data,
    size_t frame_size,
    uint32_t fourcc,
    uint32_t width,
    uint32_t height,
    bool is_keyframe,
    void *user_ctx
) {
    (void)user_ctx;
    if (!frame_data || frame_size == 0) return;

    s_ingress_frame_count++;
    s_ingress_byte_count += frame_size;
    int64_t now_ms = avx_time_ms();

    if (s_last_ingress_log_ms == 0) {
        s_last_ingress_log_ms = now_ms;
    } else if (now_ms - s_last_ingress_log_ms >= 5000) {
        double dt = (double)(now_ms - s_last_ingress_log_ms) / 1000.0;
        double fps = dt > 0.0 ? (double)s_ingress_frame_count / dt : 0.0;
        double mbps = dt > 0.0 ? ((double)s_ingress_byte_count * 8.0) / (dt * 1000000.0) : 0.0;
        const char *cname = (fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) ? "H.264" :
                            (fourcc == 0x47504A4D ? "MJPG" : "RAW");
        oavx_log_write(OAVX_LOG_INFO, "server", "Ingress active: %ux%u %s @ %.1f fps (%.2f Mb/s)",
                       width, height, cname, fps, mbps);
        s_ingress_frame_count = 0;
        s_ingress_byte_count = 0;
        s_last_ingress_log_ms = now_ms;
    }

    if (s_web_srv && fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) {
        avx_web_server_feed_frame(s_web_srv, frame_data, frame_size, 0, is_keyframe);
    }

    if (s_tx_stream) {
        oavx_video_frame_t vf;
        memset(&vf, 0, sizeof(vf));
        vf.width = width;
        vf.height = height;
        vf.fourcc = (oavx_fourcc_video_type_e)fourcc;
        vf.planes[0] = (uint8_t*)frame_data;
        vf.strides[0] = frame_size;
        vf.pts_ns = avx_time_ns();
        vf.dts_ns = vf.pts_ns;
        vf.framerate.num = 30;
        vf.framerate.den = 1;
        oavx_stream_send_video(s_tx_stream, &vf);
    }
}

static void sig_handler(int sig)
{
    (void)sig;
    s_keep_running = false;
}

int openavx_cmd_server(int argc, char** argv)
{
    optind = 1;
    s_keep_running = true;
    uint16_t port = AVX_DISCOVERY_SERVER_DEFAULT_PORT;
    uint16_t web_port = 7958;
    bool enable_web = true;
    const char *bind_ip = "0.0.0.0";
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-port") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-bind") == 0 || strcmp(argv[i], "--bind") == 0) && i + 1 < argc) {
            bind_ip = argv[++i];
        } else if ((strcmp(argv[i], "-web") == 0 || strcmp(argv[i], "--web") == 0) && i + 1 < argc) {
            int p = atoi(argv[++i]);
            if (p > 0) {
                web_port = (uint16_t)p;
                enable_web = true;
            } else {
                enable_web = false;
            }
        } else if (strcmp(argv[i], "--no-web") == 0) {
            enable_web = false;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-help") == 0) {
            printf("OpenAVX Standalone Discovery & Cloud Ingress Relay Daemon\n");
            printf("Usage: %s [options]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -port, --port <port>  Discovery TCP port to listen on (default: 7959)\n");
            printf("  -web,  --web <port>   Web camera ingress & preview port (default: 7958)\n");
            printf("  --no-web              Disable web camera ingress engine\n");
            printf("  -bind, --bind <ip>    IP address to bind to (default: 0.0.0.0)\n");
            printf("  -q,    --quiet        Suppress startup and status messages\n");
            printf("  -h,    --help         Display this help message and exit\n");
            return 0;
        }
    }

    openavx_cli_set_quiet(quiet);

    openavx_cli_header("server");
    openavx_cli_kv("bind", "%s", bind_ip);
    openavx_cli_kv("port", "%u/tcp", port);
    if (enable_web) {
        openavx_cli_kv("web ingress", "%u/tcp (/camera/ws)", web_port);
    }
    oavx_log_write(OAVX_LOG_INFO, "server", "Binding discovery service on %s:%u (TCP)", bind_ip, port);

    avx_discovery_server_t *srv = avx_discovery_server_create(bind_ip, port);
    if (!srv) {
        oavx_log_write(OAVX_LOG_ERROR, "server", "Failed to start discovery server on %s:%u", bind_ip, port);
        return 1;
    }

    /* Initialize native OpenAVX broadcast transmitter stream on TCP 7960 */
    oavx_stream_config_t tx_cfg;
    memset(&tx_cfg, 0, sizeof(tx_cfg));
    tx_cfg.name = "OpenAVX Stream";
    tx_cfg.direction = OAVX_DIR_TRANSMIT;
    tx_cfg.bandwidth = OAVX_BANDWIDTH_HIGHEST;
    tx_cfg.color_format = OAVX_COLOR_FORMAT_BEST;
    tx_cfg.transport = OAVX_TRANSPORT_TCP;
    s_tx_stream = oavx_stream_open(NULL, &tx_cfg);
    if (s_tx_stream) {
        openavx_cli_kv("native stream", "TCP 7960 (OpenAVX wire protocol)");
        oavx_log_write(OAVX_LOG_INFO, "server", "OpenAVX broadcast transmitter active on TCP 7960");
    } else {
        oavx_log_write(OAVX_LOG_WARN, "server", "Failed to initialize OpenAVX broadcast transmitter");
    }

    if (enable_web) {
        avx_web_server_config_t web_cfg = {
            .port = web_port,
            .stream_name = "OpenAVX Cloud Ingress Relay",
            .codec = AVX_RTSP_CODEC_H264,
            .on_ingress_frame = on_relay_ingress_frame,
            .user_ctx = NULL
        };
        s_web_srv = avx_web_server_create(&web_cfg);
        if (s_web_srv) {
            oavx_log_write(OAVX_LOG_INFO, "server", "Web camera ingress service active on port %u (RFC 6455)", web_port);
        } else {
            oavx_log_write(OAVX_LOG_WARN, "server", "Failed to start web camera ingress on port %u", web_port);
        }
    }

    oavx_log_write(OAVX_LOG_INFO, "server", "Discovery & ingress relay active (Ctrl+C to stop)");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    while (s_keep_running) {
        avx_sleep_ms(1000);
    }

    oavx_log_write(OAVX_LOG_INFO, "server", "Shutting down discovery server...");
    if (s_tx_stream) {
        oavx_stream_close(s_tx_stream);
        s_tx_stream = NULL;
    }
    if (s_web_srv) {
        avx_web_server_destroy(s_web_srv);
        s_web_srv = NULL;
    }
    avx_discovery_server_destroy(srv);
    oavx_log_write(OAVX_LOG_INFO, "server", "Discovery server stopped cleanly");

    return 0;
}
