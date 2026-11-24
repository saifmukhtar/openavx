/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_find.c - Network source discovery (mDNS and discovery server client)
 */

#define _GNU_SOURCE
#include "avx_internal.h"
#include "avx_discovery_server.h"
#include "avx_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(AVX_OS_POSIX)
    #include <ifaddrs.h>
    #include <net/if.h>
#endif

/* Binds UDP socket to mDNS port 5353 with SO_REUSEADDR and joins 224.0.0.251 on all active network interfaces. */
static avx_socket_t avx_mdns_open_listener_socket(void) {
    avx_socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse));
#endif

    avx_socket_set_nonblocking(sock, true);

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(AVX_MDNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        /* Fall back to ephemeral port if 5353 is already held exclusively */
        bind_addr.sin_port = 0;
        if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
            avx_socket_close(sock);
            return AVX_INVALID_SOCKET;
        }
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, AVX_MDNS_MCAST_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

#if defined(AVX_OS_POSIX)
    /* Also join multicast group on all active physical/operational interfaces */
    struct ifaddrs *ifaddr = NULL, *ifa = NULL;
    if (getifaddrs(&ifaddr) == 0) {
        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            struct sockaddr_in *s4 = (struct sockaddr_in*)ifa->ifa_addr;
            struct ip_mreq if_mreq;
            memset(&if_mreq, 0, sizeof(if_mreq));
            inet_pton(AF_INET, AVX_MDNS_MCAST_ADDR, &if_mreq.imr_multiaddr);
            if_mreq.imr_interface = s4->sin_addr;
            setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&if_mreq, sizeof(if_mreq));
        }
        freeifaddrs(ifaddr);
    }
#endif

    uint8_t loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));

    /* Also join on loopback interface explicitly for local processes / test suites */
    struct in_addr loop_addr;
    loop_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct ip_mreq lo_mreq;
    memset(&lo_mreq, 0, sizeof(lo_mreq));
    inet_pton(AF_INET, AVX_MDNS_MCAST_ADDR, &lo_mreq.imr_multiaddr);
    lo_mreq.imr_interface = loop_addr;
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&lo_mreq, sizeof(lo_mreq));

    return sock;
}

