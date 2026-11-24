/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_test.c - Basic Smoke & Integrity Test
 */

#include "openavx.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main(void) {
    printf("==========================================\n");
    printf(" OpenAVX Native Foundation Test (oavx_*)\n");
    printf("==========================================\n\n");

    printf("[*] 1. Initializing OpenAVX Context Engine...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);
    printf("[+] oavx_context_create succeeded: %p\n", (void*)ctx);

    printf("[*] 2. Querying version and CPU support...\n");
    const char* version = oavx_version();
    printf("[+] oavx_version: %s\n", version);
    assert(version != NULL);

    bool cpu_ok = oavx_is_supported_cpu();
    printf("[+] oavx_is_supported_cpu: %s\n", cpu_ok ? "YES" : "NO");
    assert(cpu_ok);

    printf("[*] 3. Testing Discovery Session...\n");
    oavx_discovery_t *disc = oavx_discovery_create(ctx, "public");
    assert(disc != NULL);
    printf("[+] Discovery instance created: %p\n", (void*)disc);

    oavx_endpoint_t sources[64];
    size_t num_sources = oavx_discovery_get_sources(disc, sources, 64);
    printf("[+] Current discovered sources: %zu\n", num_sources);

    oavx_discovery_destroy(disc);
    printf("[+] Discovery instance destroyed\n");

    printf("[*] 4. Testing Transmit Stream...\n");
    oavx_stream_config_t send_settings = {
        .name = "OpenAVX (Test Channel)",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &send_settings);
    assert(tx != NULL);
    printf("[+] Transmit stream created: %p\n", (void*)tx);

    oavx_tally_t tally = {0};
    bool tally_ok = oavx_stream_get_tally(tx, &tally, 100);
    assert(tally_ok);
    printf("[+] Stream tally queried: flags=0x%x (program=%d, preview=%d)\n",
           tally.flags, (tally.flags & OAVX_TALLY_PROGRAM) != 0, (tally.flags & OAVX_TALLY_PREVIEW) != 0);

    oavx_stream_close(tx);
    printf("[+] Transmit stream closed\n");

    printf("[*] 5. Testing Receive Stream...\n");
    oavx_stream_config_t recv_settings = {
        .name = "OpenAVX (Receiver Test)",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_BGRX_BGRA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &recv_settings);
    assert(rx != NULL);
    printf("[+] Receive stream created: %p\n", (void*)rx);

    oavx_stream_close(rx);
    printf("[+] Receive stream closed\n");

    printf("[*] 6. Destroying OpenAVX context engine...\n");
    oavx_context_destroy(ctx);
    printf("[+] oavx_context_destroy completed cleanly!\n");

    printf("\n[PASS] openavx_test\n");
    return 0;
}
