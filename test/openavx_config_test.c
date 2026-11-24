/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * openavx_config_test.c - OpenAVX JSON Configuration File (openavx.json) Verification
 */

#include "openavx.h"
#include "avx_config.h"
#include "avx_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

static void test_json_parser_rfc_standards(void) {
    printf("[*] 1. Testing Modern RFC OpenAVX JSON Schema (v1.0)...\n");

    const char* rfc_json =
        "{\n"
        "  \"$schema\": \"https://raw.githubusercontent.com/saifmukhtar/openavx/main/schema/v1.0.json\",\n"
        "  \"version\": \"1.0\",\n"
        "  \"discovery\": {\n"
        "    \"servers\": [\"192.168.1.100:7959\", \"10.0.0.1:7959\"],\n"
        "    \"static_peers\": [\"10.0.0.2\", \"10.0.0.3\"]\n"
        "  },\n"
        "  \"channels\": {\n"
        "    \"publish\": [\"Public\", \"StudioA\"],\n"
        "    \"subscribe\": [\"Public\", \"StudioA\", \"Monitoring\"]\n"
        "  },\n"
        "  \"multicast\": {\n"
        "    \"enabled\": true,\n"
        "    \"ttl\": 2,\n"
        "    \"prefix\": \"239.255.0.0/16\"\n"
        "  }\n"
        "}";

    avx_config_t cfg;
    bool ok = avx_config_parse_json(rfc_json, &cfg);
    assert(ok);
    assert(strcmp(cfg.discovery_servers, "192.168.1.100:7959,10.0.0.1:7959") == 0);
    assert(strcmp(cfg.static_peers, "10.0.0.2,10.0.0.3") == 0);
    assert(strcmp(cfg.channels_publish, "Public,StudioA") == 0);
    assert(strcmp(cfg.channels_subscribe, "Public,StudioA,Monitoring") == 0);
    assert(cfg.multicast_enabled == true);
    assert(cfg.multicast_ttl == 2);
    assert(strcmp(cfg.multicast_prefix, "239.255.0.0/16") == 0);

    printf("    [+] RFC v1.0 JSON schema parsed and validated\n");
}

static void test_json_parser_arrays(void) {
    printf("[*] 2. Testing Array-Style JSON Configurations...\n");

    const char* array_json =
        "{\n"
        "  \"discovery\": {\n"
        "    \"servers\": [\"192.168.1.50\", \"10.20.30.40:7959\"],\n"
        "    \"static_peers\": [\"172.16.0.10\", \"172.16.0.11\"]\n"
        "  },\n"
        "  \"channels\": {\n"
        "    \"publish\": [\"StudioB\", \"Live\"],\n"
        "    \"subscribe\": [\"StudioB\", \"ControlRoom\", \"Engineering\"]\n"
        "  }\n"
        "}";

    avx_config_t cfg;
    bool ok = avx_config_parse_json(array_json, &cfg);
    assert(ok);
    assert(strcmp(cfg.discovery_servers, "192.168.1.50,10.20.30.40:7959") == 0);
    assert(strcmp(cfg.static_peers, "172.16.0.10,172.16.0.11") == 0);
    assert(strcmp(cfg.channels_publish, "StudioB,Live") == 0);
    assert(strcmp(cfg.channels_subscribe, "StudioB,ControlRoom,Engineering") == 0);

    printf("    [+] Array-based configuration successfully normalized!\n");
}

static void test_json_parser_malformed(void) {
    printf("[*] 3. Testing Malformed JSON Resilience...\n");

    avx_config_t cfg;

    /* Truncated JSON */
    bool ok = avx_config_parse_json("{\"openavx\": {\"networks\": {\"disco", &cfg);
    assert(ok);

    /* Empty string */
    ok = avx_config_parse_json("", &cfg);
    assert(ok);

    /* Unclosed quotes */
    ok = avx_config_parse_json("{\"groups\": {\"send\": \"broken", &cfg);
    assert(ok);

    /* Non-JSON random garbage */
    ok = avx_config_parse_json("NOT_JSON_DATA_AT_ALL_$$$%%%", &cfg);
    assert(ok);

    printf("    [+] Parser survived malformed inputs without faults!\n");
}