/* Transmits RFC 6762 mDNS PTR query for target service string to multicast group 224.0.0.251:5353. */
static void avx_mdns_send_query(avx_socket_t sock, const char* service_str) {
    if (sock == AVX_INVALID_SOCKET) return;

    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(AVX_MDNS_PORT);
    inet_pton(AF_INET, AVX_MDNS_MCAST_ADDR, &mcast_addr.sin_addr);

    uint8_t qpkt[256];
    memset(qpkt, 0, sizeof(qpkt));

    /* Header: ID=0, Flags=0, Questions=1, Answers=0 */
    qpkt[4] = 0x00; qpkt[5] = 0x01;

    const char *svc = (service_str && service_str[0]) ? service_str : OAVX_WIRE_MDNS_SERVICE;
    size_t enc_len = avx_wire_encode_dns_service(svc, &qpkt[12], sizeof(qpkt) - 16);
    if (enc_len == 0) return;
    size_t idx = 12 + enc_len;

    /* QTYPE: PTR (12) */
    qpkt[idx++] = 0x00; qpkt[idx++] = 0x0c;
    /* QCLASS: IN (1) */
    qpkt[idx++] = 0x00; qpkt[idx++] = 0x01;

    sendto(sock, qpkt, idx, 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
}

/* Parses incoming mDNS response packets, extracting PTR, SRV, TXT metadata and updating source cache. */
static void avx_find_drain_mdns(avx_find_ctx_t *slot) {
    if (!slot || slot->mdns_sock < 0) return;

    uint8_t pkt[2048];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (1) {
        memset(&from, 0, sizeof(from));
        from_len = sizeof(from);
        ssize_t n = recvfrom(slot->mdns_sock, pkt, sizeof(pkt), 0,
                             (struct sockaddr*)&from, &from_len);
        if (n <= 12) break;

        uint16_t flags = (uint16_t)((pkt[2] << 8) | pkt[3]);
        if ((flags & 0x8000) == 0) continue; /* Not a response */

        char ip_str[64] = "127.0.0.1";
        if (from.sin_family == AF_INET) {
            inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
        }

        const char *svc = slot->wire_profile.mdns_service ? slot->wire_profile.mdns_service : OAVX_WIRE_MDNS_SERVICE;
        uint8_t s_target_service[128];
        size_t s_target_len = avx_wire_encode_dns_service(svc, s_target_service, sizeof(s_target_service));
        if (s_target_len > 1) s_target_len--; /* Match label prefix without null byte */

        char found_name[256] = {0};
        uint16_t found_port = 0;

        for (ssize_t i = 12; i + (ssize_t)s_target_len + 10 < n; ++i) {
            if (s_target_len > 0 && memcmp(&pkt[i], s_target_service, s_target_len) == 0) {
                ssize_t cur = i + (ssize_t)s_target_len;
                if (pkt[cur] == 0) cur++;
                if (cur + 10 > n) break;

                uint16_t rtype = (uint16_t)((pkt[cur] << 8) | pkt[cur + 1]);
                cur += 2;
                cur += 2; /* skip rclass */
                cur += 4; /* skip ttl */
                if (cur + 2 > n) break;
                uint16_t rdlen = (uint16_t)((pkt[cur] << 8) | pkt[cur + 1]);
                cur += 2;

                if (rtype == 0x000c && cur + rdlen <= n && rdlen > 1) {
                    uint8_t nlen = pkt[cur];
                    if (nlen > 0 && nlen < 64 && (ssize_t)(cur + 1 + nlen) <= n) {
                        memcpy(found_name, &pkt[cur + 1], nlen);
                        found_name[nlen] = '\0';
                    }
                }
            }
        }

        /* Scan for SRV record (Type 0x0021 = 33) */
        for (ssize_t i = 12; i + 10 < n; ++i) {
            if (pkt[i] == 0x00 && pkt[i + 1] == 0x21) {
                ssize_t srv_rdata = i + 2 + 2 + 4 + 2;
                if (srv_rdata + 6 <= n) {
                    uint16_t p = (uint16_t)((pkt[srv_rdata + 4] << 8) | pkt[srv_rdata + 5]);
                    if (p >= 1024) {
                        found_port = p;
                        break;
                    }
                }
            }
        }

        if (found_name[0] != '\0') {
            if (found_port == 0) found_port = slot->wire_profile.base_port ? slot->wire_profile.base_port : AVX_DEFAULT_PORT;

            char url[256];
            snprintf(url, sizeof(url), "%s:%u", ip_str, found_port);

            bool exists = false;
            for (uint32_t s = 0; s < slot->num_sources; ++s) {
                if (strcmp(slot->source_names[s], found_name) == 0 &&
                    strcmp(slot->source_urls[s], url) == 0) {
                    exists = true;
                    break;
                }
            }

            if (!exists && slot->num_sources < 64) {
                uint32_t idx = slot->num_sources++;
                snprintf(slot->source_names[idx], sizeof(slot->source_names[idx]), "%s", found_name);
                snprintf(slot->source_urls[idx], sizeof(slot->source_urls[idx]), "%s", url);
                slot->sources[idx].name = slot->source_names[idx];
                slot->sources[idx].address = slot->source_urls[idx];
                slot->sources[idx].port = found_port;
                slot->sources[idx].group = NULL;
            }
        }
    }
}

OPENAVX_API avx_find_instance_t avx_find_create(const avx_find_config_t* p_create_settings) {
    avx_mutex_lock(&g_avx_state.global_lock);
    if (!g_avx_state.initialized) {
        avx_mutex_unlock(&g_avx_state.global_lock);
        avx_engine_initialize();
        avx_mutex_lock(&g_avx_state.global_lock);
    }

    avx_find_ctx_t* slot = NULL;
    for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
        if (!g_avx_state.find_slots[i].in_use) {
            slot = &g_avx_state.find_slots[i];
            memset(slot, 0, sizeof(*slot));
            slot->in_use = true;
            break;
        }
    }
    avx_mutex_unlock(&g_avx_state.global_lock);

    if (!slot) return NULL;

    avx_mutex_init(&slot->lock);
    slot->show_local_sources = p_create_settings ? p_create_settings->show_local_sources : true;

    const char *group_env = getenv("OPENAVX_GROUPS");
    const avx_config_t *cfg = avx_config_get();

    if (p_create_settings && p_create_settings->groups && p_create_settings->groups[0] != '\0') {
        snprintf(slot->groups, sizeof(slot->groups), "%.255s", p_create_settings->groups);
    } else if (group_env && group_env[0] != '\0') {
        snprintf(slot->groups, sizeof(slot->groups), "%.255s", group_env);
    } else if (cfg && cfg->channels_subscribe[0] != '\0') {
        snprintf(slot->groups, sizeof(slot->groups), "%.255s", cfg->channels_subscribe);
    }

    if (p_create_settings && p_create_settings->extra_ips && p_create_settings->extra_ips[0] != '\0') {
        snprintf(slot->extra_ips, sizeof(slot->extra_ips), "%.255s", p_create_settings->extra_ips);
    } else if (cfg && cfg->static_peers[0] != '\0') {
        snprintf(slot->extra_ips, sizeof(slot->extra_ips), "%.255s", cfg->static_peers);
    }

    const oavx_wire_profile_t *wp = (p_create_settings && p_create_settings->wire_profile)
        ? p_create_settings->wire_profile : avx_wire_get_active_profile();
    slot->wire_profile = *wp;

    slot->mdns_sock = avx_mdns_open_listener_socket();
    if (slot->mdns_sock != AVX_INVALID_SOCKET) {
        avx_mdns_send_query(slot->mdns_sock, slot->wire_profile.mdns_service);
    }

    return (avx_find_instance_t)slot;
}

