/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_tally.c - Broadcast Tally monitor and control CLI utility
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#include "avx_platform.h"
#include "openavx.h"
#include "openavx_cli.h"

#if defined(AVX_OS_POSIX)
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
#elif defined(AVX_OS_WINDOWS)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <io.h>
    #define isatty _isatty
    #define fileno _fileno
#endif

static volatile sig_atomic_t g_stop = 0;

static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static void print_usage(const char* prog) {
    printf("OpenAVX Tally Light Monitor & Remote Controller\n");
    printf("Usage: %s [options] <stream-name | ip:port | http://url>\n\n", prog);
    printf("Modes:\n");
    printf("  Monitor Mode (Default)   Watch live tally state changes with real-time ANSI indicators\n");
    printf("  Control Mode (--set)     Command remote camera tally lights (PGM, PVW, REC, OFF)\n\n");
    printf("Options:\n");
    printf("  -s, --set <flags>        Set tally state: 'pgm', 'pvw', 'rec', 'off' (or combo 'pgm+rec')\n");
    printf("  -p, --poll               Query tally state once, output result, and exit immediately\n");
    printf("  -j, --json               Output machine-readable JSON format\n");
    printf("  -t, --text               Output plain status token (PROGRAM, PREVIEW, RECORDING, OFF)\n");
    printf("  -w, --wait <ms>          Timeout for initial response/connection (default: 3000 ms)\n");
    printf("  -d, --duration <sec>     Hold tally connection for <sec> seconds (default: until Ctrl+C)\n");
    printf("  -1, --oneshot            Command tally and disconnect immediately without holding\n");
    printf("  -q, --quiet              Quiet mode (suppress headers and banners)\n");
    printf("  -h, --help               Display this help message\n\n");
    printf("Examples:\n");
    printf("  %s \"STUDIO-CAM1\"                 # Monitor studio camera tally in real time\n", prog);
    printf("  %s http://192.168.1.50:8080       # Monitor tally over HTTP SSE stream\n", prog);
    printf("  %s \"CAM1\" --set pgm               # Switch CAM1 to Program (ON AIR / Red Tally)\n", prog);
    printf("  %s \"CAM1\" --set \"pvw+rec\"         # Switch CAM1 to Preview cued with ISO Recording\n", prog);
    printf("  %s \"CAM1\" --set off --oneshot     # Clear tally on CAM1 and exit immediately\n", prog);
    printf("  %s http://localhost:8080 --poll   # Poll local tally via REST JSON\n", prog);
}

static uint8_t parse_tally_tokens(const char* str) {
    if (!str) return OAVX_TALLY_NONE;
    if (strcasecmp(str, "off") == 0 || strcasecmp(str, "none") == 0 || strcasecmp(str, "clear") == 0) {
        return OAVX_TALLY_NONE;
    }
    uint8_t flags = OAVX_TALLY_NONE;
    if (strcasestr(str, "program") || strcasestr(str, "pgm") || strcasestr(str, "onair")) {
        flags |= OAVX_TALLY_PROGRAM;
    }
    if (strcasestr(str, "preview") || strcasestr(str, "pvw") || strcasestr(str, "cued")) {
        flags |= OAVX_TALLY_PREVIEW;
    }
    if (strcasestr(str, "recording") || strcasestr(str, "rec") || strcasestr(str, "iso")) {
        flags |= OAVX_TALLY_RECORDING;
    }
    if (flags == OAVX_TALLY_NONE) {
        int n = atoi(str);
        if (n > 0) flags = (uint8_t)n;
    }
    return flags;
}

static const char* tally_to_text(uint8_t flags) {
    bool prog = (flags & OAVX_TALLY_PROGRAM) != 0;
    bool prev = (flags & OAVX_TALLY_PREVIEW) != 0;
    bool rec  = (flags & OAVX_TALLY_RECORDING) != 0;

    if (prog && rec) return "PROGRAM+RECORDING";
    if (prog) return "PROGRAM";
    if (prev && rec) return "PREVIEW+RECORDING";
    if (prev) return "PREVIEW";
    if (rec) return "RECORDING";
    return "OFF";
}

