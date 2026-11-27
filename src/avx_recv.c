/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_recv.c - Stream receiver, session handling, and frame decoding
 */

#include "avx_internal.h"
#include "avx_audio.h"
#include "avx_speedhq.h"
#include "avx_video_codec.h"
#include "avx_rtsp.h"
#include "avx_color.h"
#include "avx_wire.h"
#include "avx_crypto.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

static ssize_t recv_all(avx_socket_t fd, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < len) {
        ssize_t r = recv(fd, p + total, (int)(len - total), 0);
        if (r <= 0) return r;
        total += (size_t)r;
    }
    return (ssize_t)total;
}

static bool avx_parse_url_host_port(const char *url, uint16_t default_port, char *host_out, size_t host_out_size,
                                    uint16_t *port_out) {
    if (!url || url[0] == '\0' || !host_out || host_out_size == 0) return false;
    uint16_t port = default_port;

    /* Strip scheme if present (e.g. openavx://, oavx://, http://) */
    const char *scheme_sep = strstr(url, "://");
    if (scheme_sep) {
        url = scheme_sep + 3;
    }

    /* Check for RFC 3986 Bracketed IPv6: "[2001:db8::1]:7960" or "[::1]:7960" */
    if (url[0] == '[') {
        const char *close_bracket = strchr(url, ']');
        if (close_bracket) {
            int hlen = (int)(close_bracket - (url + 1));
            if (hlen > (int)(host_out_size - 1)) hlen = (int)(host_out_size - 1);
            snprintf(host_out, host_out_size, "%.*s", hlen, url + 1);

            if (*(close_bracket + 1) == ':') {
                port = (uint16_t)atoi(close_bracket + 2);
            }
        } else {
            snprintf(host_out, host_out_size, "%.*s", (int)(host_out_size - 1), url);
        }
    } else {
        /* Check if there is a path or query string */
        const char *slash = strchr(url, '/');
        const char *colon = strchr(url, ':');

        if (colon && (!slash || colon < slash)) {
            int host_len = (int)(colon - url);
            if (host_len > (int)(host_out_size - 1)) host_len = (int)(host_out_size - 1);
            snprintf(host_out, host_out_size, "%.*s", host_len, url);
            port = (uint16_t)atoi(colon + 1);
        } else if (slash) {
            int host_len = (int)(slash - url);
            if (host_len > (int)(host_out_size - 1)) host_len = (int)(host_out_size - 1);
            snprintf(host_out, host_out_size, "%.*s", host_len, url);
        } else {
            snprintf(host_out, host_out_size, "%.*s", (int)(host_out_size - 1), url);
        }
    }

    if (port_out) *port_out = port;
    return true;
}

static void avx_rx_parse_failover_metadata(avx_recv_ctx_t *slot, const char *xml) {
    if (!slot || !xml) return;
    const char *tag = strstr(xml, "openavx_failover");
    if (!tag) tag = strstr(xml, "oavx_failover");
    if (!tag) tag = strstr(xml, "failover");
    if (!tag) return;

    /* Extract name */
    char name_buf[256] = {0};
    const char *p_name = strstr(tag, "name=\"");
    if (p_name) {
        p_name += 6;
        const char *end = strchr(p_name, '\"');
        if (end) {
            size_t len = (size_t)(end - p_name);
            if (len >= sizeof(name_buf)) len = sizeof(name_buf) - 1;
            strncpy(name_buf, p_name, len);
            name_buf[len] = '\0';
        }
    }

    /* Extract address or url */
    char addr_buf[256] = {0};
    const char *p_addr = strstr(tag, "address=\"");
    if (!p_addr) p_addr = strstr(tag, "url=\"");
    if (p_addr) {
        p_addr = strchr(p_addr, '\"');
        if (p_addr) {
            p_addr++;
            const char *end = strchr(p_addr, '\"');
            if (end) {
                size_t len = (size_t)(end - p_addr);
                if (len >= sizeof(addr_buf)) len = sizeof(addr_buf) - 1;
                strncpy(addr_buf, p_addr, len);
                addr_buf[len] = '\0';
            }
        }
    }

    if (addr_buf[0] != '\0' || name_buf[0] != '\0') {
        snprintf(slot->failover_name, sizeof(slot->failover_name), "%s", name_buf);
        snprintf(slot->failover_url, sizeof(slot->failover_url), "%s", addr_buf);
        slot->has_failover = true;
    }
}

static void avx_rx_parse_tally_metadata_locked(avx_recv_ctx_t *slot, const char *xml) {
    if (!slot || !xml) return;
    if (strstr(xml, "tally") != NULL) {
        const char *prog = strstr(xml, "on_program=\"");
        const char *prev = strstr(xml, "on_preview=\"");
        const char *rec = strstr(xml, "on_recording=\"");
        if (!rec) rec = strstr(xml, "recording=\"");
        uint8_t flags = OAVX_TALLY_NONE;
        if (prog && strncmp(prog + 12, "true", 4) == 0) flags |= OAVX_TALLY_PROGRAM;
        if (prev && strncmp(prev + 12, "true", 4) == 0) flags |= OAVX_TALLY_PREVIEW;
        if (rec) {
            const char *val = strchr(rec, '=');
            if (val && (strncmp(val + 1, "\"true\"", 6) == 0 || strncmp(val + 2, "true", 4) == 0)) {
                flags |= OAVX_TALLY_RECORDING;
            }
        }
        slot->tally.flags = flags;
        avx_cond_broadcast(&slot->tally_cond);
    }
}

static bool avx_tcp_probe(const char *host, uint16_t port, uint32_t timeout_ms) {
    if (!host || host[0] == '\0') return false;

    char clean_host[128];
    const char *h = host;
    if (h[0] == '[') {
        h++;
        size_t len = strlen(h);
        if (len > 0 && h[len - 1] == ']') len--;
        if (len >= sizeof(clean_host)) len = sizeof(clean_host) - 1;
        strncpy(clean_host, h, len);
        clean_host[len] = '\0';
        h = clean_host;
    }
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    if (getaddrinfo(h, port_str, &hints, &res) != 0 || !res) return false;

    bool reachable = false;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        avx_socket_t fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == AVX_INVALID_SOCKET) continue;

        avx_socket_set_nonblocking(fd, true);
        int ret = connect(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen);
        if (ret == 0) {
            reachable = true;
            avx_socket_close(fd);
            break;
        }
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK)
#else
        if (errno == EINPROGRESS)
#endif
        {
            avx_pollfd_t pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
            if (avx_poll(&pfd, 1, (int)timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) == 0 && err == 0) {
                    reachable = true;
                }
            }
        }
        avx_socket_close(fd);
        if (reachable) break;
    }
    freeaddrinfo(res);
    return reachable;
}

static bool avx_recv_step_connect_locked(avx_recv_ctx_t *slot, uint32_t step_budget_ms);
static bool avx_recv_try_connect(avx_recv_ctx_t *slot);

static void avx_recv_close_pending_locked(avx_recv_ctx_t *slot) {
    if (!slot) return;
    if (slot->pending_conn_fd != AVX_INVALID_SOCKET) {
        avx_socket_close(slot->pending_conn_fd);
        slot->pending_conn_fd = AVX_INVALID_SOCKET;
    }
    slot->pending_conn_start_ns = 0;
}

static ssize_t recv_all_timeout(avx_socket_t fd, void *buf, size_t len, uint32_t timeout_ms) {
    size_t total = 0;
    char *p = (char *)buf;
    int64_t deadline_ms = avx_time_ms() + (int64_t)timeout_ms;
    while (total < len) {
        int64_t rem = deadline_ms - avx_time_ms();
        if (rem <= 0) return -1;
        avx_pollfd_t pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        int pr = avx_poll(&pfd, 1, (int)rem);
        if (pr <= 0 || !(pfd.revents & POLLIN)) return -1;
        ssize_t r = recv(fd, p + total, (int)(len - total), 0);
        if (r <= 0) return r;
        total += (size_t)r;
    }
    return (ssize_t)total;
}

static uint64_t avx_get_raw_video_min_size(uint32_t fourcc, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > AVX_MAX_VIDEO_WIDTH || h > AVX_MAX_VIDEO_HEIGHT) {
        return 0;
    }
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    switch (fourcc) {
    case OAVX_FOURCC_VIDEO_TYPE_UYVY:
        return pixels * 2ULL;
    case OAVX_FOURCC_VIDEO_TYPE_UYVA:
        return pixels * 3ULL;
    case OAVX_FOURCC_VIDEO_TYPE_RGBA:
    case OAVX_FOURCC_VIDEO_TYPE_RGBX:
    case OAVX_FOURCC_VIDEO_TYPE_BGRA:
    case OAVX_FOURCC_VIDEO_TYPE_BGRX:
        return pixels * 4ULL;
    case OAVX_FOURCC_VIDEO_TYPE_P216:
        return pixels * 4ULL;
    case OAVX_FOURCC_VIDEO_TYPE_PA16:
        return pixels * 6ULL;
    case OAVX_FOURCC_VIDEO_TYPE_NV12: {
        uint64_t uv_h = ((uint64_t)h + 1ULL) / 2ULL;
        return pixels + (uint64_t)w * uv_h;
    }
    case OAVX_FOURCC_VIDEO_TYPE_I420:
    case OAVX_FOURCC_VIDEO_TYPE_YV12: {
        uint64_t w2 = ((uint64_t)w + 1ULL) / 2ULL;
        uint64_t h2 = ((uint64_t)h + 1ULL) / 2ULL;
        return pixels + 2ULL * (w2 * h2);
    }
    default:
        return 0;
    }
}

void avx_setup_raw_video_planes(oavx_video_frame_t *vf, uint8_t *payload, size_t payload_len) {
    if (!vf || !payload) return;
    uint32_t w = vf->width;
    uint32_t h = vf->height;

    vf->planes[0] = payload;
    vf->planes[1] = NULL;
    vf->planes[2] = NULL;
    vf->planes[3] = NULL;
    vf->strides[0] = 0;
    vf->strides[1] = 0;
    vf->strides[2] = 0;
    vf->strides[3] = 0;

    if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
        vf->strides[0] = (size_t)w * 2;
        if (payload_len >= (size_t)w * h * 3) {
            vf->planes[1] = payload + (size_t)w * h * 2;
            vf->strides[1] = (size_t)w;
        }
    } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
        vf->strides[0] = (size_t)w * 2;
        if (payload_len >= (size_t)w * h * 4) {
            vf->planes[1] = payload + (size_t)w * h * 2;
            vf->strides[1] = (size_t)w * 2;
        }
    } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_PA16) {
        vf->strides[0] = (size_t)w * 2;
        if (payload_len >= (size_t)w * h * 6) {
            vf->planes[1] = payload + (size_t)w * h * 2;
            vf->strides[1] = (size_t)w * 2;
            vf->planes[2] = payload + (size_t)w * h * 4;
            vf->strides[2] = (size_t)w * 2;
        }
    } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
        vf->strides[0] = (size_t)w;
        if (payload_len >= (size_t)w * h + (size_t)w * (((size_t)h + 1) / 2)) {
            vf->planes[1] = payload + (size_t)w * h;
            vf->strides[1] = (size_t)w;
        }
    } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420 || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
        uint32_t w2 = (w + 1) / 2;
        uint32_t h2 = (h + 1) / 2;
        vf->strides[0] = (size_t)w;
        if (payload_len >= (size_t)w * h + 2ULL * ((size_t)w2 * h2)) {
            vf->planes[1] = payload + (size_t)w * h;
            vf->strides[1] = (size_t)w2;
            vf->planes[2] = (uint8_t *)vf->planes[1] + (size_t)w2 * h2;
            vf->strides[2] = (size_t)w2;
        }
    } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA ||
               vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
        vf->strides[0] = (size_t)w * 4;
    } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC ||
               vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01 || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_MJPEG ||
               vf->fourcc == 0x47504A4D || vf->fourcc == 0x4A50474D || avx_is_speedhq_fourcc(vf->fourcc)) {
        vf->strides[0] = payload_len;
    } else {
        vf->strides[0] = (h > 0) ? (size_t)(payload_len / h) : ((size_t)w * 2);
    }
}

