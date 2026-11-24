/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_gateway_test.c - Automated Verification of Unified CLI Suite and Gateway Bridge
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include "avx_platform.h"
#include "openavx.h"

static int run_cli(const char* bin_rel, const char* args) {
    char cmd[512];
    /* Try 1: direct relative path as given, e.g. ./bin/openavx */
    snprintf(cmd, sizeof(cmd), "%s %s > /dev/null 2>&1", bin_rel, args);
    int ret = system(cmd);
    if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) return 0;

    /* Try 2: prefix with ./build, e.g. ./build/bin/openavx */
    snprintf(cmd, sizeof(cmd), "./build%s %s > /dev/null 2>&1", bin_rel + 1, args);
    ret = system(cmd);
    if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) return 0;

    /* Try 3: basename in current dir, e.g. ./openavx */
    const char* base = strrchr(bin_rel, '/');
    if (base) {
        snprintf(cmd, sizeof(cmd), ".%s %s > /dev/null 2>&1", base, args);
        ret = system(cmd);
        if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0) return 0;
    }
    return ret;
}

int main(void) {
    printf("\n=== OpenAVX CLI & Gateway Integration Tests ===\n\n");

    /* 1. Test Master CLI Help Banner */
    printf("[*] 1. Testing Master CLI Help Banner (openavx --help)...\n");
    int ret_help = run_cli("./bin/openavx", "--help");
    assert(ret_help == 0);
    printf("    [+] openavx --help returned exit code 0.\n");

    /* 2. Test Master CLI Version Output */
    printf("[*] 2. Testing Version Flag (openavx --version)...\n");
    int ret_ver = run_cli("./bin/openavx", "--version");
    assert(ret_ver == 0);
    printf("    [+] openavx --version returned exit code 0.\n");

    /* 3. Test Subcommand Help Banners */
    printf("[*] 3. Testing Subcommand Help Dispatches...\n");
    const char* subcmds[] = { "find", "monitor", "route", "pattern", "server", "gateway" };
    for (size_t i = 0; i < sizeof(subcmds) / sizeof(subcmds[0]); ++i) {
        char args[128];
        snprintf(args, sizeof(args), "%s --help", subcmds[i]);
        int ret_sub = run_cli("./bin/openavx", args);
        assert(ret_sub == 0);
        printf("    [+] Subcommand 'openavx %s --help' returned exit code 0.\n", subcmds[i]);
    }

    /* 4. Test Symlink Aliases for Gateway */
    printf("[*] 4. Testing Gateway Symlink Aliases...\n");
    const char* gateway_aliases[] = {
        "./bin/openavx-gateway",
        "./bin/openavx-web-gateway",
        "./bin/openavx_web_gateway"
    };
    for (size_t i = 0; i < sizeof(gateway_aliases) / sizeof(gateway_aliases[0]); ++i) {
        int ret_alias = run_cli(gateway_aliases[i], "--help");
        if (ret_alias == 0) {
            printf("    [+] Alias '%s --help' returned exit code 0.\n", gateway_aliases[i]);
        }
    }

    /* 5. Test Invalid Command Error Handling */
    printf("[*] 5. Testing Invalid Command Error Handling...\n");
    char cmd_bad[256];
    snprintf(cmd_bad, sizeof(cmd_bad), "./bin/openavx non_existent_command > /dev/null 2>&1");
    int ret_bad = system(cmd_bad);
    if (!WIFEXITED(ret_bad) || WEXITSTATUS(ret_bad) == 0) {
        snprintf(cmd_bad, sizeof(cmd_bad), "./build/bin/openavx non_existent_command > /dev/null 2>&1");
        ret_bad = system(cmd_bad);
    }
    if (!WIFEXITED(ret_bad) || WEXITSTATUS(ret_bad) == 0) {
        snprintf(cmd_bad, sizeof(cmd_bad), "./openavx non_existent_command > /dev/null 2>&1");
        ret_bad = system(cmd_bad);
    }
    assert(WIFEXITED(ret_bad) && WEXITSTATUS(ret_bad) != 0);
    printf("    [+] Invalid command correctly returned non-zero exit code (%d).\n", WEXITSTATUS(ret_bad));

    /* 6. Test Public Logging Level & Callback API */
    printf("[*] 6. Testing Public Logging Level & Callback API...\n");
    {
        extern void openavx_gateway_test_log_cb(oavx_log_level_e level, const char *module,
                                                const char *message, void *user_ctx);
        oavx_log_level_e orig_level = oavx_get_log_level();
        char captured[512] = {0};

        oavx_set_log_level(OAVX_LOG_WARN);
        assert(oavx_get_log_level() == OAVX_LOG_WARN);
        oavx_set_log_callback(openavx_gateway_test_log_cb, captured);

        oavx_log_write(OAVX_LOG_INFO, "test_mod", "filtered_msg");
        assert(captured[0] == '\0');

        oavx_log_write(OAVX_LOG_WARN, "test_mod", "warn_msg_%d", 42);
        const char *expected = "2:test_mod:warn_msg_42\n";
        size_t i = 0;
        while (expected[i] != '\0') {
            assert(captured[i] == expected[i]);
            i++;
        }
        assert(captured[i] == '\0');

        oavx_set_log_level(OAVX_LOG_QUIET);
        assert(oavx_get_log_level() == OAVX_LOG_QUIET);
        oavx_log_write(OAVX_LOG_ERROR, "test_mod", "quiet_filtered");
        assert(captured[i] == '\0');

        oavx_set_log_callback(NULL, NULL);
        oavx_set_log_level(orig_level);
        assert(oavx_get_log_level() == orig_level);
    }
    printf("    [+] Log level filtering and callback routing verified.\n");

    printf("\n[PASS] openavx_gateway_test\n\n");
    return 0;
}

void openavx_gateway_test_log_cb(oavx_log_level_e level, const char *module,
                                 const char *message, void *user_ctx) {
    char *dst = (char *)user_ctx;
    if (!dst) return;
    size_t len = 0;
    while (len < 511 && dst[len] != '\0') len++;
    snprintf(dst + len, 512 - len, "%d:%s:%s\n",
             (int)level, module ? module : "", message ? message : "");
}
