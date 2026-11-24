/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_multinic_test.c - Multi-NIC / Network Interface Binding Test Suite
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "openavx.h"
#include "avx_internal.h"
#include "avx_config.h"
#include "avx_discovery_server.h"

#define DS_TEST_PORT 7958

int main(void) {
    printf("\n=======================================================\n");
    printf(" OpenAVX Multi-NIC & Network Interface Binding Test\n");
    printf("=======================================================\n\n");

    /* --------------------------------------------------------------------- */
    /* 1. Interface Resolution & Auto-IP Discovery Helper Unit Tests         */
    /* --------------------------------------------------------------------- */
    printf("[*] 1. Testing Interface Name and IP Resolution Functions...\n");
    char resolved_ip[64] = {0};

    /* 1.1 Direct IPv4 literal */
    bool ok = avx_net_resolve_interface_or_ip("127.0.0.1", resolved_ip, sizeof(resolved_ip));
    assert(ok);
    assert(strcmp(resolved_ip, "127.0.0.1") == 0);
    printf("    [+] Literal '127.0.0.1' validated: %s\n", resolved_ip);

    /* 1.2 Loopback interface name 'lo' */
    memset(resolved_ip, 0, sizeof(resolved_ip));
    ok = avx_net_resolve_interface_or_ip("lo", resolved_ip, sizeof(resolved_ip));
    assert(ok);
    assert(strcmp(resolved_ip, "127.0.0.1") == 0);
    printf("    [+] Interface name 'lo' resolved to: %s\n", resolved_ip);

    /* 1.3 Invalid / non-existent NIC name */
    memset(resolved_ip, 0, sizeof(resolved_ip));
    ok = avx_net_resolve_interface_or_ip("nonexistent_nic_999", resolved_ip, sizeof(resolved_ip));
    assert(!ok);
    printf("    [+] Non-existent NIC 'nonexistent_nic_999' correctly rejected\n");

    /* 1.4 Auto-detect default local active IP */
    memset(resolved_ip, 0, sizeof(resolved_ip));
    ok = avx_net_get_default_ip(resolved_ip, sizeof(resolved_ip));
    assert(ok);
    assert(resolved_ip[0] != '\0');
    printf("    [+] Default local active IP auto-detected: %s\n", resolved_ip);

    /* --------------------------------------------------------------------- */
    /* 2. JSON Configuration File Network Binding Parsing                    */
    /* --------------------------------------------------------------------- */
    printf("\n[*] 2. Testing openavx.json Network Block Parsing...\n");
    const char *test_json =
        "{\n"
        "    \"network\": {\n"
        "        \"bind_ip\": \"192.168.10.55\",\n"
        "        \"interface\": \"eth1\"\n"
        "    }\n"
        "}";
    avx_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    bool parse_ok = avx_config_parse_json(test_json, &cfg);
    assert(parse_ok);
    assert(strcmp(cfg.bind_ip, "192.168.10.55") == 0);
    assert(strcmp(cfg.nic_interface, "eth1") == 0);
    printf("    [+] JSON 'network.bind_ip' parsed: %s\n", cfg.bind_ip);
    printf("    [+] JSON 'network.interface' parsed: %s\n", cfg.nic_interface);

    /* --------------------------------------------------------------------- */
    /* 3. Explicit Binding via API (oavx_stream_config_t::bind_ip)           */
    /* --------------------------------------------------------------------- */
    printf("\n[*] 3. Testing Explicit Binding via API (.bind_ip = \"127.0.0.1\")...\n");
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t send_cfg = {
        .name = "MULTINIC_API_TX",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .bind_ip = "127.0.0.1"
    };
    oavx_stream_t *p_send_api = oavx_stream_open(ctx, &send_cfg);
    assert(p_send_api != NULL);

    const oavx_endpoint_t *ep_api = oavx_stream_get_endpoint(p_send_api);
    assert(ep_api != NULL && ep_api->address != NULL);
    assert(strstr(ep_api->address, "127.0.0.1:") != NULL);
    printf("    [+] Sender URL matches bound IP: %s\n", ep_api->address);

    /* Connect receiver */
    oavx_stream_config_t recv_cfg = {
        .name = "MULTINIC_API_RX",
        .direction = OAVX_DIR_RECEIVE,
        .endpoint = ep_api->address,
        .clock_sync = false
    };
    oavx_stream_t *p_recv_api = oavx_stream_open(ctx, &recv_cfg);
    assert(p_recv_api != NULL);

    usleep(100000); /* 100 ms */
    assert(oavx_stream_get_connection_count(p_send_api, 500) == 1);
    printf("    [+] Receiver connected successfully over bound interface!\n");

    oavx_stream_close(p_recv_api);
    oavx_stream_close(p_send_api);

    /* --------------------------------------------------------------------- */
    /* 4. Interface Name Binding via API (.bind_ip = "lo")                   */
    /* --------------------------------------------------------------------- */
    printf("\n[*] 4. Testing Interface Name Binding via API (.bind_ip = \"lo\")...\n");
    oavx_stream_config_t send_cfg_iface = {
        .name = "MULTINIC_IFACE_TX",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .bind_ip = "lo"
    };
    oavx_stream_t *p_send_iface = oavx_stream_open(ctx, &send_cfg_iface);
    assert(p_send_iface != NULL);

    const oavx_endpoint_t *ep_iface = oavx_stream_get_endpoint(p_send_iface);
    assert(ep_iface != NULL && ep_iface->address != NULL);
    assert(strstr(ep_iface->address, "127.0.0.1:") != NULL);
    printf("    [+] Interface 'lo' resolved and bound: %s\n", ep_iface->address);
    oavx_stream_close(p_send_iface);

    /* --------------------------------------------------------------------- */
    /* 5. Environment Variable Precedence (OPENAVX_BIND_IP)                  */
    /* --------------------------------------------------------------------- */
    printf("\n[*] 5. Testing Environment Variable Precedence (OPENAVX_BIND_IP)...\n");
    setenv("OPENAVX_BIND_IP", "127.0.0.1", 1);

    oavx_stream_config_t send_cfg_env = {
        .name = "MULTINIC_ENV_TX",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .bind_ip = NULL /* Unset in API: should fall back to OPENAVX_BIND_IP */
    };
    oavx_stream_t *p_send_env = oavx_stream_open(ctx, &send_cfg_env);
    assert(p_send_env != NULL);

    const oavx_endpoint_t *ep_env = oavx_stream_get_endpoint(p_send_env);
    assert(ep_env != NULL && ep_env->address != NULL);
    assert(strstr(ep_env->address, "127.0.0.1:") != NULL);
    printf("    [+] Sender URL inherited OPENAVX_BIND_IP: %s\n", ep_env->address);
    oavx_stream_close(p_send_env);
    unsetenv("OPENAVX_BIND_IP");

    /* --------------------------------------------------------------------- */
    /* 6. mDNS Outbound Multicast Egress Binding Test                        */
    /* --------------------------------------------------------------------- */
    printf("\n[*] 6. Testing mDNS Outbound Multicast Egress Binding...\n");
    int mdns_ret = avx_mdns_broadcast_source_ex("MULTINIC_MDNS_CAM", 7965, "127.0.0.1");
    assert(mdns_ret == 0);
    printf("    [+] avx_mdns_broadcast_source_ex() on 127.0.0.1 succeeded!\n");

    /* --------------------------------------------------------------------- */
    /* 7. Discovery Server Registration with Bound Media IP                  */
    /* --------------------------------------------------------------------- */
    printf("\n[*] 7. Testing Centralized Discovery Server with Bound IP...\n");
    avx_discovery_server_t *srv = avx_discovery_server_create("127.0.0.1", DS_TEST_PORT);
    assert(srv != NULL);

    char ds_url[64];
    snprintf(ds_url, sizeof(ds_url), "127.0.0.1:%d", DS_TEST_PORT);
    setenv("OPENAVX_DISCOVERY_SERVER", ds_url, 1);
    setenv("OPENAVX_BIND_IP", "127.0.0.1", 1);

    oavx_stream_config_t ds_send_cfg = {
        .name = "ENTERPRISE_10G_CAM",
        .groups = "studio_lan",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false,
        .bind_ip = "127.0.0.1"
    };
    oavx_stream_t *p_send_ds = oavx_stream_open(ctx, &ds_send_cfg);
    assert(p_send_ds != NULL);

    usleep(100000); /* 100 ms */

    char names[16][256];
    char urls[16][256];
    int count = avx_discovery_client_query("127.0.0.1", DS_TEST_PORT, names, urls, 16);
    assert(count >= 1);

    bool found_source = false;
    for (int i = 0; i < count; ++i) {
        if (strcmp(names[i], "ENTERPRISE_10G_CAM") == 0) {
            printf("    [+] Discovery Server registered source: %s at %s\n", names[i], urls[i]);
            assert(strstr(urls[i], "127.0.0.1:") != NULL);
            found_source = true;
            break;
        }
    }
    assert(found_source);

    oavx_stream_close(p_send_ds);
    avx_discovery_server_destroy(srv);
    unsetenv("OPENAVX_DISCOVERY_SERVER");
    unsetenv("OPENAVX_BIND_IP");

    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_multinic_test\n\n");
    return 0;
}