static void avx_rx_execute_failover(avx_recv_ctx_t *slot, const char *reason) {
    if (!slot || !slot->has_failover || slot->is_on_failover || slot->offline_mode) return;
    if (slot->failover_url[0] == '\0') return;

    avx_trace_packet("[RX]", reason, slot->failover_url, strlen(slot->failover_url));

    /* Tear down dead/stalled connection */
    avx_recv_close_pending_locked(slot);
    if (slot->tcp_conn_fd != AVX_INVALID_SOCKET) {
        shutdown(slot->tcp_conn_fd, AVX_SHUT_RDWR);
        avx_socket_close(slot->tcp_conn_fd);
        slot->tcp_conn_fd = AVX_INVALID_SOCKET;
    }
    slot->connected = false;

    /* Preserve primary details if not already saved */
    if (slot->primary_url[0] == '\0' && slot->target_url[0] != '\0') {
        snprintf(slot->primary_url, sizeof(slot->primary_url), "%s", slot->target_url);
        snprintf(slot->primary_name, sizeof(slot->primary_name), "%s", slot->target_name);
    }

    /* Switch active target to failover */
    snprintf(slot->target_url, sizeof(slot->target_url), "%s", slot->failover_url);
    snprintf(slot->target_name, sizeof(slot->target_name), "%s", slot->failover_name);
    slot->target_source.name = slot->target_name;
    slot->target_source.address = slot->target_url;
    slot->is_on_failover = true;
    slot->primary_online_since_ns = 0;
    slot->last_frame_recv_ns = avx_time_ns();
    slot->last_connect_attempt_ns = 0;

    if (slot->video_decoder) {
        avx_video_decoder_reset(slot->video_decoder);
    }

    /* Immediately attempt connection to backup source */
    avx_recv_try_connect(slot);
}

static void avx_rx_execute_failback(avx_recv_ctx_t *slot) {
    if (!slot || !slot->is_on_failover || slot->primary_url[0] == '\0') return;

    avx_trace_packet("[RX]", "FAILBACK CUTOVER (PRIMARY STABLE)", slot->primary_url, strlen(slot->primary_url));

    /* Disconnect from backup */
    avx_recv_close_pending_locked(slot);
    if (slot->tcp_conn_fd != AVX_INVALID_SOCKET) {
        shutdown(slot->tcp_conn_fd, AVX_SHUT_RDWR);
        avx_socket_close(slot->tcp_conn_fd);
        slot->tcp_conn_fd = AVX_INVALID_SOCKET;
    }
    slot->connected = false;

    /* Restore active target to primary */
    snprintf(slot->target_url, sizeof(slot->target_url), "%s", slot->primary_url);
    snprintf(slot->target_name, sizeof(slot->target_name), "%s", slot->primary_name);
    slot->target_source.name = slot->target_name;
    slot->target_source.address = slot->target_url;
    slot->is_on_failover = false;
    slot->primary_online_since_ns = 0;
    slot->last_frame_recv_ns = avx_time_ns();
    slot->last_connect_attempt_ns = 0;

    if (slot->video_decoder) {
        avx_video_decoder_reset(slot->video_decoder);
    }

    /* Reconnect to primary */
    avx_recv_try_connect(slot);
}

static bool avx_recv_finish_tcp_handshake_locked(avx_recv_ctx_t *slot, avx_socket_t fd, const char *host,
                                                 const char *target_url_snap) {
    /* Unlock slot->lock during bounded XML handshake read so concurrent telemetry never stalls */
    avx_mutex_unlock(&slot->lock);
    uint32_t xml_len = 0;
    bool hdr_ok = (recv_all_timeout(fd, &xml_len, sizeof(xml_len), 1500) == (ssize_t)sizeof(xml_len));
    char *xml_data = NULL;
    if (hdr_ok && xml_len > 0 && xml_len < 65536) {
        xml_data = (char *)malloc(xml_len + 1);
        if (xml_data && recv_all_timeout(fd, xml_data, xml_len, 1500) == (ssize_t)xml_len) {
            xml_data[xml_len] = '\0';
        } else {
            free(xml_data);
            xml_data = NULL;
        }
    }
    avx_mutex_lock(&slot->lock);

    if (!hdr_ok || !slot->in_use || slot->offline_mode || strcmp(slot->target_url, target_url_snap) != 0) {
        free(xml_data);
        avx_socket_close(fd);
        return false;
    }

    if (xml_data) {
        avx_trace_packet("[RX]", "TCP HANDSHAKE", xml_data, xml_len);
        avx_rx_parse_failover_metadata(slot, xml_data);

        /* Re-derive RUDP E2EE key using the sender's advertised stream name */
        const char *sname = strstr(xml_data, "name=\"");
        if (sname && slot->rudp_receiver && slot->passphrase[0] != '\0') {
            sname += 6;
            const char *end_quote = strchr(sname, '"');
            if (end_quote && end_quote > sname) {
                char sender_name[256];
                size_t nlen = (size_t)(end_quote - sname);
                if (nlen >= sizeof(sender_name)) nlen = sizeof(sender_name) - 1;
                memcpy(sender_name, sname, nlen);
                sender_name[nlen] = '\0';
                avx_udp_rudp_receiver_set_passphrase((avx_udp_rudp_receiver_t *)slot->rudp_receiver, slot->passphrase,
                                                     sender_name);
            }
        }

        /* Parse UDP port and transport advertised by sender */
        const char *uport = strstr(xml_data, "udp_port=\"");
        if (uport) {
            int up = atoi(uport + 10);
            if (up > 0 && up <= 65535) {
                memset(&slot->sender_udp_addr, 0, sizeof(slot->sender_udp_addr));
                char clean_host[128];
                const char *h = host;
                if (h && h[0] == '[') {
                    const char *end = strchr(h, ']');
                    if (end) {
                        size_t len = (size_t)(end - (h + 1));
                        if (len < sizeof(clean_host)) {
                            memcpy(clean_host, h + 1, len);
                            clean_host[len] = '\0';
                            h = clean_host;
                        }
                    }
                }

                struct in6_addr in6;
                struct in_addr in4;
                if (h && inet_pton(AF_INET6, h, &in6) == 1) {
                    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&slot->sender_udp_addr;
                    s6->sin6_family = AF_INET6;
                    s6->sin6_port = htons((uint16_t)up);
                    s6->sin6_addr = in6;
                    slot->has_sender_udp = true;
                } else if (h && inet_pton(AF_INET, h, &in4) == 1) {
                    struct sockaddr_in *s4 = (struct sockaddr_in *)&slot->sender_udp_addr;
                    s4->sin_family = AF_INET;
                    s4->sin_port = htons((uint16_t)up);
                    s4->sin_addr = in4;
                    slot->has_sender_udp = true;
                }
            }
        }
        free(xml_data);
    }

    slot->tcp_conn_fd = fd;
    slot->connected = true;
    slot->last_frame_recv_ns = avx_time_ns();

    if (!slot->is_on_failover && slot->target_url[0] != '\0') {
        snprintf(slot->primary_url, sizeof(slot->primary_url), "%s", slot->target_url);
        snprintf(slot->primary_name, sizeof(slot->primary_name), "%s", slot->target_name);
    }

    if (slot->web_control_url[0] == '\0') {
        if (strchr(host, ':')) {
            snprintf(slot->web_control_url, sizeof(slot->web_control_url), "http://[%s]/", host);
        } else {
            snprintf(slot->web_control_url, sizeof(slot->web_control_url), "http://%s/", host);
        }
    }

    /* Send any registered connection metadata */
    for (size_t m = 0; m < slot->num_conn_metadata; ++m) {
        size_t mlen = strlen(slot->conn_metadata[m]);
        AVX_PACKED_START
        struct {
            uint32_t magic;
            int64_t timecode;
            uint32_t payload_len;
        } AVX_PACKED mhdr;
        AVX_PACKED_END
        mhdr.magic = slot->wire_profile.magic_metadata;
        mhdr.timecode = 0;
        mhdr.payload_len = (uint32_t)mlen;
        send(fd, (const char *)&mhdr, sizeof(mhdr), MSG_NOSIGNAL);
        send(fd, slot->conn_metadata[m], mlen, MSG_NOSIGNAL);
    }

    /* Send transport negotiation XML to sender if RUDP or UDP active */
    if ((slot->transport == OAVX_TRANSPORT_RUDP || slot->transport == OAVX_TRANSPORT_UDP) && slot->udp_port > 0) {
        char codecs_str[64] = "";
        bool can_av1 = avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_AV01);
        bool can_hevc = avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_HEVC);
        bool can_h264 = avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_H264);
        snprintf(codecs_str, sizeof(codecs_str), "%s%s%s", can_av1 ? "av1," : "", can_hevc ? "hevc," : "",
                 can_h264 ? "h264," : "");
        size_t c_len = strlen(codecs_str);
        if (c_len > 0 && codecs_str[c_len - 1] == ',') codecs_str[c_len - 1] = '\0';
        if (codecs_str[0] == '\0') snprintf(codecs_str, sizeof(codecs_str), "h264");

        const char *prof = (slot->bandwidth == OAVX_BANDWIDTH_HIGHEST)
                               ? "lan"
                               : ((slot->bandwidth == OAVX_BANDWIDTH_LOWEST) ? "wan" : "auto");
        char resp[256];
        int rlen =
            snprintf(resp, sizeof(resp), "<transport mode=\"%s\" port=\"%u\" codecs=\"%s\" profile=\"%s\"/>\n",
                     (slot->transport == OAVX_TRANSPORT_RUDP) ? "rudp" : "udp", slot->udp_port, codecs_str, prof);
        AVX_PACKED_START
        struct {
            uint32_t magic;
            int64_t timecode;
            uint32_t payload_len;
        } AVX_PACKED mhdr;
        AVX_PACKED_END
        mhdr.magic = slot->wire_profile.magic_metadata;
        mhdr.timecode = 0;
        mhdr.payload_len = (uint32_t)rlen;
        send(fd, (const char *)&mhdr, sizeof(mhdr), MSG_NOSIGNAL);
        send(fd, resp, rlen, MSG_NOSIGNAL);

        /* Initial punch datagram to sender's UDP port */
        if (slot->has_sender_udp && slot->udp_sock != AVX_INVALID_SOCKET) {
            uint32_t ping_magic = slot->wire_profile.magic_udp ? slot->wire_profile.magic_udp : OAVX_UDP_MAGIC;
            socklen_t slen = (slot->sender_udp_addr.ss_family == AF_INET6) ? sizeof(struct sockaddr_in6)
                                                                           : sizeof(struct sockaddr_in);
            sendto(slot->udp_sock, (const char *)&ping_magic, sizeof(ping_magic), 0,
                   (struct sockaddr *)&slot->sender_udp_addr, slen);
        }
    }

    return true;
}

static bool avx_recv_step_connect_locked(avx_recv_ctx_t *slot, uint32_t step_budget_ms) {
    if (!slot || slot->connected) return true;
    if (slot->offline_mode || slot->target_url[0] == '\0') return false;

    char url_snap[256];
    snprintf(url_snap, sizeof(url_snap), "%s", slot->target_url);

    char host[128] = {0};
    uint16_t port = slot->wire_profile.base_port ? slot->wire_profile.base_port : AVX_DEFAULT_PORT;
    if (!avx_parse_url_host_port(url_snap, port, host, sizeof(host), &port)) {
        return false;
    }

    if (slot->pending_conn_fd == AVX_INVALID_SOCKET) {
        int64_t now_ns = avx_time_ns();
        if (slot->last_connect_attempt_ns > 0 && (now_ns - slot->last_connect_attempt_ns) < 500000000LL) {
            return false;
        }

        avx_mutex_unlock(&slot->lock);
        bool connected_now = false;
        avx_socket_t new_fd = avx_socket_start_nonblocking_connect(host, port, &connected_now);
        avx_mutex_lock(&slot->lock);

        if (!slot->in_use || slot->offline_mode || strcmp(slot->target_url, url_snap) != 0) {
            if (new_fd != AVX_INVALID_SOCKET) avx_socket_close(new_fd);
            return false;
        }
        if (new_fd == AVX_INVALID_SOCKET) {
            slot->last_connect_attempt_ns = avx_time_ns();
            return false;
        }
        if (connected_now) {
            return avx_recv_finish_tcp_handshake_locked(slot, new_fd, host, url_snap);
        }
        slot->pending_conn_fd = new_fd;
        slot->pending_conn_start_ns = avx_time_ns();
    }

    avx_socket_t pfd_sock = slot->pending_conn_fd;
    if (pfd_sock == AVX_INVALID_SOCKET) return false;

    avx_pollfd_t pfd = {.fd = pfd_sock, .events = POLLOUT, .revents = 0};
    avx_mutex_unlock(&slot->lock);
    int pret = avx_poll(&pfd, 1, (int)step_budget_ms);
    avx_mutex_lock(&slot->lock);

    if (!slot->in_use || slot->offline_mode || slot->pending_conn_fd != pfd_sock) {
        return false;
    }

    if (pret > 0 && (pfd.revents & POLLOUT) && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        int sock_err = 0;
        socklen_t elen = sizeof(sock_err);
        if (getsockopt(pfd_sock, SOL_SOCKET, SO_ERROR, (char *)&sock_err, &elen) == 0 && sock_err == 0) {
            slot->pending_conn_fd = AVX_INVALID_SOCKET;
            slot->pending_conn_start_ns = 0;
            avx_socket_set_nonblocking(pfd_sock, false);
            avx_socket_set_keepalive(pfd_sock, 2, 1, 3);
            avx_socket_set_tcp_nodelay(pfd_sock, true);
            return avx_recv_finish_tcp_handshake_locked(slot, pfd_sock, host, url_snap);
        }
        /* Connect failed asynchronously (e.g. RST / ECONNREFUSED) */
        avx_recv_close_pending_locked(slot);
        slot->last_connect_attempt_ns = avx_time_ns();
        return false;
    }

    if (pret < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        avx_recv_close_pending_locked(slot);
        slot->last_connect_attempt_ns = avx_time_ns();
        return false;
    }

    /* Still in progress (WAN RTT or kernel SYN retry): preserve pending_conn_fd up to 2.2s */
    if ((avx_time_ns() - slot->pending_conn_start_ns) >= 2200000000LL) {
        avx_recv_close_pending_locked(slot);
        slot->last_connect_attempt_ns = avx_time_ns();
    }
    return false;
}

