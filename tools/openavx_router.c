/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_router.c - Virtual matrix routing CLI
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include "avx_platform.h"

#include "openavx.h"
#include "avx_routing.h"
#include "openavx_cli.h"

static volatile bool g_running = true;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = false;
}

static inline bool check_stdin_ready(int timeout_ms) {
#if defined(AVX_OS_WINDOWS) || defined(_WIN32)
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return false;
    DWORD res = WaitForSingleObject(h, (DWORD)timeout_ms);
    return (res == WAIT_OBJECT_0);
#else
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
    int ret = poll(&pfd, 1, timeout_ms);
    return (ret > 0 && (pfd.revents & POLLIN));
#endif
}

static void print_usage(const char* prog) {
    printf("OpenAVX Matrix Router (Virtual Matrix Routing Switcher)\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -n, --name <NAME>       Name of router output stream (default: \"OpenAVX Matrix Router\")\n");
    printf("  -s, --source <SOURCE>   Initial upstream source name or IP:port to route\n");
    printf("  -g, --groups <GROUPS>   Broadcast groups (default: public)\n");
    printf("  -i, --interactive       Run interactive routing console on stdin\n");
    printf("  -l, --list              List active network sources and exit\n");
    printf("  -q, --quiet             Suppress banner and informational logs\n");
    printf("  -h, --help              Display this help and exit\n\n");
    printf("Interactive Commands:\n");
    printf("  list                    Scan and list all active upstream OpenAVX sources\n");
    printf("  switch <INDEX|NAME|URL> Switch router output to specified source\n");
    printf("  clear                   Clear routing (output black/idle frame)\n");
    printf("  status                  Display current router status and active route\n");
    printf("  help                    Display command help\n");
    printf("  quit / exit             Shutdown router\n\n");
    printf("Examples:\n");
    printf("  %s --source \"Studio Camera 1\"       # Route Camera 1 continuously\n", prog);
    printf("  %s --interactive                    # Start interactive matrix switcher\n", prog);
}

static void do_list_sources(oavx_discovery_t* p_find, oavx_endpoint_t* out_sources, size_t* out_count) {
    if (!p_find) return;

    oavx_log_write(OAVX_LOG_INFO, "router", "Scanning network for active OpenAVX sources...");
    oavx_discovery_wait(p_find, 1000);

    oavx_endpoint_t sources[64];
    size_t count = oavx_discovery_get_sources(p_find, sources, 64);

    if (out_sources) memcpy(out_sources, sources, count * sizeof(oavx_endpoint_t));
    if (out_count) *out_count = count;

    if (count == 0) {
        oavx_log_write(OAVX_LOG_INFO, "router", "No OpenAVX sources discovered on current subnet/groups");
        return;
    }

    printf("----------------------------------------------------------------------\n");
    printf("  #   %-35s %s\n", "SOURCE NAME", "URL ADDRESS");
    printf("----------------------------------------------------------------------\n");
    for (size_t i = 0; i < count; ++i) {
        printf(" [%2zu] %-35s %s\n", i + 1,
               sources[i].name ? sources[i].name : "<unknown>",
               sources[i].address ? sources[i].address : "<unknown>");
    }
    printf("----------------------------------------------------------------------\n");
    printf("Found %zu source(s).\n\n", count);
}