static void print_tally_result(const char* target, uint8_t flags, bool json_out, bool text_out, bool use_ansi) {
    bool prog = (flags & OAVX_TALLY_PROGRAM) != 0;
    bool prev = (flags & OAVX_TALLY_PREVIEW) != 0;
    bool rec  = (flags & OAVX_TALLY_RECORDING) != 0;

    if (json_out) {
        printf("{\"stream\":\"%s\",\"program\":%s,\"preview\":%s,\"recording\":%s,\"flags\":%u}\n",
               target ? target : "",
               prog ? "true" : "false",
               prev ? "true" : "false",
               rec  ? "true" : "false",
               flags);
        fflush(stdout);
        return;
    }

    if (text_out) {
        printf("%s\n", tally_to_text(flags));
        fflush(stdout);
        return;
    }

    time_t now = time(NULL);
    struct tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif

    if (use_ansi) {
        char badge[256];
        if (prog && rec) {
            snprintf(badge, sizeof(badge), "\033[1;41;37m ● ON AIR \033[0m \033[1;43;30m ● REC \033[0m");
        } else if (prog) {
            snprintf(badge, sizeof(badge), "\033[1;41;37m ● ON AIR (PROGRAM) \033[0m");
        } else if (prev && rec) {
            snprintf(badge, sizeof(badge), "\033[1;42;30m ● CUED \033[0m \033[1;43;30m ● REC \033[0m");
        } else if (prev) {
            snprintf(badge, sizeof(badge), "\033[1;42;30m ● CUED (PREVIEW)   \033[0m");
        } else if (rec) {
            snprintf(badge, sizeof(badge), "\033[1;43;30m ● RECORDING (ISO) \033[0m");
        } else {
            snprintf(badge, sizeof(badge), "\033[90m ○ OFF AIR          \033[0m");
        }

        fprintf(stdout, "\r\033[K[%02d:%02d:%02d] %s  \033[1;37m%-24s\033[0m",
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, badge, target ? target : "");
        fflush(stdout);
    } else {
        fprintf(stdout, "[%02d:%02d:%02d] [%s] %s\n",
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                tally_to_text(flags), target ? target : "");
        fflush(stdout);
    }
}

/* ========================================================================= */
/* HTTP REST & SSE CLIENT ENGINE                                             */
/* ========================================================================= */

static bool parse_http_url(const char* url, char* host_out, size_t host_sz, uint16_t* port_out, char* path_out, size_t path_sz) {
    if (!url || strncmp(url, "http://", 7) != 0) return false;
    const char* p = url + 7;
    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_sz) hlen = host_sz - 1;
        strncpy(host_out, p, hlen);
        host_out[hlen] = '\0';
        *port_out = (uint16_t)atoi(colon + 1);
    } else if (slash) {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= host_sz) hlen = host_sz - 1;
        strncpy(host_out, p, hlen);
        host_out[hlen] = '\0';
        *port_out = 80;
    } else {
        snprintf(host_out, host_sz, "%s", p);
        *port_out = 80;
    }

    if (slash) {
        snprintf(path_out, path_sz, "%s", slash);
    } else {
        snprintf(path_out, path_sz, "/api/tally");
    }
    return true;
}

static avx_socket_t http_connect(const char* host, uint16_t port, uint32_t timeout_ms) {
    avx_platform_sockets_init();
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return AVX_INVALID_SOCKET;
    }

    avx_socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (!avx_socket_is_valid(fd)) {
        freeaddrinfo(res);
        return AVX_INVALID_SOCKET;
    }

#if defined(AVX_OS_POSIX)
    struct timeval tv;
    tv.tv_sec = (time_t)(timeout_ms / 1000);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const void*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const void*)&tv, sizeof(tv));
#endif

    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) < 0) {
        avx_socket_close(fd);
        freeaddrinfo(res);
        return AVX_INVALID_SOCKET;
    }

    freeaddrinfo(res);
    return fd;
}

