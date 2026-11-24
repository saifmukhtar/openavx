/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_config.h - JSON configuration parser (openavx.json)
 */

#ifndef AVX_CONFIG_H
#define AVX_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard JSON Schema URI */
#define OAVX_CONFIG_SCHEMA_URI "https://raw.githubusercontent.com/saifmukhtar/openavx/main/schema/v1.0.json"

typedef struct avx_config_t {
    char discovery_servers[512]; /* Registry / discovery servers (e.g. "192.168.1.100:7959,10.0.0.1") */
    char static_peers[512];      /* Static unicast peer endpoints (e.g. "10.0.0.1,10.0.0.2") */
    char channels_publish[256];  /* Publish channels / domains (e.g. "Public,StudioA") */
    char channels_subscribe[256];/* Subscribe channels / domains (e.g. "Public,StudioA") */
    bool multicast_enabled;      /* Multicast transport enabled */
    int  multicast_ttl;          /* Multicast packet TTL (default 1) */
    char multicast_prefix[64];   /* Multicast prefix / CIDR (e.g. "239.255.0.0/16") */
    char multicast_netmask[64];  /* Multicast netmask (e.g. "255.255.0.0") */
    int  max_clients;            /* Max simultaneous client connections per sender (default 32) */
    char bind_ip[64];            /* Explicit IP or NIC interface to bind (e.g. "10.0.0.100" or "eth1") */
    char nic_interface[64];      /* Explicit network interface adapter name (e.g. "eth1", "enp4s0") */
    char transport[32];          /* Transport mode: "tcp" (default), "rudp", or "udp" */
    bool loaded;                 /* True if config was successfully loaded from file */
    char config_path[512];       /* Path to loaded config file */
} avx_config_t;

/* Initialize and parse configuration from disk according to standard search order:
 * 0. $OPENAVX_CONFIG_FILE
 * 1. $OPENAVX_CONFIG_DIR/openavx.json
 * 2. $HOME/.openavx/openavx.json
 * 3. /etc/openavx/openavx.json
 */
OPENAVX_API void avx_config_init(void);

/* Free/reset cached configuration */
OPENAVX_API void avx_config_cleanup(void);

/* Get pointer to cached global configuration */
OPENAVX_API const avx_config_t* avx_config_get(void);

/* Locate configuration file path according to standard precedence.
 * Returns true if file exists and is readable, populating out_path.
 */
OPENAVX_API bool avx_config_find_path(char* out_path, size_t max_len);

/* Pure in-memory JSON parser for openavx.json.
 * Populates out_cfg with parsed fields.
 * Returns true if valid JSON and configuration was parsed.
 */
OPENAVX_API bool avx_config_parse_json(const char* json_str, avx_config_t* out_cfg);

#ifdef __cplusplus
}
#endif

#endif /* AVX_CONFIG_H */