static bool avx_recv_try_connect(avx_recv_ctx_t *slot) {
    return avx_recv_step_connect_locked(slot, 500);
}

/*
 * Allocates receiver session slot, configures requested color/bandwidth profiles,
 * and binds optional UDP/RUDP listener socket for datagram transport.
 */
OPENAVX_API avx_rx_instance_t avx_rx_create(const avx_rx_config_t *p_create_settings) {
    avx_mutex_lock(&g_avx_state.global_lock);
    if (!g_avx_state.initialized) {
        avx_mutex_unlock(&g_avx_state.global_lock);
        avx_engine_initialize();
        avx_mutex_lock(&g_avx_state.global_lock);
    }

    avx_recv_ctx_t *slot = NULL;
    for (int i = 0; i < AVX_MAX_SESSIONS; ++i) {
        if (!g_avx_state.recv_slots[i].in_use) {
            slot = &g_avx_state.recv_slots[i];
            memset(slot, 0, sizeof(*slot));
            slot->in_use = true;
            break;
        }
    }
    avx_mutex_unlock(&g_avx_state.global_lock);

    if (!slot) return NULL;

    avx_mutex_init(&slot->lock);
    slot->tcp_conn_fd = AVX_INVALID_SOCKET;
    slot->pending_conn_fd = AVX_INVALID_SOCKET;
    slot->pending_conn_start_ns = 0;
    slot->connected = false;
    slot->video_buf = NULL;
    slot->video_buf_size = 0;
    slot->audio_buf = NULL;
    slot->audio_buf_size = 0;
    slot->metadata_buf = NULL;
    slot->metadata_buf_size = 0;
    slot->num_conn_metadata = 0;
    slot->ptz_supported = true;
    slot->recording_supported = true;
    slot->is_recording = false;
    slot->current_recording_filename[0] = '\0';
    slot->current_recording_error[0] = '\0';
    slot->web_control_url[0] = '\0';
    slot->tally.flags = OAVX_TALLY_NONE;
    avx_cond_init(&slot->tally_cond);

    const oavx_wire_profile_t *wp = (p_create_settings && p_create_settings->wire_profile)
                                        ? p_create_settings->wire_profile
                                        : avx_wire_get_active_profile();
    slot->wire_profile = *wp;

    if (p_create_settings) {
        if (p_create_settings->source_to_connect_to.name) {
            snprintf(slot->target_name, sizeof(slot->target_name), "%s", p_create_settings->source_to_connect_to.name);
            snprintf(slot->primary_name, sizeof(slot->primary_name), "%s",
                     p_create_settings->source_to_connect_to.name);
        }
        if (p_create_settings->source_to_connect_to.address) {
            snprintf(slot->target_url, sizeof(slot->target_url), "%s", p_create_settings->source_to_connect_to.address);
            snprintf(slot->primary_url, sizeof(slot->primary_url), "%s",
                     p_create_settings->source_to_connect_to.address);
        }
        slot->target_source.name = slot->target_name;
        slot->target_source.address = slot->target_url;
        slot->color_format = p_create_settings->color_format;
        slot->bandwidth = p_create_settings->bandwidth;
        slot->allow_video_fields = p_create_settings->allow_video_fields;
        if (p_create_settings->name) {
            snprintf(slot->recv_name, sizeof(slot->recv_name), "%s", p_create_settings->name);
        }
    }

    slot->failover_timeout_ms = 500;
    slot->auto_revert = false;
    slot->revert_hold_ms = 3000;
    slot->last_frame_recv_ns = avx_time_ns();
    slot->offline_mode = false;

    slot->transport = p_create_settings ? p_create_settings->transport : OAVX_TRANSPORT_TCP;
    slot->udp_sock = AVX_INVALID_SOCKET;
    slot->udp_port = 0;
    slot->rudp_receiver = NULL;
    slot->has_sender_udp = false;
    slot->passphrase[0] = '\0';
    if (p_create_settings && p_create_settings->passphrase && p_create_settings->passphrase[0] != '\0') {
        snprintf(slot->passphrase, sizeof(slot->passphrase), "%s", p_create_settings->passphrase);
    }

    slot->audio_decoder = NULL;
    slot->audio_decoder_fourcc = 0;
    slot->audio_decoder_channels = 0;
    slot->audio_decoder_rate = 0;

    slot->video_decoder = NULL;
    slot->video_decoder_fourcc = 0;
    slot->video_decoder_width = 0;
    slot->video_decoder_height = 0;

    if (slot->transport == OAVX_TRANSPORT_RUDP || slot->transport == OAVX_TRANSPORT_UDP) {
        const char *rx_bind = NULL;
        if (slot->target_url[0] == '[' ||
            (strchr(slot->target_url, ':') && strchr(strchr(slot->target_url, ':') + 1, ':'))) {
            rx_bind = "::";
        }
        slot->udp_sock = avx_udp_create_socket_bind(rx_bind, 0, true);
        if (slot->udp_sock != AVX_INVALID_SOCKET) {
            struct sockaddr_storage sin;
            avx_socklen_t slen = sizeof(sin);
            if (getsockname(slot->udp_sock, (struct sockaddr *)&sin, &slen) == 0) {
                if (sin.ss_family == AF_INET6) {
                    slot->udp_port = ntohs(((struct sockaddr_in6 *)&sin)->sin6_port);
                } else {
                    slot->udp_port = ntohs(((struct sockaddr_in *)&sin)->sin_port);
                }
            }
            if (slot->transport == OAVX_TRANSPORT_RUDP) {
                slot->rudp_receiver = avx_udp_rudp_receiver_create(slot->udp_sock);
                if (slot->rudp_receiver && p_create_settings && p_create_settings->passphrase) {
                    avx_udp_rudp_receiver_set_passphrase((avx_udp_rudp_receiver_t *)slot->rudp_receiver,
                                                         p_create_settings->passphrase, p_create_settings->name);
                }
            }
        }
    }

    /* Attempt initial connection with bounded timeout (500ms); keeps pending_conn_fd alive across WAN RTT */
    avx_mutex_lock(&slot->lock);
    avx_recv_step_connect_locked(slot, 500);
    avx_mutex_unlock(&slot->lock);

    return (avx_rx_instance_t)slot;
}

OPENAVX_API void avx_rx_destroy(avx_rx_instance_t p_instance) {
    if (!p_instance) return;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;

    /* Signal offline and shutdown sockets BEFORE acquiring slot->lock.
     * This unblocks any concurrent thread blocked inside recv_all() or poll(),
     * eliminating the teardown hang / deadlock. */
    slot->offline_mode = true;
#if defined(AVX_OS_WINDOWS) || defined(_WIN32)
    avx_socket_t conn_fd = slot->tcp_conn_fd;
    if (conn_fd != AVX_INVALID_SOCKET) {
        shutdown(conn_fd, AVX_SHUT_RDWR);
        closesocket(conn_fd);
        slot->tcp_conn_fd = AVX_INVALID_SOCKET;
    }
    avx_socket_t pend_fd = slot->pending_conn_fd;
    if (pend_fd != AVX_INVALID_SOCKET) {
        shutdown(pend_fd, AVX_SHUT_RDWR);
        closesocket(pend_fd);
        slot->pending_conn_fd = AVX_INVALID_SOCKET;
    }
    avx_socket_t udp_fd = slot->udp_sock;
    if (udp_fd != AVX_INVALID_SOCKET) {
        shutdown(udp_fd, AVX_SHUT_RDWR);
        closesocket(udp_fd);
        slot->udp_sock = AVX_INVALID_SOCKET;
    }
#else
    avx_socket_t conn_fd = slot->tcp_conn_fd;
    if (conn_fd != AVX_INVALID_SOCKET) {
        shutdown(conn_fd, AVX_SHUT_RDWR);
    }
    avx_socket_t pend_fd = slot->pending_conn_fd;
    if (pend_fd != AVX_INVALID_SOCKET) {
        shutdown(pend_fd, AVX_SHUT_RDWR);
    }
    avx_socket_t udp_fd = slot->udp_sock;
    if (udp_fd != AVX_INVALID_SOCKET) {
        shutdown(udp_fd, AVX_SHUT_RDWR);
    }
#endif

    /* Wait if a decoding operation is actively running in capture */
    while (slot->decoding_video) {
        avx_sleep_ms(1);
    }

    avx_mutex_lock(&slot->lock);
    avx_recv_close_pending_locked(slot);
    if (slot->tcp_conn_fd != AVX_INVALID_SOCKET) {
        avx_socket_close(slot->tcp_conn_fd);
        slot->tcp_conn_fd = AVX_INVALID_SOCKET;
    }
    slot->connected = false;

    if (slot->video_buf) {
        free(slot->video_buf);
        slot->video_buf = NULL;
        slot->video_buf_size = 0;
    }
    if (slot->decompressed_buf) {
        free(slot->decompressed_buf);
        slot->decompressed_buf = NULL;
        slot->decompressed_buf_size = 0;
    }
    if (slot->converted_buf) {
        free(slot->converted_buf);
        slot->converted_buf = NULL;
        slot->converted_buf_size = 0;
    }
    if (slot->audio_buf) {
        free(slot->audio_buf);
        slot->audio_buf = NULL;
        slot->audio_buf_size = 0;
    }
    if (slot->metadata_buf) {
        free(slot->metadata_buf);
        slot->metadata_buf = NULL;
        slot->metadata_buf_size = 0;
    }
    if (slot->rudp_receiver) {
        avx_udp_rudp_receiver_destroy((avx_udp_rudp_receiver_t *)slot->rudp_receiver);
        slot->rudp_receiver = NULL;
    }
    volatile char *vp = slot->passphrase;
    for (size_t i = 0; i < sizeof(slot->passphrase); ++i)
        vp[i] = 0;
    if (slot->udp_sock != AVX_INVALID_SOCKET) {
        avx_socket_close(slot->udp_sock);
        slot->udp_sock = AVX_INVALID_SOCKET;
    }
    if (slot->audio_decoder) {
        avx_audio_decoder_destroy(slot->audio_decoder);
        slot->audio_decoder = NULL;
    }
    if (slot->video_decoder) {
        avx_video_decoder_destroy(slot->video_decoder);
        slot->video_decoder = NULL;
    }
    avx_mutex_unlock(&slot->lock);
    avx_mutex_destroy(&slot->lock);
    avx_cond_destroy(&slot->tally_cond);

    avx_mutex_lock(&g_avx_state.global_lock);
    slot->in_use = false;
    avx_mutex_unlock(&g_avx_state.global_lock);
}

