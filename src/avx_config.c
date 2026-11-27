/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_config.c - JSON configuration parser (openavx.json)
 */

#include "avx_config.h"
#include "avx_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static avx_mutex_t g_config_lock = AVX_MUTEX_INITIALIZER;
static avx_config_t g_avx_config = {0};

static const char* skip_ws(const char* p) {
    if (!p) return NULL;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

static const char* parse_string(const char* p, char* out, size_t max_len) {
    p = skip_ws(p);
    if (!p || *p != '"') return NULL;
    p++; /* skip opening quote */

    size_t idx = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            if (*p == '"' || *p == '\\' || *p == '/') {
                if (idx + 1 < max_len) out[idx++] = *p;
            } else if (*p == 'n') {
                if (idx + 1 < max_len) out[idx++] = '\n';
            } else if (*p == 'r') {
                if (idx + 1 < max_len) out[idx++] = '\r';
            } else if (*p == 't') {
                if (idx + 1 < max_len) out[idx++] = '\t';
            } else {
                if (idx + 1 < max_len) out[idx++] = *p;
            }
        } else {
            if (idx + 1 < max_len) out[idx++] = *p;
        }
        p++;
    }
    out[idx] = '\0';
    if (*p == '"') {
        p++; /* skip closing quote */
    } else {
        return NULL; /* Malformed / unclosed string */
    }
    return p;
}

static const char* parse_string_or_array(const char* p, char* out, size_t max_len) {
    p = skip_ws(p);
    if (!p) return NULL;

    if (*p == '"') {
        return parse_string(p, out, max_len);
    }

    if (*p == '[') {
        p++; /* skip '[' */
        size_t idx = 0;
        out[0] = '\0';
        while (p && *p && *p != ']') {
            p = skip_ws(p);
            if (!p || !*p || *p == ']') break;
            if (*p == '"') {
                char item[256];
                p = parse_string(p, item, sizeof(item));
                if (!p) break;
                size_t ilen = strlen(item);
                if (idx > 0 && idx + 1 < max_len) {
                    out[idx++] = ',';
                    out[idx] = '\0';
                }
                if (idx + ilen < max_len) {
                    memcpy(out + idx, item, ilen);
                    idx += ilen;
                    out[idx] = '\0';
                }
            } else if (*p == ',') {
                p++;
            } else {
                p++;
            }
        }
        if (p && *p == ']') p++;
        return p;
    }

    return p;
}

static const char* parse_bool(const char* p, bool* out_val) {
    p = skip_ws(p);
    if (!p) return NULL;

    if (strncmp(p, "true", 4) == 0) {
        if (out_val) *out_val = true;
        return p + 4;
    }
    if (strncmp(p, "false", 5) == 0) {
        if (out_val) *out_val = false;
        return p + 5;
    }
    return p;
}

static const char* parse_int(const char* p, int* out_val) {
    p = skip_ws(p);
    if (!p) return NULL;

    char* endptr = NULL;
    long val = strtol(p, &endptr, 10);
    if (endptr && endptr != p) {
        if (out_val) *out_val = (int)val;
        return endptr;
    }
    return p;
}