int openavx_cmd_route(int argc, char** argv) {
    optind = 1;
    g_running = true;
    char router_name[256] = "OpenAVX Matrix Router";
    char initial_source[256] = {0};
    char groups[256] = "public";
    bool interactive = false;
    bool list_and_exit = false;
    bool quiet = false;

    static struct option long_opts[] = {
        { "name",        required_argument, 0, 'n' },
        { "source",      required_argument, 0, 's' },
        { "groups",      required_argument, 0, 'g' },
        { "interactive", no_argument,       0, 'i' },
        { "list",        no_argument,       0, 'l' },
        { "quiet",       no_argument,       0, 'q' },
        { "help",        no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:s:g:ilqh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'n':
            snprintf(router_name, sizeof(router_name), "%s", optarg);
            break;
        case 's':
            snprintf(initial_source, sizeof(initial_source), "%s", optarg);
            break;
        case 'g':
            snprintf(groups, sizeof(groups), "%s", optarg);
            break;
        case 'i':
            interactive = true;
            break;
        case 'l':
            list_and_exit = true;
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

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        oavx_log_write(OAVX_LOG_ERROR, "router", "Failed to initialize OpenAVX context engine");
        return 1;
    }

    /* Handle --list and exit mode */
    if (list_and_exit) {
        oavx_discovery_t *p_find = oavx_discovery_create(ctx, groups);
        if (p_find) {
            do_list_sources(p_find, NULL, NULL);
            oavx_discovery_destroy(p_find);
        }
        oavx_context_destroy(ctx);
        return 0;
    }

    /* Create Matrix Router Output */
    oavx_router_t *p_router = oavx_router_create(ctx, router_name, groups);
    if (!p_router) {
        oavx_log_write(OAVX_LOG_ERROR, "router", "Failed to create matrix router '%s'", router_name);
        oavx_context_destroy(ctx);
        return 1;
    }

    oavx_endpoint_t router_adv = oavx_router_get_source(p_router);

    openavx_cli_header("route");
    openavx_cli_kv("stream", "%s", router_name);
    openavx_cli_kv("url", "%s", router_adv.address ? router_adv.address : "0.0.0.0");
    openavx_cli_kv("groups", "%s", groups);
    openavx_cli_kv("mode", "%s", interactive ? "interactive" : "headless");

    /* Create Finder for source resolution */
    oavx_discovery_t *p_find = oavx_discovery_create(ctx, groups);

    char current_route_name[256] = "(none - idle/black)";
    char current_route_url[256] = {0};

    /* Switch to initial source if requested */
    if (initial_source[0] != '\0') {
        oavx_endpoint_t src = { .name = initial_source, .address = NULL };
        if (strchr(initial_source, ':') && !strchr(initial_source, ' ')) {
            src.address = initial_source;
        }
        if (oavx_router_route(p_router, &src)) {
            snprintf(current_route_name, sizeof(current_route_name), "%s", initial_source);
            oavx_log_write(OAVX_LOG_INFO, "router", "Initial route switched to '%s'", initial_source);
        } else {
            oavx_log_write(OAVX_LOG_WARN, "router", "Failed to route initial source '%s'", initial_source);
        }
    }

    if (interactive) {
        printf("Type 'help' for commands, 'list' to view sources, 'quit' to exit.\n");
        printf("router> ");
        fflush(stdout);
    }

    char line_buf[512];
    while (g_running) {
        if (check_stdin_ready(500)) {
            if (!fgets(line_buf, sizeof(line_buf), stdin)) {
                /* EOF on stdin (pipe or terminal closed) */
                if (interactive) break;
                /* If headless and stdin closed, keep running daemon until signal */
                avx_sleep_ms(1000);
                continue;
            }

            /* Strip trailing newline */
            line_buf[strcspn(line_buf, "\r\n")] = '\0';
            char* cmd = line_buf;
            while (*cmd == ' ' || *cmd == '\t') cmd++;
            if (*cmd == '\0') {
                if (interactive) { printf("router> "); fflush(stdout); }
                continue;
            }

            if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
                printf("Commands:\n");
                printf("  list                          List available network OpenAVX sources\n");
                printf("  switch <INDEX | NAME | URL>   Switch router active input\n");
                printf("  clear                         Clear route (output idle black)\n");
                printf("  status                        Show current route and router state\n");
                printf("  quit / exit                   Shutdown router\n");
            } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
                do_list_sources(p_find, NULL, NULL);
            } else if (strncmp(cmd, "switch", 6) == 0) {
                char* target = cmd + 6;
                while (*target == ' ' || *target == '\t') target++;
                if (*target == '\0') {
                    printf("Usage: switch <INDEX | NAME | URL>\n");
                } else {
                    /* Check if target is a numeric index from list */
                    char* endptr = NULL;
                    long idx = strtol(target, &endptr, 10);
                    bool switched = false;

                    if (endptr && *endptr == '\0' && idx > 0 && p_find) {
                        oavx_endpoint_t p_sources[64];
                        size_t count = oavx_discovery_get_sources(p_find, p_sources, 64);
                        if (idx <= (long)count) {
                            const oavx_endpoint_t* sel = &p_sources[idx - 1];
                            switched = oavx_router_route(p_router, sel);
                            if (switched) {
                                snprintf(current_route_name, sizeof(current_route_name), "%s", sel->name ? sel->name : "<unknown>");
                                if (sel->address) snprintf(current_route_url, sizeof(current_route_url), "%s", sel->address);
                                oavx_log_write(OAVX_LOG_INFO, "router", "Routed [%ld] %s (%s)", idx, current_route_name, current_route_url);
                            }
                        } else {
                            oavx_log_write(OAVX_LOG_ERROR, "router", "Source index %ld out of range (1..%zu); type 'list'", idx, count);
                        }
                    }

                    if (!switched && (endptr == NULL || *endptr != '\0')) {
                        /* Match by name or URL */
                        oavx_endpoint_t src = { .name = target, .address = NULL };
                        if (strchr(target, ':') && !strchr(target, ' ')) {
                            src.address = target;
                        }
                        if (oavx_router_route(p_router, &src)) {
                            snprintf(current_route_name, sizeof(current_route_name), "%s", target);
                            oavx_log_write(OAVX_LOG_INFO, "router", "Routed '%s'", target);
                        } else {
                            oavx_log_write(OAVX_LOG_ERROR, "router", "Failed to switch to source '%s'", target);
                        }
                    }
                }
            } else if (strcmp(cmd, "clear") == 0) {
                oavx_router_clear(p_router);
                strcpy(current_route_name, "(none - idle/black)");
                current_route_url[0] = '\0';
                oavx_log_write(OAVX_LOG_INFO, "router", "Route cleared (idle black output)");
            } else if (strcmp(cmd, "status") == 0) {
                printf("Router Status:\n");
                printf("  Output Stream : %s\n", router_name);
                printf("  Active Route  : %s\n", current_route_name);
                if (current_route_url[0]) {
                    printf("  Source URL    : %s\n", current_route_url);
                }
            } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
                oavx_log_write(OAVX_LOG_INFO, "router", "Exiting router...");
                g_running = false;
                break;
            } else {
                oavx_log_write(OAVX_LOG_WARN, "router", "Unknown command '%s' (type 'help' for options)", cmd);
            }

            if (interactive && g_running) {
                printf("router> ");
                fflush(stdout);
            }
        }
    }

    oavx_log_write(OAVX_LOG_INFO, "router", "Stopping matrix router...");
    if (p_find) oavx_discovery_destroy(p_find);
    oavx_router_destroy(p_router);
    oavx_context_destroy(ctx);

    oavx_log_write(OAVX_LOG_INFO, "router", "Matrix router stopped cleanly");
    return 0;
}