static avx_video_decoder_t *avx_rx_ensure_video_decoder_locked(avx_recv_ctx_t *slot, uint32_t fourcc, uint32_t width,
                                                               uint32_t height) {
    if (!slot || width == 0 || height == 0 || width > AVX_MAX_VIDEO_WIDTH || height > AVX_MAX_VIDEO_HEIGHT) {
        return NULL;
    }

    if (!slot->video_decoder || slot->video_decoder_fourcc != fourcc || slot->video_decoder_width != width ||
        slot->video_decoder_height != height) {
        if (slot->video_decoder) {
            avx_video_decoder_destroy(slot->video_decoder);
            slot->video_decoder = NULL;
        }
        avx_video_dec_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.fourcc = fourcc;
        cfg.width = width;
        cfg.height = height;
        slot->video_decoder = avx_video_decoder_create(&cfg);
        slot->video_decoder_fourcc = slot->video_decoder ? fourcc : 0;
        slot->video_decoder_width = slot->video_decoder ? width : 0;
        slot->video_decoder_height = slot->video_decoder ? height : 0;
    }

    return slot->video_decoder;
}

static bool avx_rx_is_compressed_video_fourcc(uint32_t fourcc) {
    return avx_is_speedhq_fourcc(fourcc) || fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 || fourcc == 0x34363268 ||
           fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || fourcc == 0x43564568 || fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01 ||
           fourcc == OAVX_FOURCC_VIDEO_TYPE_MJPEG || fourcc == 0x47504A4D || fourcc == 0x4A50474D;
}

static int avx_rx_decode_video_payload(avx_video_decoder_t *vdec, const uint8_t *payload, size_t payload_len,
                                       oavx_video_frame_t *p_video_data) {
    if (!vdec || !payload || payload_len == 0 || !p_video_data) return AVX_VIDEO_CODEC_ERROR;

    oavx_video_frame_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    int rc = avx_video_decoder_decode(vdec, payload, payload_len, &decoded);
    if (rc != AVX_VIDEO_CODEC_OK) {
        return rc;
    }

    p_video_data->fourcc = decoded.fourcc;
    if (decoded.width > 0) p_video_data->width = decoded.width;
    if (decoded.height > 0) p_video_data->height = decoded.height;
    for (int i = 0; i < 4; ++i) {
        p_video_data->planes[i] = decoded.planes[i];
        p_video_data->strides[i] = decoded.strides[i];
    }
    return AVX_VIDEO_CODEC_OK;
}

static oavx_fourcc_video_type_e avx_rx_select_target_fourcc(oavx_fourcc_video_type_e src_fourcc,
                                                            oavx_color_format_e color_fmt) {
    oavx_fourcc_video_type_e target_fourcc = src_fourcc;
    if (color_fmt == OAVX_COLOR_FORMAT_RGBX_RGBA) {
        if (src_fourcc != OAVX_FOURCC_VIDEO_TYPE_RGBA && src_fourcc != OAVX_FOURCC_VIDEO_TYPE_RGBX) {
            target_fourcc = OAVX_FOURCC_VIDEO_TYPE_RGBA;
        }
    } else if (color_fmt == OAVX_COLOR_FORMAT_BGRX_BGRA) {
        if (src_fourcc != OAVX_FOURCC_VIDEO_TYPE_BGRA && src_fourcc != OAVX_FOURCC_VIDEO_TYPE_BGRX) {
            target_fourcc = OAVX_FOURCC_VIDEO_TYPE_BGRA;
        }
    } else if (color_fmt == OAVX_COLOR_FORMAT_UYVY_RGBA) {
        if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
            target_fourcc = OAVX_FOURCC_VIDEO_TYPE_RGBA;
        } else if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX ||
                   src_fourcc == OAVX_FOURCC_VIDEO_TYPE_P216 || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12 ||
                   src_fourcc == OAVX_FOURCC_VIDEO_TYPE_I420 || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            target_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY;
        }
    } else if (color_fmt == OAVX_COLOR_FORMAT_UYVY_BGRA) {
        if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
            target_fourcc = OAVX_FOURCC_VIDEO_TYPE_BGRA;
        } else if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX ||
                   src_fourcc == OAVX_FOURCC_VIDEO_TYPE_P216 || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12 ||
                   src_fourcc == OAVX_FOURCC_VIDEO_TYPE_I420 || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            target_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY;
        }
    } else if (color_fmt == OAVX_COLOR_FORMAT_P216) {
        target_fourcc = OAVX_FOURCC_VIDEO_TYPE_P216;
    }
    return target_fourcc;
}

static bool avx_rx_ensure_converted_buf(avx_recv_ctx_t *slot, size_t needed) {
    if (slot->converted_buf_size < needed) {
        uint8_t *nb = (uint8_t *)realloc(slot->converted_buf, needed);
        if (!nb) return false;
        slot->converted_buf = nb;
        slot->converted_buf_size = needed;
    }
    return slot->converted_buf && slot->converted_buf_size >= needed;
}

/*
 * Converts native decoded pixel buffer to target requested color format
 * using SIMD (e.g. UYVY to RGBA/BGRA/P216, NV12/I420 planar to UYVY).
 */
