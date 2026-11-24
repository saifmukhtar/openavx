/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_discovery_server_main.c - Standalone discovery server executable
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "avx_platform.h"
#include "avx_internal.h"
#include "avx_discovery_server.h"

#if defined(AVX_OS_POSIX)
    #include <unistd.h>
#endif

static volatile bool s_keep_running = true;

static void sig_handler(int sig)
{
    (void)sig;
    s_keep_running = false;
}

static void server_log(const char *level, const char *ansi_color, const char *fmt, ...)
{
    bool use_color = false;
#if defined(AVX_OS_POSIX)
    use_color = (isatty(fileno(stderr)) != 0);
#endif
    if (use_color && ansi_color) {
        fprintf(stderr, "%s[openavx:server] [%s]\033[0m ", ansi_color, level);
    } else {
        fprintf(stderr, "[openavx:server] [%s] ", level);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* CLI entry point for the standalone OpenAVX discovery registry daemon. */
int main(int argc, char *argv[])
{
    uint16_t port = AVX_DISCOVERY_SERVER_DEFAULT_PORT;
    const char *bind_ip = "0.0.0.0";
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-port") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-bind") == 0 || strcmp(argv[i], "--bind") == 0) && i + 1 < argc) {
            bind_ip = argv[++i];
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-help") == 0) {
            printf("OpenAVX Standalone Discovery Server Daemon\n");
            printf("Usage: %s [options]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -port, --port <port>  TCP port to listen on (default: 7959)\n");
            printf("  -bind, --bind <ip>    IP address to bind to (default: 0.0.0.0)\n");
            printf("  -q,    --quiet        Suppress startup and status messages\n");
            printf("  -h,    --help         Display this help message and exit\n");
            return 0;
        }
    }

    if (!quiet) {
        fprintf(stderr, "openavx server (%s)\n", OPENAVX_VERSION);
        fprintf(stderr, "  %-12s: %s\n", "bind", bind_ip);
        fprintf(stderr, "  %-12s: %u/tcp\n", "port", port);
        server_log("info", "\033[36m", "Binding discovery service on %s:%u (TCP)", bind_ip, port);
    }

    avx_discovery_server_t *srv = avx_discovery_server_create(bind_ip, port);
    if (!srv) {
        server_log("error", "\033[1;31m", "Failed to start discovery server on %s:%u", bind_ip, port);
        return 1;
    }

    if (!quiet) {
        server_log("info", "\033[36m", "Discovery service active (Ctrl+C to stop)");
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    while (s_keep_running) {
        avx_sleep_ms(1000);
    }

    if (!quiet) {
        server_log("info", "\033[36m", "Shutting down discovery server...");
    }
    avx_discovery_server_destroy(srv);
    if (!quiet) {
        server_log("info", "\033[36m", "Discovery server stopped cleanly");
    }

    return 0;
}