OPENAVX_API void avx_find_destroy(avx_find_instance_t p_instance) {
    if (!p_instance) return;
    avx_find_ctx_t* slot = (avx_find_ctx_t*)p_instance;

    avx_mutex_lock(&slot->lock);
    if (slot->mdns_sock != AVX_INVALID_SOCKET) {
        avx_socket_close(slot->mdns_sock);
        slot->mdns_sock = AVX_INVALID_SOCKET;
    }
    avx_mutex_unlock(&slot->lock);
    avx_mutex_destroy(&slot->lock);

    avx_mutex_lock(&g_avx_state.global_lock);
    slot->in_use = false;
    avx_mutex_unlock(&g_avx_state.global_lock);
}

/* Refreshes stream registry by draining local mDNS multicast responses and querying central discovery servers. */
static void avx_find_refresh_sources(avx_find_ctx_t *slot) {
    if (!slot) return;

    /* Prevent concurrent discovery queries from blocking concurrent callers */
    if (__sync_lock_test_and_set(&slot->refresh_in_progress, 1) != 0) {
        return;
    }

    /* 1. Snapshot active local senders under global_lock without holding slot->lock
     * (Eliminates ABBA lock order inversion between slot->lock and global_lock) */
    struct {
        char name[256];
        uint16_t port;
        const char *group;
    } local_senders[AVX_MAX_SESSIONS];
    int num_local_senders = 0;

    bool show_local = false;
    char extra_ips_copy[256] = {0};
    avx_mutex_lock(&slot->lock);
    show_local = slot->show_local_sources;
    if (slot->extra_ips[0]) {
        snprintf(extra_ips_copy, sizeof(extra_ips_copy), "%s", slot->extra_ips);
    }
    avx_mutex_unlock(&slot->lock);

    if (show_local) {
        avx_mutex_lock(&g_avx_state.global_lock);
        for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
            avx_send_ctx_t *s = &g_avx_state.send_slots[i];
            if (s->in_use && s->running && s->port > 0 && num_local_senders < AVX_MAX_SESSIONS) {
                snprintf(local_senders[num_local_senders].name, sizeof(local_senders[num_local_senders].name), "%s", s->name);
                local_senders[num_local_senders].port = s->port;
                local_senders[num_local_senders].group = s->groups;
                num_local_senders++;
            }
        }
        avx_mutex_unlock(&g_avx_state.global_lock);
    }

    /* 2. Query Centralized Discovery Server(s) outside of any lock
     * (Prevents blocking network I/O from stalling concurrent finder callers):
     * 1. OPENAVX_DISCOVERY_SERVER environment variable
     * 2. cfg->discovery_servers from openavx.json
     * 3. extra_ips_copy (if provided)
     */
    char rem_names[64][256];
    char rem_urls[64][256];
    uint16_t rem_ports[64];
    int total_rem = 0;

    const char *ds_env = getenv("OPENAVX_DISCOVERY_SERVER");
    const avx_config_t *cfg = avx_config_get();
    const char *ds_targets = (ds_env && ds_env[0] != '\0') ? ds_env :
                             (cfg && cfg->discovery_servers[0] != '\0' ? cfg->discovery_servers : extra_ips_copy);

    if (ds_targets && ds_targets[0] != '\0') {
        char targets_copy[512];
        snprintf(targets_copy, sizeof(targets_copy), "%s", ds_targets);

        char *saveptr = NULL;
        char *token = strtok_r(targets_copy, ",; \t", &saveptr);
        while (token && total_rem < 64) {
            char srv_ip[64] = "127.0.0.1";
            uint16_t srv_port = AVX_DISCOVERY_SERVER_DEFAULT_PORT;
            char *colon = strchr(token, ':');
            if (colon) {
                *colon = '\0';
                srv_port = (uint16_t)atoi(colon + 1);
            }
            snprintf(srv_ip, sizeof(srv_ip), "%s", token);

            char batch_names[32][256];
            char batch_urls[32][256];
            int n_rem = avx_discovery_client_query(srv_ip, srv_port, batch_names, batch_urls, 32);
            for (int i = 0; i < n_rem && total_rem < 64; ++i) {
                snprintf(rem_names[total_rem], sizeof(rem_names[total_rem]), "%.255s", batch_names[i]);
                snprintf(rem_urls[total_rem], sizeof(rem_urls[total_rem]), "%.255s", batch_urls[i]);
                rem_ports[total_rem] = srv_port;
                total_rem++;
            }
            token = strtok_r(NULL, ",; \t", &saveptr);
        }
    }

    /* 3. Acquire slot->lock strictly to merge discovered endpoints in-memory and drain mDNS */
    avx_mutex_lock(&slot->lock);

    /* Merge local senders */
    for (int i = 0; i < num_local_senders; ++i) {
        bool found = false;
        for (uint32_t n = 0; n < slot->num_sources; ++n) {
            if (strcmp(slot->source_names[n], local_senders[i].name) == 0) {
                found = true;
                break;
            }
        }
        if (!found && slot->num_sources < 64) {
            uint32_t idx = slot->num_sources++;
            snprintf(slot->source_names[idx], sizeof(slot->source_names[idx]), "%.255s", local_senders[i].name);
            snprintf(slot->source_urls[idx], sizeof(slot->source_urls[idx]), "127.0.0.1:%u", local_senders[i].port);
            slot->sources[idx].name = slot->source_names[idx];
            slot->sources[idx].address = slot->source_urls[idx];
            slot->sources[idx].port = local_senders[i].port;
            slot->sources[idx].group = local_senders[i].group;
        }
    }

    /* Merge remote centralized sources */
    for (int i = 0; i < total_rem; ++i) {
        bool found = false;
        for (uint32_t n = 0; n < slot->num_sources; ++n) {
            if (strcmp(slot->source_names[n], rem_names[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found && slot->num_sources < 64) {
            uint32_t idx = slot->num_sources++;
            snprintf(slot->source_names[idx], sizeof(slot->source_names[idx]), "%.255s", rem_names[i]);
            snprintf(slot->source_urls[idx], sizeof(slot->source_urls[idx]), "%.255s", rem_urls[i]);
            slot->sources[idx].name = slot->source_names[idx];
            slot->sources[idx].address = slot->source_urls[idx];
            slot->sources[idx].port = rem_ports[i];
            slot->sources[idx].group = NULL;
        }
    }

    /* Drain and parse any incoming mDNS multicast response datagrams (RFC 6762 / UDP 5353) */
    avx_find_drain_mdns(slot);

    avx_mutex_unlock(&slot->lock);
    slot->refresh_in_progress = 0;
}

OPENAVX_API const oavx_endpoint_t* avx_find_get_sources(avx_find_instance_t p_instance, uint32_t* p_no_sources) {
    if (!p_instance || !p_no_sources) return NULL;
    avx_find_ctx_t* slot = (avx_find_ctx_t*)p_instance;

    avx_find_refresh_sources(slot);

    avx_mutex_lock(&slot->lock);
    *p_no_sources = slot->num_sources;
    const oavx_endpoint_t* p_src = slot->num_sources > 0 ? slot->sources : NULL;
    avx_mutex_unlock(&slot->lock);

    return p_src;
}

bool avx_find_wait(avx_find_instance_t p_instance, uint32_t timeout_in_ms) {
    if (!p_instance) return false;
    avx_find_ctx_t* slot = (avx_find_ctx_t*)p_instance;

    uint32_t elapsed = 0;
    while (elapsed < timeout_in_ms) {
        avx_find_refresh_sources(slot);

        avx_mutex_lock(&slot->lock);
        uint32_t count = slot->num_sources;
        avx_socket_t msock = slot->mdns_sock;
        const char *svc = slot->wire_profile.mdns_service;
        avx_mutex_unlock(&slot->lock);

        if (count > 0) return true;

        /* Periodically broadcast mDNS PTR query to wake up any silent LAN sources */
        if (msock != AVX_INVALID_SOCKET && (elapsed % 200) == 0) {
            avx_mdns_send_query(msock, svc);
        }

        avx_sleep_ms(10);
        elapsed += 10;
    }

    avx_find_refresh_sources(slot);
    avx_mutex_lock(&slot->lock);
    bool has_sources = slot->num_sources > 0;
    avx_mutex_unlock(&slot->lock);

    return has_sources;
}

/* Broadcasts gratuitous mDNS announcement records across all local interfaces advertising the sender stream. */
int avx_mdns_broadcast_source_ex(const char* name, uint16_t port, const char* bind_ip) {
    avx_socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == AVX_INVALID_SOCKET) return -1;
    avx_socket_set_nosigpipe(sock);

    uint8_t mcast_loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&mcast_loop, sizeof(mcast_loop));
    uint8_t mcast_ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&mcast_ttl, sizeof(mcast_ttl));

    uint32_t src_ip_net = htonl(0x7F000001);
    if (bind_ip && bind_ip[0] != '\0' && strcmp(bind_ip, "0.0.0.0") != 0) {
        struct in_addr if_addr;
        if (inet_pton(AF_INET, bind_ip, &if_addr) > 0) {
            setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&if_addr, sizeof(if_addr));
            src_ip_net = if_addr.s_addr;
        }
    }

    struct sockaddr_in mcast_addr;
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port = htons(AVX_MDNS_PORT);
    inet_pton(AF_INET, AVX_MDNS_MCAST_ADDR, &mcast_addr.sin_addr);

    const oavx_wire_profile_t *prof = avx_wire_get_active_profile();
    const char *svc = prof->mdns_service ? prof->mdns_service : OAVX_WIRE_MDNS_SERVICE;

    uint8_t pkt[1024];
    memset(pkt, 0, sizeof(pkt));

    /* Header: ID=0, Flags=0x8400 (Response, Authoritative), Questions=0, Answers=1, Auth=0, Addl=1 */
    pkt[2] = 0x84; pkt[3] = 0x00;
    pkt[6] = 0x00; pkt[7] = 0x01; /* 1 Answer (PTR) */
    pkt[10] = 0x00; pkt[11] = 0x01; /* 1 Additional (SRV) */

    size_t idx = 12;
    size_t svc_labels_len = avx_wire_encode_dns_service(svc, &pkt[idx], sizeof(pkt) - idx - 64);
    if (svc_labels_len == 0) {
        avx_socket_close(sock);
        return -1;
    }
    idx += svc_labels_len;

    /* Type: PTR (12), Class: IN (1), TTL: 120s */
    pkt[idx++] = 0x00; pkt[idx++] = 0x0c;
    pkt[idx++] = 0x00; pkt[idx++] = 0x01;
    pkt[idx++] = 0x00; pkt[idx++] = 0x00; pkt[idx++] = 0x00; pkt[idx++] = 0x78;

    /* Data: target domain name: len + Name + svc_labels */
    const char *src_name = (name && name[0]) ? name : "openavx";
    size_t name_len = strlen(src_name);
    if (name_len > 63) name_len = 63;
    size_t rd_len = 1 + name_len + svc_labels_len;
    pkt[idx++] = (uint8_t)(rd_len >> 8);
    pkt[idx++] = (uint8_t)(rd_len & 0xff);

    pkt[idx++] = (uint8_t)name_len;
    memcpy(&pkt[idx], src_name, name_len);
    idx += name_len;

    memcpy(&pkt[idx], &pkt[12], svc_labels_len);
    idx += svc_labels_len;

    /* Additional 1: SRV Record announcing Port */
    /* Name: len + Name + svc_labels */
    pkt[idx++] = (uint8_t)name_len;
    memcpy(&pkt[idx], src_name, name_len);
    idx += name_len;

    memcpy(&pkt[idx], &pkt[12], svc_labels_len);
    idx += svc_labels_len;

    /* Type: SRV (33 / 0x0021), Class: IN (1), TTL: 120s */
    pkt[idx++] = 0x00; pkt[idx++] = 0x21;
    pkt[idx++] = 0x00; pkt[idx++] = 0x01;
    pkt[idx++] = 0x00; pkt[idx++] = 0x00; pkt[idx++] = 0x00; pkt[idx++] = 0x78;

    /* SRV Target: oavx-src.local */
    static const uint8_t s_target_host[] = { 8, 'o', 'a', 'v', 'x', '-', 's', 'r', 'c', 5, 'l', 'o', 'c', 'a', 'l', 0 };
    uint16_t srv_port = port ? port : (prof->base_port ? prof->base_port : AVX_DEFAULT_PORT);
    size_t srv_rd_len = 2 + 2 + 2 + sizeof(s_target_host);
    pkt[idx++] = (uint8_t)(srv_rd_len >> 8);
    pkt[idx++] = (uint8_t)(srv_rd_len & 0xff);

    pkt[idx++] = 0x00; pkt[idx++] = 0x00; /* Priority 0 */
    pkt[idx++] = 0x00; pkt[idx++] = 0x00; /* Weight 0 */
    pkt[idx++] = (uint8_t)(srv_port >> 8);
    pkt[idx++] = (uint8_t)(srv_port & 0xff);

    memcpy(&pkt[idx], s_target_host, sizeof(s_target_host));
    idx += sizeof(s_target_host);

    ssize_t sent = sendto(sock, (const char*)pkt, (int)idx, 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
    if (sent <= 0) {
        /* Fall back to loopback interface if no default multicast route exists (macOS/BSD/CI VMs) */
        struct in_addr lo_if;
        lo_if.s_addr = htonl(INADDR_LOOPBACK);
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&lo_if, sizeof(lo_if));
        sent = sendto(sock, (const char*)pkt, (int)idx, 0, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
    }
    avx_socket_close(sock);

    if (sent > 0) {
        avx_trace_packet("[TX]", "mDNS ANNOUNCE", pkt, idx);
        avx_pcap_write_udp(src_ip_net, 5353, htonl(0xE00000FB), 5353, pkt, idx);
        return 0;
    }
    return -1;
}

int avx_mdns_broadcast_source(const char* name, uint16_t port) {
    return avx_mdns_broadcast_source_ex(name, port, NULL);
}