static void avx_rx_convert_video_format(avx_recv_ctx_t *slot, oavx_color_format_e color_fmt, oavx_video_frame_t *vf) {
    if (!slot || !vf || !vf->planes[0] || color_fmt == OAVX_COLOR_FORMAT_FASTEST ||
        color_fmt == OAVX_COLOR_FORMAT_COMPRESSED_V1) {
        return;
    }

    oavx_fourcc_video_type_e target_fourcc = avx_rx_select_target_fourcc(vf->fourcc, color_fmt);
    if (target_fourcc == vf->fourcc) return;

    int w = (int)vf->width;
    int h = (int)vf->height;
    if (w <= 0 || h <= 0 || w > AVX_MAX_VIDEO_WIDTH || h > AVX_MAX_VIDEO_HEIGHT) return;

    size_t bpp = (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) ? 2 : 4;
    uint64_t needed_u64 = (uint64_t)w * (uint64_t)h * (uint64_t)bpp;
    if (needed_u64 > AVX_MAX_DECOMPRESSED_FRAME_SIZE) return;
    size_t needed = (size_t)needed_u64;
    if (!avx_rx_ensure_converted_buf(slot, needed)) return;

    if (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA) {
        if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
            const uint8_t *alpha_src = vf->planes[1] ? vf->planes[1] : (vf->planes[0] + (size_t)w * h * 2);
            avx_color_uyva_to_rgba(vf->planes[0], alpha_src, slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
            avx_color_uyvy_to_rgba(vf->planes[0], slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
            avx_color_rgba_to_bgra(vf->planes[0], slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
            avx_color_p216_to_rgba(vf->planes[0], slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
            avx_color_nv12_to_rgba(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], slot->converted_buf,
                                   (size_t)w * 4, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420) {
            avx_color_i420_to_rgba(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], vf->planes[2],
                                   vf->strides[2], slot->converted_buf, (size_t)w * 4, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            avx_color_yv12_to_rgba(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], vf->planes[2],
                                   vf->strides[2], slot->converted_buf, (size_t)w * 4, w, h);
        } else {
            return;
        }
    } else if (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA) {
        if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
            const uint8_t *alpha_src = vf->planes[1] ? vf->planes[1] : (vf->planes[0] + (size_t)w * h * 2);
            avx_color_uyva_to_bgra(vf->planes[0], alpha_src, slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
            avx_color_uyvy_to_bgra(vf->planes[0], slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX) {
            avx_color_rgba_to_bgra(vf->planes[0], slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
            avx_color_p216_to_rgba(vf->planes[0], slot->converted_buf, w, h);
            avx_color_rgba_to_bgra(slot->converted_buf, slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
            avx_color_nv12_to_bgra(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], slot->converted_buf,
                                   (size_t)w * 4, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420) {
            avx_color_i420_to_rgba(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], vf->planes[2],
                                   vf->strides[2], slot->converted_buf, (size_t)w * 4, w, h);
            avx_color_rgba_to_bgra(slot->converted_buf, slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            avx_color_yv12_to_rgba(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], vf->planes[2],
                                   vf->strides[2], slot->converted_buf, (size_t)w * 4, w, h);
            avx_color_rgba_to_bgra(slot->converted_buf, slot->converted_buf, w, h);
        } else {
            return;
        }
    } else if (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
        if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX) {
            avx_color_rgba_to_uyvy(vf->planes[0], slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA || vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
            avx_color_bgra_to_uyvy(vf->planes[0], slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
            avx_color_p216_to_uyvy(vf->planes[0], slot->converted_buf, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
            avx_color_nv12_to_uyvy(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], slot->converted_buf,
                                   (size_t)w * 2, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420) {
            avx_color_i420_to_uyvy(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], vf->planes[2],
                                   vf->strides[2], slot->converted_buf, (size_t)w * 2, w, h);
        } else if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
            avx_color_yv12_to_uyvy(vf->planes[0], vf->strides[0], vf->planes[1], vf->strides[1], vf->planes[2],
                                   vf->strides[2], slot->converted_buf, (size_t)w * 2, w, h);
        } else {
            return;
        }
    } else if (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
        if (vf->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
            avx_color_uyvy_to_p216(vf->planes[0], slot->converted_buf, w, h);
        } else {
            return;
        }
        vf->planes[0] = slot->converted_buf;
        vf->planes[1] = slot->converted_buf + (size_t)w * h * 2ULL;
        vf->planes[2] = NULL;
        vf->planes[3] = NULL;
        vf->fourcc = target_fourcc;
        vf->strides[0] = (size_t)w * 2ULL;
        vf->strides[1] = (size_t)w * 2ULL;
        vf->strides[2] = 0;
        vf->strides[3] = 0;
        return;
    } else {
        return;
    }

    vf->planes[0] = slot->converted_buf;
    vf->planes[1] = NULL;
    vf->planes[2] = NULL;
    vf->planes[3] = NULL;
    vf->fourcc = target_fourcc;
    vf->strides[0] = (size_t)w * bpp;
    vf->strides[1] = 0;
    vf->strides[2] = 0;
    vf->strides[3] = 0;
}

static oavx_frame_type_t avx_rx_deliver_video_frame(avx_recv_ctx_t *slot, avx_video_decoder_t *vdec,
                                                    oavx_color_format_e color_fmt, bool is_compressed,
                                                    const char *metadata_str, uint32_t fourcc, uint32_t width,
                                                    uint32_t height, int32_t fps_num, int32_t fps_den, int64_t timecode,
                                                    uint8_t *payload, size_t payload_len,
                                                    oavx_video_frame_t *p_video_data) {
    if (!p_video_data) {
        return OAVX_FRAME_TYPE_VIDEO;
    }

    memset(p_video_data, 0, sizeof(*p_video_data));
    p_video_data->width = width;
    p_video_data->height = height;
    p_video_data->framerate.num = (uint32_t)fps_num;
    p_video_data->framerate.den = (uint32_t)fps_den;
    p_video_data->aspect_ratio.num = width;
    p_video_data->aspect_ratio.den = height > 0 ? height : 1;
    p_video_data->scan_type = OAVX_SCAN_PROGRESSIVE;
    p_video_data->pts_ns = timecode;
    p_video_data->dts_ns = timecode;
    p_video_data->metadata = metadata_str;
    p_video_data->metadata_len = metadata_str ? strlen(metadata_str) : 0;
    p_video_data->user_ctx = NULL;

    int dec_ok = AVX_VIDEO_CODEC_ERROR;
    if (is_compressed) {
        if (color_fmt == OAVX_COLOR_FORMAT_FASTEST || color_fmt == OAVX_COLOR_FORMAT_COMPRESSED_V1) {
            p_video_data->fourcc = (oavx_fourcc_video_type_e)fourcc;
            p_video_data->planes[0] = payload;
            p_video_data->strides[0] = payload_len;
        } else {
            dec_ok = avx_rx_decode_video_payload(vdec, payload, payload_len, p_video_data);
            if (dec_ok == AVX_VIDEO_CODEC_NEED_MORE_DATA) {
                return OAVX_FRAME_TYPE_NONE;
            }
            if (dec_ok != AVX_VIDEO_CODEC_OK) {
                /* Decoding unavailable or failed; deliver compressed frame */
                p_video_data->fourcc = (oavx_fourcc_video_type_e)fourcc;
                p_video_data->planes[0] = payload;
                p_video_data->strides[0] = payload_len;
            }
        }
    } else {
        p_video_data->fourcc = (oavx_fourcc_video_type_e)fourcc;
        avx_setup_raw_video_planes(p_video_data, payload, payload_len);
    }

    if (!is_compressed || dec_ok == AVX_VIDEO_CODEC_OK) {
        avx_rx_convert_video_format(slot, color_fmt, p_video_data);
    }
    return OAVX_FRAME_TYPE_VIDEO;
}

static oavx_frame_type_t avx_rx_process_compressed_audio(avx_recv_ctx_t *slot, uint32_t codec_fourcc,
                                                         int32_t sample_rate, int32_t channels, int32_t samples,
                                                         int64_t timecode, uint32_t payload_len,
                                                         const uint8_t *payload_data,
                                                         oavx_audio_frame_t *p_audio_data) {
    if (channels <= 0 || channels > OAVX_MAX_AUDIO_CHANNELS || samples < 0 || samples > 192000 || payload_len == 0 ||
        payload_len > 16 * 1024 * 1024 || sample_rate <= 0 || sample_rate > 192000) {
        return OAVX_FRAME_TYPE_ERROR;
    }

    if (!slot->audio_decoder || slot->audio_decoder_fourcc != codec_fourcc ||
        slot->audio_decoder_channels != channels || slot->audio_decoder_rate != sample_rate) {
        if (slot->audio_decoder) {
            avx_audio_decoder_destroy(slot->audio_decoder);
            slot->audio_decoder = NULL;
        }
        slot->audio_decoder = avx_audio_decoder_create(sample_rate, channels, codec_fourcc);
        slot->audio_decoder_fourcc = codec_fourcc;
        slot->audio_decoder_channels = channels;
        slot->audio_decoder_rate = sample_rate;
    }

    if (!slot->audio_decoder) {
        return OAVX_FRAME_TYPE_ERROR;
    }

    int max_dec_samples = samples > 0 ? samples : 4096;
    if (max_dec_samples < 960) max_dec_samples = 960;
    size_t needed_buf = (size_t)channels * (size_t)max_dec_samples * sizeof(float);
    if (slot->audio_buf_size < needed_buf) {
        float *new_buf = (float *)realloc(slot->audio_buf, needed_buf);
        if (!new_buf) return OAVX_FRAME_TYPE_ERROR;
        slot->audio_buf = new_buf;
        slot->audio_buf_size = needed_buf;
    }

    float *dec_planes[OAVX_MAX_AUDIO_CHANNELS];
    for (int c = 0; c < channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
        dec_planes[c] = slot->audio_buf + (size_t)c * (size_t)max_dec_samples;
    }

    int decoded_frames = avx_audio_decoder_decode(slot->audio_decoder, codec_fourcc, payload_data, (size_t)payload_len,
                                                  dec_planes, max_dec_samples);
    if (decoded_frames <= 0) {
        return OAVX_FRAME_TYPE_ERROR;
    }

    if (p_audio_data) {
        memset(p_audio_data, 0, sizeof(*p_audio_data));
        p_audio_data->sample_rate = (uint32_t)sample_rate;
        p_audio_data->channels = (uint16_t)channels;
        p_audio_data->frames = (uint32_t)decoded_frames;
        p_audio_data->format = OAVX_AUDIO_FMT_FLT_PLANAR;
        p_audio_data->stride_bytes = (size_t)decoded_frames * sizeof(float);
        p_audio_data->pts_ns = timecode;
        p_audio_data->channel_mask = avx_audio_layout_to_mask(avx_audio_layout_from_channels((uint16_t)channels));
        p_audio_data->metadata = NULL;
        p_audio_data->metadata_len = 0;
        p_audio_data->user_ctx = NULL;
        for (int c = 0; c < channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
            p_audio_data->planes[c] = dec_planes[c];
        }
        for (int c = channels; c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
            p_audio_data->planes[c] = NULL;
        }
    }

    slot->perf.audio_frames++;
    return OAVX_FRAME_TYPE_AUDIO;
}

static void avx_recv_update_video_telemetry_locked(avx_recv_ctx_t *slot, uint32_t fourcc, uint32_t width,
                                                   uint32_t height, int32_t fps_n, int32_t fps_d, size_t wire_bytes,
                                                   int64_t now_ns) {
    slot->telem.total_bytes += wire_bytes;
    slot->telem.window_bytes += wire_bytes;
    slot->telem.window_frames++;
    slot->telem.last_fourcc = fourcc;
    slot->telem.last_width = width;
    slot->telem.last_height = height;

    if (slot->telem.last_frame_arrival_ns > 0) {
        int64_t dt_ns = now_ns - slot->telem.last_frame_arrival_ns;
        int64_t nominal_ns = (fps_n > 0 && fps_d > 0) ? ((int64_t)fps_d * 1000000000LL) / fps_n : 16666666LL;
        float diff_ms = fabsf((float)(dt_ns - nominal_ns) / 1000000.0f);
        if (slot->telem.inter_frame_jitter_ms == 0.0f) {
            slot->telem.inter_frame_jitter_ms = diff_ms;
        } else {
            slot->telem.inter_frame_jitter_ms = 0.85f * slot->telem.inter_frame_jitter_ms + 0.15f * diff_ms;
        }
    }
    slot->telem.last_frame_arrival_ns = now_ns;
    slot->last_frame_recv_ns = now_ns;

    if (slot->telem.last_window_time_ns == 0) {
        slot->telem.last_window_time_ns = now_ns;
    } else {
        int64_t elapsed_ns = now_ns - slot->telem.last_window_time_ns;
        if (elapsed_ns >= 500000000LL) { /* 500 ms sliding window */
            float elapsed_sec = (float)elapsed_ns / 1000000000.0f;
            slot->telem.instant_fps = (float)slot->telem.window_frames / elapsed_sec;
            slot->telem.instant_mbps = ((float)slot->telem.window_bytes * 8.0f) / (elapsed_sec * 1000000.0f);
            slot->telem.window_bytes = 0;
            slot->telem.window_frames = 0;
            slot->telem.last_window_time_ns = now_ns;
        }
    }
}

/*
 * Polls active stream for next media frame up to timeout_in_ms. Handles TCP
 * stream parsing, RUDP datagram reassembly, stall failover, and auto-revert.
 * Returned buffer memory is owned by receiver and valid until next receive call.
 */
OPENAVX_API oavx_frame_type_t avx_rx_receive(avx_rx_instance_t p_instance, oavx_video_frame_t *p_video_data,
                                             oavx_audio_frame_t *p_audio_data, oavx_metadata_frame_t *metadata,
                                             uint32_t timeout_in_ms) {
    if (!p_instance) return OAVX_FRAME_TYPE_ERROR;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;

    avx_mutex_lock(&slot->lock);
    int64_t now_ns = avx_time_ns();

    /* 1. Check Debounced Auto-Revert if on failover */
    if (slot->auto_revert && slot->is_on_failover && slot->primary_url[0] != '\0' && !slot->offline_mode) {
        if (slot->last_probe_time_ns == 0 || (now_ns - slot->last_probe_time_ns) >= 1000000000LL) {
            slot->last_probe_time_ns = now_ns;
            char p_host[128] = {0};
            uint16_t p_port = slot->wire_profile.base_port ? slot->wire_profile.base_port : AVX_DEFAULT_PORT;
            if (avx_parse_url_host_port(slot->primary_url, p_port, p_host, sizeof(p_host), &p_port)) {
                if (avx_tcp_probe(p_host, p_port, 20)) {
                    if (slot->primary_online_since_ns == 0) {
                        slot->primary_online_since_ns = now_ns;
                    } else if ((now_ns - slot->primary_online_since_ns) >= (int64_t)slot->revert_hold_ms * 1000000LL) {
                        avx_rx_execute_failback(slot);
                    }
                } else {
                    slot->primary_online_since_ns = 0;
                }
            }
        }
    }

    /* 2. Check Stream Stall on Connected Stream */
    if (slot->connected) {
        uint32_t stall_threshold_ms = slot->failover_timeout_ms ? slot->failover_timeout_ms : 500;
        if (slot->has_failover && !slot->is_on_failover && slot->last_frame_recv_ns != 0 &&
            (now_ns - slot->last_frame_recv_ns) > (int64_t)stall_threshold_ms * 1000000LL) {
            avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (STREAM STALL)");
        }
    }

    /* 3. Reconnection / Initial Connection */
    if (!slot->connected) {
        bool had_pending = (slot->pending_conn_fd != AVX_INVALID_SOCKET);
        uint32_t step_budget = (timeout_in_ms > 0) ? timeout_in_ms : 25;
        if (step_budget > 500) step_budget = 500;

        if (!avx_recv_step_connect_locked(slot, step_budget)) {
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0' &&
                slot->pending_conn_fd == AVX_INVALID_SOCKET) {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (PRIMARY UNREACHABLE)");
            }
        }
        if (!slot->connected) {
            bool has_pending_now = (slot->pending_conn_fd != AVX_INVALID_SOCKET);
            avx_mutex_unlock(&slot->lock);
            /* Only sleep if cooldown prevented polling a socket this iteration */
            if (!had_pending && !has_pending_now && timeout_in_ms > 0) {
                avx_sleep_ms(timeout_in_ms);
            }
            return OAVX_FRAME_TYPE_NONE;
        }
    }

    /* 3b. Datagram / RUDP Reception Path */
    if ((slot->transport == OAVX_TRANSPORT_RUDP || slot->transport == OAVX_TRANSPORT_UDP) && slot->has_sender_udp) {
        /* Check TCP for control / metadata packets non-blockingly */
        if (slot->tcp_conn_fd != AVX_INVALID_SOCKET) {
            avx_pollfd_t cpfd = {.fd = slot->tcp_conn_fd, .events = POLLIN, .revents = 0};
            if (avx_poll(&cpfd, 1, 0) > 0 && (cpfd.revents & POLLIN)) {
                uint32_t cmagic = 0;
#ifdef MSG_DONTWAIT
                const int recv_flags = MSG_DONTWAIT;
#else
                const int recv_flags = 0;
#endif
                if (recv(slot->tcp_conn_fd, (char *)&cmagic, sizeof(cmagic), recv_flags) == (ssize_t)sizeof(cmagic)) {
                    if (cmagic == slot->wire_profile.magic_metadata ||
                        (slot->wire_profile.compat_magic_metadata &&
                         cmagic == slot->wire_profile.compat_magic_metadata)) {
                        AVX_PACKED_START
                        struct {
                            int64_t timecode;
                            uint32_t payload_len;
                        } AVX_PACKED mhdr;
                        AVX_PACKED_END
                        if (recv_all(slot->tcp_conn_fd, &mhdr, sizeof(mhdr)) == (ssize_t)sizeof(mhdr)) {
                            if (mhdr.payload_len > 0 && mhdr.payload_len < 65536) {
                                if (slot->metadata_buf_size < mhdr.payload_len + 1) {
                                    char *nb = (char *)realloc(slot->metadata_buf, mhdr.payload_len + 1);
                                    if (nb) {
                                        slot->metadata_buf = nb;
                                        slot->metadata_buf_size = mhdr.payload_len + 1;
                                    }
                                }
                                if (slot->metadata_buf && recv_all(slot->tcp_conn_fd, slot->metadata_buf,
                                                                   mhdr.payload_len) == (ssize_t)mhdr.payload_len) {
                                    slot->metadata_buf[mhdr.payload_len] = '\0';
                                    avx_rx_parse_tally_metadata_locked(slot, slot->metadata_buf);
                                    if (metadata) {
                                        metadata->length = mhdr.payload_len;
                                        metadata->pts_ns = mhdr.timecode;
                                        metadata->data = slot->metadata_buf;
                                    }
                                    avx_mutex_unlock(&slot->lock);
                                    return OAVX_FRAME_TYPE_METADATA;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Receive Media Frame via RUDP / UDP */
        if (!slot->video_buf || slot->video_buf_size < 16 * 1024 * 1024) {
            size_t new_sz = 16 * 1024 * 1024;
            uint8_t *nb = (uint8_t *)realloc(slot->video_buf, new_sz);
            if (nb) {
                slot->video_buf = nb;
                slot->video_buf_size = new_sz;
            }
        }

        size_t assembled_len = 0;
        uint32_t assembled_fourcc = 0;
        int64_t assembled_tc = 0;
        int ret = -1;

        if (slot->transport == OAVX_TRANSPORT_RUDP && slot->rudp_receiver) {
            ret = avx_udp_rudp_recv_frame((avx_udp_rudp_receiver_t *)slot->rudp_receiver, &slot->sender_udp_addr,
                                          slot->video_buf, slot->video_buf_size, &assembled_len, &assembled_fourcc,
                                          &assembled_tc, (int)timeout_in_ms);
        } else if (slot->udp_sock != AVX_INVALID_SOCKET) {
            ret = avx_udp_recv_frame(slot->udp_sock, slot->video_buf, slot->video_buf_size, &assembled_len,
                                     &assembled_fourcc, &assembled_tc, (int)timeout_in_ms);
        }

        if (ret == 0 && assembled_len > 0) {
            slot->last_frame_recv_ns = avx_time_ns();

            if (assembled_fourcc == OAVX_FOURCC_AUDIO_TYPE_FLTP) {
                if (assembled_len >= 28) {
                    int32_t s_rate = 48000;
                    int32_t s_channels = 2;
                    int32_t s_samples = 0;
                    int64_t s_tc = assembled_tc;
                    memcpy(&s_rate, slot->video_buf + 4, sizeof(s_rate));
                    memcpy(&s_channels, slot->video_buf + 8, sizeof(s_channels));
                    memcpy(&s_samples, slot->video_buf + 12, sizeof(s_samples));
                    memcpy(&s_tc, slot->video_buf + 16, sizeof(s_tc));

                    size_t pcm_offset = 28;
                    size_t pcm_bytes = assembled_len - pcm_offset;

                    if (s_channels <= 0 || s_channels > OAVX_MAX_AUDIO_CHANNELS || s_samples < 0 ||
                        s_samples > 1920000 ||
                        (uint64_t)s_channels * (uint64_t)s_samples * sizeof(float) > (uint64_t)pcm_bytes) {
                        avx_mutex_unlock(&slot->lock);
                        return OAVX_FRAME_TYPE_NONE;
                    }

                    if (p_audio_data) {
                        memset(p_audio_data, 0, sizeof(*p_audio_data));
                        p_audio_data->sample_rate = s_rate > 0 ? (uint32_t)s_rate : 48000;
                        p_audio_data->channels = (uint16_t)s_channels;
                        p_audio_data->frames = (uint32_t)s_samples;
                        p_audio_data->format = OAVX_AUDIO_FMT_FLT_PLANAR;
                        p_audio_data->pts_ns = s_tc;
                        p_audio_data->stride_bytes = (size_t)s_samples * sizeof(float);
                        p_audio_data->channel_mask =
                            avx_audio_layout_to_mask(avx_audio_layout_from_channels((uint16_t)s_channels));
                        for (int c = 0; c < s_channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
                            p_audio_data->planes[c] =
                                (slot->video_buf && s_samples > 0)
                                    ? (slot->video_buf + pcm_offset + (size_t)c * p_audio_data->stride_bytes)
                                    : NULL;
                        }
                        for (int c = s_channels; c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
                            p_audio_data->planes[c] = NULL;
                        }
                    }
                    slot->perf.audio_frames++;
                    avx_mutex_unlock(&slot->lock);
                    return OAVX_FRAME_TYPE_AUDIO;
                }
            } else if (assembled_fourcc == OAVX_FOURCC_AUDIO_TYPE_OPUS ||
                       assembled_fourcc == OAVX_FOURCC_AUDIO_TYPE_ADPC) {
                if (assembled_len >= 32) {
                    uint32_t c_fourcc = assembled_fourcc;
                    int32_t c_rate = 48000;
                    int32_t c_channels = 2;
                    int32_t c_samples = 0;
                    int64_t c_tc = assembled_tc;
                    uint32_t c_payload_len = 0;
                    memcpy(&c_fourcc, slot->video_buf + 4, sizeof(c_fourcc));
                    memcpy(&c_rate, slot->video_buf + 8, sizeof(c_rate));
                    memcpy(&c_channels, slot->video_buf + 12, sizeof(c_channels));
                    memcpy(&c_samples, slot->video_buf + 16, sizeof(c_samples));
                    memcpy(&c_tc, slot->video_buf + 20, sizeof(c_tc));
                    memcpy(&c_payload_len, slot->video_buf + 28, sizeof(c_payload_len));

                    if (32 + c_payload_len <= assembled_len) {
                        oavx_frame_type_t ft =
                            avx_rx_process_compressed_audio(slot, c_fourcc, c_rate, c_channels, c_samples, c_tc,
                                                            c_payload_len, slot->video_buf + 32, p_audio_data);
                        avx_mutex_unlock(&slot->lock);
                        return ft;
                    }
                }
            } else {
                /* Video frame */
                AVX_PACKED_START
                struct {
                    uint32_t magic;
                    uint32_t xres;
                    uint32_t yres;
                    uint32_t fourcc;
                    int32_t frame_rate_n;
                    int32_t frame_rate_d;
                    int64_t timecode;
                    uint32_t payload_len;
                } AVX_PACKED vhdr;
                AVX_PACKED_END

                if (assembled_len >= sizeof(vhdr)) {
                    memcpy(&vhdr, slot->video_buf, sizeof(vhdr));
                    uint8_t *raw_payload = slot->video_buf + sizeof(vhdr);
                    size_t raw_payload_len = assembled_len - sizeof(vhdr);

                    if (vhdr.xres == 0 || vhdr.yres == 0 || vhdr.xres > AVX_MAX_VIDEO_WIDTH ||
                        vhdr.yres > AVX_MAX_VIDEO_HEIGHT || vhdr.payload_len > raw_payload_len) {
                        avx_mutex_unlock(&slot->lock);
                        return OAVX_FRAME_TYPE_NONE;
                    }

                    bool is_compressed = avx_rx_is_compressed_video_fourcc(vhdr.fourcc);
                    size_t frame_payload_len = (vhdr.payload_len > 0) ? (size_t)vhdr.payload_len : raw_payload_len;
                    if (!is_compressed) {
                        uint64_t min_raw = avx_get_raw_video_min_size(vhdr.fourcc, vhdr.xres, vhdr.yres);
                        if (min_raw > 0 && (uint64_t)frame_payload_len < min_raw) {
                            avx_mutex_unlock(&slot->lock);
                            return OAVX_FRAME_TYPE_NONE;
                        }
                    }

                    avx_recv_update_video_telemetry_locked(slot, vhdr.fourcc, vhdr.xres, vhdr.yres, vhdr.frame_rate_n,
                                                           vhdr.frame_rate_d, assembled_len, avx_time_ns());

                    slot->perf.video_frames++;
                    oavx_color_format_e color_fmt = slot->color_format;
                    const char *metadata_str = slot->last_video_metadata[0] ? slot->last_video_metadata : NULL;
                    avx_video_decoder_t *vdec = NULL;
                    if (is_compressed && p_video_data && color_fmt != OAVX_COLOR_FORMAT_FASTEST &&
                        color_fmt != OAVX_COLOR_FORMAT_COMPRESSED_V1) {
                        vdec = avx_rx_ensure_video_decoder_locked(slot, vhdr.fourcc, vhdr.xres, vhdr.yres);
                    }
                    slot->decoding_video = true;
                    avx_mutex_unlock(&slot->lock);

                    oavx_frame_type_t ft =
                        avx_rx_deliver_video_frame(slot, vdec, color_fmt, is_compressed, metadata_str, vhdr.fourcc,
                                                   vhdr.xres, vhdr.yres, vhdr.frame_rate_n, vhdr.frame_rate_d,
                                                   vhdr.timecode, raw_payload, frame_payload_len, p_video_data);

                    avx_mutex_lock(&slot->lock);
                    slot->decoding_video = false;
                    avx_mutex_unlock(&slot->lock);
                    return ft;
                }
            }
        }
        avx_mutex_unlock(&slot->lock);
        return OAVX_FRAME_TYPE_NONE;
    }

    avx_pollfd_t pfd = {.fd = slot->tcp_conn_fd, .events = POLLIN, .revents = 0};

    avx_mutex_unlock(&slot->lock);
    int poll_res = avx_poll(&pfd, 1, (int)timeout_in_ms);
    avx_mutex_lock(&slot->lock);

    if (slot->tcp_conn_fd == AVX_INVALID_SOCKET || !slot->connected) {
        avx_mutex_unlock(&slot->lock);
        return OAVX_FRAME_TYPE_NONE;
    }

    if (poll_res > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
        avx_socket_close(slot->tcp_conn_fd);
        slot->tcp_conn_fd = AVX_INVALID_SOCKET;
        slot->connected = false;
        if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
            avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT / POLLHUP)");
        }
        avx_mutex_unlock(&slot->lock);
        return OAVX_FRAME_TYPE_ERROR;
    }

    if (poll_res <= 0 || !(pfd.revents & POLLIN)) {
        int64_t curr_ns = avx_time_ns();
        uint32_t stall_threshold_ms = slot->failover_timeout_ms ? slot->failover_timeout_ms : 500;
        if (slot->has_failover && !slot->is_on_failover && slot->last_frame_recv_ns != 0 &&
            (curr_ns - slot->last_frame_recv_ns) > (int64_t)stall_threshold_ms * 1000000LL) {
            avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (STREAM STALL IN POLL)");
        }
        avx_mutex_unlock(&slot->lock);
        return OAVX_FRAME_TYPE_NONE;
    }

    /* Read packet magic (4 bytes) */
    uint32_t magic = 0;
    if (recv_all(slot->tcp_conn_fd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic)) {
        avx_socket_close(slot->tcp_conn_fd);
        slot->tcp_conn_fd = AVX_INVALID_SOCKET;
        slot->connected = false;
        if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
            avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
        }
        avx_mutex_unlock(&slot->lock);
        return OAVX_FRAME_TYPE_ERROR;
    }

    if (magic == slot->wire_profile.magic_video ||
        (slot->wire_profile.compat_magic_video && magic == slot->wire_profile.compat_magic_video)) {
        AVX_PACKED_START
        struct {
            uint32_t xres;
            uint32_t yres;
            uint32_t fourcc;
            int32_t frame_rate_n;
            int32_t frame_rate_d;
            int64_t timecode;
            uint32_t payload_len;
        } AVX_PACKED vhdr;
        AVX_PACKED_END

        if (recv_all(slot->tcp_conn_fd, &vhdr, sizeof(vhdr)) != (ssize_t)sizeof(vhdr)) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
            }
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        /* Enforce security sanity bounds on video dimensions and payload */
        if (vhdr.xres == 0 || vhdr.yres == 0 || vhdr.xres > AVX_MAX_VIDEO_WIDTH || vhdr.yres > AVX_MAX_VIDEO_HEIGHT ||
            vhdr.payload_len > 256 * 1024 * 1024) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        if (slot->video_buf_size < vhdr.payload_len) {
            uint8_t *new_buf = (uint8_t *)realloc(slot->video_buf, vhdr.payload_len);
            if (!new_buf) {
                avx_mutex_unlock(&slot->lock);
                return OAVX_FRAME_TYPE_ERROR;
            }
            slot->video_buf = new_buf;
            slot->video_buf_size = vhdr.payload_len;
        }

        if (recv_all(slot->tcp_conn_fd, slot->video_buf, vhdr.payload_len) != (ssize_t)vhdr.payload_len) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
            }
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        /* Check if incoming frame is compressed (SpeedHQ, H.264, HEVC, AV1) */
        bool is_compressed = avx_rx_is_compressed_video_fourcc(vhdr.fourcc);

        if (!is_compressed) {
            uint64_t min_raw = avx_get_raw_video_min_size(vhdr.fourcc, vhdr.xres, vhdr.yres);
            if (min_raw > 0 && (uint64_t)vhdr.payload_len < min_raw) {
                /* Corrupt or truncated raw video payload; drop connection */
                avx_socket_close(slot->tcp_conn_fd);
                slot->tcp_conn_fd = AVX_INVALID_SOCKET;
                slot->connected = false;
                avx_mutex_unlock(&slot->lock);
                return OAVX_FRAME_TYPE_ERROR;
            }
        }

        /* Telemetry tracking for video */
        size_t v_wire = sizeof(magic) + sizeof(vhdr) + vhdr.payload_len;
        avx_recv_update_video_telemetry_locked(slot, vhdr.fourcc, vhdr.xres, vhdr.yres, vhdr.frame_rate_n,
                                               vhdr.frame_rate_d, v_wire, avx_time_ns());

        slot->perf.video_frames++;
        oavx_color_format_e color_fmt = slot->color_format;
        const char *metadata_str = slot->last_video_metadata[0] ? slot->last_video_metadata : NULL;
        avx_video_decoder_t *vdec = NULL;
        if (is_compressed && p_video_data && color_fmt != OAVX_COLOR_FORMAT_FASTEST &&
            color_fmt != OAVX_COLOR_FORMAT_COMPRESSED_V1) {
            vdec = avx_rx_ensure_video_decoder_locked(slot, vhdr.fourcc, vhdr.xres, vhdr.yres);
        }
        slot->decoding_video = true;
        avx_mutex_unlock(&slot->lock);

        oavx_frame_type_t ft = avx_rx_deliver_video_frame(
            slot, vdec, color_fmt, is_compressed, metadata_str, vhdr.fourcc, vhdr.xres, vhdr.yres, vhdr.frame_rate_n,
            vhdr.frame_rate_d, vhdr.timecode, slot->video_buf, vhdr.payload_len, p_video_data);

        avx_mutex_lock(&slot->lock);
        slot->decoding_video = false;
        avx_mutex_unlock(&slot->lock);
        return ft;
    } else if (magic == slot->wire_profile.magic_audio ||
               (slot->wire_profile.compat_magic_audio && magic == slot->wire_profile.compat_magic_audio)) {
        AVX_PACKED_START
        struct {
            int32_t sample_rate;
            int32_t channels;
            int32_t samples;
            int64_t timecode;
            uint32_t payload_len;
        } AVX_PACKED ahdr;
        AVX_PACKED_END

        if (recv_all(slot->tcp_conn_fd, &ahdr, sizeof(ahdr)) != (ssize_t)sizeof(ahdr)) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
            }
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        /* Enforce security sanity bounds on audio channels, samples, sample_rate, and payload */
        if (ahdr.channels <= 0 || ahdr.channels > OAVX_MAX_AUDIO_CHANNELS || ahdr.samples <= 0 ||
            ahdr.samples > 1920000 || ahdr.sample_rate <= 0 || ahdr.sample_rate > 192000 || ahdr.payload_len == 0 ||
            ahdr.payload_len > 16 * 1024 * 1024 ||
            (uint64_t)ahdr.channels * (uint64_t)ahdr.samples * sizeof(float) > (uint64_t)ahdr.payload_len) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        if (slot->audio_buf_size < ahdr.payload_len) {
            float *new_buf = (float *)realloc(slot->audio_buf, ahdr.payload_len);
            if (!new_buf) {
                avx_mutex_unlock(&slot->lock);
                return OAVX_FRAME_TYPE_ERROR;
            }
            slot->audio_buf = new_buf;
            slot->audio_buf_size = ahdr.payload_len;
        }

        if (recv_all(slot->tcp_conn_fd, slot->audio_buf, ahdr.payload_len) != (ssize_t)ahdr.payload_len) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
            }
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        if (p_audio_data) {
            memset(p_audio_data, 0, sizeof(*p_audio_data));
            p_audio_data->sample_rate = (uint32_t)ahdr.sample_rate;
            p_audio_data->channels = (uint16_t)ahdr.channels;
            p_audio_data->frames = (uint32_t)ahdr.samples;
            p_audio_data->format = OAVX_AUDIO_FMT_FLT_PLANAR;
            p_audio_data->stride_bytes = (size_t)ahdr.samples * sizeof(float);
            p_audio_data->pts_ns = ahdr.timecode;
            p_audio_data->metadata = NULL;
            p_audio_data->metadata_len = 0;
            p_audio_data->user_ctx = NULL;
            p_audio_data->channel_mask =
                avx_audio_layout_to_mask(avx_audio_layout_from_channels((uint16_t)ahdr.channels));
            for (int c = 0; c < ahdr.channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
                p_audio_data->planes[c] =
                    (slot->audio_buf && ahdr.samples > 0) ? (slot->audio_buf + (size_t)c * ahdr.samples) : NULL;
            }
            for (int c = ahdr.channels; c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
                p_audio_data->planes[c] = NULL;
            }
        }

        /* Telemetry tracking for audio */
        size_t a_wire = sizeof(magic) + sizeof(ahdr) + ahdr.payload_len;
        slot->telem.total_bytes += a_wire;
        slot->telem.window_bytes += a_wire;
        slot->last_frame_recv_ns = avx_time_ns();

        slot->perf.audio_frames++;
        avx_mutex_unlock(&slot->lock);
        return OAVX_FRAME_TYPE_AUDIO;
    } else if (magic == OAVX_WIRE_MAGIC_AUDIO_COMP) {
        AVX_PACKED_START
        struct {
            uint32_t codec_fourcc; /* 'OPUS' or 'ADPC' */
            int32_t sample_rate;
            int32_t channels;
            int32_t samples;
            int64_t timecode;
            uint32_t payload_len;
        } AVX_PACKED cahdr;
        AVX_PACKED_END

        if (recv_all(slot->tcp_conn_fd, &cahdr, sizeof(cahdr)) != (ssize_t)sizeof(cahdr)) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
            }
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        /* Enforce security sanity bounds on compressed audio header */
        if (cahdr.channels <= 0 || cahdr.channels > OAVX_MAX_AUDIO_CHANNELS || cahdr.samples < 0 ||
            cahdr.samples > 192000 || cahdr.payload_len == 0 || cahdr.payload_len > 16 * 1024 * 1024 ||
            cahdr.sample_rate <= 0 || cahdr.sample_rate > 192000) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        if (slot->decompressed_buf_size < cahdr.payload_len) {
            uint8_t *new_buf = (uint8_t *)realloc(slot->decompressed_buf, cahdr.payload_len);
            if (!new_buf) {
                avx_mutex_unlock(&slot->lock);
                return OAVX_FRAME_TYPE_ERROR;
            }
            slot->decompressed_buf = new_buf;
            slot->decompressed_buf_size = cahdr.payload_len;
        }

        if (recv_all(slot->tcp_conn_fd, slot->decompressed_buf, cahdr.payload_len) != (ssize_t)cahdr.payload_len) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
            }
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        oavx_frame_type_t ft =
            avx_rx_process_compressed_audio(slot, cahdr.codec_fourcc, cahdr.sample_rate, cahdr.channels, cahdr.samples,
                                            cahdr.timecode, cahdr.payload_len, slot->decompressed_buf, p_audio_data);

        /* Telemetry tracking for audio */
        size_t a_wire = sizeof(magic) + sizeof(cahdr) + cahdr.payload_len;
        slot->telem.total_bytes += a_wire;
        slot->telem.window_bytes += a_wire;
        slot->last_frame_recv_ns = avx_time_ns();

        avx_mutex_unlock(&slot->lock);
        return ft;
    } else if (magic == slot->wire_profile.magic_metadata ||
               (slot->wire_profile.compat_magic_metadata && magic == slot->wire_profile.compat_magic_metadata)) {
        AVX_PACKED_START
        struct {
            int64_t timecode;
            uint32_t payload_len;
        } AVX_PACKED mhdr;
        AVX_PACKED_END

        if (recv_all(slot->tcp_conn_fd, &mhdr, sizeof(mhdr)) != (ssize_t)sizeof(mhdr)) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
            }
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        /* Enforce security sanity bounds on metadata payload */
        if (mhdr.payload_len > 16 * 1024 * 1024) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }

        if (slot->metadata_buf_size < mhdr.payload_len + 1) {
            char *new_buf = (char *)realloc(slot->metadata_buf, mhdr.payload_len + 1);
            if (!new_buf) {
                avx_mutex_unlock(&slot->lock);
                return OAVX_FRAME_TYPE_ERROR;
            }
            slot->metadata_buf = new_buf;
            slot->metadata_buf_size = mhdr.payload_len + 1;
        }

        if (recv_all(slot->tcp_conn_fd, slot->metadata_buf, mhdr.payload_len) != (ssize_t)mhdr.payload_len) {
            avx_socket_close(slot->tcp_conn_fd);
            slot->tcp_conn_fd = AVX_INVALID_SOCKET;
            slot->connected = false;
            if (slot->has_failover && !slot->is_on_failover && slot->failover_url[0] != '\0') {
                avx_rx_execute_failover(slot, "FAILOVER TRIGGERED (SOCKET DISCONNECT)");
            }
            avx_mutex_unlock(&slot->lock);
            return OAVX_FRAME_TYPE_ERROR;
        }
        slot->metadata_buf[mhdr.payload_len] = '\0';
        snprintf(slot->last_video_metadata, sizeof(slot->last_video_metadata), "%s", slot->metadata_buf);

        /* Inspect XML metadata for PTZ and Recording status updates */
        if (strstr(slot->metadata_buf, "record_status") != NULL) {
            const char *rec = strstr(slot->metadata_buf, "recording=\"");
            if (rec) {
                slot->is_recording = (strncmp(rec + 11, "true", 4) == 0);
            }
            const char *fn = strstr(slot->metadata_buf, "filename=\"");
            if (fn) {
                const char *end = strchr(fn + 10, '\"');
                if (end) {
                    size_t flen = (size_t)(end - (fn + 10));
                    if (flen >= sizeof(slot->current_recording_filename))
                        flen = sizeof(slot->current_recording_filename) - 1;
                    strncpy(slot->current_recording_filename, fn + 10, flen);
                    slot->current_recording_filename[flen] = '\0';
                }
            }
            const char *err = strstr(slot->metadata_buf, "error=\"");
            if (err) {
                const char *end = strchr(err + 7, '\"');
                if (end) {
                    size_t elen = (size_t)(end - (err + 7));
                    if (elen >= sizeof(slot->current_recording_error)) elen = sizeof(slot->current_recording_error) - 1;
                    strncpy(slot->current_recording_error, err + 7, elen);
                    slot->current_recording_error[elen] = '\0';
                }
            }
        }
        if (strstr(slot->metadata_buf, "web_control") != NULL) {
            const char *u = strstr(slot->metadata_buf, "url=\"");
            if (u) {
                const char *end = strchr(u + 5, '\"');
                if (end) {
                    size_t ulen = (size_t)(end - (u + 5));
                    if (ulen >= sizeof(slot->web_control_url)) ulen = sizeof(slot->web_control_url) - 1;
                    strncpy(slot->web_control_url, u + 5, ulen);
                    slot->web_control_url[ulen] = '\0';
                }
            }
        }
        if (strstr(slot->metadata_buf, "failover") != NULL) {
            avx_rx_parse_failover_metadata(slot, slot->metadata_buf);
        }
        avx_rx_parse_tally_metadata_locked(slot, slot->metadata_buf);

        if (metadata) {
            metadata->length = mhdr.payload_len;
            metadata->pts_ns = mhdr.timecode;
            metadata->data = slot->metadata_buf;
        }

        avx_trace_packet("[RX]", "AVX METADATA FRAME", slot->metadata_buf, mhdr.payload_len);

        /* Telemetry tracking for metadata */
        size_t m_wire = sizeof(magic) + sizeof(mhdr) + mhdr.payload_len;
        slot->telem.total_bytes += m_wire;
        slot->telem.window_bytes += m_wire;
        slot->last_frame_recv_ns = avx_time_ns();

        slot->perf.metadata_frames++;
        avx_mutex_unlock(&slot->lock);
        return OAVX_FRAME_TYPE_METADATA;
    }

    avx_mutex_unlock(&slot->lock);
    return OAVX_FRAME_TYPE_NONE;
}

