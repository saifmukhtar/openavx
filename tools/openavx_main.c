/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_main.c - Main CLI entry point and subcommand dispatcher
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#include "avx_platform.h"
#include "avx_simd.h"
#include "openavx.h"
#include "avx_audio_codec.h"
#include "avx_video_codec.h"
#include "openavx_cli.h"

#if defined(AVX_OS_POSIX)
    #include <unistd.h>
#endif

static const char* get_prog_basename(const char* path) {
    if (!path) return "openavx";
    const char* p = strrchr(path, '/');
#ifdef _WIN32
    const char* p2 = strrchr(path, '\\');
    if (p2 && (!p || p2 > p)) p = p2;
#endif
    return p ? p + 1 : path;
}

static bool prog_matches(const char* base, const char* target) {
    size_t target_len = strlen(target);
    if (strncasecmp(base, target, target_len) == 0) {
        const char* rest = base + target_len;
        if (*rest == '\0' || strcasecmp(rest, ".exe") == 0) {
            return true;
        }
    }
    return false;
}

void openavx_cli_init(void) {
    const char* env = getenv("OPENAVX_LOG_LEVEL");
    if (!env || env[0] == '\0') {
        oavx_set_log_level(OAVX_LOG_INFO);
    }
}

void openavx_cli_set_quiet(bool quiet) {
    if (quiet) {
        oavx_set_log_level(OAVX_LOG_ERROR);
    }
}

void openavx_cli_header(const char* tool_name) {
    if (oavx_get_log_level() < OAVX_LOG_INFO) return;
    fprintf(stderr, "openavx %s (%s)\n", tool_name ? tool_name : "cli", oavx_version());
}

void openavx_cli_kv(const char* key, const char* fmt, ...) {
    if (oavx_get_log_level() < OAVX_LOG_INFO || !key || !fmt) return;
    fprintf(stderr, "  %-12s: ", key);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static bool g_status_line_active = false;

void openavx_cli_status(const char* module, const char* fmt, ...) {
    if (!fmt || oavx_get_log_level() < OAVX_LOG_INFO) return;

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    bool use_tty = false;
#if defined(AVX_OS_POSIX)
    use_tty = (isatty(fileno(stderr)) != 0);
#endif
    if (use_tty) {
        if (module && module[0] != '\0') {
            fprintf(stderr, "\r\033[36m[openavx:%s] [info]\033[0m %s\033[K", module, msg);
        } else {
            fprintf(stderr, "\r\033[36m[openavx] [info]\033[0m %s\033[K", msg);
        }
        fflush(stderr);
        g_status_line_active = true;
    } else {
        oavx_log_write(OAVX_LOG_INFO, module, "%s", msg);
    }
}

void openavx_cli_status_end(void) {
    if (g_status_line_active) {
        fputc('\n', stderr);
        fflush(stderr);
        g_status_line_active = false;
    }
}

static void print_main_usage(const char* prog) {
    printf("OpenAVX CLI (%s) - Open Audio-Video eXchange\n", oavx_version());
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  find        Discover and scan active OpenAVX streams across the network\n");
    printf("  monitor     Terminal video monitor, ASCII/Braille player & audio receiver\n");
    printf("  route       Virtual matrix routing switcher (interactive console or daemon)\n");
    printf("  pattern     Broadcast test signal & SMPTE pattern generator with audio tone\n");
    printf("  gateway     Network stream ingress to HTTP/fMP4 Web Preview & RTSP bridge\n");
    printf("  server      Centralized OpenAVX discovery registry daemon\n");
    printf("  virtcam     Virtual camera sink & phone capture (alias 'cam', Zoom / Meet)\n");
    printf("  tally       Inspect, monitor, and command broadcast tally lights (PGM/PVW/REC)\n\n");
    printf("Global Flags:\n");
    printf("  -v, --version   Display version, runtime codecs, SIMD, and build information\n");
    printf("  -h, --help      Display this help message\n\n");
    printf("Subcommand Help:\n");
    printf("  %s <find|monitor|route|pattern|gateway|server|virtcam|tally> --help\n\n", prog);
    printf("Compatibility Symlinks:\n");
    printf("  openavx-find, openavx-monitor, openavx-router, openavx-test-pattern,\n");
    printf("  openavx-gateway, openavx-web-gateway, openavx-discovery-server, openavx-virtcam,\n");
    printf("  openavx-tally\n");
}

static const char* get_libc_name(char* buf, size_t len) {
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    snprintf(buf, len, "Windows CRT");
#elif defined(__APPLE__) && defined(__MACH__)
    snprintf(buf, len, "macOS libSystem");
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    snprintf(buf, len, "BSD libc");
#elif defined(__GLIBC__)
    snprintf(buf, len, "glibc %d.%d", __GLIBC__, __GLIBC_MINOR__);
#elif defined(__linux__)
    snprintf(buf, len, "musl / non-glibc");
#else
    snprintf(buf, len, "POSIX libc");
#endif
    return buf;
}

static void print_version_info(void) {
    char libc_buf[48];
    bool has_opus = avx_audio_codec_has_opus();
    bool has_h264 = avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_H264);
    bool has_hevc = avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_HEVC);
    bool has_av1  = avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_AV01);

    char hx_buf[64] = {0};
    if (!has_h264 && !has_hevc && !has_av1) {
        snprintf(hx_buf, sizeof(hx_buf), "passthrough-only");
    } else {
        snprintf(hx_buf, sizeof(hx_buf), "%s%s%s%s%s",
                 has_h264 ? "h264" : "",
                 (has_h264 && (has_hevc || has_av1)) ? "/" : "",
                 has_hevc ? "hevc" : "",
                 ((has_h264 || has_hevc) && has_av1) ? "/" : "",
                 has_av1 ? "av1" : "");
    }

    printf("OpenAVX CLI (%s)\n", oavx_version());
    printf("Platform: %s (%s, %s)\n",
           AVX_PLATFORM_NAME, AVX_ARCH_NAME, get_libc_name(libc_buf, sizeof(libc_buf)));
    printf("Architecture: %s\n", oavx_is_supported_cpu() ? "Validated" : "Generic");
    printf("SIMD Acceleration: %s\n", avx_simd_get_name());
    printf("Runtime Codecs: SpeedHQ/ADPCM (built-in), video_hx=%s, libopus=%s\n",
           hx_buf, has_opus ? "loaded" : "absent");
    printf("Build: C99/C++11, POSIX/Win32/BSD\n");
}

