/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_tally_test.c - Automated Verification of openavx tally CLI Utility & APIs
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "avx_platform.h"

#include "openavx.h"
#include "avx_internal.h"

#if defined(AVX_OS_POSIX)
    #include <unistd.h>
    #include <sys/wait.h>
#endif

static int run_cmd_capture(const char* cmd, char* out_buf, size_t out_sz) {
    if (out_buf && out_sz > 0) out_buf[0] = '\0';
    FILE* fp = popen(cmd, "r");
    if (!fp) return -1;
    if (out_buf && out_sz > 0) {
        size_t n = fread(out_buf, 1, out_sz - 1, fp);
        out_buf[n] = '\0';
    }
    int status = pclose(fp);
#if defined(AVX_OS_POSIX)
    if (WIFEXITED(status)) return WEXITSTATUS(status);
#endif
    return status;
}

static const char* find_tally_binary(bool symlink) {
    static char found_cli[256];
    static char found_sym[256];
    char* found = symlink ? found_sym : found_cli;
    const char* symlink_candidates[] = {
        "./bin/openavx-tally",
        "./bin/openavx_tally",
        "./openavx-tally",
        "./build/bin/openavx-tally",
        "./build/bin/openavx_tally"
    };
    const char* direct_candidates[] = {
        "./bin/openavx",
        "./bin/openavx.exe",
        "bin/openavx.exe",
        "./openavx",
        "./openavx.exe",
        "./build/bin/openavx",
        "./build/bin/openavx.exe"
    };

    if (symlink) {
        for (size_t i = 0; i < sizeof(symlink_candidates)/sizeof(symlink_candidates[0]); ++i) {
            if (access(symlink_candidates[i], X_OK) == 0) {
                snprintf(found, 256, "%s", symlink_candidates[i]);
                return found;
            }
        }
    } else {
        for (size_t i = 0; i < sizeof(direct_candidates)/sizeof(direct_candidates[0]); ++i) {
            if (access(direct_candidates[i], X_OK) == 0) {
                snprintf(found, 256, "%s", direct_candidates[i]);
                return found;
            }
        }
    }
    return symlink ? "./bin/openavx-tally" : "./bin/openavx";
}

int main(void) {
    const char* bin_cli = find_tally_binary(false);
    const char* bin_sym = find_tally_binary(true);

    printf("\n=======================================================\n");
    printf(" OpenAVX Broadcast Tally CLI & API Verification Test\n");
    printf("=======================================================\n\n");
    printf("Using CLI Binary: %s\n", bin_cli);
    printf("Using Symlink:    %s\n\n", bin_sym);

    /* 1. Test CLI Help Banner */
    printf("[*] 1. Testing CLI Help Banner (--help)...\n");
    char output[2048] = {0};
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s tally --help", bin_cli);
    int rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "Tally Light Monitor & Remote Controller") != NULL);
    assert(strstr(output, "--set") != NULL);
    assert(strstr(output, "--poll") != NULL);
    printf("    [+] 'openavx tally --help' executed successfully with complete usage documentation.\n");

    /* 2. Test Symlink Invocation */
#ifndef _WIN32
    printf("[*] 2. Testing Multi-call Symlink 'openavx-tally'...\n");
    snprintf(cmd, sizeof(cmd), "%s --help", bin_sym);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "Tally Light Monitor") != NULL);
    printf("    [+] Multi-call symlink 'openavx-tally --help' confirmed functional.\n");
#else
    printf("[*] 2. Skipping Multi-call Symlink on Windows (symlinks not supported)...\n");
