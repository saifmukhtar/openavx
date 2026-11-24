/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_test_pattern.c - SMPTE test pattern and tone generator CLI
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include "avx_platform.h"

#include "openavx.h"
#include "smpte_h264_pattern.h"
#include "openavx_cli.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static volatile bool g_running = true;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = false;
}

typedef enum {
    CODEC_UYVY,
    CODEC_H264
} codec_type_e;

typedef enum {
    PATTERN_SMPTE_BARS,
    PATTERN_COLOR_BARS_100,
    PATTERN_GRID,
    PATTERN_RAMP,
    PATTERN_SOLID_WHITE,
    PATTERN_SOLID_BLACK,
    PATTERN_SOLID_GRAY
} pattern_type_e;

typedef enum {
    TONE_1000HZ,
    TONE_440HZ,
    TONE_BEEP,
    TONE_NONE
} tone_type_e;

/* Rec. 709 / Rec. 601 8-bit YCbCr Color definitions for UYVY (U, Y, V) */
typedef struct {
    uint8_t u;
    uint8_t y;
    uint8_t v;
} yuv_color_t;

#if defined(__GNUC__) || defined(__clang__)
#define AVX_UNUSED __attribute__((unused))
#else
#define AVX_UNUSED
#endif

static const yuv_color_t COLOR_75_WHITE   = { 128, 180, 128 };
static const yuv_color_t COLOR_75_YELLOW  AVX_UNUSED = {  44, 168, 142 };
static const yuv_color_t COLOR_75_CYAN    AVX_UNUSED = { 148, 145,  24 };
static const yuv_color_t COLOR_75_GREEN   AVX_UNUSED = {  64, 133,  38 };
static const yuv_color_t COLOR_75_MAGENTA = { 192,  63, 218 };
static const yuv_color_t COLOR_75_RED     AVX_UNUSED = { 108,  51, 232 };
static const yuv_color_t COLOR_75_BLUE    = { 212,  28, 114 };
static const yuv_color_t COLOR_BLACK      = { 128,  16, 128 };

static const yuv_color_t COLOR_100_WHITE  = { 128, 235, 128 };
static const yuv_color_t COLOR_100_YELLOW AVX_UNUSED = {  16, 210, 146 };
static const yuv_color_t COLOR_100_CYAN   AVX_UNUSED = { 166, 170,  16 };
static const yuv_color_t COLOR_100_GREEN  AVX_UNUSED = {  54, 145,  34 };
static const yuv_color_t COLOR_100_MAGENTA AVX_UNUSED = { 202, 106, 222 };
static const yuv_color_t COLOR_100_RED    AVX_UNUSED = {  90,  81, 240 };
static const yuv_color_t COLOR_100_BLUE   AVX_UNUSED = { 240,  41, 110 };

static const yuv_color_t SMPTE_75_BARS[8] = {
    { 128, 180, 128 }, /* 75% White / Gray */
    {  44, 168, 142 }, /* Yellow */
    { 148, 145,  24 }, /* Cyan */
    {  64, 133,  38 }, /* Green */
    { 192,  63, 218 }, /* Magenta */
    { 108,  51, 232 }, /* Red */
    { 212,  28, 114 }, /* Blue */
    { 128,  16, 128 }  /* Black */
};

static const yuv_color_t COLOR_100_BARS[8] = {
    { 128, 235, 128 }, /* 100% White */
    {  16, 210, 146 }, /* Yellow */
    { 166, 170,  16 }, /* Cyan */
    {  54, 145,  34 }, /* Green */
    { 202, 106, 222 }, /* Magenta */
    {  90,  81, 240 }, /* Red */
    { 240,  41, 110 }, /* Blue */
    { 128,  16, 128 }  /* Black */
};

static inline void set_macropixel_uyvy(uint8_t* p, yuv_color_t c0, yuv_color_t c1) {
    p[0] = (uint8_t)(((int)c0.u + c1.u) >> 1);
    p[1] = c0.y;
    p[2] = (uint8_t)(((int)c0.v + c1.v) >> 1);
    p[3] = c1.y;
}