/* Zero-dependency C99 JSON parser extracting discovery servers, network interfaces, and wire profiles. */
bool avx_config_parse_json(const char* json_str, avx_config_t* out_cfg) {
    if (!json_str || !out_cfg) return false;
    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->multicast_ttl = 1;
    out_cfg->max_clients = 32;
    snprintf(out_cfg->transport, sizeof(out_cfg->transport), "tcp");

    const char* p = json_str;
    char scope_stack[8][64];
    int scope_depth = 0;

    while (p && *p) {
        p = skip_ws(p);
        if (!p || !*p) break;

        if (*p == '{') {
            p++;
            continue;
        } else if (*p == '}') {
            if (scope_depth > 0) scope_depth--;
            p++;
            continue;
        } else if (*p == ',' || *p == ':') {
            p++;
            continue;
        } else if (*p == '"') {
            char key[64] = {0};
            p = parse_string(p, key, sizeof(key));
            if (!p) break;

            p = skip_ws(p);
            if (p && *p == ':') p++;
            p = skip_ws(p);

            if (*p == '{') {
                /* Enter new nested object scope */
                if (scope_depth < 8) {
                    snprintf(scope_stack[scope_depth], sizeof(scope_stack[scope_depth]), "%s", key);
                    scope_depth++;
                }
                p++;
                continue;
            }

            /* Identify current enclosing object */
            const char* current_scope = (scope_depth > 0) ? scope_stack[scope_depth - 1] : "";
            bool in_network = (strcmp(current_scope, "network") == 0);
            bool in_discovery = (strcmp(current_scope, "discovery") == 0);
            bool in_channels_or_sources = (strcmp(current_scope, "channels") == 0 ||
                                           strcmp(current_scope, "sources") == 0);
            bool in_multicast = (strcmp(current_scope, "multicast") == 0);
            bool in_transports = (strcmp(current_scope, "transports") == 0);

            /* Discovery & Peer settings */
            if ((in_discovery || in_network || scope_depth <= 2) &&
                (strcmp(key, "servers") == 0 || strcmp(key, "discovery_servers") == 0)) {
                p = parse_string_or_array(p, out_cfg->discovery_servers, sizeof(out_cfg->discovery_servers));
            } else if ((in_discovery || in_network || scope_depth <= 2) &&
                       (strcmp(key, "static_peers") == 0 || strcmp(key, "peers") == 0)) {
                p = parse_string_or_array(p, out_cfg->static_peers, sizeof(out_cfg->static_peers));
            }
            /* Channel & Stream distribution domains */
            else if ((in_channels_or_sources || scope_depth <= 2) &&
                     (strcmp(key, "publish") == 0 || strcmp(key, "groups") == 0)) {
                p = parse_string_or_array(p, out_cfg->channels_publish, sizeof(out_cfg->channels_publish));
            } else if ((in_channels_or_sources || scope_depth <= 2) &&
                       strcmp(key, "subscribe") == 0) {
                p = parse_string_or_array(p, out_cfg->channels_subscribe, sizeof(out_cfg->channels_subscribe));
            }
            /* Multicast Layer 3 Transport settings */
            else if (in_multicast && strcmp(key, "enabled") == 0) {
                p = parse_bool(p, &out_cfg->multicast_enabled);
            } else if (in_multicast && strcmp(key, "ttl") == 0) {
                p = parse_int(p, &out_cfg->multicast_ttl);
            } else if (in_multicast && strcmp(key, "prefix") == 0) {
                p = parse_string(p, out_cfg->multicast_prefix, sizeof(out_cfg->multicast_prefix));
            } else if (in_multicast && strcmp(key, "netmask") == 0) {
                p = parse_string(p, out_cfg->multicast_netmask, sizeof(out_cfg->multicast_netmask));
            }
            /* Max client fan-out ceiling (top-level or inside sources) */
            else if ((in_channels_or_sources || scope_depth <= 2) &&
                     (strcmp(key, "max_clients") == 0 || strcmp(key, "max_clients_per_stream") == 0)) {
                p = parse_int(p, &out_cfg->max_clients);
            }
            /* Transport mode: "tcp" (default), "rudp", "udp" */
            else if ((in_transports || scope_depth <= 2) &&
                     (strcmp(key, "transport") == 0 || strcmp(key, "mode") == 0)) {
                p = parse_string(p, out_cfg->transport, sizeof(out_cfg->transport));
            }
            /* Network Interface & IP binding */
            else if ((in_network || scope_depth <= 2) &&
                     (strcmp(key, "bind_ip") == 0 || strcmp(key, "bind_address") == 0 || strcmp(key, "ip") == 0)) {
                p = parse_string(p, out_cfg->bind_ip, sizeof(out_cfg->bind_ip));
            } else if ((in_network || scope_depth <= 2) &&
                       (strcmp(key, "interface") == 0 || strcmp(key, "nic") == 0 || strcmp(key, "adapter") == 0)) {
                p = parse_string(p, out_cfg->nic_interface, sizeof(out_cfg->nic_interface));
                if (out_cfg->bind_ip[0] == '\0') {
                    snprintf(out_cfg->bind_ip, sizeof(out_cfg->bind_ip), "%s", out_cfg->nic_interface);
                }
            }
        } else {
            /* Skip unhandled token / character */
            p++;
        }
    }

    return true;
}

