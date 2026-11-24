/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_mdns_discovery_test.c - Passive LAN mDNS Listener Verification Suite
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

#include "openavx.h"

/* Internal prototype for broadcasting mDNS packet */
extern int avx_mdns_broadcast_source(const char* name, uint16_t port);

int main(void) {
    printf("\n=== OpenAVX Passive mDNS Discovery Tests ===\n\n");

    /* 1. Initialize openavx */
    printf("[*] 1. Initializing OpenAVX Context...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* 2. Create Finder */
    printf("[*] 2. Spawning Discovery Grid...\n");
    oavx_discovery_t *p_find = oavx_discovery_create(ctx, NULL);
    assert(p_find != NULL);

    /* Initially, no sources should be present */
    oavx_endpoint_t sources[32];
    size_t count = oavx_discovery_get_sources(p_find, sources, 32);
    printf("    [+] Initial sources count: %zu\n", count);

    /* 3. Simulate an external LAN camera broadcasting mDNS announcement */
    printf("[*] 3. Broadcasting external mDNS announcement: \"STUDIO_CAMERA_1\" on port 7965...\n");
    int b_res = avx_mdns_broadcast_source("STUDIO_CAMERA_1", 7965);
    assert(b_res == 0);

    /* Also broadcast a second camera */
    printf("[*] 4. Broadcasting second external mDNS announcement: \"ROBOTIC_PTZ_1\" on port 7968...\n");
    b_res = avx_mdns_broadcast_source("ROBOTIC_PTZ_1", 7968);
    assert(b_res == 0);

    /* 5. Wait for sources via mDNS listener */
    printf("[*] 5. Waiting for mDNS listener to capture multicast responses...\n");
    bool found = oavx_discovery_wait(p_find, 1500);
    assert(found);

    count = oavx_discovery_get_sources(p_find, sources, 32);
    assert(count >= 2);

    printf("    [+] Discovered %zu mDNS source(s) over LAN multicast:\n", count);
    bool found_cam1 = false;
    bool found_ptz1 = false;

    for (size_t i = 0; i < count; ++i) {
        printf("        [%zu] \"%s\" @ %s\n", i, sources[i].name, sources[i].address);
        if (strcmp(sources[i].name, "STUDIO_CAMERA_1") == 0) {
            found_cam1 = true;
            assert(strstr(sources[i].address, ":7965") != NULL);
        }
        if (strcmp(sources[i].name, "ROBOTIC_PTZ_1") == 0) {
            found_ptz1 = true;
            assert(strstr(sources[i].address, ":7968") != NULL);
        }
    }

    assert(found_cam1);
    assert(found_ptz1);
    printf("    [+] Successfully verified PTR name and SRV port resolution!\n");

    /* 6. Destroy Finder */
    printf("[*] 6. Tearing down discovery grid...\n");
    oavx_discovery_destroy(p_find);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_mdns_discovery_test\n\n");
    return 0;
}