static void render_base_pattern(uint8_t* buf, int width, int height, pattern_type_e pat) {
    int stride = width * 2;

    switch (pat) {
    case PATTERN_SMPTE_BARS:
    case PATTERN_COLOR_BARS_100: {
        const yuv_color_t* bars = (pat == PATTERN_COLOR_BARS_100) ? COLOR_100_BARS : SMPTE_75_BARS;
        int top_h = (height * 3) / 4;

        /* Top 75%: 8 Primary Color Bars */
        for (int y = 0; y < top_h; ++y) {
            uint8_t* row = buf + y * stride;
            for (int x = 0; x < width; x += 2) {
                int bar_idx = (x * 8) / width;
                if (bar_idx >= 8) bar_idx = 7;
                set_macropixel_uyvy(row + x * 2, bars[bar_idx], bars[bar_idx]);
            }
        }

        /* Bottom 25%: SMPTE cast bars (Blue, White, Purple, Black Pluge) */
        for (int y = top_h; y < height; ++y) {
            uint8_t* row = buf + y * stride;
            for (int x = 0; x < width; x += 2) {
                int col_idx = (x * 4) / width;
                yuv_color_t c;
                if (col_idx == 0) c = COLOR_75_BLUE;
                else if (col_idx == 1) c = COLOR_100_WHITE;
                else if (col_idx == 2) c = COLOR_75_MAGENTA;
                else c = COLOR_BLACK;
                set_macropixel_uyvy(row + x * 2, c, c);
            }
        }
        break;
    }
    case PATTERN_GRID: {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = buf + y * stride;
            bool is_grid_y = (y % 60 == 0) || (y == height - 1);
            for (int x = 0; x < width; x += 2) {
                bool is_grid_x = (x % 60 < 2);
                yuv_color_t c = (is_grid_y || is_grid_x) ? COLOR_100_WHITE : COLOR_BLACK;
                set_macropixel_uyvy(row + x * 2, c, c);
            }
        }
        break;
    }
    case PATTERN_RAMP: {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = buf + y * stride;
            for (int x = 0; x < width; x += 2) {
                uint8_t y_val = (uint8_t)(16 + (x * (235 - 16)) / width);
                yuv_color_t c = { 128, y_val, 128 };
                set_macropixel_uyvy(row + x * 2, c, c);
            }
        }
        break;
    }
    case PATTERN_SOLID_WHITE: {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = buf + y * stride;
            for (int x = 0; x < width; x += 2) {
                set_macropixel_uyvy(row + x * 2, COLOR_100_WHITE, COLOR_100_WHITE);
            }
        }
        break;
    }
    case PATTERN_SOLID_BLACK: {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = buf + y * stride;
            for (int x = 0; x < width; x += 2) {
                set_macropixel_uyvy(row + x * 2, COLOR_BLACK, COLOR_BLACK);
            }
        }
        break;
    }
    case PATTERN_SOLID_GRAY: {
        for (int y = 0; y < height; ++y) {
            uint8_t* row = buf + y * stride;
            for (int x = 0; x < width; x += 2) {
                set_macropixel_uyvy(row + x * 2, COLOR_75_WHITE, COLOR_75_WHITE);
            }
        }
        break;
    }
    }
}

static void overlay_moving_ticker(uint8_t* dest, const uint8_t* src, int width, int height, int bar_pos_x, int bar_w) {
    int stride = width * 2;
    memcpy(dest, src, (size_t)stride * height);

    /* Draw moving high-contrast white box with dark border across the screen */
    int x_start = bar_pos_x;
    int x_end = bar_pos_x + bar_w;
    if (x_end > width) x_end = width;

    int box_y_start = height / 4;
    int box_y_end = (height * 3) / 4;

    for (int y = box_y_start; y < box_y_end; ++y) {
        uint8_t* row = dest + y * stride;
        for (int x = x_start; x < x_end; x += 2) {
            yuv_color_t c = (y == box_y_start || y == box_y_end - 1 || x == x_start || x >= x_end - 2)
                            ? COLOR_BLACK : COLOR_100_WHITE;
            set_macropixel_uyvy(row + x * 2, c, c);
        }
    }
}

