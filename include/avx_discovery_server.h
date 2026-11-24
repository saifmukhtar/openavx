/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_discovery_server.h - Centralized discovery server (TCP port 7959)
 */

#ifndef AVX_DISCOVERY_SERVER_H
#define AVX_DISCOVERY_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "avx_platform.h"
#include "openavx.h"

#define AVX_DISCOVERY_SERVER_DEFAULT_PORT 7959
#define AVX_DISCOVERY_SERVER_MAX_SOURCES 256
#define AVX_DISCOVERY_SERVER_MAX_CLIENTS 128

#define AVX_RELAY_SERVER_DEFAULT_PORT 7960
#define AVX_RELAY_MAGIC 0x55585241 /* 'ARXU' - OpenAVX Relay UDP */
#define AVX_STUN_MAGIC  0x55585341 /* 'ASXU' - OpenAVX STUN UDP */

#define AVX_RELAY_CHANNEL_MEDIA   0
#define AVX_RELAY_CHANNEL_CONTROL 1

#define AVX_RELAY_FLAG_DATA       0x00
#define AVX_RELAY_FLAG_BIND       0x01
#define AVX_RELAY_FLAG_ACK        0x02
#define AVX_RELAY_FLAG_HEARTBEAT  0x04
#define AVX_RELAY_FLAG_LEAVE      0x08

#define AVX_RELAY_MAX_SESSIONS 256
#define AVX_RELAY_MAX_SUBSCRIBERS_PER_SESSION 64

#ifdef __cplusplus
extern "C" {
#endif

AVX_PACKED_START
typedef struct {
    uint32_t magic;         /* AVX_RELAY_MAGIC (0x55585241) */
    uint16_t session_id;    /* Session ID (1..65535) */
    uint8_t  channel;       /* AVX_RELAY_CHANNEL_MEDIA (0) or AVX_RELAY_CHANNEL_CONTROL (1) */
    uint8_t  flags;         /* AVX_RELAY_FLAG_* */
} AVX_PACKED avx_relay_header_t;

typedef struct {
    uint32_t magic;         /* AVX_STUN_MAGIC (0x55585341) */
    uint32_t cookie;        /* Transaction/correlation ID */
    uint16_t family;        /* Address family (AF_INET or AF_INET6) */
    uint16_t mapped_port;   /* Reflexive port (network byte order) */
    uint8_t  mapped_ip[16]; /* Reflexive IP (4 bytes for IPv4, 16 for IPv6) */
} AVX_PACKED avx_stun_msg_t;
AVX_PACKED_END

typedef struct {
    char name[256];
    char url[256];
    char groups[256];
    avx_socket_t owner_fd;
    char public_ip[64];
    uint16_t public_port;
} avx_server_source_entry_t;

typedef struct {
    const char *bind_ip;        /* IP to bind (NULL or "0.0.0.0" for dual-stack wildcard) */
    uint16_t    tcp_port;       /* TCP discovery port (default: 7959) */
    uint16_t    relay_udp_port; /* UDP relay & STUN port (default: 7960) */
    bool        enable_relay;   /* Whether to enable UDP relay & STUN reflection (default: true) */
    const char *public_ip;      /* Optional advertised public IP of the relay server */
} avx_discovery_server_config_t;

typedef struct {
    uint32_t active_sessions;
    uint32_t active_subscribers;
    uint64_t packets_relayed;
    uint64_t bytes_relayed;
} avx_relay_stats_t;

typedef struct avx_discovery_server_t avx_discovery_server_t;

/**
 * Creates and starts a centralized discovery server (with UDP relay enabled by default on port 7960).
 */
OPENAVX_API avx_discovery_server_t* avx_discovery_server_create(const char *bind_ip, uint16_t port);

/**
 * Creates and starts a centralized discovery and relay server with custom configuration.
 */
OPENAVX_API avx_discovery_server_t* avx_discovery_server_create_ex(const avx_discovery_server_config_t *cfg);

/**
 * Stops the discovery server and frees resources.
 */
OPENAVX_API void avx_discovery_server_destroy(avx_discovery_server_t *srv);

/**
 * Retrieves current statistics of the UDP relay.
 */
OPENAVX_API bool avx_relay_server_get_stats(avx_discovery_server_t *srv, avx_relay_stats_t *out_stats);

/**
 * Client Helper: Register a sender with a discovery server.
 */
OPENAVX_API int avx_discovery_client_register(const char *server_ip, uint16_t server_port,
                                              const char *name, const char *url, const char *groups);

/**
 * Client Helper: Query active sources from a discovery server.
 */
OPENAVX_API int avx_discovery_client_query(const char *server_ip, uint16_t server_port,
                                           char names[][256], char urls[][256], size_t max_sources);

/**
 * Client Helper: Resolve stream details from discovery server (returns local URL and public reflexive IP:port).
 */
OPENAVX_API int avx_discovery_client_resolve(const char *server_ip, uint16_t server_port,
                                             const char *name, char *out_url, size_t out_url_max,
                                             char *out_wan_addr, size_t out_wan_max);

/**
 * Client Helper: Request NAT hole punching for a stream via discovery signaling.
 */
OPENAVX_API int avx_discovery_client_request_punch(const char *server_ip, uint16_t server_port,
                                                   const char *name, const char *local_candidate,
                                                   char *out_peer_candidate, size_t out_cand_max);

/**
 * Client Helper: STUN-like query using a caller-provided UDP socket to discover its reflexive address.
 */
OPENAVX_API int avx_discovery_client_get_reflexive_addr_sock(avx_socket_t sock,
                                                             const char *server_ip, uint16_t udp_port,
                                                             char *out_ip, uint16_t *out_port);

/**
 * Client Helper: STUN-like query to discover reflexive address (creates temporary socket).
 */
OPENAVX_API int avx_discovery_client_get_reflexive_addr(const char *server_ip, uint16_t udp_port,
                                                        char *out_ip, uint16_t *out_port);

/**
 * Client Helper: Bind a socket to a relay session (as publisher or subscriber).
 */
OPENAVX_API int avx_relay_client_bind_session(avx_socket_t sock, const char *relay_ip, uint16_t relay_port,
                                              uint32_t session_id, bool is_publisher);

/**
 * Client Helper: Send an encapsulated packet through the UDP relay.
 */
OPENAVX_API int avx_relay_send_packet(avx_socket_t sock, const struct sockaddr *relay_addr, avx_socklen_t addr_len,
                                      uint32_t session_id, uint8_t channel,
                                      const void *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AVX_DISCOVERY_SERVER_H */