static void test_config_resolution_and_precedence(void) {
    printf("[*] 4. Testing Config Resolution & Precedence Hierarchy...\n");

    /* Create temporary test config directory */
#ifdef _WIN32
    char tmp_path[MAX_PATH];
    DWORD plen = GetTempPathA(sizeof(tmp_path), tmp_path);
    assert(plen > 0 && plen < sizeof(tmp_path));
    char tmp_dir[MAX_PATH + 64];
    snprintf(tmp_dir, sizeof(tmp_dir), "%sopenavx_cfg_test_XXXXXX", tmp_path);
#else
    char tmp_dir[] = "/tmp/openavx_cfg_test_XXXXXX";
#endif
    char* dname = mkdtemp(tmp_dir);
    assert(dname != NULL);

    char cfg_file[512];
    snprintf(cfg_file, sizeof(cfg_file), "%s/openavx.json", dname);

    FILE* f = fopen(cfg_file, "w");
    assert(f != NULL);
    fprintf(f,
        "{\n"
        "  \"$schema\": \"https://raw.githubusercontent.com/saifmukhtar/openavx/main/schema/v1.0.json\",\n"
        "  \"version\": \"1.0\",\n"
        "  \"discovery\": {\n"
        "    \"servers\": \"127.0.0.1:7959\",\n"
        "    \"static_peers\": \"10.99.88.77\"\n"
        "  },\n"
        "  \"channels\": {\n"
        "    \"publish\": \"JsonSendGroup\",\n"
        "    \"subscribe\": \"JsonRecvGroup\"\n"
        "  }\n"
        "}\n");
    fclose(f);

    /* Set OPENAVX_CONFIG_DIR to our test directory */
    setenv("OPENAVX_CONFIG_DIR", dname, 1);
    unsetenv("OPENAVX_DISCOVERY_SERVER");
    unsetenv("OPENAVX_GROUPS");

    /* Reset and initialize library */
    avx_config_cleanup();
    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    const avx_config_t* cfg = avx_config_get();
    assert(cfg != NULL);
    assert(cfg->loaded == true);
    assert(strcmp(cfg->channels_publish, "JsonSendGroup") == 0);
    assert(strcmp(cfg->channels_subscribe, "JsonRecvGroup") == 0);
    assert(strcmp(cfg->discovery_servers, "127.0.0.1:7959") == 0);
    assert(strcmp(cfg->static_peers, "10.99.88.77") == 0);
    printf("    [+] Config correctly loaded from $OPENAVX_CONFIG_DIR (%s)\n", cfg->config_path);

    /* Test Case A: Sender default group from JSON */
    oavx_stream_config_t send_settings_default = {
        .name = "Test Sender A",
        .groups = NULL,
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send_a = oavx_stream_open(ctx, &send_settings_default);
    assert(p_send_a != NULL);
    const oavx_endpoint_t *ep_a = oavx_stream_get_endpoint(p_send_a);
    assert(ep_a != NULL && ep_a->group != NULL);
    assert(strcmp(ep_a->group, "JsonSendGroup") == 0);
    printf("    [+] Sender inherited groups from JSON: '%s'\n", ep_a->group);
    oavx_stream_close(p_send_a);

    /* Test Case B: Sender explicit API group overrides JSON */
    oavx_stream_config_t send_settings_explicit = {
        .name = "Test Sender B",
        .groups = "ExplicitApiGroup",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = true
    };
    oavx_stream_t *p_send_b = oavx_stream_open(ctx, &send_settings_explicit);
    assert(p_send_b != NULL);
    const oavx_endpoint_t *ep_b = oavx_stream_get_endpoint(p_send_b);
    assert(ep_b != NULL && ep_b->group != NULL);
    assert(strcmp(ep_b->group, "ExplicitApiGroup") == 0);
    printf("    [+] Explicit API argument overrode JSON: '%s'\n", ep_b->group);
    oavx_stream_close(p_send_b);

    /* Test Case C: Environment variable overrides JSON */
    setenv("OPENAVX_GROUPS", "EnvOverrideGroup", 1);
    oavx_stream_t *p_send_c = oavx_stream_open(ctx, &send_settings_default);
    assert(p_send_c != NULL);
    const oavx_endpoint_t *ep_c = oavx_stream_get_endpoint(p_send_c);
    assert(ep_c != NULL && ep_c->group != NULL);
    assert(strcmp(ep_c->group, "EnvOverrideGroup") == 0);
    printf("    [+] Environment variable OPENAVX_GROUPS overrode JSON: '%s'\n", ep_c->group);
    oavx_stream_close(p_send_c);

    /* Test Case D: Finder default groups and extra_ips from JSON */
    unsetenv("OPENAVX_GROUPS");
    oavx_discovery_t *p_find = oavx_discovery_create(ctx, NULL);
    assert(p_find != NULL);
    printf("    [+] Finder inherited receive groups and extra_ips from JSON!\n");
    oavx_discovery_destroy(p_find);

    /* Teardown */
    oavx_context_destroy(ctx);
    unlink(cfg_file);
    rmdir(dname);
    unsetenv("OPENAVX_CONFIG_DIR");
    unsetenv("OPENAVX_GROUPS");
}

static void test_json_schema_v1_layout(void) {
    printf("[*] 5. Testing Full schema/v1.0.json Layout (network/sources/transports)...\n");

    const char* schema_json =
        "{\n"
        "  \"$schema\": \"https://raw.githubusercontent.com/saifmukhtar/openavx/main/schema/v1.0.json\",\n"
        "  \"version\": \"1.0\",\n"
        "  \"network\": {\n"
        "    \"bind_ip\": \"10.10.0.5\",\n"
        "    \"interface\": \"eth1\",\n"
        "    \"discovery_servers\": [\"10.10.0.10:7959\"],\n"
        "    \"peers\": [\"10.10.0.20\"]\n"
        "  },\n"
        "  \"sources\": {\n"
        "    \"groups\": [\"StudioA\"],\n"
        "    \"subscribe\": [\"Control\"],\n"
        "    \"max_clients\": 64\n"
        "  },\n"
        "  \"transports\": {\n"
        "    \"mode\": \"rudp\",\n"
        "    \"multicast\": {\n"
        "      \"enabled\": true,\n"
        "      \"ttl\": 4,\n"
        "      \"prefix\": \"239.255.0.0/16\",\n"
        "      \"netmask\": \"255.255.0.0\"\n"
        "    }\n"
        "  }\n"
        "}";

    avx_config_t cfg;
    bool ok = avx_config_parse_json(schema_json, &cfg);
    assert(ok);
    assert(strcmp(cfg.bind_ip, "10.10.0.5") == 0);
    assert(strcmp(cfg.nic_interface, "eth1") == 0);
    assert(strcmp(cfg.discovery_servers, "10.10.0.10:7959") == 0);
    assert(strcmp(cfg.static_peers, "10.10.0.20") == 0);
    assert(strcmp(cfg.channels_publish, "StudioA") == 0);
    assert(strcmp(cfg.channels_subscribe, "Control") == 0);
    assert(cfg.max_clients == 64);
    assert(strcmp(cfg.transport, "rudp") == 0);
    assert(cfg.multicast_enabled == true);
    assert(cfg.multicast_ttl == 4);
    assert(strcmp(cfg.multicast_prefix, "239.255.0.0/16") == 0);
    assert(strcmp(cfg.multicast_netmask, "255.255.0.0") == 0);

    printf("    [+] Full schema/v1.0.json layout parsed with sources.max_clients = %d\n", cfg.max_clients);
}

int main(void) {
    printf("=== OpenAVX JSON Configuration Tests ===\n\n");

    test_json_parser_rfc_standards();
    test_json_parser_arrays();
    test_json_parser_malformed();
    test_config_resolution_and_precedence();
    test_json_schema_v1_layout();

    printf("\n[PASS] openavx_config_test\n");
    return 0;
}