static int execute_http_tally(
    const char* url,
    bool is_set,
    uint8_t set_flags,
    bool poll_once,
    bool json_out,
    bool text_out,
    uint32_t wait_ms,
    bool quiet)
{
    char host[128];
    char path[256];
    uint16_t port = 80;

    if (!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        fprintf(stderr, "Error: Invalid HTTP URL '%s'\n", url);
        return 1;
    }

    bool is_tty = isatty(fileno(stdout)) != 0;
    bool use_ansi = is_tty && !json_out && !text_out;

    if (!quiet && !json_out && !text_out) {
        openavx_cli_header("tally");
        openavx_cli_kv("endpoint", "http://%s:%u%s", host, port, path);
        openavx_cli_kv("transport", "HTTP REST / Server-Sent Events (SSE)");
    }

    /* 1. Control Mode: HTTP POST /api/tally */
    if (is_set) {
        avx_socket_t fd = http_connect(host, port, wait_ms);
        if (!avx_socket_is_valid(fd)) {
            fprintf(stderr, "Error: Failed to connect to HTTP host %s:%u\n", host, port);
            return 1;
        }

        char post_path[256];
        if (strcmp(path, "/") == 0 || strcmp(path, "/camera") == 0) {
            snprintf(post_path, sizeof(post_path), "/api/tally");
        } else {
            snprintf(post_path, sizeof(post_path), "%s", path);
        }

        char body[128];
        snprintf(body, sizeof(body), "{\"flags\":%u}\n", set_flags);

        char req[512];
        int req_len = snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: openavx-tally/%s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n"
            "%s",
            post_path, host, port, oavx_version(), strlen(body), body);

        if (send(fd, req, req_len, 0) < 0) {
            fprintf(stderr, "Error: Failed to send HTTP POST request\n");
            avx_socket_close(fd);
            return 1;
        }

        char resp[1024];
        ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
        avx_socket_close(fd);

        if (n <= 0) {
            fprintf(stderr, "Error: No response from HTTP server\n");
            return 1;
        }
        resp[n] = '\0';

        if (strstr(resp, "200 OK") != NULL) {
            if (json_out) {
                printf("{\"ok\":true,\"stream\":\"%s\",\"flags\":%u,\"status\":\"%s\"}\n",
                       url, set_flags, tally_to_text(set_flags));
            } else if (text_out) {
                printf("%s\n", tally_to_text(set_flags));
            } else {
                printf("\033[1;32m[OK]\033[0m Successfully set tally -> %s on %s\n",
                       tally_to_text(set_flags), url);
            }
            return 0;
        } else {
            fprintf(stderr, "Error: HTTP server returned error response\n");
            return 1;
        }
    }

    /* 2. Poll Once Mode: HTTP GET /api/tally */
    if (poll_once) {
        avx_socket_t fd = http_connect(host, port, wait_ms);
        if (!avx_socket_is_valid(fd)) {
            fprintf(stderr, "Error: Failed to connect to HTTP host %s:%u\n", host, port);
            return 1;
        }

        char get_path[256];
        if (strcmp(path, "/") == 0 || strcmp(path, "/camera") == 0) {
            snprintf(get_path, sizeof(get_path), "/api/tally");
        } else {
            snprintf(get_path, sizeof(get_path), "%s", path);
        }

        char req[512];
        int req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: openavx-tally/%s\r\n"
            "Accept: application/json, text/plain\r\n"
            "Connection: close\r\n\r\n",
            get_path, host, port, oavx_version());

        send(fd, req, req_len, 0);

        char resp[2048];
        ssize_t n = recv(fd, resp, sizeof(resp) - 1, 0);
        avx_socket_close(fd);

        if (n <= 0) {
            fprintf(stderr, "Error: Empty response from HTTP server\n");
            return 1;
        }
        resp[n] = '\0';

        const char* body_start = strstr(resp, "\r\n\r\n");
        if (body_start) body_start += 4;
        else body_start = resp;

        uint8_t flags = OAVX_TALLY_NONE;
        const char* f_ptr = strstr(body_start, "\"flags\":");
        if (f_ptr) {
            flags = (uint8_t)atoi(f_ptr + 8);
        } else {
            flags = parse_tally_tokens(body_start);
        }

        print_tally_result(url, flags, json_out, text_out, false);
        return 0;
    }

    /* 3. Monitor Mode: HTTP SSE Streaming (GET /api/tally/sse) */
    char sse_path[256];
    if (strstr(path, "sse") || strstr(path, "events")) {
        snprintf(sse_path, sizeof(sse_path), "%s", path);
    } else {
        snprintf(sse_path, sizeof(sse_path), "/api/tally/sse");
    }

    while (!g_stop) {
        avx_socket_t fd = http_connect(host, port, wait_ms);
        if (!avx_socket_is_valid(fd)) {
            if (g_stop) break;
            if (!quiet && !json_out && !text_out) {
                fprintf(stderr, "\r\033[K[tally] Connecting to %s:%u... (retrying in 1s)", host, port);
                fflush(stderr);
            }
            avx_sleep_ms(1000);
            continue;
        }

        char req[512];
        int req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%u\r\n"
            "User-Agent: openavx-tally/%s\r\n"
            "Accept: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n\r\n",
            sse_path, host, port, oavx_version());

        send(fd, req, req_len, 0);

        char sse_buf[4096];
        size_t sse_len = 0;

        while (!g_stop) {
            ssize_t n = recv(fd, sse_buf + sse_len, sizeof(sse_buf) - 1 - sse_len, 0);
            if (n <= 0) break;
            sse_len += (size_t)n;
            sse_buf[sse_len] = '\0';

            char* line = sse_buf;
            char* next_line = NULL;

            while ((next_line = strstr(line, "\n")) != NULL) {
                *next_line = '\0';
                char* cur = line;
                line = next_line + 1;

                if (strncmp(cur, "data:", 5) == 0) {
                    char* json = cur + 5;
                    while (*json == ' ') json++;
                    const char* f_ptr = strstr(json, "\"flags\":");
                    if (f_ptr) {
                        uint8_t flags = (uint8_t)atoi(f_ptr + 8);
                        print_tally_result(url, flags, json_out, text_out, use_ansi);
                    }
                }
            }

            size_t rem = strlen(line);
            if (rem > 0 && line != sse_buf) {
                memmove(sse_buf, line, rem);
            }
            sse_len = rem;
        }

        avx_socket_close(fd);
        if (!g_stop) {
            avx_sleep_ms(1000);
        }
    }

    if (use_ansi) {
        fputc('\n', stdout);
        fflush(stdout);
    }
    return 0;
}