static bool check_config_in_dir(const char* dir, char* out_path, size_t max_len) {
    if (!dir || dir[0] == '\0') return false;
    /* Primary modern standard OpenAVX config file */
    snprintf(out_path, max_len, "%s/openavx.json", dir);
    if (access(out_path, R_OK) == 0) return true;
    return false;
}

/* Locates openavx.json by hierarchical lookup in working directory, ~/.openavx/, and /etc/openavx/. */
bool avx_config_find_path(char* out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;

    /* 0. Explicit configuration file path */
    const char* config_file = getenv("OPENAVX_CONFIG_FILE");
    if (config_file && config_file[0] != '\0') {
        if (access(config_file, R_OK) == 0) {
            snprintf(out_path, max_len, "%s", config_file);
            return true;
        }
    }

    /* 1. Check $OPENAVX_CONFIG_DIR */
    const char* config_dir = getenv("OPENAVX_CONFIG_DIR");
    if (check_config_in_dir(config_dir, out_path, max_len)) return true;

    /* 2. Check $HOME/.openavx */
    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
        char user_dir[512];
        snprintf(user_dir, sizeof(user_dir), "%s/.openavx", home);
        if (check_config_in_dir(user_dir, out_path, max_len)) return true;
    }

    /* 3. Check /etc/openavx */
    if (check_config_in_dir("/etc/openavx", out_path, max_len)) return true;

    /* 4. Windows ProgramData fallback */
    const char* prog_data = getenv("PROGRAMDATA");
    if (prog_data && prog_data[0] != '\0') {
        char win_dir[512];
        snprintf(win_dir, sizeof(win_dir), "%s/OpenAVX", prog_data);
        if (check_config_in_dir(win_dir, out_path, max_len)) return true;
    }

    out_path[0] = '\0';
    return false;
}

static void avx_config_init_locked(void) {
    if (g_avx_config.loaded) {
        return;
    }

    char path[512] = {0};
    if (avx_config_find_path(path, sizeof(path))) {
        FILE* f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 65536) {
                char* buf = (char*)malloc((size_t)sz + 1);
                if (buf) {
                    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
                    buf[read_bytes] = '\0';
                    avx_config_parse_json(buf, &g_avx_config);
                    snprintf(g_avx_config.config_path, sizeof(g_avx_config.config_path), "%s", path);
                    g_avx_config.loaded = true;
                    free(buf);
                }
            }
            fclose(f);
        }
    }

    if (!g_avx_config.loaded) {
        snprintf(g_avx_config.transport, sizeof(g_avx_config.transport), "tcp");
        g_avx_config.max_clients = 32;
        g_avx_config.multicast_ttl = 1;
    }

    const char* env_transport = getenv("OPENAVX_TRANSPORT");
    if (env_transport && env_transport[0] != '\0') {
        snprintf(g_avx_config.transport, sizeof(g_avx_config.transport), "%s", env_transport);
    }

    g_avx_config.loaded = true;
}

void avx_config_init(void) {
    avx_mutex_lock(&g_config_lock);
    avx_config_init_locked();
    avx_mutex_unlock(&g_config_lock);
}

void avx_config_cleanup(void) {
    avx_mutex_lock(&g_config_lock);
    memset(&g_avx_config, 0, sizeof(g_avx_config));
    avx_mutex_unlock(&g_config_lock);
}

const avx_config_t* avx_config_get(void) {
    avx_mutex_lock(&g_config_lock);
    if (!g_avx_config.loaded) {
        avx_config_init_locked();
    }
    avx_mutex_unlock(&g_config_lock);
    return &g_avx_config;
}