/*
 * Releases video frame back to receiver instance. Buffers are reused
 * internally for subsequent frames; caller must not use data after release.
 */
void avx_rx_free_video(avx_rx_instance_t p_instance, const oavx_video_frame_t *p_video_data) {
    (void)p_instance;
    (void)p_video_data;
    /* In openavx, video buffer memory is reused inside the receiver instance for zero allocations */
}

/* Releases audio frame back to receiver internal buffer pool. */
void avx_rx_free_audio(avx_rx_instance_t p_instance, const oavx_audio_frame_t *p_audio_data) {
    (void)p_instance;
    (void)p_audio_data;
    /* Reused inside receiver instance */
}

void avx_rx_free_metadata(avx_rx_instance_t p_instance, const oavx_metadata_frame_t *metadata) {
    (void)p_instance;
    (void)metadata;
}

bool avx_rx_send_metadata(avx_rx_instance_t p_instance, const oavx_metadata_frame_t *metadata) {
    if (!p_instance || !metadata || !metadata->data) return false;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;

    avx_mutex_lock(&slot->lock);
    if (!slot->connected || slot->tcp_conn_fd == AVX_INVALID_SOCKET) {
        avx_mutex_unlock(&slot->lock);
        return false;
    }

    size_t len = metadata->length > 0 ? metadata->length : strlen(metadata->data);

    AVX_PACKED_START
    struct {
        uint32_t magic;
        int64_t timecode;
        uint32_t payload_len;
    } AVX_PACKED hdr;
    AVX_PACKED_END

    hdr.magic = slot->wire_profile.magic_metadata;
    hdr.timecode = metadata->pts_ns;
    hdr.payload_len = (uint32_t)len;

    ssize_t ret1 = send(slot->tcp_conn_fd, (const char *)&hdr, sizeof(hdr), MSG_NOSIGNAL);
    ssize_t ret2 = send(slot->tcp_conn_fd, (const char *)metadata->data, len, MSG_NOSIGNAL);

    bool ok = (ret1 == (ssize_t)sizeof(hdr) && ret2 == (ssize_t)len);
    if (ok) {
        avx_trace_packet("[TX]", "AVX METADATA (UPSTREAM)", metadata->data, len);
    }
    avx_mutex_unlock(&slot->lock);
    return ok;
}

