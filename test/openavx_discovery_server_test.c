/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_discovery_server_test.c - Centralized Discovery Server Verification
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "avx_platform.h"

#include "openavx.h"
#include "avx_discovery_server.h"

#define DS_TEST_PORT 7959

int main(void) {
    printf("\n=======================================================\n");
    printf(" OpenAVX Centralized Discovery Server Test\n");
    printf("=======================================================\n\n");

    /* 1. Start Centralized Discovery Server on 127.0.0.1:7959 */
    printf("[*] 1. Launching Discovery Server daemon on 127.0.0.1:%d...\n", DS_TEST_PORT);
    avx_discovery_server_t *srv = avx_discovery_server_create("127.0.0.1", DS_TEST_PORT);
    assert(srv != NULL);
    printf("    [+] Centralized Discovery Server listening on TCP port %d!\n", DS_TEST_PORT);

    /* 2. Initialize openavx */
    printf("[*] 2. Initializing OpenAVX library...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* Set environment variable pointing to our discovery server */
    setenv("OPENAVX_DISCOVERY_SERVER", "127.0.0.1:7959", 1);

    /* 3. Create Sender */
    printf("[*] 3. Creating Sender \"ENTERPRISE_STUDIO_TX\"...\n");
    oavx_stream_config_t send_create = {
        .name = "ENTERPRISE_STUDIO_TX",
        .groups = "corporate",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);
    printf("    [+] Sender registered automatically with Discovery Server!\n");

    /* Give server time to record registration */
    avx_sleep_ms(50);

    /* 4. Create Finder configured with corporate group */
    printf("[*] 4. Creating Finder targeting Discovery Server...\n");
    oavx_discovery_t *p_find = oavx_discovery_create(ctx, "corporate");
    assert(p_find != NULL);

    bool found = oavx_discovery_wait(p_find, 1000);
    assert(found);

    oavx_endpoint_t sources[16];
    size_t num_sources = oavx_discovery_get_sources(p_find, sources, 16);
    assert(num_sources >= 1);

    printf("    [+] Discovery Server returned %zu source(s):\n", num_sources);
    int match_idx = -1;
    for (size_t i = 0; i < num_sources; ++i) {
        printf("        [%zu] \"%s\" at %s\n", i, sources[i].name, sources[i].address);
        if (strcmp(sources[i].name, "ENTERPRISE_STUDIO_TX") == 0) {
            match_idx = (int)i;
        }
    }
    assert(match_idx >= 0);

    /* 5. Connect Receiver via discovered URL */
    printf("[*] 5. Connecting Receiver via Centralized Discovery catalog...\n");
    oavx_stream_config_t recv_create = {
        .name = "Enterprise_Client",
        .endpoint = sources[match_idx].address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_UYVY_RGBA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);
    printf("    [+] Receiver connected successfully!\n");

    /* 6. Verify Source Deregistration on Sender Shutdown */
    printf("[*] 6. Verifying Discovery Server auto-purge on Sender disconnect...\n");
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);

    avx_sleep_ms(50);

    /* Direct client query to verify server purged the source */
    char names[16][256];
    char urls[16][256];
    int count = avx_discovery_client_query("127.0.0.1", DS_TEST_PORT, names, urls, 16);
    assert(count == 0);
    printf("    [+] Server purged disconnected sender: remaining active sources = %d\n", count);

    /* 7. Cleanup */
    printf("[*] 7. Tearing down Discovery Server...\n");
    oavx_discovery_destroy(p_find);
    oavx_context_destroy(ctx);
    avx_discovery_server_destroy(srv);
    unsetenv("OPENAVX_DISCOVERY_SERVER");

    printf("\n[PASS] openavx_discovery_server_test\n\n");
    return 0;
}
