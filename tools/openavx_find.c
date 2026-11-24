/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_find.c - Network stream discovery CLI
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "avx_platform.h"

#include "openavx.h"
#include "openavx_cli.h"

static void print_usage(const char* prog) {
    printf("OpenAVX Network Source Finder\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -w, --wait <ms>        Discovery search duration in ms (default: 1500)\n");
    printf("  -g, --groups <names>   Comma-separated list of groups to query\n");
    printf("  -e, --extra-ips <ips>  Comma-separated list of extra unicast IPs to scan\n");
    printf("  -j, --json             Output discovered sources as machine-readable JSON\n");
    printf("  -q, --quiet            Only output found source URLs/names (suppress logs)\n");
    printf("  -h, --help             Display this help message\n\n");
    printf("Examples:\n");
    printf("  %s                         # Scan LAN over zero-conf mDNS for 1.5s\n", prog);
    printf("  %s --wait 3000             # Scan LAN with extended 3-second timeout\n", prog);
    printf("  %s --extra-ips 10.0.0.50   # Scan specific remote unicast IP\n", prog);
    printf("  %s --json                  # Output results formatted as JSON array\n", prog);
}

int openavx_cmd_find(int argc, char** argv) {
    optind = 1;
    int timeout_ms = 1500;
    const char* groups = NULL;
    const char* extra_ips = NULL;
    bool json_output = false;
    bool quiet_output = false;

    static struct option long_opts[] = {
        {"wait",      required_argument, 0, 'w'},
        {"groups",    required_argument, 0, 'g'},
        {"extra-ips", required_argument, 0, 'e'},
        {"json",      no_argument,       0, 'j'},
        {"quiet",     no_argument,       0, 'q'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "w:g:e:jqh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'w':
                timeout_ms = atoi(optarg);
                if (timeout_ms <= 0) timeout_ms = 1500;
                break;
            case 'g':
                groups = optarg;
                break;
            case 'e':
                extra_ips = optarg;
                break;
            case 'j':
                json_output = true;
                break;
            case 'q':
                quiet_output = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    openavx_cli_set_quiet(quiet_output || json_output);

    if (extra_ips && extra_ips[0] != '\0') {
        setenv("OPENAVX_DISCOVERY_SERVER", extra_ips, 1);
    }

    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        if (!quiet_output && !json_output) {
            oavx_log_write(OAVX_LOG_ERROR, "find", "Failed to initialize OpenAVX context engine");
        }
        return 1;
    }

    oavx_discovery_t *p_find = oavx_discovery_create(ctx, groups);
    if (!p_find) {
        if (!quiet_output && !json_output) {
            oavx_log_write(OAVX_LOG_ERROR, "find", "Failed to create discovery instance");
        }
        oavx_context_destroy(ctx);
        return 1;
    }

    if (!quiet_output && !json_output) {
        openavx_cli_header("find");
        openavx_cli_kv("groups", "%s", groups ? groups : "public");
        openavx_cli_kv("timeout", "%d ms", timeout_ms);
        oavx_log_write(OAVX_LOG_INFO, "find", "Scanning network for active OpenAVX sources...");
    }

    /* Wait for mDNS multicast responses and discovery server queries */
    oavx_discovery_wait(p_find, (uint32_t)timeout_ms);

    oavx_endpoint_t sources[128];
    size_t num_sources = oavx_discovery_get_sources(p_find, sources, 128);

    if (json_output) {
        printf("[\n");
        for (size_t i = 0; i < num_sources; ++i) {
            const char* name = sources[i].name ? sources[i].name : "";
            const char* url = sources[i].address ? sources[i].address : "";
            printf("  {\"name\": \"%s\", \"url\": \"%s\"}%s\n",
                   name, url, (i + 1 < num_sources) ? "," : "");
        }
        printf("]\n");
    } else if (quiet_output) {
        for (size_t i = 0; i < num_sources; ++i) {
            if (sources[i].name && sources[i].name[0]) {
                printf("%s\n", sources[i].name);
            } else if (sources[i].address) {
                printf("%s\n", sources[i].address);
            }
        }
    } else {
        if (num_sources == 0) {
            oavx_log_write(OAVX_LOG_INFO, "find", "No active OpenAVX sources found");
        } else {
            printf("Found %zu active OpenAVX source%s:\n", num_sources, num_sources == 1 ? "" : "s");
            for (size_t i = 0; i < num_sources; ++i) {
                printf("  [%zu] %-30s -> %s\n",
                       i,
                       sources[i].name ? sources[i].name : "(Unnamed)",
                       sources[i].address ? sources[i].address : "mDNS");
            }
        }
    }

    oavx_discovery_destroy(p_find);
    oavx_context_destroy(ctx);

    return (num_sources > 0) ? 0 : 2;
}