bool avx_rx_set_tally(avx_rx_instance_t p_instance, const oavx_tally_t *p_tally) {
    if (!p_instance || !p_tally) return false;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    char tally_xml[256];
    int len = snprintf(tally_xml, sizeof(tally_xml), "<%s on_program=\"%s\" on_preview=\"%s\" on_recording=\"%s\"/>\n",
                       slot->wire_profile.tally_tag ? slot->wire_profile.tally_tag : OAVX_WIRE_TALLY_TAG,
                       (p_tally->flags & OAVX_TALLY_PROGRAM) ? "true" : "false",
                       (p_tally->flags & OAVX_TALLY_PREVIEW) ? "true" : "false",
                       (p_tally->flags & OAVX_TALLY_RECORDING) ? "true" : "false");

    oavx_metadata_frame_t meta = {.length = (size_t)len, .pts_ns = 0, .data = tally_xml};
    bool ok = avx_rx_send_metadata(p_instance, &meta);
    avx_mutex_lock(&slot->lock);
    slot->tally = *p_tally;
    avx_cond_broadcast(&slot->tally_cond);
    avx_mutex_unlock(&slot->lock);
    return ok;
}

bool avx_rx_get_tally(avx_rx_instance_t p_instance, oavx_tally_t *p_tally, uint32_t timeout_in_ms) {
    if (!p_instance || !p_tally) return false;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;

    avx_mutex_lock(&slot->lock);
    if (timeout_in_ms > 0 && !(slot->tally.flags & (OAVX_TALLY_PROGRAM | OAVX_TALLY_PREVIEW | OAVX_TALLY_RECORDING))) {
        avx_cond_timedwait(&slot->tally_cond, &slot->lock, timeout_in_ms);
    }
    *p_tally = slot->tally;
    avx_mutex_unlock(&slot->lock);
    return true;
}