/* ========================================================================= */
/* OPENAVX NATIVE MEDIA STREAM TALLY ENGINE                                  */
/* ========================================================================= */

static int execute_native_tally(
    const char* target,
    bool is_set,
    uint8_t set_flags,
    bool poll_once,
    bool json_out,
    bool text_out,
    uint32_t wait_ms,
    uint32_t hold_sec,
    bool oneshot,
    bool quiet)
{
    bool is_tty = isatty(fileno(stdout)) != 0;
    bool use_ansi = is_tty && !json_out && !text_out;

    oavx_context_t* ctx = oavx_context_create();
    if (!ctx) {
        fprintf(stderr, "Error: Failed to initialize OpenAVX context\n");
        return 1;
    }

    oavx_stream_config_t cfg = {
        .name = target ? target : "OPENAVX_TALLY_CLI",
        .direction = OAVX_DIR_RECEIVE,
        .bandwidth = OAVX_BANDWIDTH_METADATA_ONLY,
        .color_format = OAVX_COLOR_FORMAT_RGBX_RGBA,
        .clock_sync = false
    };

    /* Check if target looks like ip:port */
    if (target && strchr(target, ':') != NULL && !strchr(target, ' ')) {
        cfg.endpoint = target;
    }

    if (!quiet && !json_out && !text_out) {
        openavx_cli_header("tally");
        openavx_cli_kv("source", "%s", target ? target : "(auto)");
        openavx_cli_kv("mode", "%s", is_set ? "Controller (Inject)" : (poll_once ? "Poll Once" : "Continuous Monitor"));
        openavx_cli_kv("transport", "OpenAVX Native Reverse Metadata");
    }

    oavx_stream_t* stream = oavx_stream_open(ctx, &cfg);
    if (!stream) {
        fprintf(stderr, "Error: Failed to connect to stream '%s'\n", target ? target : "");
        oavx_context_destroy(ctx);
        return 1;
    }

    /* 1. Control Mode: Command tally state to remote sender */
    if (is_set) {
        oavx_tally_t set_tally = { .flags = set_flags };
        bool ok = oavx_stream_set_tally(stream, &set_tally);
        if (!ok) {
            fprintf(stderr, "Error: Failed to command tally to stream '%s'\n", target ? target : "");
            oavx_stream_close(stream);
            oavx_context_destroy(ctx);
            return 1;
        }

        if (json_out) {
            printf("{\"ok\":true,\"stream\":\"%s\",\"flags\":%u,\"status\":\"%s\"}\n",
                   target ? target : "", set_flags, tally_to_text(set_flags));
            fflush(stdout);
        } else if (text_out) {
            printf("%s\n", tally_to_text(set_flags));
            fflush(stdout);
        } else {
            printf("\033[1;32m[OK]\033[0m Commanded tally %s to '%s'\n",
                   tally_to_text(set_flags), target ? target : "");
        }

        if (oneshot) {
            avx_sleep_ms(150);
            oavx_stream_close(stream);
            oavx_context_destroy(ctx);
            return 0;
        }

        if (!quiet && !json_out && !text_out) {
            if (hold_sec > 0) {
                printf("[tally] Holding tally state for %u seconds...\n", hold_sec);
            } else {
                printf("[tally] Holding active tally connection... (Press Ctrl+C to release)\n");
            }
            fflush(stdout);
        }

        int64_t start_ms = avx_time_ns() / 1000000LL;
        while (!g_stop) {
            avx_sleep_ms(100);
            if (hold_sec > 0) {
                int64_t elapsed_ms = (avx_time_ns() / 1000000LL) - start_ms;
                if (elapsed_ms >= (int64_t)(hold_sec * 1000)) break;
            }
        }

        oavx_stream_close(stream);
        oavx_context_destroy(ctx);
        return 0;
    }

    /* 2. Poll Once Mode */
    if (poll_once) {
        oavx_tally_t tally = {0};
        bool got = oavx_stream_get_tally(stream, &tally, wait_ms);
        if (!got) {
            /* If no immediate tally packet, poll once more with fallback */
            tally.flags = OAVX_TALLY_NONE;
        }
        print_tally_result(target, tally.flags, json_out, text_out, false);
        oavx_stream_close(stream);
        oavx_context_destroy(ctx);
        return 0;
    }

    /* 3. Monitor Mode (Continuous) */
    uint8_t last_flags = 0xFF;
    while (!g_stop) {
        oavx_tally_t tally = {0};
        bool got = oavx_stream_get_tally(stream, &tally, 200);
        if (got || last_flags == 0xFF) {
            if (tally.flags != last_flags) {
                print_tally_result(target, tally.flags, json_out, text_out, use_ansi);
                last_flags = tally.flags;
            }
        }
        avx_sleep_ms(50);
    }

    if (use_ansi) {
        fputc('\n', stdout);
        fflush(stdout);
    }

    oavx_stream_close(stream);
    oavx_context_destroy(ctx);
    return 0;
}