int main(int argc, char* argv[]) {
    if (argc < 1 || !argv[0]) {
        print_main_usage("openavx");
        return 1;
    }

    openavx_cli_init();

    const char* base = get_prog_basename(argv[0]);

    /* 1. Direct Multi-Call Symlink Dispatch */
    if (prog_matches(base, "openavx-find") || prog_matches(base, "openavx_find")) {
        return openavx_cmd_find(argc, argv);
    }
    if (prog_matches(base, "openavx-monitor") || prog_matches(base, "openavx_monitor")) {
        return openavx_cmd_monitor(argc, argv);
    }
    if (prog_matches(base, "openavx-router") || prog_matches(base, "openavx_router") || prog_matches(base, "openavx-route")) {
        return openavx_cmd_route(argc, argv);
    }
    if (prog_matches(base, "openavx-test-pattern") || prog_matches(base, "openavx_test_pattern") || prog_matches(base, "openavx-pattern")) {
        return openavx_cmd_pattern(argc, argv);
    }
    if (prog_matches(base, "openavx-discovery-server") || prog_matches(base, "openavx_discovery_server") || prog_matches(base, "openavx-server")) {
        return openavx_cmd_server(argc, argv);
    }
    if (prog_matches(base, "openavx-gateway") || prog_matches(base, "openavx_gateway") ||
        prog_matches(base, "openavx-web-gateway") || prog_matches(base, "openavx_web_gateway")) {
        return openavx_cmd_gateway(argc, argv);
    }
    if (prog_matches(base, "openavx-virtcam") || prog_matches(base, "openavx_virtcam") ||
        prog_matches(base, "openavx-cam") || prog_matches(base, "openavx_cam") ||
        prog_matches(base, "openavx-v4l2sink") || prog_matches(base, "openavx_v4l2sink")) {
        return openavx_cmd_virtcam(argc, argv);
    }
    if (prog_matches(base, "openavx-tally") || prog_matches(base, "openavx_tally")) {
        return openavx_cmd_tally(argc, argv);
    }

    /* 2. Top-level 'openavx' command invocation */
    if (argc < 2) {
        print_main_usage(argv[0]);
        return 1;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        print_main_usage(argv[0]);
        return 0;
    }

    if (strcmp(cmd, "-v") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        print_version_info();
        return 0;
    }

    /* Subcommand dispatch: adjust argc and argv so argv[0] becomes the subcommand */
    int sub_argc = argc - 1;
    char** sub_argv = argv + 1;

    if (strcmp(cmd, "find") == 0) {
        return openavx_cmd_find(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "monitor") == 0) {
        return openavx_cmd_monitor(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "route") == 0 || strcmp(cmd, "router") == 0) {
        return openavx_cmd_route(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "pattern") == 0 || strcmp(cmd, "test-pattern") == 0) {
        return openavx_cmd_pattern(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "server") == 0 || strcmp(cmd, "discovery-server") == 0) {
        return openavx_cmd_server(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "gateway") == 0 || strcmp(cmd, "web-gateway") == 0) {
        return openavx_cmd_gateway(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "virtcam") == 0 || strcmp(cmd, "cam") == 0 || strcmp(cmd, "v4l2sink") == 0 || strcmp(cmd, "virtual-camera") == 0) {
        return openavx_cmd_virtcam(sub_argc, sub_argv);
    }
    if (strcmp(cmd, "tally") == 0) {
        return openavx_cmd_tally(sub_argc, sub_argv);
    }

    oavx_log_write(OAVX_LOG_ERROR, "cli", "Unknown command '%s'", cmd);
    print_main_usage(argv[0]);
    return 1;
}