bool avx_rx_get_performance(avx_rx_instance_t p_instance, oavx_performance_t *p_total, oavx_performance_t *p_dropped) {
    if (!p_instance) return false;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    if (p_total) *p_total = slot->perf;
    if (p_dropped) memset(p_dropped, 0, sizeof(*p_dropped));
    avx_mutex_unlock(&slot->lock);
    return true;
}

bool avx_rx_get_queue(avx_rx_instance_t p_instance, oavx_queue_t *p_total) {
    if (!p_instance || !p_total) return false;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    *p_total = slot->queue;
    avx_mutex_unlock(&slot->lock);
    return true;
}

OPENAVX_API bool avx_rx_get_telemetry(avx_rx_instance_t p_instance, oavx_telemetry_t *out) {
    if (!p_instance || !out) return false;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    memset(out, 0, sizeof(*out));

    avx_mutex_lock(&slot->lock);

    int64_t now_ns = avx_time_ns();
    float fps = slot->telem.instant_fps;
    float mbps = slot->telem.instant_mbps;

    if (slot->telem.last_frame_arrival_ns > 0 && (now_ns - slot->telem.last_frame_arrival_ns) > 1000000000LL) {
        fps = 0.0f;
        mbps = 0.0f;
    } else if (fps == 0.0f && slot->telem.window_frames > 0 && slot->telem.last_window_time_ns > 0) {
        int64_t elapsed_ns = now_ns - slot->telem.last_window_time_ns;
        if (elapsed_ns >= 10000000LL) {
            float elapsed_sec = (float)elapsed_ns / 1000000000.0f;
            fps = (float)slot->telem.window_frames / elapsed_sec;
            mbps = ((float)slot->telem.window_bytes * 8.0f) / (elapsed_sec * 1000000.0f);
        }
    }

    out->video_frames_total = slot->perf.video_frames;
    out->audio_frames_total = slot->perf.audio_frames;
    out->metadata_frames_total = slot->perf.metadata_frames;
    out->dropped_frames_total = 0;
    out->fps = fps;
    out->bitrate_mbps = mbps;
    out->total_wire_bytes = slot->telem.total_bytes;
    out->inter_frame_jitter_ms = slot->telem.inter_frame_jitter_ms;
    out->fourcc = slot->telem.last_fourcc;
    out->width = slot->telem.last_width;
    out->height = slot->telem.last_height;
    out->is_connected = slot->connected && (slot->tcp_conn_fd != AVX_INVALID_SOCKET);

    if (slot->target_url[0]) {
        snprintf(out->remote_endpoint, sizeof(out->remote_endpoint), "%.127s", slot->target_url);
    } else if (slot->target_name[0]) {
        snprintf(out->remote_endpoint, sizeof(out->remote_endpoint), "%.127s", slot->target_name);
    }

    avx_socket_t sock = slot->tcp_conn_fd;
    avx_mutex_unlock(&slot->lock);

    if (sock != AVX_INVALID_SOCKET) {
        avx_socket_get_diagnostics(sock, &out->kernel_rtt_ms, &out->tcp_retransmissions, &out->socket_send_q_bytes,
                                   &out->socket_recv_q_bytes);
        if (out->socket_recv_q_bytes >= 2000000) {
            out->congestion_level = 2;
        } else if (out->socket_recv_q_bytes >= 500000) {
            out->congestion_level = 1;
        } else {
            out->congestion_level = 0;
        }
    }

    return true;
}

void avx_rx_clear_connection_metadata(avx_rx_instance_t p_instance) {
    if (!p_instance) return;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    slot->num_conn_metadata = 0;
    avx_mutex_unlock(&slot->lock);
}

void avx_rx_add_connection_metadata(avx_rx_instance_t p_instance, const oavx_metadata_frame_t *metadata) {
    if (!p_instance || !metadata || !metadata->data) return;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    if (slot->num_conn_metadata < 16) {
        snprintf(slot->conn_metadata[slot->num_conn_metadata], sizeof(slot->conn_metadata[0]), "%s", metadata->data);
        slot->num_conn_metadata++;
    }
    avx_mutex_unlock(&slot->lock);
}

int avx_rx_get_connection_count(avx_rx_instance_t p_instance) {
    if (!p_instance) return 0;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    int active = slot->connected ? 1 : 0;
    avx_mutex_unlock(&slot->lock);
    return active;
}

/*
 * Connects receiver to a new upstream source. Closes any existing connection,
 * aborts pending async handshakes, and resets video/audio decoder state.
 */
void avx_rx_connect(avx_rx_instance_t p_instance, const oavx_endpoint_t *p_src) {
    if (!p_instance) return;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;

    avx_mutex_lock(&slot->lock);
    avx_recv_close_pending_locked(slot);
    if (slot->tcp_conn_fd != AVX_INVALID_SOCKET) {
        avx_socket_close(slot->tcp_conn_fd);
        slot->tcp_conn_fd = AVX_INVALID_SOCKET;
    }
    slot->connected = false;
    slot->is_on_failover = false;
    slot->last_connect_attempt_ns = 0;

    if (p_src) {
        if (p_src->name) {
            snprintf(slot->target_name, sizeof(slot->target_name), "%s", p_src->name);
            snprintf(slot->primary_name, sizeof(slot->primary_name), "%s", p_src->name);
        }
        if (p_src->address) {
            snprintf(slot->target_url, sizeof(slot->target_url), "%s", p_src->address);
            snprintf(slot->primary_url, sizeof(slot->primary_url), "%s", p_src->address);
        }
        slot->target_source.name = slot->target_name;
        slot->target_source.address = slot->target_url;
        avx_recv_try_connect(slot);
    }
    avx_mutex_unlock(&slot->lock);
}

void avx_rx_set_failover(avx_rx_instance_t p_instance, const oavx_endpoint_t *p_failover_source) {
    if (!p_instance) return;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    if (p_failover_source && (p_failover_source->address != NULL || p_failover_source->name != NULL)) {
        if (p_failover_source->name) {
            snprintf(slot->failover_name, sizeof(slot->failover_name), "%s", p_failover_source->name);
        } else {
            slot->failover_name[0] = '\0';
        }
        if (p_failover_source->address) {
            snprintf(slot->failover_url, sizeof(slot->failover_url), "%s", p_failover_source->address);
        } else {
            slot->failover_url[0] = '\0';
        }
        slot->has_failover = (slot->failover_url[0] != '\0' || slot->failover_name[0] != '\0');
    } else {
        slot->has_failover = false;
        slot->failover_name[0] = '\0';
        slot->failover_url[0] = '\0';
    }
    avx_mutex_unlock(&slot->lock);
}

bool avx_rx_is_on_failover(avx_rx_instance_t p_instance) {
    if (!p_instance) return false;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    bool on_failover = slot->is_on_failover;
    avx_mutex_unlock(&slot->lock);
    return on_failover;
}

void avx_rx_set_failover_timeout(avx_rx_instance_t p_instance, uint32_t timeout_ms) {
    if (!p_instance) return;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    slot->failover_timeout_ms = timeout_ms;
    avx_mutex_unlock(&slot->lock);
}

void avx_rx_set_auto_revert(avx_rx_instance_t p_instance, bool auto_revert, uint32_t hold_time_ms) {
    if (!p_instance) return;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    slot->auto_revert = auto_revert;
    slot->revert_hold_ms = hold_time_ms ? hold_time_ms : 3000;
    avx_mutex_unlock(&slot->lock);
}

/*
 * Transmits upstream transport/codec profile request (e.g. "wan" for H.264
 * simulcast) to sender over active control channel.
 */
OPENAVX_API bool avx_rx_request_profile(avx_rx_instance_t p_instance, const char *profile) {
    if (!p_instance || !profile) return false;
    avx_recv_ctx_t *slot = (avx_recv_ctx_t *)p_instance;
    avx_mutex_lock(&slot->lock);
    if (slot->tcp_conn_fd == AVX_INVALID_SOCKET || !slot->connected) {
        avx_mutex_unlock(&slot->lock);
        return false;
    }

    char codecs_str[64] = "";
    if (avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_AV01))
        strncat(codecs_str, "av1,", sizeof(codecs_str) - strlen(codecs_str) - 1);
    if (avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_HEVC))
        strncat(codecs_str, "hevc,", sizeof(codecs_str) - strlen(codecs_str) - 1);
    if (avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_H264))
        strncat(codecs_str, "h264,", sizeof(codecs_str) - strlen(codecs_str) - 1);
    size_t clen = strlen(codecs_str);
    if (clen > 0 && codecs_str[clen - 1] == ',') codecs_str[clen - 1] = '\0';
    if (codecs_str[0] == '\0') snprintf(codecs_str, sizeof(codecs_str), "h264");

    char resp[256];
    int rlen = snprintf(
        resp, sizeof(resp), "<transport mode=\"%s\" port=\"%u\" codecs=\"%s\" profile=\"%s\"/>\n",
        (slot->transport == OAVX_TRANSPORT_RUDP) ? "rudp" : (slot->transport == OAVX_TRANSPORT_UDP ? "udp" : "tcp"),
        slot->udp_port, codecs_str, profile);

    AVX_PACKED_START
    struct {
        uint32_t magic;
        int64_t timecode;
        uint32_t payload_len;
    } AVX_PACKED mhdr;
    AVX_PACKED_END
    mhdr.magic = slot->wire_profile.magic_metadata;
    mhdr.timecode = 0;
    mhdr.payload_len = (uint32_t)rlen;

    ssize_t s1 = send(slot->tcp_conn_fd, (const char *)&mhdr, sizeof(mhdr), MSG_NOSIGNAL);
    ssize_t s2 = send(slot->tcp_conn_fd, resp, rlen, MSG_NOSIGNAL);
    avx_mutex_unlock(&slot->lock);
    return (s1 == (ssize_t)sizeof(mhdr) && s2 == (ssize_t)rlen);
}