/* ========================================================================= */
/* MAIN CLI SUBCOMMAND ENTRY POINT                                           */
/* ========================================================================= */

int openavx_cmd_tally(int argc, char** argv) {
    optind = 1;
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    const char* set_arg = NULL;
    bool poll_once = false;
    bool json_out = false;
    bool text_out = false;
    bool oneshot = false;
    bool quiet = false;
    uint32_t wait_ms = 3000;
    uint32_t hold_sec = 0;

    static struct option long_opts[] = {
        {"set",      required_argument, 0, 's'},
        {"poll",     no_argument,       0, 'p'},
        {"json",     no_argument,       0, 'j'},
        {"text",     no_argument,       0, 't'},
        {"wait",     required_argument, 0, 'w'},
        {"duration", required_argument, 0, 'd'},
        {"oneshot",  no_argument,       0, '1'},
        {"quiet",    no_argument,       0, 'q'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:pjtw:d:1qh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 's':
                set_arg = optarg;
                break;
            case 'p':
                poll_once = true;
                break;
            case 'j':
                json_out = true;
                break;
            case 't':
                text_out = true;
                break;
            case 'w':
                wait_ms = (uint32_t)atoi(optarg);
                if (wait_ms == 0) wait_ms = 3000;
                break;
            case 'd':
                hold_sec = (uint32_t)atoi(optarg);
                break;
            case '1':
                oneshot = true;
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

    if (optind >= argc) {
        fprintf(stderr, "Error: Missing target stream name or URL\n\n");
        print_usage(argv[0]);
        return 1;
    }

    const char* target = argv[optind];
    bool is_set = (set_arg != NULL);
    uint8_t set_flags = is_set ? parse_tally_tokens(set_arg) : OAVX_TALLY_NONE;

    if (strncmp(target, "http://", 7) == 0 || strncmp(target, "https://", 8) == 0) {
        return execute_http_tally(target, is_set, set_flags, poll_once, json_out, text_out, wait_ms, quiet);
    } else {
        return execute_native_tally(target, is_set, set_flags, poll_once, json_out, text_out, wait_ms, hold_sec, oneshot, quiet);
    }
}
