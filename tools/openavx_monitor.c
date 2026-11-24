/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_monitor.c - Terminal video monitor and audio player CLI
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include "avx_platform.h"
#if !defined(AVX_OS_WINDOWS) && !defined(_WIN32)
#include <sys/ioctl.h>
#endif

#include "openavx.h"
#include "avx_audio.h"
#include "avx_hdr.h"
#include "avx_audio_output.h"
#include "openavx_cli.h"

static volatile bool g_running = true;

static void handle_signal(int sig) {
    (void)sig;
    g_running = false;
}

static inline void get_terminal_size(int* out_cols, int* out_rows) {
#if defined(AVX_OS_WINDOWS) || defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        *out_cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        *out_rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 5) {
        *out_cols = ws.ws_col;
        *out_rows = ws.ws_row;
        return;
    }
#endif
    *out_cols = 80;
    *out_rows = 24;
}

static inline uint8_t clamp_u8(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

static void append_fmt(char** p, const char* end, const char* fmt, ...) {
    if (!p || !*p || !end || *p >= end) return;
    size_t avail = (size_t)(end - *p);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, avail, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    *p += ((size_t)n < avail) ? (size_t)n : (avail - 1);
}

static inline void yuv_to_rgb(int y_val, int u, int v, uint8_t* r, uint8_t* g, uint8_t* b) {
    int c = y_val - 16;
    int d_u = u - 128;
    int e_v = v - 128;

    *r = clamp_u8((298 * c + 409 * e_v + 128) >> 8);
    *g = clamp_u8((298 * c - 100 * d_u - 208 * e_v + 128) >> 8);
    *b = clamp_u8((298 * c + 516 * d_u + 128) >> 8);
}

static bool frame_is_renderable(const oavx_video_frame_t* frame) {
    if (!frame || !frame->planes[0] || frame->width == 0 || frame->height == 0) {
        return false;
    }

    switch (frame->fourcc) {
        case OAVX_FOURCC_VIDEO_TYPE_RGBA:
        case OAVX_FOURCC_VIDEO_TYPE_RGBX:
        case OAVX_FOURCC_VIDEO_TYPE_BGRA:
        case OAVX_FOURCC_VIDEO_TYPE_BGRX:
            return frame->strides[0] >= (size_t)frame->width * 4;

        case OAVX_FOURCC_VIDEO_TYPE_UYVY:
        case OAVX_FOURCC_VIDEO_TYPE_UYVA:
            return frame->strides[0] >= (size_t)frame->width * 2;

        case OAVX_FOURCC_VIDEO_TYPE_P216:
        case OAVX_FOURCC_VIDEO_TYPE_PA16:
            return frame->strides[0] >= (size_t)frame->width * 2 &&
                   (!frame->planes[1] || frame->strides[1] >= (size_t)((frame->width + 1U) & ~1U) * 2);

        case OAVX_FOURCC_VIDEO_TYPE_NV12:
            return frame->planes[1] != NULL &&
                   frame->strides[0] >= (size_t)frame->width &&
                   frame->strides[1] >= (size_t)((frame->width + 1U) & ~1U);

        case OAVX_FOURCC_VIDEO_TYPE_I420:
        case OAVX_FOURCC_VIDEO_TYPE_YV12: {
            size_t cw = (size_t)((frame->width + 1U) / 2U);
            return frame->planes[1] != NULL &&
                   frame->planes[2] != NULL &&
                   frame->strides[0] >= (size_t)frame->width &&
                   frame->strides[1] >= cw &&
                   frame->strides[2] >= cw;
        }

        default:
            return false;
    }
}

static void get_pixel_rgb(const oavx_video_frame_t* frame, int x, int y, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (!frame_is_renderable(frame)) {
        *r = *g = *b = 0;
        return;
    }

    if (x < 0) x = 0;
    if (x >= (int)frame->width) x = (int)frame->width - 1;
    if (y < 0) y = 0;
    if (y >= (int)frame->height) y = (int)frame->height - 1;

    const uint8_t* data = frame->planes[0];

    switch (frame->fourcc) {
        case OAVX_FOURCC_VIDEO_TYPE_RGBA:
        case OAVX_FOURCC_VIDEO_TYPE_RGBX: {
            size_t off = (size_t)y * frame->strides[0] + (size_t)x * 4;
            *r = data[off + 0];
            *g = data[off + 1];
            *b = data[off + 2];
            break;
        }

        case OAVX_FOURCC_VIDEO_TYPE_BGRA:
        case OAVX_FOURCC_VIDEO_TYPE_BGRX: {
            size_t off = (size_t)y * frame->strides[0] + (size_t)x * 4;
            *b = data[off + 0];
            *g = data[off + 1];
            *r = data[off + 2];
            break;
        }

        case OAVX_FOURCC_VIDEO_TYPE_P216:
        case OAVX_FOURCC_VIDEO_TYPE_PA16: {
            /* Semi-planar 16-bit P216/PA16: Y plane followed by interleaved UV plane */
            size_t y_off = (size_t)y * frame->strides[0] + (size_t)x * 2;
            const uint16_t* py = (const uint16_t*)(data + y_off);
            const uint16_t* puv;
            if (frame->planes[1]) {
                size_t uv_off = (size_t)y * frame->strides[1] + (size_t)(x / 2) * 4;
                puv = (const uint16_t*)((const uint8_t*)frame->planes[1] + uv_off);
            } else {
                size_t uv_off = frame->strides[0] * frame->height +
                                (size_t)y * frame->strides[0] + (size_t)(x / 2) * 4;
                puv = (const uint16_t*)(data + uv_off);
            }
            yuv_to_rgb(py[0] >> 8, puv[0] >> 8, puv[1] >> 8, r, g, b);
            break;
        }

        case OAVX_FOURCC_VIDEO_TYPE_UYVY:
        case OAVX_FOURCC_VIDEO_TYPE_UYVA: {
            int pair_x = x & ~1;
            size_t off = (size_t)y * frame->strides[0] + (size_t)pair_x * 2;
            int u = data[off + 0];
            int y0 = data[off + 1];
            int v = 128;
            int y1 = y0;
            if (pair_x + 1 < (int)frame->width) {
                v = data[off + 2];
                y1 = data[off + 3];
            }
            yuv_to_rgb((x & 1) ? y1 : y0, u, v, r, g, b);
            break;
        }

        case OAVX_FOURCC_VIDEO_TYPE_NV12: {
            const uint8_t* y_row = frame->planes[0] + (size_t)y * frame->strides[0];
            const uint8_t* uv_row = frame->planes[1] + (size_t)(y / 2) * frame->strides[1];
            yuv_to_rgb(y_row[x], uv_row[(x / 2) * 2 + 0], uv_row[(x / 2) * 2 + 1], r, g, b);
            break;
        }

        case OAVX_FOURCC_VIDEO_TYPE_I420: {
            const uint8_t* y_row = frame->planes[0] + (size_t)y * frame->strides[0];
            const uint8_t* u_row = frame->planes[1] + (size_t)(y / 2) * frame->strides[1];
            const uint8_t* v_row = frame->planes[2] + (size_t)(y / 2) * frame->strides[2];
            yuv_to_rgb(y_row[x], u_row[x / 2], v_row[x / 2], r, g, b);
            break;
        }

        case OAVX_FOURCC_VIDEO_TYPE_YV12: {
            const uint8_t* y_row = frame->planes[0] + (size_t)y * frame->strides[0];
            const uint8_t* v_row = frame->planes[1] + (size_t)(y / 2) * frame->strides[1];
            const uint8_t* u_row = frame->planes[2] + (size_t)(y / 2) * frame->strides[2];
            yuv_to_rgb(y_row[x], u_row[x / 2], v_row[x / 2], r, g, b);
            break;
        }

        default:
            *r = *g = *b = 0;
            break;
    }
}

static void print_usage(const char* prog) {
    printf("OpenAVX Standalone Terminal Player & TrueColor Video Monitor\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -u, --url <addr>       Direct connect to source URL (e.g. 127.0.0.1:7960)\n");
    printf("  -s, --source <name>    Connect to discovered source by name\n");
    printf("  -W, --width <cols>     Override render width in terminal columns (default: auto)\n");
    printf("  -H, --height <rows>    Override render height in terminal rows (default: auto)\n");
    printf("  -n, --frames <count>   Exit after rendering <count> frames (0 = loop forever)\n");
    printf("  -b, --braille          Use high-density Unicode Braille 2x4 dot rendering\n");
    printf("  -f, --fps <rate>       Max rendering frame rate (default: 30)\n");
    printf("  -t, --telemetry        Print stream telemetry JSON upon exit\n");
    printf("  -a, --audio <backend>  Audio backend: auto, pulse, alsa, pipe, none (default: auto)\n");
    printf("      --no-audio         Disable audio output\n");
    printf("  -q, --quiet            Suppress banner and informational logs\n");
    printf("  -h, --help             Display this help message\n\n");
    printf("Examples:\n");
    printf("  %s                               # Auto-discover and monitor first available source\n", prog);
    printf("  %s --url 127.0.0.1:7960          # Connect directly to local sender on port 7960\n", prog);
    printf("  %s --source \"STUDIO_CAM_1\"       # Connect to specific studio camera\n", prog);
    printf("  %s --url 127.0.0.1:7960 --frames 5 # Render 5 frames and exit cleanly\n", prog);
}

int openavx_cmd_monitor(int argc, char** argv) {
    optind = 1;
    g_running = true;
    char target_url[128] = "";
    char target_name[128] = "";
    char audio_backend[32] = "auto";
    int user_width = 0;
    int user_height = 0;
    int max_frames = 0;
    bool braille_mode = false;
    int target_fps = 30;
    bool show_telemetry = false;
    bool quiet = false;

    static struct option long_opts[] = {
        {"url",       required_argument, 0, 'u'},
        {"source",    required_argument, 0, 's'},
        {"width",     required_argument, 0, 'W'},
        {"height",    required_argument, 0, 'H'},
        {"frames",    required_argument, 0, 'n'},
        {"braille",   no_argument,       0, 'b'},
        {"fps",       required_argument, 0, 'f'},
        {"telemetry", no_argument,       0, 't'},
        {"stats",     no_argument,       0, 't'},
        {"audio",     required_argument, 0, 'a'},
        {"no-audio",  no_argument,       0, 1001},
        {"quiet",     no_argument,       0, 'q'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "u:s:W:H:n:bf:ta:qh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'u': strncpy(target_url, optarg, sizeof(target_url) - 1); break;
            case 's': strncpy(target_name, optarg, sizeof(target_name) - 1); break;
            case 'W': user_width = atoi(optarg); break;
            case 'H': user_height = atoi(optarg); break;
            case 'n': max_frames = atoi(optarg); break;
            case 'b': braille_mode = true; break;
            case 'f': target_fps = atoi(optarg); if (target_fps <= 0) target_fps = 30; break;
            case 't': show_telemetry = true; break;
            case 'a': strncpy(audio_backend, optarg, sizeof(audio_backend) - 1); break;
            case 1001: strncpy(audio_backend, "none", sizeof(audio_backend) - 1); break;
            case 'q': quiet = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    openavx_cli_set_quiet(quiet);

    /* Signal handlers for clean terminal restore */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Initialize OpenAVX context engine */
    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        oavx_log_write(OAVX_LOG_ERROR, "monitor", "Failed to initialize OpenAVX context engine");
        return 1;
    }

    /* Source discovery if neither URL nor Name was given */
    if (target_url[0] == '\0' && target_name[0] == '\0') {
        oavx_log_write(OAVX_LOG_INFO, "monitor", "Discovering active OpenAVX sources...");
        oavx_discovery_t *p_find = oavx_discovery_create(ctx, NULL);
        if (!p_find) {
            oavx_log_write(OAVX_LOG_ERROR, "monitor", "Failed to create discovery instance");
            oavx_context_destroy(ctx);
            return 1;
        }

        /* Wait up to 1500ms for sources */
        oavx_discovery_wait(p_find, 1500);
        oavx_endpoint_t sources[64];
        size_t num_sources = oavx_discovery_get_sources(p_find, sources, 64);

        if (num_sources == 0) {
            oavx_log_write(OAVX_LOG_ERROR, "monitor", "No active OpenAVX sources found (use --url <ip:port> to connect directly)");
            oavx_discovery_destroy(p_find);
            oavx_context_destroy(ctx);
            return 1;
        }

        oavx_log_write(OAVX_LOG_INFO, "monitor", "Found %zu active OpenAVX source(s):", num_sources);
        for (size_t i = 0; i < num_sources; ++i) {
            oavx_log_write(OAVX_LOG_INFO, "monitor", "  [%zu] %s (%s)", i,
                           sources[i].name ? sources[i].name : "<unknown>",
                           sources[i].address ? sources[i].address : "mDNS");
        }

        /* Auto-select first source */
        if (sources[0].name) strncpy(target_name, sources[0].name, sizeof(target_name) - 1);
        if (sources[0].address) strncpy(target_url, sources[0].address, sizeof(target_url) - 1);
        oavx_log_write(OAVX_LOG_INFO, "monitor", "Auto-selecting source '%s' (%s)", target_name, target_url);

        oavx_discovery_destroy(p_find);
    }

    /* Configure OpenAVX Stream Receiver */
    oavx_stream_config_t recv_create = {
        .name = target_name[0] ? target_name : "OPENAVX_MONITOR_TARGET",
        .endpoint = target_url[0] ? target_url : NULL,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_RGBX_RGBA, /* 32-bit RGB with alpha */
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };

    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    if (!p_recv) {
        oavx_log_write(OAVX_LOG_ERROR, "monitor", "Failed to connect to OpenAVX source (%s)",
                       target_url[0] ? target_url : target_name);
        oavx_context_destroy(ctx);
        return 1;
    }

    openavx_cli_header("monitor");
    openavx_cli_kv("source", "%s%s%s%s",
                   target_name[0] ? target_name : (target_url[0] ? target_url : "auto"),
                   (target_name[0] && target_url[0]) ? " (" : "",
                   (target_name[0] && target_url[0]) ? target_url : "",
                   (target_name[0] && target_url[0]) ? ")" : "");
    openavx_cli_kv("fps", "%d", target_fps);
    openavx_cli_kv("audio", "%s", audio_backend);
    openavx_cli_kv("render", "%s", braille_mode ? "braille" : "truecolor");

    /* Hide cursor and clear screen if running in a TTY */
    bool is_terminal = isatty(STDOUT_FILENO);
    if (is_terminal) {
        printf("\033[?25l\033[2J");
        fflush(stdout);
    }

    int frame_count = 0;
    int audio_packets = 0;
    int audio_sample_rate = 0;
    int audio_channels = 0;
    int64_t last_timecode = 0;

    int render_w = user_width;
    int render_h = user_height;

    avx_audio_output_t* audio_out = NULL;

    /* Frame buffer for atomic terminal writing */
    size_t out_buf_cap = 512 * 1024;
    char* out_buf = (char*)malloc(out_buf_cap);
    if (!out_buf) {
        oavx_log_write(OAVX_LOG_ERROR, "monitor", "Out of memory allocating terminal frame buffer");
        oavx_stream_close(p_recv);
        oavx_context_destroy(ctx);
        return 1;
    }

    uint64_t frame_interval_ns = 1000000000ULL / (uint64_t)target_fps;
    int64_t last_render_time_ns = 0;

    while (g_running) {
        /* Determine terminal dimensions */
        if (user_width <= 0 || user_height <= 0) {
            int term_w = 80, term_h = 24;
            get_terminal_size(&term_w, &term_h);
            render_w = term_w;
            render_h = (term_h > 3) ? term_h - 3 : term_h; /* reserve lines for header & footer */
        }

        if (render_w < 10) render_w = 10;
        if (render_h < 5) render_h = 5;

        /* Ensure write buffer is large enough: ~45 bytes per cell + headers */
        size_t needed_buf = (size_t)render_w * render_h * 50 + 8192;
        if (needed_buf > out_buf_cap) {
            char* nb = (char*)realloc(out_buf, needed_buf);
            if (nb) {
                out_buf = nb;
                out_buf_cap = needed_buf;
            }
        }

        oavx_video_frame_t vframe;
        oavx_audio_frame_t aframe;
        oavx_metadata_frame_t mframe;

        oavx_frame_type_t ftype = oavx_stream_receive(p_recv, &vframe, &aframe, &mframe, 30);

        if (ftype == OAVX_FRAME_TYPE_AUDIO) {
            audio_packets++;
            (void)audio_packets;
            audio_sample_rate = aframe.sample_rate;
            audio_channels = aframe.channels;

            const oavx_audio_frame_t* p_play_frame = &aframe;
            oavx_audio_frame_t downmixed_frame = {0};
            bool did_downmix = false;

            if (aframe.channels > 2) {
                if (avx_audio_frame_downmix_to_stereo(&aframe, &downmixed_frame, AVX_AUDIO_NORMALIZE_ATTEN_3DB)) {
                    p_play_frame = &downmixed_frame;
                    did_downmix = true;
                }
            }

            if (!audio_out && strcmp(audio_backend, "none") != 0) {
                audio_out = avx_audio_output_create(audio_backend, aframe.sample_rate, p_play_frame->channels);
            }
            if (audio_out) {
                avx_audio_output_write(audio_out, p_play_frame);
            }

            if (did_downmix) {
                avx_audio_frame_free_data(&downmixed_frame);
            }
            oavx_stream_release_audio(p_recv, &aframe);
            continue;
        } else if (ftype == OAVX_FRAME_TYPE_METADATA) {
            oavx_stream_release_metadata(p_recv, &mframe);
            continue;
        } else if (ftype != OAVX_FRAME_TYPE_VIDEO) {
            continue;
        }

        /* Frame Rate Throttle */
        int64_t now_ns = avx_time_ns();
        uint64_t elapsed_ns = (uint64_t)(now_ns - last_render_time_ns);
        if (elapsed_ns < frame_interval_ns && max_frames == 0) {
            oavx_stream_release_video(p_recv, &vframe);
            continue;
        }
        last_render_time_ns = now_ns;

        frame_count++;
        last_timecode = vframe.pts_ns;
        (void)last_timecode;

        /* Build ASCII / ANSI Render Buffer */
        char* p = out_buf;
        char* end = out_buf + out_buf_cap;

        /* Cursor Home */
        append_fmt(&p, end, "\033[H");

        /* FourCC ASCII string */
        char fcc_str[5] = {
            (char)(vframe.fourcc & 0xFF),
            (char)((vframe.fourcc >> 8) & 0xFF),
            (char)((vframe.fourcc >> 16) & 0xFF),
            (char)((vframe.fourcc >> 24) & 0xFF),
            '\0'
        };
        for (int i = 0; i < 4; ++i) {
            if (fcc_str[i] < 32 || fcc_str[i] > 126) fcc_str[i] = '?';
        }

        /* Check for HDR metadata */
        avx_color_info_t cinfo;
        const char* hdr_str = "";
        if (avx_hdr_frame_get_color_info(&vframe, &cinfo) && avx_hdr_is_hdr(&cinfo)) {
            if (cinfo.transfer == AVX_COLOR_TRANSFER_BT2100_HLG) hdr_str = " | HDR: HLG (Rec.2020)";
            else if (cinfo.transfer == AVX_COLOR_TRANSFER_BT2100_PQ) hdr_str = " | HDR: PQ (Rec.2020)";
        }

        /* Header HUD */
        append_fmt(&p, end,
            "\033[1;37;44m [openavx Monitor] Source: %-18.18s | %ux%u @ %u/%u fps | FourCC: %s%s \033[0m\n",
            recv_create.name, vframe.width, vframe.height,
            vframe.framerate.num, vframe.framerate.den ? vframe.framerate.den : 1,
            fcc_str, hdr_str);

        if (!frame_is_renderable(&vframe)) {
            append_fmt(&p, end,
                "\033[33m [openavx Monitor] Undecoded/unsupported FourCC '%s' (%ux%u, stride=%zu) — "
                "awaiting keyframe or runtime decoder \033[0m\n",
                fcc_str, vframe.width, vframe.height, vframe.strides[0]);
            for (int y = 1; y < render_h; ++y) {
                append_fmt(&p, end, "\033[K\n");
            }
        } else if (!braille_mode) {
            /* TrueColor Half-Block Rendering (2 vertical pixels per character) */
            int pixel_h = render_h * 2;
            for (int y = 0; y < render_h; ++y) {
                int y_top = (y * 2 * (int)vframe.height) / pixel_h;
                int y_bot = ((y * 2 + 1) * (int)vframe.height) / pixel_h;

                for (int x = 0; x < render_w; ++x) {
                    int src_x = (x * (int)vframe.width) / render_w;
                    uint8_t rt, gt, bt, rb, gb, bb;
                    get_pixel_rgb(&vframe, src_x, y_top, &rt, &gt, &bt);
                    get_pixel_rgb(&vframe, src_x, y_bot, &rb, &gb, &bb);

                    /* Foreground = Top half, Background = Bottom half */
                    append_fmt(&p, end, "\033[38;2;%u;%u;%um\033[48;2;%u;%u;%um▀",
                               rt, gt, bt, rb, gb, bb);
                }
                append_fmt(&p, end, "\033[0m\n");
            }
        } else {
            /* Braille 2x4 Dot Matrix Mode */
            int braille_w = render_w;
            int braille_h = render_h;
            for (int by = 0; by < braille_h; ++by) {
                for (int bx = 0; bx < braille_w; ++bx) {
                    uint8_t dots = 0;
                    uint32_t avg_r = 0, avg_g = 0, avg_b = 0;

                    for (int dy = 0; dy < 4; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            int px = ((bx * 2 + dx) * (int)vframe.width) / (braille_w * 2);
                            int py = ((by * 4 + dy) * (int)vframe.height) / (braille_h * 4);
                            uint8_t r, g, b;
                            get_pixel_rgb(&vframe, px, py, &r, &g, &b);
                            avg_r += r; avg_g += g; avg_b += b;
                            int luma = (66 * r + 129 * g + 25 * b + 128) >> 8;
                            if (luma > 90) {
                                static const uint8_t dot_bits[4][2] = {
                                    {0x01, 0x08},
                                    {0x02, 0x10},
                                    {0x04, 0x20},
                                    {0x40, 0x80}
                                };
                                dots |= dot_bits[dy][dx];
                            }
                        }
                    }
                    avg_r /= 8; avg_g /= 8; avg_b /= 8;

                    /* UTF-8 encode Braille pattern U+2800..U+28FF */
                    uint32_t cp = 0x2800 | dots;
                    char utf8[4];
                    utf8[0] = (char)(0xE0 | (cp >> 12));
                    utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    utf8[2] = (char)(0x80 | (cp & 0x3F));
                    utf8[3] = '\0';

                    append_fmt(&p, end, "\033[38;2;%u;%u;%um%s", avg_r, avg_g, avg_b, utf8);
                }
                append_fmt(&p, end, "\033[0m\n");
            }
        }

        char audio_desc[64];
        if (audio_channels > 2) {
            snprintf(audio_desc, sizeof(audio_desc), "%d Hz, %d ch -> 2 ch", audio_sample_rate, audio_channels);
        } else {
            snprintf(audio_desc, sizeof(audio_desc), "%d Hz, %d ch", audio_sample_rate, audio_channels);
        }

        /* Footer status bar */
        oavx_telemetry_t telem = {0};
        oavx_stream_get_telemetry(p_recv, &telem);

        append_fmt(&p, end,
            "\033[1;30;47m Frame: %-5d | %4.1f FPS | %5.1f Mbps | RTT: %4.1fms | Jitter: %4.2fms | Q: %uB | Audio: %s | [Ctrl+C to Exit] \033[0m",
            frame_count, (double)telem.fps, (double)telem.bitrate_mbps,
            (double)telem.kernel_rtt_ms, (double)telem.inter_frame_jitter_ms,
            telem.socket_recv_q_bytes,
            audio_out ? avx_audio_output_get_backend_name(audio_out) : (strcmp(audio_backend, "none") == 0 ? "Muted" : "Connecting..."));

        /* Single atomic terminal write */
        size_t to_write = (size_t)(p - out_buf);
        ssize_t written = write(STDOUT_FILENO, out_buf, to_write);
        (void)written;

        oavx_stream_release_video(p_recv, &vframe);

        if (max_frames > 0 && frame_count >= max_frames) {
            break;
        }
    }

    /* Print stream stats JSON if requested */
    if (show_telemetry) {
        oavx_telemetry_t final_telem = {0};
        if (oavx_stream_get_telemetry(p_recv, &final_telem)) {
            char json_buf[2048];
            oavx_telemetry_to_json(&final_telem, json_buf, sizeof(json_buf));
            oavx_log_write(OAVX_LOG_INFO, "monitor", "Stream telemetry JSON:");
            printf("%s\n", json_buf);
        }
    }

    /* Restore terminal */
    if (is_terminal) {
        printf("\033[?25h\033[0m\n");
        fflush(stdout);
    } else {
        printf("\n");
    }

    if (audio_out) {
        avx_audio_output_destroy(audio_out);
        audio_out = NULL;
    }

    free(out_buf);
    oavx_stream_close(p_recv);
    oavx_context_destroy(ctx);

    return 0;
}