static void generate_audio_frame(float* left, float* right, uint32_t frames, tone_type_e tone, int64_t* sample_counter) {
    const double sample_rate = 48000.0;
    const double amplitude = 0.10; /* -20 dBFS nominal broadcast line level */

    double freq = 1000.0;
    if (tone == TONE_440HZ) freq = 440.0;

    for (uint32_t i = 0; i < frames; ++i) {
        int64_t s_idx = (*sample_counter)++;
        double val = 0.0;

        if (tone == TONE_1000HZ || tone == TONE_440HZ) {
            double t = (double)s_idx / sample_rate;
            val = amplitude * sin(2.0 * M_PI * freq * t);
        } else if (tone == TONE_BEEP) {
            /* 100ms tone burst at the top of every second (4800 samples) */
            int second_pos = (int)(s_idx % 48000);
            if (second_pos < 4800) {
                double t = (double)s_idx / sample_rate;
                val = amplitude * sin(2.0 * M_PI * 1000.0 * t);
            } else {
                val = 0.0;
            }
        } else {
            val = 0.0;
        }

        left[i] = (float)val;
        right[i] = (float)val;
    }
}

static void print_usage(const char* prog) {
    printf("OpenAVX Test Pattern Generator (Broadcast Signal Generator)\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -n, --name <NAME>       Stream name (default: \"OpenAVX Test Pattern\")\n");
    printf("  -p, --pattern <TYPE>    Test pattern: smpte, bars100, grid, ramp, white, black, gray (default: smpte)\n");
    printf("  -r, --res <RESOLUTION>  Video resolution: 1080p, 720p, 4k, or WxH (default: 1920x1080)\n");
    printf("  -f, --fps <FRAMERATE>   Framerate: 60, 59.94, 50, 30, 29.97, 25, 24 (default: 60)\n");
    printf("  -t, --tone <TONE>       Audio tone: 1000, 440, beep, none (default: 1000 Hz @ -20 dBFS)\n");
    printf("  -C, --codec <CODEC>     Video codec: uyvy, h264 (default: uyvy, auto-switches to h264 for web/rtsp)\n");
    printf("      --h264              Shorthand for --codec h264 (Annex-B H.264 bitstream)\n");
    printf("  -W, --web [PORT]        Enable embedded HTTP/fMP4 Web Preview server (default: 8080)\n");
    printf("  -R, --rtsp [PORT]       Enable native RTSP 1.0 server (default: 8554)\n");
    printf("  -m, --moving            Enable moving live motion ticker (UYVY only, default: enabled)\n");
    printf("      --no-moving         Disable moving live motion ticker\n");
    printf("  -g, --groups <GROUPS>   Broadcast groups (default: public)\n");
    printf("  -c, --count <FRAMES>    Transmit N frames then exit (default: run indefinitely)\n");
    printf("  -q, --quiet             Quiet mode, suppress live frame stats\n");
    printf("  -h, --help              Display this help and exit\n\n");
    printf("Examples:\n");
    printf("  %s                               # 1080p60 SMPTE bars with 1kHz tone\n", prog);
    printf("  %s --web                         # Instant Web preview on http://localhost:8080\n", prog);
    printf("  %s --rtsp 8554 --web 8080 --h264 # Dual RTSP & Web streaming reference bars\n", prog);
    printf("  %s --res 4k --fps 59.94          # 4K UHD 59.94 FPS\n", prog);
    printf("  %s --pattern grid --tone beep    # Alignment grid with 1-second sync beeper\n", prog);
}

