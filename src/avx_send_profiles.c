/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_send_profiles.c - Congestion evaluation, link adaptation, and profile metrics
 */

#include "avx_send_internal.h"
#include "avx_video_codec.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#if defined(AVX_OS_POSIX)
#include <sys/ioctl.h>
#if defined(__linux__)
#include <linux/sockios.h>
#endif
#endif

/* Computes client transport congestion level and frame drop cadence from socket queue and RTT metrics. */
uint32_t avx_send_evaluate_client_congestion(
    avx_client_slot_t *client,
    int64_t now_ns,
    size_t wire_bytes,
    float target_fps,
    uint32_t *out_cadence
) {
    if (!client) {
        if (out_cadence) *out_cadence = 1;
        return 0;
    }

    int v_queued = client->video_queued;
    uint32_t cadence = 1;
    uint32_t level = 0;

    if (client->transport == OAVX_TRANSPORT_TCP) {
        int unsent_bytes = 0;
#if defined(AVX_OS_POSIX)
        if (ioctl(client->fd, TIOCOUTQ, &unsent_bytes) != 0) {
            unsent_bytes = 0;
        }
#endif
        if (client->last_rtt_sample_ns == 0 ||
            (now_ns - client->last_rtt_sample_ns) >= 100000000LL) {
            float rtt_sample = 0.0f;
            avx_socket_get_diagnostics(client->fd, &rtt_sample, NULL, NULL, NULL);
            client->last_rtt_ms = rtt_sample;
            client->last_rtt_sample_ns = now_ns;
        }
        level = avx_send_eval_tcp_congestion(
            client, now_ns, wire_bytes, target_fps,
            unsent_bytes, v_queued, client->last_rtt_ms, &cadence);
    } else if (client->transport == OAVX_TRANSPORT_RUDP && client->parent_ctx && client->parent_ctx->rate_ctl) {
        avx_rate_controller_t *rc = client->parent_ctx->rate_ctl;
        avx_wan_congestion_state_t st = avx_rate_controller_get_congestion_state(rc);
        cadence = avx_rate_controller_get_cadence(rc);
        if (st == AVX_WAN_CONG_OVERUSE) {
            level = 2;
            if (cadence < 2) cadence = 2;
        } else if (st == AVX_WAN_CONG_PRESSURE) {
            level = 1;
        } else {
            level = 0;
            cadence = 1;
        }
        /* Async queue backpressure guard */
        if (v_queued >= 2) {
            cadence = 4;
            level = 2;
        } else if (v_queued >= 1 && cadence < 2) {
            cadence = 2;
            level = 1;
        }
    } else {
        if (v_queued >= 2) {
            cadence = 4;
            level = 2;
        } else if (v_queued >= 1) {
            cadence = 2;
            level = 1;
        } else {
            cadence = 1;
            level = 0;
        }
    }

    if (out_cadence) *out_cadence = cadence;
    return level;
}

/* Periodically updates rate controller with RUDP telemetry and adjusts dynamic encoder quality. */
void avx_send_profiles_tick(avx_send_ctx_t *slot, int64_t now_ms) {
    if (!slot || !slot->rate_ctl) return;

    if (slot->rudp_sender) {
        avx_rudp_telemetry_t telem;
        if (avx_udp_rudp_sender_get_telemetry(slot->rudp_sender, &telem)) {
            if (telem.packets_sent > 0) {
                avx_rate_controller_update_telemetry(slot->rate_ctl, &telem, now_ms);
                avx_rate_controller_apply_to_sender(slot->rate_ctl, slot->rudp_sender);
            }
        }
    }

    /* Auto-tune video encoder quality if user has not set a static quality override */
    if (slot->video_quality == 0) {
        uint8_t dynamic_q = avx_rate_controller_get_quality(slot->rate_ctl);
        if (slot->video_encoder) {
            avx_video_encoder_set_quality(slot->video_encoder, dynamic_q);
        }
    }
}