#endif

    /* 3. Test Missing Argument Handling */
    printf("[*] 3. Testing Error Handling on Missing Target...\n");
    snprintf(cmd, sizeof(cmd), "%s tally 2>&1", bin_cli);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc != 0);
    assert(strstr(output, "Error: Missing target") != NULL);
    printf("    [+] Missing target argument rejected with non-zero exit code.\n");

    /* 4. Start Test Stream Sender with Web Preview Server */
    printf("[*] 4. Launching Test Stream Sender with Web Server...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t send_cfg = {
        .name = "TEST_TALLY_STREAM_99",
        .direction = OAVX_DIR_TRANSMIT,
        .color_format = OAVX_COLOR_FORMAT_RGBX_RGBA,
        .clock_sync = false
    };

    oavx_stream_t *stream = oavx_stream_open(ctx, &send_cfg);
    assert(stream != NULL);

    /* Enable embedded web server on ephemeral port (port 0) */
    oavx_status_t web_st = oavx_stream_enable_web_preview(stream, 0);
    assert(web_st == OAVX_SUCCESS);
    uint16_t web_port = oavx_stream_get_web_preview_port(stream);
    assert(web_port > 0);
    printf("    [+] Test stream sender initialized, Web server listening on port %u\n", web_port);

    /* 5. Test Polling Tally via HTTP REST (Initial State: OFF) */
    printf("[*] 5. Testing HTTP REST Tally Poll (Initial State)...\n");
    snprintf(cmd, sizeof(cmd), "%s tally http://127.0.0.1:%u --poll --text", bin_cli, web_port);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "OFF") != NULL);

    snprintf(cmd, sizeof(cmd), "%s tally http://127.0.0.1:%u --poll --json", bin_cli, web_port);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "\"program\":false") != NULL);
    assert(strstr(output, "\"preview\":false") != NULL);
    assert(strstr(output, "\"recording\":false") != NULL);
    assert(strstr(output, "\"flags\":0") != NULL);
    printf("    [+] Initial tally polled over HTTP confirmed OFF (flags=0).\n");

    /* 6. Test Commanding Tally via HTTP REST: Program (Red) */
    printf("[*] 6. Testing HTTP REST Tally Control (--set pgm)...\n");
    snprintf(cmd, sizeof(cmd), "%s tally http://127.0.0.1:%u --set pgm --json", bin_cli, web_port);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "\"ok\":true") != NULL);
    assert(strstr(output, "\"flags\":1") != NULL);

    snprintf(cmd, sizeof(cmd), "%s tally http://127.0.0.1:%u --poll --text", bin_cli, web_port);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "PROGRAM") != NULL);
    printf("    [+] HTTP REST commanded Program tally successfully verified.\n");

    /* 7. Test Commanding Tally via HTTP REST: Preview + Recording (Green + Amber) */
    printf("[*] 7. Testing HTTP REST Tally Control (--set 'pvw+rec')...\n");
    snprintf(cmd, sizeof(cmd), "%s tally http://127.0.0.1:%u --set \"pvw+rec\" --json", bin_cli, web_port);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "\"ok\":true") != NULL);
    assert(strstr(output, "\"flags\":6") != NULL);

    snprintf(cmd, sizeof(cmd), "%s tally http://127.0.0.1:%u --poll --text", bin_cli, web_port);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "PREVIEW+RECORDING") != NULL);
    printf("    [+] HTTP REST commanded Preview+Recording (flags=6) successfully verified.\n");

    /* 8. Test Commanding Tally via HTTP REST: Clear / Off */
    printf("[*] 8. Testing HTTP REST Tally Control (--set off)...\n");
    snprintf(cmd, sizeof(cmd), "%s tally http://127.0.0.1:%u --set off --json", bin_cli, web_port);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "\"flags\":0") != NULL);

    snprintf(cmd, sizeof(cmd), "%s tally http://127.0.0.1:%u --poll --text", bin_cli, web_port);
    rc = run_cmd_capture(cmd, output, sizeof(output));
    assert(rc == 0);
    assert(strstr(output, "OFF") != NULL);
    printf("    [+] HTTP REST cleared tally to OFF successfully.\n");

    /* 9. Test Sender & Receiver Pipeline APIs */
    printf("[*] 9. Testing Pipeline Stream Tally API Consistency...\n");
    oavx_tally_t direct_tally = { .flags = OAVX_TALLY_PROGRAM | OAVX_TALLY_RECORDING };
    bool set_ok = oavx_stream_set_tally(stream, &direct_tally);
    assert(set_ok);

    oavx_tally_t queried_tally = {0};
    bool get_ok = oavx_stream_get_tally(stream, &queried_tally, 500);
    assert(get_ok);
    assert(queried_tally.flags == (OAVX_TALLY_PROGRAM | OAVX_TALLY_RECORDING));
    printf("    [+] oavx_stream_set_tally and oavx_stream_get_tally bidirectional match verified.\n");

    /* 10. Test Native Stream Tally Command (--set via endpoint) */
    printf("[*] 10. Testing Native OpenAVX Stream Tally Control...\n");
    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(stream);
    if (ep && ep->address) {
        snprintf(cmd, sizeof(cmd), "%s tally %s --set pgm --oneshot --json", bin_cli, ep->address);
        rc = run_cmd_capture(cmd, output, sizeof(output));
        assert(rc == 0);
        assert(strstr(output, "\"ok\":true") != NULL);
        assert(strstr(output, "\"flags\":1") != NULL);
        printf("    [+] Native stream tally commanded over TCP metadata socket successfully.\n");
    }

    /* Clean up */
    oavx_stream_close(stream);
    oavx_context_destroy(ctx);

    printf("\n[SUCCESS] All OpenAVX Tally CLI & API verification tests passed (100%%).\n\n");
    return 0;
}
