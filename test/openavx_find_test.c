/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_find_test.c - Automated Verification of openavx-find CLI Utility
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "avx_platform.h"

#include "openavx.h"
#include "avx_internal.h"

static volatile bool s_beacon_running = true;
static uint16_t s_send_port = 7960;

static void* beacon_thread(void* arg) {
    (void)arg;
    while (s_beacon_running) {
        avx_mdns_broadcast_source("TEST_SCAN_SOURCE_42", s_send_port);
        usleep(100000); /* Broadcast every 100ms */
    }
    return NULL;
}

static const char* get_find_cmd_prefix(void) {
    if (access("./bin/openavx-find", X_OK) == 0) return "./bin/openavx-find";
    if (access("./openavx-find", X_OK) == 0) return "./openavx-find";
    if (access("./bin/openavx.exe", X_OK) == 0) return "./bin/openavx.exe find";
    if (access("bin/openavx.exe", X_OK) == 0) return "bin/openavx.exe find";
    if (access("./openavx.exe", X_OK) == 0) return "./openavx.exe find";
    if (access("./bin/openavx", X_OK) == 0) return "./bin/openavx find";
    return "./bin/openavx find";
}

int main(void) {
    printf("\n=======================================================\n");
    printf(" OpenAVX (openavx) Standalone Source Finder CLI Test\n");
    printf("=======================================================\n\n");

    const char* find_prefix = get_find_cmd_prefix();

    /* 1. Test CLI Help Banner */
    printf("[*] 1. Testing CLI Help Banner (--help)...\n");
    char cmd[512];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "%s --help > NUL 2>&1", find_prefix);
#else
    snprintf(cmd, sizeof(cmd), "%s --help > /dev/null 2>&1", find_prefix);
#endif
    int ret_help = system(cmd);
    assert(WIFEXITED(ret_help) && WEXITSTATUS(ret_help) == 0);
    printf("    [+] Help flag on %s returned exit code 0.\n", find_prefix);

    /* 2. Start Live Test Sender */
    printf("[*] 2. Spawning Live Test Sender 'TEST_SCAN_SOURCE_42'...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t send_cfg = {
        .name = "TEST_SCAN_SOURCE_42",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_cfg);
    assert(p_send != NULL);

    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(p_send);
    if (ep && ep->address) {
        const char *colon = strchr(ep->address, ':');
        if (colon) s_send_port = (uint16_t)atoi(colon + 1);
    }
    avx_thread_t th;
    s_beacon_running = true;
    avx_thread_create(&th, beacon_thread, NULL);
    usleep(50000); /* 50ms initial wait */

    /* 3. Test Standard Text Output */
    printf("[*] 3. Testing Standard Scanner Output (%s)...\n", find_prefix);
    snprintf(cmd, sizeof(cmd), "%s --wait 1200", find_prefix);
    FILE *fp = popen(cmd, "r");
    assert(fp != NULL);
    char buf[2048] = {0};
    size_t bytes_read = fread(buf, 1, sizeof(buf) - 1, fp);
    int pclose_ret = pclose(fp);
    assert(WIFEXITED(pclose_ret) && WEXITSTATUS(pclose_ret) == 0);
    assert(bytes_read > 0);
    assert(strstr(buf, "TEST_SCAN_SOURCE_42") != NULL);
    printf("    [+] Successfully discovered 'TEST_SCAN_SOURCE_42' in text output!\n");

    /* 4. Test Machine-Readable JSON Output */
    printf("[*] 4. Testing JSON Output (%s --json)...\n", find_prefix);
    snprintf(cmd, sizeof(cmd), "%s --wait 1200 --json", find_prefix);
    fp = popen(cmd, "r");
    assert(fp != NULL);
    memset(buf, 0, sizeof(buf));
    bytes_read = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose_ret = pclose(fp);
    assert(WIFEXITED(pclose_ret) && WEXITSTATUS(pclose_ret) == 0);
    assert(bytes_read > 0);
    assert(strstr(buf, "\"name\": \"TEST_SCAN_SOURCE_42\"") != NULL);
    assert(buf[0] == '[');
    printf("    [+] Valid JSON array format verified with matching source name!\n");

    /* 5. Test Quiet URL Output */
    printf("[*] 5. Testing Quiet URL Output (%s --quiet)...\n", find_prefix);
    snprintf(cmd, sizeof(cmd), "%s --wait 1200 --quiet", find_prefix);
    fp = popen(cmd, "r");
    assert(fp != NULL);
    memset(buf, 0, sizeof(buf));
    bytes_read = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose_ret = pclose(fp);
    assert(WIFEXITED(pclose_ret) && WEXITSTATUS(pclose_ret) == 0);
    assert(bytes_read > 0);
    assert(strstr(buf, "127.0.0.1") != NULL || strstr(buf, "TEST_SCAN_SOURCE_42") != NULL);
    printf("    [+] Quiet output returned direct URL/name: %s", buf);

    /* 6. Clean up */
    s_beacon_running = false;
    avx_thread_join(th);
    oavx_stream_close(p_send);
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_find_test\n\n");
    return 0;
}