int openavx_cmd_pattern(int argc, char** argv) {
    optind = 1;
    g_running = true;
    char stream_name[256] = "OpenAVX Test Pattern";
    char groups[256] = "public";
    pattern_type_e pat_type = PATTERN_SMPTE_BARS;
    tone_type_e tone_type = TONE_1000HZ;
    codec_type_e codec = CODEC_UYVY;
    bool codec_explicitly_set = false;
    bool enable_web = false;
    uint16_t web_port = 8080;
    bool enable_rtsp = false;
    uint16_t rtsp_port = 8554;
    char rtsp_path[128] = "live";
    int width = 1920;
    int height = 1080;
    int fps_n = 60000;
    int fps_d = 1000;
    bool moving_bar = true;
    int frame_limit = 0;
    bool quiet = false;
    const char* bad_pattern = NULL;
    const char* bad_res = NULL;
    const char* bad_codec = NULL;

    static struct option long_opts[] = {
        { "name",      required_argument, 0, 'n' },
        { "pattern",   required_argument, 0, 'p' },
        { "res",       required_argument, 0, 'r' },
        { "fps",       required_argument, 0, 'f' },
        { "tone",      required_argument, 0, 't' },
        { "codec",     required_argument, 0, 'C' },
        { "h264",      no_argument,       0, 1002 },
        { "web",       optional_argument, 0, 'W' },
        { "rtsp",      optional_argument, 0, 'R' },
        { "moving",    no_argument,       0, 'm' },
        { "no-moving", no_argument,       0, 1001 },
        { "groups",    required_argument, 0, 'g' },
        { "count",     required_argument, 0, 'c' },
        { "quiet",     no_argument,       0, 'q' },
        { "help",      no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:p:r:f:t:C:W::R::mg:c:qh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n':
            strncpy(stream_name, optarg, sizeof(stream_name) - 1);
            break;
        case 'p':
            if (strcmp(optarg, "smpte") == 0) { pat_type = PATTERN_SMPTE_BARS; bad_pattern = NULL; }
            else if (strcmp(optarg, "bars100") == 0) { pat_type = PATTERN_COLOR_BARS_100; bad_pattern = NULL; }
            else if (strcmp(optarg, "grid") == 0) { pat_type = PATTERN_GRID; bad_pattern = NULL; }
            else if (strcmp(optarg, "ramp") == 0) { pat_type = PATTERN_RAMP; bad_pattern = NULL; }
            else if (strcmp(optarg, "white") == 0) { pat_type = PATTERN_SOLID_WHITE; bad_pattern = NULL; }
            else if (strcmp(optarg, "black") == 0) { pat_type = PATTERN_SOLID_BLACK; bad_pattern = NULL; }
            else if (strcmp(optarg, "gray") == 0) { pat_type = PATTERN_SOLID_GRAY; bad_pattern = NULL; }
            else {
                bad_pattern = optarg;
            }
            break;
        case 'r':
            if (strcmp(optarg, "1080p") == 0) { width = 1920; height = 1080; bad_res = NULL; }
            else if (strcmp(optarg, "720p") == 0) { width = 1280; height = 720; bad_res = NULL; }
            else if (strcmp(optarg, "4k") == 0) { width = 3840; height = 2160; bad_res = NULL; }
            else if (sscanf(optarg, "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
                bad_res = NULL;
            } else {
                bad_res = optarg;
                width = 1920; height = 1080;
            }
            break;
        case 'f':
            if (strcmp(optarg, "60") == 0) { fps_n = 60000; fps_d = 1000; }
            else if (strcmp(optarg, "59.94") == 0) { fps_n = 60000; fps_d = 1001; }
            else if (strcmp(optarg, "50") == 0) { fps_n = 50000; fps_d = 1000; }
            else if (strcmp(optarg, "30") == 0) { fps_n = 30000; fps_d = 1000; }
            else if (strcmp(optarg, "29.97") == 0) { fps_n = 30000; fps_d = 1001; }
            else if (strcmp(optarg, "25") == 0) { fps_n = 25000; fps_d = 1000; }
            else if (strcmp(optarg, "24") == 0) { fps_n = 24000; fps_d = 1000; }
            else {
                double f = atof(optarg);
                if (f > 1.0) { fps_n = (int)(f * 1000.0 + 0.5); fps_d = 1000; }
            }
            break;
        case 't':
            if (strcmp(optarg, "1000") == 0) tone_type = TONE_1000HZ;
            else if (strcmp(optarg, "440") == 0) tone_type = TONE_440HZ;
            else if (strcmp(optarg, "beep") == 0) tone_type = TONE_BEEP;
            else if (strcmp(optarg, "none") == 0) tone_type = TONE_NONE;
            break;
        case 'C':
            if (strcasecmp(optarg, "h264") == 0 || strcasecmp(optarg, "avc") == 0) {
                codec = CODEC_H264;
                bad_codec = NULL;
            } else if (strcasecmp(optarg, "uyvy") == 0 || strcasecmp(optarg, "raw") == 0) {
                codec = CODEC_UYVY;
                bad_codec = NULL;
            } else {
                bad_codec = optarg;
                codec = CODEC_UYVY;
            }
            codec_explicitly_set = true;
            break;
        case 1002:
            codec = CODEC_H264;
            bad_codec = NULL;
            codec_explicitly_set = true;
            break;
        case 'W': {
            enable_web = true;
            if (!optarg && optind < argc && argv[optind][0] != '-') {
                optarg = argv[optind++];
            }
            if (optarg) {
                int p = atoi(optarg);
                if (p > 0 && p <= 65535) web_port = (uint16_t)p;
            }
            break;
        }
        case 'R': {
            enable_rtsp = true;
            if (!optarg && optind < argc && argv[optind][0] != '-') {
                optarg = argv[optind++];
            }
            if (optarg) {
                int p = atoi(optarg);
                if (p > 0 && p <= 65535) rtsp_port = (uint16_t)p;
            }
            break;
        }
        case 'm':
            moving_bar = true;
            break;
        case 1001:
            moving_bar = false;
            break;
        case 'g':
            strncpy(groups, optarg, sizeof(groups) - 1);
            break;
        case 'c':
            frame_limit = atoi(optarg);
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

    if (bad_pattern) oavx_log_write(OAVX_LOG_WARN, "pattern", "Unknown pattern '%s', defaulting to 'smpte'", bad_pattern);
    if (bad_res)     oavx_log_write(OAVX_LOG_WARN, "pattern", "Invalid resolution '%s', defaulting to 1920x1080", bad_res);
    if (bad_codec)   oavx_log_write(OAVX_LOG_WARN, "pattern", "Unknown codec '%s', defaulting to 'uyvy'", bad_codec);

    /* Auto-resolve codec: Web preview & RTSP require H.264 bitstream */
    if (enable_web || enable_rtsp) {
        if (codec == CODEC_UYVY && !codec_explicitly_set) {
            codec = CODEC_H264;
        } else if (codec == CODEC_UYVY && codec_explicitly_set) {
            oavx_log_write(OAVX_LOG_WARN, "pattern",
                           "Web/RTSP enabled with raw UYVY; browser preview & RTSP players require H.264 (--codec h264)");
        }
    }

    if (codec == CODEC_H264) {
        if (width != 1920 || height != 1080) {
            oavx_log_write(OAVX_LOG_INFO, "pattern",
                           "Embedded H.264 bitstream is fixed at 1920x1080; overriding resolution");
            width = 1920;
            height = 1080;
        }
    }

    /* Enforce even width for UYVY macropixels */
    if (width % 2 != 0) width++;

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        oavx_log_write(OAVX_LOG_ERROR, "pattern", "Failed to initialize OpenAVX context engine");
        return 1;
    }

    oavx_stream_config_t send_settings = {
        .name = stream_name,
        .groups = groups,
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };

    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_settings);
    if (!p_send) {
        oavx_log_write(OAVX_LOG_ERROR, "pattern", "Failed to create broadcast sender '%s'", stream_name);
        oavx_context_destroy(ctx);
        return 1;
    }

    /* Activate Web Preview and RTSP servers if requested */
    char web_url[256] = "";
    if (enable_web) {
        if (oavx_stream_enable_web_preview(p_send, web_port) == OAVX_SUCCESS) {
            web_port = oavx_stream_get_web_preview_port(p_send);
            oavx_stream_get_web_preview_url(p_send, web_url, sizeof(web_url));
        } else {
            oavx_log_write(OAVX_LOG_ERROR, "pattern", "Failed to start HTTP Web Preview server on port %u", web_port);
            enable_web = false;
        }
    }

    if (enable_rtsp) {
        if (oavx_stream_enable_rtsp_server(p_send, rtsp_port, rtsp_path) == OAVX_SUCCESS) {
            rtsp_port = oavx_stream_get_rtsp_port(p_send);
        } else {
            oavx_log_write(OAVX_LOG_ERROR, "pattern", "Failed to start RTSP 1.0 server on port %u", rtsp_port);
            enable_rtsp = false;
        }
    }

    openavx_cli_header("pattern");
    openavx_cli_kv("stream", "%s", stream_name);
    openavx_cli_kv("codec", "%s", codec == CODEC_H264 ? "H.264 / AVC (SMPTE 75% Annex-B)" : "UYVY 4:2:2 (raw)");
    openavx_cli_kv("resolution", "%dx%d", width, height);
    openavx_cli_kv("framerate", "%.2f fps (%d/%d)", (double)fps_n / fps_d, fps_n, fps_d);
    openavx_cli_kv("groups", "%s", groups);
    openavx_cli_kv("audio", "%s",
                   tone_type == TONE_1000HZ ? "1 kHz sine @ -20 dBFS" :
                   tone_type == TONE_440HZ  ? "440 Hz sine @ -20 dBFS" :
                   tone_type == TONE_BEEP   ? "1s A/V sync beep" : "none");
    openavx_cli_kv("motion", "%s", codec == CODEC_H264 ? "embedded SMPTE IDR" : (moving_bar ? "moving ticker" : "static"));
    if (enable_web) {
        openavx_cli_kv("web", "%s", web_url[0] ? web_url : "active");
    }
    if (enable_rtsp) {
        openavx_cli_kv("rtsp", "rtsp://127.0.0.1:%u/%s", rtsp_port, rtsp_path);
    }
    oavx_log_write(OAVX_LOG_INFO, "pattern", "Broadcasting stream (Ctrl+C to stop)");

    /* Allocate video buffers if UYVY raw mode is active */
    uint8_t* base_frame = NULL;
    uint8_t* render_frame = NULL;
    if (codec == CODEC_UYVY) {
        size_t frame_bytes = (size_t)width * height * 2;
        base_frame = (uint8_t*)malloc(frame_bytes);
        render_frame = (uint8_t*)malloc(frame_bytes);
        if (!base_frame || !render_frame) {
            oavx_log_write(OAVX_LOG_ERROR, "pattern", "Failed to allocate video frame memory");
            free(base_frame);
            free(render_frame);
            if (enable_web) oavx_stream_disable_web_preview(p_send);
            if (enable_rtsp) oavx_stream_disable_rtsp_server(p_send);
            oavx_stream_close(p_send);
            oavx_context_destroy(ctx);
            return 1;
        }

        render_base_pattern(base_frame, width, height, pat_type);
        memcpy(render_frame, base_frame, frame_bytes);
    }

    /* Allocate audio buffers (48 kHz stereo) */
    const int max_samples_per_frame = 48000 / 24 + 128;
    float* audio_left = (float*)malloc(max_samples_per_frame * sizeof(float));
    float* audio_right = (float*)malloc(max_samples_per_frame * sizeof(float));
    float* audio_planar = (float*)malloc(2 * max_samples_per_frame * sizeof(float));
    if (!audio_left || !audio_right || !audio_planar) {
        oavx_log_write(OAVX_LOG_ERROR, "pattern", "Failed to allocate audio buffers");
        free(base_frame); free(render_frame);
        free(audio_left); free(audio_right); free(audio_planar);
        if (enable_web) oavx_stream_disable_web_preview(p_send);
        if (enable_rtsp) oavx_stream_disable_rtsp_server(p_send);
        oavx_stream_close(p_send);
        oavx_context_destroy(ctx);
        return 1;
    }

    int64_t audio_sample_counter = 0;
    int64_t frame_index = 0;
    int bar_pos_x = 0;
    int bar_width = width / 20;
    if (bar_width < 16) bar_width = 16;
    if (bar_width % 2 != 0) bar_width++;

    uint64_t frame_duration_ns = (uint64_t)((double)fps_d * 1000000000.0 / (double)fps_n);
    int64_t start_time_ns = avx_time_ns();

    while (g_running) {
        if (frame_limit > 0 && frame_index >= frame_limit) {
            break;
        }

        /* Update and send video frame */
        int64_t frame_pts_ns = (int64_t)(frame_index * (1000000000LL * fps_d / fps_n));

        if (codec == CODEC_H264) {
            oavx_video_frame_t vf = {
                .width = width,
                .height = height,
                .fourcc = OAVX_FOURCC_VIDEO_TYPE_H264,
                .scan_type = OAVX_SCAN_PROGRESSIVE,
                .planes = { (uint8_t*)g_smpte_h264_1080p, NULL, NULL, NULL },
                .strides = { g_smpte_h264_1080p_len, 0, 0, 0 },
                .framerate = { (uint32_t)fps_n, (uint32_t)fps_d },
                .aspect_ratio = { (uint32_t)width, (uint32_t)height },
                .pts_ns = frame_pts_ns,
                .dts_ns = 0,
                .metadata = NULL,
                .metadata_len = 0,
                .user_ctx = NULL
            };
            oavx_stream_send_video(p_send, &vf);
        } else {
            if (moving_bar) {
                overlay_moving_ticker(render_frame, base_frame, width, height, bar_pos_x, bar_width);
                bar_pos_x = (bar_pos_x + (width / 240) + 4) % (width - bar_width);
            }

            oavx_video_frame_t vf = {
                .width = width,
                .height = height,
                .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
                .scan_type = OAVX_SCAN_PROGRESSIVE,
                .planes = { render_frame, NULL, NULL, NULL },
                .strides = { (size_t)width * 2, 0, 0, 0 },
                .framerate = { (uint32_t)fps_n, (uint32_t)fps_d },
                .aspect_ratio = { (uint32_t)width, (uint32_t)height },
                .pts_ns = frame_pts_ns,
                .dts_ns = 0,
                .metadata = NULL,
                .metadata_len = 0,
                .user_ctx = NULL
            };
            oavx_stream_send_video(p_send, &vf);
        }

        /* Calculate exact audio samples needed for this frame at 48 kHz */
        int audio_samples = (int)((double)(frame_index + 1) * 48000.0 * fps_d / fps_n) -
                            (int)((double)frame_index * 48000.0 * fps_d / fps_n);

        if (tone_type != TONE_NONE && audio_samples > 0) {
            generate_audio_frame(audio_left, audio_right, (uint32_t)audio_samples, tone_type, &audio_sample_counter);

            /* Pack planar audio (Ch0 followed by Ch1) */
            memcpy(audio_planar, audio_left, (size_t)audio_samples * sizeof(float));
            memcpy(audio_planar + audio_samples, audio_right, (size_t)audio_samples * sizeof(float));

            oavx_audio_frame_t af = {
                .sample_rate = 48000,
                .channels = 2,
                .frames = (uint32_t)audio_samples,
                .format = OAVX_AUDIO_FMT_FLT_PLANAR,
                .planes = { audio_left, audio_right, NULL, NULL, NULL, NULL, NULL, NULL },
                .stride_bytes = (size_t)audio_samples * sizeof(float),
                .pts_ns = frame_pts_ns,
                .metadata = NULL,
                .metadata_len = 0,
                .user_ctx = NULL
            };
            oavx_stream_send_audio(p_send, &af);
        }

        frame_index++;

        if (oavx_get_log_level() >= OAVX_LOG_INFO && (frame_index % (fps_n / fps_d) == 0)) {
            oavx_tally_t tally = {0};
            oavx_stream_get_tally(p_send, &tally, 0);
            int conns = oavx_stream_get_connection_count(p_send, 0);
            int rtsp_c = enable_rtsp ? oavx_stream_get_rtsp_client_count(p_send) : 0;
            int web_c = enable_web ? oavx_stream_get_web_client_count(p_send) : 0;
            if (enable_web || enable_rtsp) {
                openavx_cli_status("pattern", "frames=%ld (%.1fs) clients=[avx:%d rtsp:%d web:%d] tally=[PGM:%s PVW:%s]",
                                   (long)frame_index, (double)frame_index * fps_d / fps_n,
                                   conns, rtsp_c, web_c,
                                   (tally.flags & OAVX_TALLY_PROGRAM) ? "YES" : "NO",
                                   (tally.flags & OAVX_TALLY_PREVIEW) ? "YES" : "NO");
            } else {
                openavx_cli_status("pattern", "frames=%ld (%.1fs) receivers=%d tally=[PGM:%s PVW:%s]",
                                   (long)frame_index, (double)frame_index * fps_d / fps_n, conns,
                                   (tally.flags & OAVX_TALLY_PROGRAM) ? "YES" : "NO",
                                   (tally.flags & OAVX_TALLY_PREVIEW) ? "YES" : "NO");
            }
        }

        /* Accurate frame rate pacing */
        int64_t now_ns = avx_time_ns();
        uint64_t target_ns = (uint64_t)frame_index * frame_duration_ns;
        uint64_t elapsed_ns = (uint64_t)(now_ns - start_time_ns);

        if (target_ns > elapsed_ns) {
            avx_sleep_ns(target_ns - elapsed_ns);
        }
    }

    openavx_cli_status_end();
    oavx_log_write(OAVX_LOG_INFO, "pattern", "Shutting down signal generator...");

    if (enable_web) {
        oavx_stream_disable_web_preview(p_send);
    }
    if (enable_rtsp) {
        oavx_stream_disable_rtsp_server(p_send);
    }

    free(base_frame);
    free(render_frame);
    free(audio_left);
    free(audio_right);
    free(audio_planar);

    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    oavx_log_write(OAVX_LOG_INFO, "pattern", "Transmitted %ld frames total; generator stopped", (long)frame_index);
    return 0;
}