static inline bool avx_is_private_ipv4(uint32_t ip_host_order) {
    uint8_t b1 = (uint8_t)((ip_host_order >> 24) & 0xFF);
    uint8_t b2 = (uint8_t)((ip_host_order >> 16) & 0xFF);

    if (b1 == 127) return true;                           /* Loopback 127.0.0.0/8 */
    if (b1 == 10) return true;                            /* RFC 1918 10.0.0.0/8 */
    if (b1 == 172 && (b2 >= 16 && b2 <= 31)) return true; /* RFC 1918 172.16.0.0/12 */
    if (b1 == 192 && b2 == 168) return true;              /* RFC 1918 192.168.0.0/16 */
    if (b1 == 169 && b2 == 254) return true;              /* RFC 3927 Link-local 169.254.0.0/16 */
    return false;
}

static inline bool avx_is_private_ipv6(const struct in6_addr *addr) {
    if (IN6_IS_ADDR_LOOPBACK(addr)) return true;
    if (IN6_IS_ADDR_LINKLOCAL(addr)) return true;
    if (IN6_IS_ADDR_SITELOCAL(addr)) return true;
    if ((addr->s6_addr[0] & 0xFE) == 0xFC) return true;   /* RFC 4193 ULA fc00::/7 */
    if (IN6_IS_ADDR_V4MAPPED(addr)) {
        uint32_t v4 = ((uint32_t)addr->s6_addr[12] << 24) |
                      ((uint32_t)addr->s6_addr[13] << 16) |
                      ((uint32_t)addr->s6_addr[14] << 8)  |
                      ((uint32_t)addr->s6_addr[15]);
        return avx_is_private_ipv4(v4);
    }
    return false;
}

/* Determines whether peer IP address belongs to RFC 1918 / RFC 4193 private LAN subnets. */
OPENAVX_API bool avx_is_lan_sockaddr(const struct sockaddr_storage *addr) {
    if (!addr) return true;
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in*)addr;
        return avx_is_private_ipv4(ntohl(sin->sin_addr.s_addr));
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6*)addr;
        return avx_is_private_ipv6(&sin6->sin6_addr);
    }
    return true;
}

/* Negotiates highest priority shared video compression codec (AV1 > HEVC > H.264) with receiver. */
OPENAVX_API uint32_t avx_negotiate_best_codec(const char *codecs_str) {
    if (!codecs_str || codecs_str[0] == '\0') {
        if (avx_video_encoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_AV01)) return OAVX_FOURCC_VIDEO_TYPE_AV01;
        if (avx_video_encoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_HEVC)) return OAVX_FOURCC_VIDEO_TYPE_HEVC;
        if (avx_video_encoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_H264)) return OAVX_FOURCC_VIDEO_TYPE_H264;
        return OAVX_FOURCC_VIDEO_TYPE_H264;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", codecs_str);
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",; \t", &saveptr);
    while (tok) {
        if (strcasecmp(tok, "av1") == 0 || strcasecmp(tok, "av01") == 0) {
            if (avx_video_encoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_AV01)) {
                return OAVX_FOURCC_VIDEO_TYPE_AV01;
            }
        } else if (strcasecmp(tok, "hevc") == 0 || strcasecmp(tok, "h265") == 0) {
            if (avx_video_encoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_HEVC)) {
                return OAVX_FOURCC_VIDEO_TYPE_HEVC;
            }
        } else if (strcasecmp(tok, "h264") == 0 || strcasecmp(tok, "avc") == 0) {
            if (avx_video_encoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_H264)) {
                return OAVX_FOURCC_VIDEO_TYPE_H264;
            }
        }
        tok = strtok_r(NULL, ",; \t", &saveptr);
    }

    if (avx_video_encoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_H264)) return OAVX_FOURCC_VIDEO_TYPE_H264;
    return OAVX_FOURCC_VIDEO_TYPE_H264;
}
