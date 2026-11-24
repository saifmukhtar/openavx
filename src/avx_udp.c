/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_udp.c - UDP datagram transport, RUDP NACK, and fragmentation
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "avx_udp.h"
#include "avx_internal.h"
#include "avx_wire.h"

#if defined(AVX_OS_POSIX)
    #include <net/if.h>
    #include <ifaddrs.h>
#endif

#if (defined(__linux__) || defined(__FreeBSD__)) && defined(_GNU_SOURCE)
    #define AVX_HAS_MMSG 1
#endif

#define AVX_UDP_BATCH_SIZE 32

#ifdef AVX_OS_WINDOWS
    typedef ADDRESS_FAMILY avx_sa_family_t;
#else
    typedef sa_family_t avx_sa_family_t;
#endif

static inline socklen_t avx_udp_prepare_dest_addr(avx_socket_t sock, const void *dest,
                                                  struct sockaddr_storage *out_addr,
                                                  const struct sockaddr **out_dest_ptr) {
    if (!dest || !out_addr || !out_dest_ptr) return 0;
    const struct sockaddr *sa = (const struct sockaddr*)dest;
    avx_sa_family_t fam = sa->sa_family;
    if (fam == 0) fam = AF_INET;

    struct sockaddr_storage sock_addr;
    socklen_t sock_len = sizeof(sock_addr);
    avx_sa_family_t sock_fam = AF_INET;
    if (getsockname(sock, (struct sockaddr*)&sock_addr, &sock_len) == 0) {
        sock_fam = sock_addr.ss_family;
    }

    if (sock_fam == AF_INET6 && fam == AF_INET) {
        const struct sockaddr_in *sin4 = (const struct sockaddr_in*)dest;
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)out_addr;
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = sin4->sin_port;
        sin6->sin6_addr.s6_addr[10] = 0xFF;
        sin6->sin6_addr.s6_addr[11] = 0xFF;
        memcpy(&sin6->sin6_addr.s6_addr[12], &sin4->sin_addr.s_addr, 4);
        *out_dest_ptr = (const struct sockaddr*)sin6;
        return sizeof(*sin6);
    } else if (sock_fam == AF_INET && fam == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6*)dest;
        if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
            struct sockaddr_in *sin4 = (struct sockaddr_in*)out_addr;
            memset(sin4, 0, sizeof(*sin4));
            sin4->sin_family = AF_INET;
            sin4->sin_port = sin6->sin6_port;
            memcpy(&sin4->sin_addr.s_addr, &sin6->sin6_addr.s6_addr[12], 4);
            *out_dest_ptr = (const struct sockaddr*)sin4;
            return sizeof(*sin4);
        }
        return 0;
    } else if (fam == AF_INET6) {
        *out_dest_ptr = sa;
        return sizeof(struct sockaddr_in6);
    } else {
        *out_dest_ptr = sa;
        return sizeof(struct sockaddr_in);
    }
}

/*
 * Creates dual-stack IPv4/IPv6 UDP socket with SO_REUSEADDR and 16MB socket
 * buffer sizing (SO_RCVBUF/SO_SNDBUF) sized for gigabit media streaming.
 */
avx_socket_t avx_udp_create_socket_bind(const char* bind_ip, uint16_t port, bool reuse)
{
    int opt = 1;
    int buf_size = 16 * 1024 * 1024; /* 16 MB socket buffers for high-bandwidth UDP streaming */

    /* Case A: Explicit IPv6 literal (e.g. "::1" or "[::1]" or "2001:...") */
    char unbracketed[128];
    const char* clean_ip = bind_ip;
    if (bind_ip && bind_ip[0] == '[') {
        const char* end = strchr(bind_ip, ']');
        if (end) {
            size_t len = (size_t)(end - (bind_ip + 1));
            if (len < sizeof(unbracketed)) {
                memcpy(unbracketed, bind_ip + 1, len);
                unbracketed[len] = '\0';
                clean_ip = unbracketed;
            }
        }
    }

    struct in6_addr in6;
    if (clean_ip && clean_ip[0] != '\0' && strcmp(clean_ip, "::") != 0 && inet_pton(AF_INET6, clean_ip, &in6) == 1) {
        avx_socket_t sock = socket(AF_INET6, SOCK_DGRAM, 0);
        if (sock == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

        if (reuse) {
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
            setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif
        }
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        addr6.sin6_addr = in6;

        if (bind(sock, (struct sockaddr*)&addr6, sizeof(addr6)) < 0) {
            avx_socket_close(sock);
            return AVX_INVALID_SOCKET;
        }
        return sock;
    }

    /* Case B: Explicit IPv4 literal (e.g. "192.168.1.100" or "127.0.0.1") */
    struct in_addr in4;
    if (clean_ip && clean_ip[0] != '\0' && strcmp(clean_ip, "0.0.0.0") != 0 && strcmp(clean_ip, "::") != 0) {
        if (inet_pton(AF_INET, clean_ip, &in4) == 1) {
            avx_socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

            if (reuse) {
                setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
                setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif
            }
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));
            setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));

            struct sockaddr_in addr4;
            memset(&addr4, 0, sizeof(addr4));
            addr4.sin_family = AF_INET;
            addr4.sin_port = htons(port);
            addr4.sin_addr = in4;

            if (bind(sock, (struct sockaddr*)&addr4, sizeof(addr4)) < 0) {
                avx_socket_close(sock);
                return AVX_INVALID_SOCKET;
            }
            return sock;
        }
    }

    /* Case C: Wildcard Dual-Stack Binding (bind_ip is NULL, "", "0.0.0.0", or "::") */
    avx_socket_t sock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (sock != AVX_INVALID_SOCKET) {
        int no = 0;
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&no, sizeof(no));
        if (reuse) {
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
            setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif
        }
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        addr6.sin6_addr = in6addr_any;

        if (bind(sock, (struct sockaddr*)&addr6, sizeof(addr6)) == 0) {
            return sock;
        }
        avx_socket_close(sock);
    }

    /* Fallback to AF_INET if dual-stack IPv6 is unsupported */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

    if (reuse) {
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#ifdef SO_REUSEPORT
        setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#endif
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));

    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(port);
    addr4.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&addr4, sizeof(addr4)) < 0) {
        avx_socket_close(sock);
        return AVX_INVALID_SOCKET;
    }
    return sock;
}

avx_socket_t avx_udp_create_socket(uint16_t port, bool reuse)
{
    return avx_udp_create_socket_bind(NULL, port, reuse);
}

#if defined(AVX_OS_POSIX)
/* Fallback for hosts/containers lacking a default IPv6 multicast route (ENODEV). */
static bool avx_udp_ipv6_mcast_fallback(avx_socket_t sock, int opt, struct ipv6_mreq* mreq6) {
    struct ifaddrs* ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) return false;

    for (int pass = 0; pass < 2; ++pass) {
        for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_MULTICAST) == 0) continue;
            bool is_lo = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
            if ((pass == 0 && is_lo) || (pass == 1 && !is_lo)) continue;

            unsigned int idx = if_nametoindex(ifa->ifa_name);
            if (idx == 0) continue;

            mreq6->ipv6mr_interface = idx;
            if (setsockopt(sock, IPPROTO_IPV6, opt, (const char*)mreq6, sizeof(*mreq6)) == 0) {
                freeifaddrs(ifaddr);
                return true;
            }
        }
    }

    freeifaddrs(ifaddr);
    return false;
}
#endif

static bool avx_udp_multicast_membership(avx_socket_t sock, const char* mcast_addr, const char* iface_or_ip, bool join) {
    if (sock == AVX_INVALID_SOCKET || !mcast_addr || mcast_addr[0] == '\0') return false;

    /* Check if IPv4 */
    struct in_addr m4;
    if (inet_pton(AF_INET, mcast_addr, &m4) == 1) {
        uint32_t h = ntohl(m4.s_addr);
        if ((h >> 28) != 0xE) return false; /* Not in 224.0.0.0/4 */

        struct ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr = m4;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);

        if (iface_or_ip && iface_or_ip[0] != '\0') {
            char resolved[64];
            if (avx_net_resolve_interface_or_ip(iface_or_ip, resolved, sizeof(resolved))) {
                inet_pton(AF_INET, resolved, &mreq.imr_interface);
            }
        }

        int opt = join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP;
        return (setsockopt(sock, IPPROTO_IP, opt, (const char*)&mreq, sizeof(mreq)) == 0);
    }

    /* Check if IPv6 */
    char unbracketed[128];
    const char* clean_mcast = mcast_addr;
    if (mcast_addr[0] == '[') {
        const char* end = strchr(mcast_addr, ']');
        if (end) {
            size_t len = (size_t)(end - (mcast_addr + 1));
            if (len < sizeof(unbracketed)) {
                memcpy(unbracketed, mcast_addr + 1, len);
                unbracketed[len] = '\0';
                clean_mcast = unbracketed;
            }
        }
    }

    struct in6_addr m6;
    if (inet_pton(AF_INET6, clean_mcast, &m6) == 1) {
        if (m6.s6_addr[0] != 0xFF) return false; /* IPv6 multicast prefix ff00::/8 */

        struct ipv6_mreq mreq6;
        memset(&mreq6, 0, sizeof(mreq6));
        mreq6.ipv6mr_multiaddr = m6;
        mreq6.ipv6mr_interface = 0; /* Default interface */

        bool has_iface = (iface_or_ip && iface_or_ip[0] != '\0');
        if (has_iface) {
#if defined(AVX_OS_POSIX)
            unsigned int idx = if_nametoindex(iface_or_ip);
            if (idx == 0) return false;
            mreq6.ipv6mr_interface = idx;
#endif
        }

#if defined(IPV6_JOIN_GROUP)
        int opt = join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP;
#elif defined(IPV6_ADD_MEMBERSHIP)
        int opt = join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP;
#else
        int opt = join ? 20 : 21;
#endif
        if (setsockopt(sock, IPPROTO_IPV6, opt, (const char*)&mreq6, sizeof(mreq6)) == 0) {
            return true;
        }
#if defined(AVX_OS_POSIX)
        if (!has_iface && (errno == ENODEV || errno == EADDRNOTAVAIL)) {
            return avx_udp_ipv6_mcast_fallback(sock, opt, &mreq6);
        }
#endif
        return false;
    }

    return false;
}

bool avx_udp_join_multicast(avx_socket_t sock, const char* mcast_addr, const char* iface_or_ip) {
    return avx_udp_multicast_membership(sock, mcast_addr, iface_or_ip, true);
}

bool avx_udp_leave_multicast(avx_socket_t sock, const char* mcast_addr, const char* iface_or_ip) {
    return avx_udp_multicast_membership(sock, mcast_addr, iface_or_ip, false);
}

bool avx_udp_set_multicast_ttl(avx_socket_t sock, int ttl) {
    if (sock == AVX_INVALID_SOCKET || ttl < 0 || ttl > 255) return false;

    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    avx_sa_family_t fam = AF_INET;
    if (getsockname(sock, (struct sockaddr*)&addr, &len) == 0) {
        fam = addr.ss_family;
    }

    bool ok = false;
    unsigned char t = (unsigned char)ttl;
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&t, sizeof(t)) == 0) {
        ok = true;
    }
    if (fam == AF_INET6) {
        int h = ttl;
        if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char*)&h, sizeof(h)) == 0) {
            ok = true;
        }
    }
    return ok;
}

bool avx_udp_set_multicast_loop(avx_socket_t sock, bool loop) {
    if (sock == AVX_INVALID_SOCKET) return false;

    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    avx_sa_family_t fam = AF_INET;
    if (getsockname(sock, (struct sockaddr*)&addr, &len) == 0) {
        fam = addr.ss_family;
    }

    bool ok = false;
    unsigned char l4 = loop ? 1 : 0;
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&l4, sizeof(l4)) == 0) {
        ok = true;
    }
    if (fam == AF_INET6) {
        unsigned int l6 = loop ? 1 : 0;
        if (setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char*)&l6, sizeof(l6)) == 0) {
            ok = true;
        }
    }
    return ok;
}

static int avx_udp_calc_frag_count(size_t payload_len, uint16_t *out_frag_count)
{
    size_t max_bytes = (size_t)AVX_UDP_MAX_FRAGS * (size_t)AVX_UDP_MAX_FRAG_SIZE;
    if (payload_len > max_bytes) return -1;

    uint16_t frags = (uint16_t)((payload_len + AVX_UDP_MAX_FRAG_SIZE - 1) / AVX_UDP_MAX_FRAG_SIZE);
    if (frags == 0) frags = 1;
    *out_frag_count = frags;
    return 0;
}

static int avx_udp_send_frame_internal(avx_socket_t sock, const void *dest,
                                       uint32_t frame_seq, uint32_t fourcc, int64_t timecode,
                                       const void *payload, size_t payload_len,
                                       uint16_t send_ts_ms, uint16_t srtt_ms,
                                       const avx_crypto_ctx_t *crypto)
{
    if (sock == AVX_INVALID_SOCKET || !dest || (!payload && payload_len > 0)) return -1;

    uint16_t frag_count = 0;
    if (avx_udp_calc_frag_count(payload_len, &frag_count) != 0) {
        return -1;
    }

    struct sockaddr_storage prepared_dest;
    const struct sockaddr *dest_sa = NULL;
    socklen_t dest_len = avx_udp_prepare_dest_addr(sock, dest, &prepared_dest, &dest_sa);
    if (dest_len == 0 || !dest_sa) return -1;

    uint32_t pcap_dst_ip = htonl(0x7F000001);
    uint16_t pcap_dst_port = 0;
    if (dest_sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in*)dest_sa;
        pcap_dst_ip = s4->sin_addr.s_addr;
        pcap_dst_port = ntohs(s4->sin_port);
    } else if (dest_sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6*)dest_sa;
        pcap_dst_port = ntohs(s6->sin6_port);
        memcpy(&pcap_dst_ip, &s6->sin6_addr.s6_addr[12], 4);
    }

    const uint8_t *src = (const uint8_t*)payload;
    size_t offset = 0;

    const oavx_wire_profile_t *prof = avx_wire_get_active_profile();
    uint32_t udp_magic = (prof && prof->magic_udp) ? prof->magic_udp : OAVX_UDP_MAGIC;
    uint16_t pcap_src_port = (prof && prof->base_port) ? (prof->base_port + 1) : (OAVX_WIRE_DEFAULT_PORT + 1);
    (void)pcap_src_port;

#if defined(AVX_HAS_MMSG)
    /* High-performance Linux / BSD batch transmission via sendmmsg() */
    uint16_t idx = 0;
    while (idx < frag_count) {
        uint16_t batch_count = frag_count - idx;
        if (batch_count > AVX_UDP_BATCH_SIZE) {
            batch_count = AVX_UDP_BATCH_SIZE;
        }

        avx_udp_header_t hdrs[AVX_UDP_BATCH_SIZE];
        struct iovec iovs[AVX_UDP_BATCH_SIZE][2];
        struct mmsghdr msgs[AVX_UDP_BATCH_SIZE];
        uint8_t enc_bufs[AVX_UDP_BATCH_SIZE][AVX_UDP_MAX_FRAG_SIZE + AVX_CRYPTO_OVERHEAD];
        memset(msgs, 0, sizeof(msgs));

        size_t batch_offset = offset;
        for (uint16_t b = 0; b < batch_count; ++b) {
            uint16_t cur_idx = idx + b;
            size_t chunk_len = payload_len - batch_offset;
            if (chunk_len > AVX_UDP_MAX_FRAG_SIZE) {
                chunk_len = AVX_UDP_MAX_FRAG_SIZE;
            }

            hdrs[b].magic = udp_magic;
            hdrs[b].frame_seq = frame_seq;
            hdrs[b].frag_idx = cur_idx;
            hdrs[b].frag_count = frag_count;
            hdrs[b].fourcc = fourcc;
            hdrs[b].timecode = timecode;
            hdrs[b].payload_len = (uint16_t)chunk_len;
            hdrs[b].send_ts_ms = send_ts_ms;
            hdrs[b].srtt_ms = srtt_ms;
            hdrs[b].reserved = 0;

            iovs[b][0].iov_base = &hdrs[b];
            iovs[b][0].iov_len = sizeof(hdrs[b]);

            if (crypto && crypto->enabled) {
                hdrs[b].reserved |= AVX_UDP_FLAG_ENCRYPTED;
                size_t enc_len = sizeof(enc_bufs[b]);
                if (avx_crypto_encrypt_payload(crypto, frame_seq, cur_idx, frag_count,
                                               &hdrs[b], sizeof(hdrs[b]),
                                               src + batch_offset, chunk_len,
                                               enc_bufs[b], &enc_len) == 0) {
                    iovs[b][1].iov_base = enc_bufs[b];
                    iovs[b][1].iov_len = enc_len;
                } else {
                    iovs[b][1].iov_base = (void*)(src + batch_offset);
                    iovs[b][1].iov_len = chunk_len;
                }
            } else {
                iovs[b][1].iov_base = (void*)(src + batch_offset);
                iovs[b][1].iov_len = chunk_len;
            }

            msgs[b].msg_hdr.msg_name = (void*)dest_sa;
            msgs[b].msg_hdr.msg_namelen = dest_len;
            msgs[b].msg_hdr.msg_iov = iovs[b];
            msgs[b].msg_hdr.msg_iovlen = (iovs[b][1].iov_len > 0) ? 2 : 1;

            /* Write sample fragment to PCAP if enabled */
            if (cur_idx == 0 || cur_idx == frag_count - 1) {
                size_t pcap_chunk = iovs[b][1].iov_len > 256 ? 256 : iovs[b][1].iov_len;
                size_t pcap_frag_len = sizeof(hdrs[b]) + pcap_chunk;
                uint8_t *pcap_buf = (uint8_t*)malloc(pcap_frag_len);
                if (pcap_buf) {
                    memcpy(pcap_buf, &hdrs[b], sizeof(hdrs[b]));
                    if (pcap_chunk > 0) {
                        memcpy(pcap_buf + sizeof(hdrs[b]), iovs[b][1].iov_base, pcap_chunk);
                    }
                    avx_pcap_write_udp(htonl(0x7F000001), pcap_src_port, pcap_dst_ip, pcap_dst_port, pcap_buf, pcap_frag_len);
                    free(pcap_buf);
                }
            }

            batch_offset += chunk_len;
        }

        int sent = sendmmsg(sock, msgs, batch_count, 0);
        if (sent <= 0) {
            return -1;
        }

        for (int s = 0; s < sent; ++s) {
            size_t chunk_len = payload_len - offset;
            if (chunk_len > AVX_UDP_MAX_FRAG_SIZE) {
                chunk_len = AVX_UDP_MAX_FRAG_SIZE;
            }
            offset += chunk_len;
        }
        idx += (uint16_t)sent;
    }

    return 0;
#else
    /* Non-batching fallback: Windows WSASendTo scatter-gather / POSIX sendmsg */
    for (uint16_t idx = 0; idx < frag_count; ++idx) {
        size_t chunk_len = payload_len - offset;
        if (chunk_len > AVX_UDP_MAX_FRAG_SIZE) {
            chunk_len = AVX_UDP_MAX_FRAG_SIZE;
        }

        avx_udp_header_t hdr;
        hdr.magic = udp_magic;
        hdr.frame_seq = frame_seq;
        hdr.frag_idx = idx;
        hdr.frag_count = frag_count;
        hdr.fourcc = fourcc;
        hdr.timecode = timecode;
        hdr.payload_len = (uint16_t)chunk_len;
        hdr.send_ts_ms = send_ts_ms;
        hdr.srtt_ms = srtt_ms;
        hdr.reserved = 0;

        uint8_t enc_buf[AVX_UDP_MAX_FRAG_SIZE + AVX_CRYPTO_OVERHEAD];
        void *send_payload_ptr = (void*)(src + offset);
        size_t send_payload_len = chunk_len;

        if (crypto && crypto->enabled) {
            hdr.reserved |= AVX_UDP_FLAG_ENCRYPTED;
            size_t enc_len = sizeof(enc_buf);
            if (avx_crypto_encrypt_payload(crypto, frame_seq, idx, frag_count,
                                           &hdr, sizeof(hdr),
                                           src + offset, chunk_len,
                                           enc_buf, &enc_len) == 0) {
                send_payload_ptr = enc_buf;
                send_payload_len = enc_len;
            }
        }

#if defined(AVX_OS_WINDOWS)
        WSABUF wbufs[2];
        wbufs[0].buf = (char*)&hdr;
        wbufs[0].len = (ULONG)sizeof(hdr);
        wbufs[1].buf = (char*)send_payload_ptr;
        wbufs[1].len = (ULONG)send_payload_len;

        DWORD bytes_sent = 0;
        if (WSASendTo(sock, wbufs, (send_payload_len > 0) ? 2 : 1, &bytes_sent, 0,
                      dest_sa, (int)dest_len, NULL, NULL) == SOCKET_ERROR) {
            return -1;
        }
#else
        struct iovec iov[2];
        iov[0].iov_base = &hdr;
        iov[0].iov_len = sizeof(hdr);
        iov[1].iov_base = send_payload_ptr;
        iov[1].iov_len = send_payload_len;

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_name = (void*)dest_sa;
        msg.msg_namelen = dest_len;
        msg.msg_iov = iov;
        msg.msg_iovlen = (send_payload_len > 0) ? 2 : 1;

        if (sendmsg(sock, &msg, 0) < 0) {
            return -1;
        }
#endif

        /* Write sample fragment to PCAP if enabled */
        if (idx == 0 || idx == frag_count - 1) {
            size_t pcap_chunk = send_payload_len > 256 ? 256 : send_payload_len;
            size_t pcap_frag_len = sizeof(hdr) + pcap_chunk;
            uint8_t *pcap_buf = (uint8_t*)malloc(pcap_frag_len);
            if (pcap_buf) {
                memcpy(pcap_buf, &hdr, sizeof(hdr));
                if (pcap_chunk > 0) {
                    memcpy(pcap_buf + sizeof(hdr), send_payload_ptr, pcap_chunk);
                }
                avx_pcap_write_udp(htonl(0x7F000001), pcap_src_port, pcap_dst_ip, pcap_dst_port, pcap_buf, pcap_frag_len);
                free(pcap_buf);
            }
        }

        offset += chunk_len;
    }

    return 0;
#endif
}

int avx_udp_send_frame(avx_socket_t sock, const void *dest,
                       uint32_t frame_seq, uint32_t fourcc, int64_t timecode,
                       const void *payload, size_t payload_len)
{
    return avx_udp_send_frame_internal(sock, dest, frame_seq, fourcc, timecode, payload, payload_len,
                                       (uint16_t)(avx_time_ms() & 0xFFFF), 0, NULL);
}

int avx_udp_recv_frame(avx_socket_t sock, void *out_buf, size_t out_max,
                       size_t *out_size, uint32_t *out_fourcc, int64_t *out_timecode,
                       int timeout_ms)
{
    if (sock == AVX_INVALID_SOCKET || !out_buf || !out_size) return -1;

    uint32_t curr_frame_seq = 0;
    uint16_t total_frags = 0;
    uint16_t received_frags = 0;
    uint8_t received_bitmap[AVX_UDP_MAX_FRAGS / 8];
    memset(received_bitmap, 0, sizeof(received_bitmap));

    size_t assembled_bytes = 0;
    int remaining_ms = timeout_ms;
    const oavx_wire_profile_t *prof = avx_wire_get_active_profile();
    uint32_t expected_magic = (prof && prof->magic_udp) ? prof->magic_udp : OAVX_UDP_MAGIC;
    uint32_t compat_magic = (prof) ? prof->compat_magic_udp : 0;

    while (1) {
        avx_pollfd_t pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
        int pr = avx_poll(&pfd, 1, remaining_ms > 0 ? remaining_ms : 50);
        if (pr <= 0) return -1; /* Timeout or error */

#if defined(AVX_HAS_MMSG)
        struct mmsghdr msgs[AVX_UDP_BATCH_SIZE];
        struct iovec iovs[AVX_UDP_BATCH_SIZE];
        uint8_t packet_bufs[AVX_UDP_BATCH_SIZE][2048];
        memset(msgs, 0, sizeof(msgs));

        for (int b = 0; b < AVX_UDP_BATCH_SIZE; ++b) {
            iovs[b].iov_base = packet_bufs[b];
            iovs[b].iov_len = sizeof(packet_bufs[b]);
            msgs[b].msg_hdr.msg_iov = &iovs[b];
            msgs[b].msg_hdr.msg_iovlen = 1;
        }

        int num_pkts = recvmmsg(sock, msgs, AVX_UDP_BATCH_SIZE, MSG_DONTWAIT, NULL);
        if (num_pkts <= 0) {
            if (num_pkts < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return -1;
            }
            ssize_t n = recv(sock, (char*)packet_bufs[0], sizeof(packet_bufs[0]), 0);
            if (n <= 0) continue;
            num_pkts = 1;
            msgs[0].msg_len = (unsigned int)n;
        }

        for (int p = 0; p < num_pkts; ++p) {
            if (msgs[p].msg_len < sizeof(avx_udp_header_t)) continue;

            avx_udp_header_t *hdr = (avx_udp_header_t*)packet_bufs[p];
            if ((hdr->magic != expected_magic && (!compat_magic || hdr->magic != compat_magic)) ||
                hdr->frag_count == 0 || hdr->frag_count > AVX_UDP_MAX_FRAGS ||
                hdr->frag_idx >= hdr->frag_count ||
                hdr->payload_len > AVX_UDP_MAX_FRAG_SIZE ||
                msgs[p].msg_len < (sizeof(avx_udp_header_t) + hdr->payload_len)) {
                continue;
            }

            /* Check for new frame */
            if (total_frags == 0 || hdr->frame_seq > curr_frame_seq) {
                curr_frame_seq = hdr->frame_seq;
                total_frags = hdr->frag_count;
                received_frags = 0;
                assembled_bytes = 0;
                memset(received_bitmap, 0, sizeof(received_bitmap));

                if (out_fourcc) *out_fourcc = hdr->fourcc;
                if (out_timecode) *out_timecode = hdr->timecode;
            }

            /* If this fragment belongs to current frame and not yet received */
            if (hdr->frame_seq == curr_frame_seq && hdr->frag_idx < total_frags) {
                int byte_idx = hdr->frag_idx / 8;
                int bit_idx = hdr->frag_idx % 8;

                if (!(received_bitmap[byte_idx] & (1 << bit_idx))) {
                    received_bitmap[byte_idx] |= (1 << bit_idx);
                    received_frags++;

                    size_t dest_offset = (size_t)hdr->frag_idx * AVX_UDP_MAX_FRAG_SIZE;
                    if (dest_offset + hdr->payload_len <= out_max) {
                        memcpy((uint8_t*)out_buf + dest_offset, packet_bufs[p] + sizeof(avx_udp_header_t), hdr->payload_len);
                        if (dest_offset + hdr->payload_len > assembled_bytes) {
                            assembled_bytes = dest_offset + hdr->payload_len;
                        }
                    }

                    /* Check if entire frame is complete */
                    if (received_frags == total_frags) {
                        *out_size = assembled_bytes;
                        return 0;
                    }
                }
            }
        }
#else
        uint8_t packet_buf[sizeof(avx_udp_header_t) + AVX_UDP_MAX_FRAG_SIZE];
        ssize_t n = recv(sock, (char*)packet_buf, sizeof(packet_buf), 0);
        if (n < (ssize_t)sizeof(avx_udp_header_t)) continue;

        avx_udp_header_t *hdr = (avx_udp_header_t*)packet_buf;
        if ((hdr->magic != expected_magic && (!compat_magic || hdr->magic != compat_magic)) ||
            hdr->frag_count == 0 || hdr->frag_count > AVX_UDP_MAX_FRAGS ||
            hdr->frag_idx >= hdr->frag_count ||
            hdr->payload_len > AVX_UDP_MAX_FRAG_SIZE ||
            n < (ssize_t)(sizeof(avx_udp_header_t) + hdr->payload_len)) {
            continue;
        }

        /* Check for new frame */
        if (total_frags == 0 || hdr->frame_seq > curr_frame_seq) {
            curr_frame_seq = hdr->frame_seq;
            total_frags = hdr->frag_count;
            received_frags = 0;
            assembled_bytes = 0;
            memset(received_bitmap, 0, sizeof(received_bitmap));

            if (out_fourcc) *out_fourcc = hdr->fourcc;
            if (out_timecode) *out_timecode = hdr->timecode;
        }

        /* If this fragment belongs to current frame and not yet received */
        if (hdr->frame_seq == curr_frame_seq && hdr->frag_idx < total_frags) {
            int byte_idx = hdr->frag_idx / 8;
            int bit_idx = hdr->frag_idx % 8;

            if (!(received_bitmap[byte_idx] & (1 << bit_idx))) {
                received_bitmap[byte_idx] |= (1 << bit_idx);
                received_frags++;

                size_t dest_offset = (size_t)hdr->frag_idx * AVX_UDP_MAX_FRAG_SIZE;
                if (dest_offset + hdr->payload_len <= out_max) {
                    memcpy((uint8_t*)out_buf + dest_offset, packet_buf + sizeof(avx_udp_header_t), hdr->payload_len);
                    if (dest_offset + hdr->payload_len > assembled_bytes) {
                        assembled_bytes = dest_offset + hdr->payload_len;
                    }
                }

                /* Check if entire frame is complete */
                if (received_frags == total_frags) {
                    *out_size = assembled_bytes;
                    return 0;
                }
            }
        }
#endif
    }
}

/* ========================================================================= */
/* RELIABLE UDP (RUDP) TRANSPORT WITH SELECTIVE NACK ARQ RETRANSMISSION      */
/* ========================================================================= */

typedef struct {
    uint32_t frame_seq;
    uint32_t fourcc;
    int64_t  timecode;
    uint16_t frag_count;
    size_t   payload_len;
    uint8_t *payload;
    bool     valid;
} avx_udp_cached_frame_t;

struct avx_udp_rudp_sender_t {
    avx_socket_t sock;
    avx_mutex_t lock;
    int max_cached;
    int head_idx;
    avx_udp_cached_frame_t* cache;
    avx_fec_config_t fec_cfg;
    avx_crypto_ctx_t crypto;

    /* Live Telemetry & RTT tracking (RFC 6298 filter) */
    uint32_t srtt_ms;
    uint32_t rttvar_ms;
    uint64_t packets_sent;
    uint64_t packets_recv;
    uint64_t packets_retransmitted;
    uint64_t packets_recovered;
    uint64_t packets_lost;
};

int avx_udp_rudp_sender_set_passphrase(avx_udp_rudp_sender_t* s, const char *passphrase, const char *stream_name) {
    if (!s) return -1;
    avx_mutex_lock(&s->lock);
    int ret = avx_crypto_init(&s->crypto, passphrase, stream_name);
    avx_mutex_unlock(&s->lock);
    return ret;
}

void avx_udp_rudp_sender_set_fec(avx_udp_rudp_sender_t* s, const avx_fec_config_t* cfg) {
    if (!s) return;
    avx_mutex_lock(&s->lock);
    if (cfg) {
        s->fec_cfg.enabled = cfg->enabled;
        s->fec_cfg.matrix_d = (cfg->matrix_d > 0 && cfg->matrix_d <= AVX_FEC_MAX_COLS) ? cfg->matrix_d : 10;
        s->fec_cfg.matrix_l = (cfg->matrix_l > 0 && cfg->matrix_l <= AVX_FEC_MAX_ROWS) ? cfg->matrix_l : 5;
        s->fec_cfg.enable_row_fec = cfg->enable_row_fec;
    } else {
        memset(&s->fec_cfg, 0, sizeof(s->fec_cfg));
    }
    avx_mutex_unlock(&s->lock);
}

avx_udp_rudp_sender_t* avx_udp_rudp_sender_create(avx_socket_t sock, int max_cached_frames) {
    if (sock == AVX_INVALID_SOCKET) return NULL;
    if (max_cached_frames <= 0) max_cached_frames = 16;
    if (max_cached_frames > 128) max_cached_frames = 128;

    avx_udp_rudp_sender_t* s = (avx_udp_rudp_sender_t*)calloc(1, sizeof(avx_udp_rudp_sender_t));
    if (!s) return NULL;

    s->sock = sock;
    s->max_cached = max_cached_frames;
    s->head_idx = 0;
    s->srtt_ms = 20;
    s->rttvar_ms = 10;
    s->fec_cfg.enabled = false;
    s->fec_cfg.matrix_d = 10;
    s->fec_cfg.matrix_l = 5;
    s->fec_cfg.enable_row_fec = false;
    avx_mutex_init(&s->lock);

    s->cache = (avx_udp_cached_frame_t*)calloc((size_t)s->max_cached, sizeof(avx_udp_cached_frame_t));
    if (!s->cache) {
        avx_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    return s;
}

void avx_udp_rudp_sender_destroy(avx_udp_rudp_sender_t* s) {
    if (!s) return;
    avx_mutex_lock(&s->lock);
    if (s->cache) {
        for (int i = 0; i < s->max_cached; ++i) {
            if (s->cache[i].payload) {
                free(s->cache[i].payload);
                s->cache[i].payload = NULL;
            }
        }
        free(s->cache);
        s->cache = NULL;
    }
    avx_crypto_wipe(&s->crypto);
    avx_mutex_unlock(&s->lock);
    avx_mutex_destroy(&s->lock);
    free(s);
}

/*
 * Ingests receiver ACK/NACK feedback non-blockingly. Updates smoothed RTT
 * via Jacobson/Karels filter and retransmits lost packets from frame cache.
 */
int avx_udp_rudp_sender_service_nacks(avx_udp_rudp_sender_t* s) {
    if (!s || s->sock == AVX_INVALID_SOCKET) return -1;

    int retransmitted_total = 0;
    const oavx_wire_profile_t *prof = avx_wire_get_active_profile();
    uint32_t udp_magic = (prof && prof->magic_udp) ? prof->magic_udp : OAVX_UDP_MAGIC;

    while (1) {
        avx_pollfd_t pfd = { .fd = s->sock, .events = POLLIN, .revents = 0 };
        int pr = avx_poll(&pfd, 1, 0); /* Non-blocking poll */
        if (pr <= 0 || !(pfd.revents & POLLIN)) {
            break;
        }

        uint8_t nack_buf[sizeof(avx_udp_nack_t) + 64];
        struct sockaddr_storage from_addr;
        socklen_t from_len = sizeof(from_addr);
        memset(&from_addr, 0, sizeof(from_addr));

        ssize_t n = recvfrom(s->sock, (char*)nack_buf, sizeof(nack_buf), 0,
                             (struct sockaddr*)&from_addr, &from_len);
        if (n <= 0) {
            break;
        }
        if (n < 4) {
            continue;
        }

        uint32_t pkt_magic = 0;
        memcpy(&pkt_magic, nack_buf, 4);

        if (pkt_magic == AVX_UDP_ACK_MAGIC) {
            if (n >= (ssize_t)sizeof(avx_udp_feedback_t)) {
                const avx_udp_feedback_t *fb = (const avx_udp_feedback_t*)nack_buf;
                avx_mutex_lock(&s->lock);
                s->packets_recv++;
                if (fb->echo_ts_ms > 0) {
                    uint16_t now_ms = (uint16_t)(avx_time_ms() & 0xFFFF);
                    uint16_t delay = fb->rx_delay_ms;
                    int32_t raw_rtt = (int32_t)((now_ms - fb->echo_ts_ms - delay) & 0xFFFF);
                    if (raw_rtt >= 0 && raw_rtt < 10000) {
                        if (s->srtt_ms == 0) {
                            s->srtt_ms = (uint32_t)raw_rtt;
                            s->rttvar_ms = (uint32_t)(raw_rtt / 2);
                        } else {
                            int32_t delta = raw_rtt - (int32_t)s->srtt_ms;
                            if (delta < 0) delta = -delta;
                            s->rttvar_ms = (3 * s->rttvar_ms + (uint32_t)delta) / 4;
                            s->srtt_ms = (7 * s->srtt_ms + (uint32_t)raw_rtt) / 8;
                        }
                        if (s->srtt_ms < 1) s->srtt_ms = 1;
                    }
                }
                avx_mutex_unlock(&s->lock);
            }
            continue;
        }

        if (pkt_magic != AVX_UDP_NACK_MAGIC || n < (ssize_t)sizeof(avx_udp_nack_t)) {
            continue;
        }

        avx_udp_nack_t *nack = (avx_udp_nack_t*)nack_buf;
        avx_mutex_lock(&s->lock);
        s->packets_recv++;
        if (nack->echo_ts_ms > 0) {
            uint16_t now_ms = (uint16_t)(avx_time_ms() & 0xFFFF);
            uint16_t delay = nack->rx_delay_ms;
            int32_t raw_rtt = (int32_t)((now_ms - nack->echo_ts_ms - delay) & 0xFFFF);
            if (raw_rtt >= 0 && raw_rtt < 10000) {
                if (s->srtt_ms == 0) {
                    s->srtt_ms = (uint32_t)raw_rtt;
                    s->rttvar_ms = (uint32_t)(raw_rtt / 2);
                } else {
                    int32_t delta = raw_rtt - (int32_t)s->srtt_ms;
                    if (delta < 0) delta = -delta;
                    s->rttvar_ms = (3 * s->rttvar_ms + (uint32_t)delta) / 4;
                    s->srtt_ms = (7 * s->srtt_ms + (uint32_t)raw_rtt) / 8;
                }
                if (s->srtt_ms < 1) s->srtt_ms = 1;
            }
        }

        const avx_udp_cached_frame_t* found = NULL;
        for (int i = 0; i < s->max_cached; ++i) {
            if (s->cache[i].valid && s->cache[i].frame_seq == nack->frame_seq) {
                found = &s->cache[i];
                break;
            }
        }

        if (!found) {
            avx_mutex_unlock(&s->lock);
            continue;
        }

        uint16_t rc = nack->range_count;
        if (rc > AVX_UDP_NACK_MAX_RANGES) rc = AVX_UDP_NACK_MAX_RANGES;

        for (uint16_t r = 0; r < rc; ++r) {
            uint16_t start = nack->ranges[r].start;
            uint16_t count = nack->ranges[r].count;

            for (uint16_t f = start; f < start + count && f < found->frag_count; ++f) {
                size_t offset = (size_t)f * AVX_UDP_MAX_FRAG_SIZE;
                size_t chunk_len = (offset < found->payload_len) ? (found->payload_len - offset) : 0;
                if (chunk_len > AVX_UDP_MAX_FRAG_SIZE) chunk_len = AVX_UDP_MAX_FRAG_SIZE;

                avx_udp_header_t hdr;
                hdr.magic = udp_magic;
                hdr.frame_seq = found->frame_seq;
                hdr.frag_idx = f;
                hdr.frag_count = found->frag_count;
                hdr.fourcc = found->fourcc;
                hdr.timecode = found->timecode;
                hdr.payload_len = (uint16_t)chunk_len;
                hdr.send_ts_ms = (uint16_t)(avx_time_ms() & 0xFFFF);
                hdr.srtt_ms = (uint16_t)s->srtt_ms;
                hdr.reserved = 0;

                uint8_t packet_buf[sizeof(avx_udp_header_t) + AVX_UDP_MAX_FRAG_SIZE + AVX_CRYPTO_OVERHEAD];
                size_t packet_len = 0;

                if (s->crypto.enabled) {
                    hdr.reserved |= AVX_UDP_FLAG_ENCRYPTED;
                    size_t enc_len = sizeof(packet_buf) - sizeof(hdr);
                    if (avx_crypto_encrypt_payload(&s->crypto, hdr.frame_seq, hdr.frag_idx, hdr.frag_count,
                                                   &hdr, sizeof(hdr),
                                                   found->payload + offset, chunk_len,
                                                   packet_buf + sizeof(hdr), &enc_len) == 0) {
                        memcpy(packet_buf, &hdr, sizeof(hdr));
                        packet_len = sizeof(hdr) + enc_len;
                    }
                } else {
                    memcpy(packet_buf, &hdr, sizeof(hdr));
                    if (chunk_len > 0) {
                        memcpy(packet_buf + sizeof(hdr), found->payload + offset, chunk_len);
                    }
                    packet_len = sizeof(hdr) + chunk_len;
                }

                if (packet_len > 0) {
                    ssize_t sent = sendto(s->sock, (const char*)packet_buf, packet_len, 0,
                                          (const struct sockaddr*)&from_addr, from_len);
                    if (sent > 0) {
                        retransmitted_total++;
                        s->packets_retransmitted++;
                    }
                }
            }
        }
        avx_mutex_unlock(&s->lock);
    }

    return retransmitted_total;
}

/*
 * Emits video/audio frame over UDP: services pending NACKs, caches frame into
 * retransmission ring, fragments into MTU datagrams, and generates 2D FEC parity.
 */
int avx_udp_rudp_send_frame(avx_udp_rudp_sender_t* s, const void *dest,
                            uint32_t frame_seq, uint32_t fourcc, int64_t timecode,
                            const void *payload, size_t payload_len)
{
    if (!s || s->sock == AVX_INVALID_SOCKET || !dest || (!payload && payload_len > 0)) return -1;

    uint16_t frag_count = 0;
    if (avx_udp_calc_frag_count(payload_len, &frag_count) != 0) {
        return -1;
    }

    /* 1. Service any pending NACK requests first */
    avx_udp_rudp_sender_service_nacks(s);

    /* 2. Cache frame in circulating retransmission ring */
    avx_mutex_lock(&s->lock);
    int slot = s->head_idx;
    s->head_idx = (s->head_idx + 1) % s->max_cached;

    avx_udp_cached_frame_t* c = &s->cache[slot];
    uint8_t* nb = (uint8_t*)realloc(c->payload, payload_len > 0 ? payload_len : 1);
    if (nb) {
        c->payload = nb;
        if (payload && payload_len > 0) {
            memcpy(c->payload, payload, payload_len);
        }
        c->payload_len = payload_len;
        c->frame_seq = frame_seq;
        c->fourcc = fourcc;
        c->timecode = timecode;
        c->frag_count = frag_count;
        c->valid = true;
    }
    uint16_t cur_srtt = (uint16_t)s->srtt_ms;
    avx_fec_config_t fec_cfg = s->fec_cfg;
    avx_mutex_unlock(&s->lock);

    /* 3. Send out datagram bursts with timestamp and SRTT */
    uint16_t send_ts = (uint16_t)(avx_time_ms() & 0xFFFF);
    int ret = avx_udp_send_frame_internal(s->sock, dest, frame_seq, fourcc, timecode, payload, payload_len,
                                          send_ts, cur_srtt, s->crypto.enabled ? &s->crypto : NULL);
    if (ret == 0) {
        avx_mutex_lock(&s->lock);
        s->packets_sent += frag_count;
        avx_mutex_unlock(&s->lock);

        /* 3b. If Forward Error Correction (FEC) is enabled, generate & transmit parity datagrams */
        if (fec_cfg.enabled && frag_count > 0) {
            uint16_t D = fec_cfg.matrix_d ? fec_cfg.matrix_d : 10;
            uint16_t L = fec_cfg.matrix_l ? fec_cfg.matrix_l : 5;
            uint16_t block_size = (uint16_t)(D * L);
            uint16_t total_blocks = (uint16_t)((frag_count + block_size - 1) / block_size);

            struct sockaddr_storage prepared;
            const struct sockaddr *dest_sa = NULL;
            socklen_t dest_len = avx_udp_prepare_dest_addr(s->sock, dest, &prepared, &dest_sa);

            if (dest_len > 0 && dest_sa) {
                const oavx_wire_profile_t *prof = avx_wire_get_active_profile();
                uint32_t udp_magic = (prof && prof->magic_udp) ? prof->magic_udp : OAVX_UDP_MAGIC;

                for (uint16_t b = 0; b < total_blocks; ++b) {
                    uint16_t base_frag_idx = (uint16_t)(b * block_size);
                    uint16_t block_frags = (uint16_t)(frag_count - base_frag_idx);
                    if (block_frags > block_size) block_frags = block_size;

                    avx_fec_slice_t media_slices[AVX_FEC_MAX_BLOCK_FRAGS];
                    for (uint16_t k = 0; k < block_frags; ++k) {
                        uint16_t global_frag = (uint16_t)(base_frag_idx + k);
                        size_t offset = (size_t)global_frag * AVX_UDP_MAX_FRAG_SIZE;
                        size_t chunk_len = (offset < payload_len) ? (payload_len - offset) : 0;
                        if (chunk_len > AVX_UDP_MAX_FRAG_SIZE) chunk_len = AVX_UDP_MAX_FRAG_SIZE;
                        media_slices[k].data = (uint8_t*)payload + offset;
                        media_slices[k].len = chunk_len;
                    }

                    uint8_t col_data[AVX_FEC_MAX_COLS][AVX_UDP_MAX_FRAG_SIZE];
                    avx_fec_slice_t col_slices[AVX_FEC_MAX_COLS];
                    for (uint16_t j = 0; j < D; ++j) {
                        col_slices[j].data = col_data[j];
                        col_slices[j].len = 0;
                    }

                    uint8_t row_data[AVX_FEC_MAX_ROWS][AVX_UDP_MAX_FRAG_SIZE];
                    avx_fec_slice_t row_slices[AVX_FEC_MAX_ROWS];
                    if (fec_cfg.enable_row_fec) {
                        for (uint16_t i = 0; i < L; ++i) {
                            row_slices[i].data = row_data[i];
                            row_slices[i].len = 0;
                        }
                    }

                    if (avx_fec_generate_block(media_slices, block_frags, D, L, fec_cfg.enable_row_fec,
                                               col_slices, fec_cfg.enable_row_fec ? row_slices : NULL) == 0) {
                        uint16_t last_frag_len = (uint16_t)media_slices[block_frags - 1].len;
                        uint8_t fec_pkt[sizeof(avx_udp_header_t) + sizeof(avx_fec_wire_header_t) + AVX_UDP_MAX_FRAG_SIZE + AVX_CRYPTO_OVERHEAD];

                        /* Transmit Column Parity Packets ('FEC1') */
                        for (uint16_t j = 0; j < D; ++j) {
                            if (col_slices[j].len == 0) continue;

                            avx_udp_header_t *udp_hdr = (avx_udp_header_t*)fec_pkt;
                            udp_hdr->magic = udp_magic;
                            udp_hdr->frame_seq = frame_seq;
                            udp_hdr->frag_idx = j;
                            udp_hdr->frag_count = frag_count;
                            udp_hdr->fourcc = OAVX_FOURCC_FEC_COLUMN;
                            udp_hdr->timecode = timecode;
                            udp_hdr->payload_len = (uint16_t)(sizeof(avx_fec_wire_header_t) + col_slices[j].len);
                            udp_hdr->send_ts_ms = send_ts;
                            udp_hdr->srtt_ms = cur_srtt;
                            udp_hdr->reserved = 0;

                            avx_fec_wire_header_t fec_hdr;
                            fec_hdr.media_fourcc = fourcc;
                            fec_hdr.matrix_d = D;
                            fec_hdr.matrix_l = L;
                            fec_hdr.block_idx = b;
                            fec_hdr.parity_idx = j;
                            fec_hdr.base_frag_idx = base_frag_idx;
                            fec_hdr.block_frags = block_frags;
                            fec_hdr.parity_len = (uint16_t)col_slices[j].len;
                            fec_hdr.last_frag_len = last_frag_len;

                            uint8_t raw_fec[sizeof(avx_fec_wire_header_t) + AVX_UDP_MAX_FRAG_SIZE];
                            memcpy(raw_fec, &fec_hdr, sizeof(fec_hdr));
                            memcpy(raw_fec + sizeof(fec_hdr), col_slices[j].data, col_slices[j].len);
                            size_t raw_fec_len = sizeof(fec_hdr) + col_slices[j].len;

                            size_t send_fec_len = 0;
                            if (s->crypto.enabled) {
                                udp_hdr->reserved |= AVX_UDP_FLAG_ENCRYPTED;
                                size_t enc_len = sizeof(fec_pkt) - sizeof(avx_udp_header_t);
                                if (avx_crypto_encrypt_payload(&s->crypto, frame_seq, j, frag_count,
                                                               udp_hdr, sizeof(avx_udp_header_t),
                                                               raw_fec, raw_fec_len,
                                                               fec_pkt + sizeof(avx_udp_header_t), &enc_len) == 0) {
                                    send_fec_len = sizeof(avx_udp_header_t) + enc_len;
                                }
                            } else {
                                memcpy(fec_pkt + sizeof(avx_udp_header_t), raw_fec, raw_fec_len);
                                send_fec_len = sizeof(avx_udp_header_t) + raw_fec_len;
                            }

                            if (send_fec_len > 0) {
                                if (sendto(s->sock, (const char*)fec_pkt, send_fec_len, 0, dest_sa, dest_len) > 0) {
                                    avx_mutex_lock(&s->lock);
                                    s->packets_sent++;
                                    avx_mutex_unlock(&s->lock);
                                }
                            }
                        }

                        /* Transmit Row Parity Packets ('FEC2') if enabled */
                        if (fec_cfg.enable_row_fec) {
                            for (uint16_t i = 0; i < L; ++i) {
                                if (row_slices[i].len == 0) continue;

                                avx_udp_header_t *udp_hdr = (avx_udp_header_t*)fec_pkt;
                                udp_hdr->magic = udp_magic;
                                udp_hdr->frame_seq = frame_seq;
                                udp_hdr->frag_idx = i;
                                udp_hdr->frag_count = frag_count;
                                udp_hdr->fourcc = OAVX_FOURCC_FEC_ROW;
                                udp_hdr->timecode = timecode;
                                udp_hdr->payload_len = (uint16_t)(sizeof(avx_fec_wire_header_t) + row_slices[i].len);
                                udp_hdr->send_ts_ms = send_ts;
                                udp_hdr->srtt_ms = cur_srtt;
                                udp_hdr->reserved = 0;

                                avx_fec_wire_header_t fec_hdr;
                                fec_hdr.media_fourcc = fourcc;
                                fec_hdr.matrix_d = D;
                                fec_hdr.matrix_l = L;
                                fec_hdr.block_idx = b;
                                fec_hdr.parity_idx = i;
                                fec_hdr.base_frag_idx = base_frag_idx;
                                fec_hdr.block_frags = block_frags;
                                fec_hdr.parity_len = (uint16_t)row_slices[i].len;
                                fec_hdr.last_frag_len = last_frag_len;

                                uint8_t raw_fec[sizeof(avx_fec_wire_header_t) + AVX_UDP_MAX_FRAG_SIZE];
                                memcpy(raw_fec, &fec_hdr, sizeof(fec_hdr));
                                memcpy(raw_fec + sizeof(fec_hdr), row_slices[i].data, row_slices[i].len);
                                size_t raw_fec_len = sizeof(fec_hdr) + row_slices[i].len;

                                size_t send_fec_len = 0;
                                if (s->crypto.enabled) {
                                    udp_hdr->reserved |= AVX_UDP_FLAG_ENCRYPTED;
                                    size_t enc_len = sizeof(fec_pkt) - sizeof(avx_udp_header_t);
                                    if (avx_crypto_encrypt_payload(&s->crypto, frame_seq, i, frag_count,
                                                                   udp_hdr, sizeof(avx_udp_header_t),
                                                                   raw_fec, raw_fec_len,
                                                                   fec_pkt + sizeof(avx_udp_header_t), &enc_len) == 0) {
                                        send_fec_len = sizeof(avx_udp_header_t) + enc_len;
                                    }
                                } else {
                                    memcpy(fec_pkt + sizeof(avx_udp_header_t), raw_fec, raw_fec_len);
                                    send_fec_len = sizeof(avx_udp_header_t) + raw_fec_len;
                                }

                                if (send_fec_len > 0) {
                                    if (sendto(s->sock, (const char*)fec_pkt, send_fec_len, 0, dest_sa, dest_len) > 0) {
                                        avx_mutex_lock(&s->lock);
                                        s->packets_sent++;
                                        avx_mutex_unlock(&s->lock);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* 4. Check for immediate NACKs before returning */
    avx_udp_rudp_sender_service_nacks(s);

    return ret;
}

#define AVX_RUDP_RX_SLOTS        64
#define AVX_RUDP_MAX_RETRIES      8
#define AVX_RUDP_NACK_COOLDOWN_NS 20000000LL /* 20 ms minimum floor */
#define AVX_RUDP_REORDER_GRACE_NS  3000000LL /* 3 ms minimum floor */

typedef struct {
    uint16_t matrix_d;
    uint16_t matrix_l;
    uint16_t base_frag_idx;
    uint16_t block_frags;
    uint16_t last_frag_len;
    bool     col_present[AVX_FEC_MAX_COLS];
    uint16_t col_len[AVX_FEC_MAX_COLS];
    uint8_t *col_parity[AVX_FEC_MAX_COLS];
    bool     row_present[AVX_FEC_MAX_ROWS];
    uint16_t row_len[AVX_FEC_MAX_ROWS];
    uint8_t *row_parity[AVX_FEC_MAX_ROWS];
} avx_rudp_fec_block_t;

typedef struct {
    int num_blocks;
    avx_rudp_fec_block_t *blocks;
} avx_rudp_fec_frame_t;

typedef struct {
    bool     active;
    bool     completed;
    uint32_t frame_seq;
    uint32_t fourcc;
    int64_t  timecode;
    uint16_t total_frags;
    uint16_t received_frags;
    uint16_t highest_frag_seen;
    uint8_t  received_bitmap[AVX_UDP_MAX_FRAGS / 8];
    uint8_t  nack_retries[AVX_UDP_MAX_FRAGS];
    int64_t  first_rx_ns;
    int64_t  last_frag_ns;
    int64_t  last_nack_ns;
    uint8_t *buf;
    size_t   buf_cap;
    size_t   assembled_bytes;
    avx_rudp_fec_frame_t *fec;
} avx_rudp_rx_slot_t;

struct avx_udp_rudp_receiver_t {
    avx_socket_t       sock;
    avx_mutex_t        lock;
    avx_rudp_rx_slot_t slots[AVX_RUDP_RX_SLOTS];
    bool               has_delivered[2];
    uint32_t           delivered_head[2];
    uint64_t           delivered_mask[2];
    avx_fec_config_t   fec_cfg;
    avx_crypto_ctx_t   crypto;

    /* WAN RTT, Jitter and Telemetry State */
    uint16_t           latest_sender_ts_ms;
    int64_t            latest_sender_rx_ns;
    uint32_t           srtt_ms;
    uint32_t           rttvar_ms;
    int64_t            last_feedback_ns;
    uint64_t           packets_recv;
    uint64_t           packets_recovered;
    uint64_t           packets_lost;
};

int avx_udp_rudp_receiver_set_passphrase(avx_udp_rudp_receiver_t* r, const char *passphrase, const char *stream_name) {
    if (!r) return -1;
    avx_mutex_lock(&r->lock);
    int ret = avx_crypto_init(&r->crypto, passphrase, stream_name);
    avx_mutex_unlock(&r->lock);
    return ret;
}

static void rudp_rx_slot_free_fec(avx_rudp_rx_slot_t *s) {
    if (!s || !s->fec) return;
    if (s->fec->blocks) {
        for (int b = 0; b < s->fec->num_blocks; ++b) {
            avx_rudp_fec_block_t *block = &s->fec->blocks[b];
            for (int j = 0; j < AVX_FEC_MAX_COLS; ++j) {
                if (block->col_parity[j]) {
                    free(block->col_parity[j]);
                    block->col_parity[j] = NULL;
                }
            }
            for (int i = 0; i < AVX_FEC_MAX_ROWS; ++i) {
                if (block->row_parity[i]) {
                    free(block->row_parity[i]);
                    block->row_parity[i] = NULL;
                }
            }
        }
        free(s->fec->blocks);
        s->fec->blocks = NULL;
    }
    free(s->fec);
    s->fec = NULL;
}

/*
 * Reconstructs lost media packets using 2D matrix XOR erasure decoding
 * (column/row parity) before resorting to network NACK retransmissions.
 */
static void rudp_try_fec_reconstruct(avx_udp_rudp_receiver_t* r, avx_rudp_rx_slot_t* s) {
    if (!r || !s || !s->active || s->completed || !s->fec || !s->buf) return;

    for (int b = 0; b < s->fec->num_blocks; ++b) {
        avx_rudp_fec_block_t *block = &s->fec->blocks[b];
        if (block->block_frags == 0 || block->matrix_d == 0 || block->matrix_l == 0) continue;

        int missing_in_block = 0;
        for (uint16_t k = 0; k < block->block_frags; ++k) {
            uint16_t f = (uint16_t)(block->base_frag_idx + k);
            if (f >= s->total_frags) break;
            if (!(s->received_bitmap[f / 8] & (1 << (f % 8)))) {
                missing_in_block++;
            }
        }
        if (missing_in_block == 0) continue;

        avx_fec_slice_t media_slices[AVX_FEC_MAX_BLOCK_FRAGS];
        bool is_present[AVX_FEC_MAX_BLOCK_FRAGS];
        memset(is_present, 0, sizeof(is_present));

        for (uint16_t k = 0; k < block->block_frags; ++k) {
            uint16_t f = (uint16_t)(block->base_frag_idx + k);
            if (f >= s->total_frags) break;
            media_slices[k].data = s->buf + (size_t)f * AVX_UDP_MAX_FRAG_SIZE;
            bool pres = (s->received_bitmap[f / 8] & (1 << (f % 8))) != 0;
            is_present[k] = pres;
            if (pres) {
                if (f == s->total_frags - 1 && block->last_frag_len > 0) {
                    media_slices[k].len = block->last_frag_len;
                } else {
                    media_slices[k].len = AVX_UDP_MAX_FRAG_SIZE;
                }
            } else {
                media_slices[k].len = 0;
            }
        }

        avx_fec_slice_t col_slices[AVX_FEC_MAX_COLS];
        for (uint16_t j = 0; j < block->matrix_d; ++j) {
            col_slices[j].data = block->col_parity[j];
            col_slices[j].len = block->col_present[j] ? block->col_len[j] : 0;
        }

        avx_fec_slice_t row_slices[AVX_FEC_MAX_ROWS];
        for (uint16_t i = 0; i < block->matrix_l; ++i) {
            row_slices[i].data = block->row_parity[i];
            row_slices[i].len = block->row_present[i] ? block->row_len[i] : 0;
        }

        int recovered = avx_fec_reconstruct_block(
            media_slices,
            is_present,
            block->block_frags,
            block->matrix_d,
            block->matrix_l,
            col_slices,
            block->col_present,
            row_slices,
            block->row_present
        );

        if (recovered > 0) {
            for (uint16_t k = 0; k < block->block_frags; ++k) {
                uint16_t f = (uint16_t)(block->base_frag_idx + k);
                if (f >= s->total_frags) break;
                if (!(s->received_bitmap[f / 8] & (1 << (f % 8))) && is_present[k]) {
                    s->received_bitmap[f / 8] |= (uint8_t)(1 << (f % 8));
                    s->received_frags++;
                    r->packets_recovered++;
                    if (f > s->highest_frag_seen) {
                        s->highest_frag_seen = f;
                    }
                    size_t frag_len = media_slices[k].len;
                    if (f == s->total_frags - 1 && block->last_frag_len > 0) {
                        frag_len = block->last_frag_len;
                    }
                    size_t dest_offset = (size_t)f * AVX_UDP_MAX_FRAG_SIZE;
                    if (dest_offset + frag_len > s->assembled_bytes) {
                        s->assembled_bytes = dest_offset + frag_len;
                    }
                }
            }
            if (s->received_frags == s->total_frags) {
                s->completed = true;
            }
        }
    }
}

void avx_udp_rudp_receiver_set_fec(avx_udp_rudp_receiver_t* r, const avx_fec_config_t* cfg) {
    if (!r) return;
    avx_mutex_lock(&r->lock);
    if (cfg) {
        r->fec_cfg.enabled = cfg->enabled;
        r->fec_cfg.matrix_d = (cfg->matrix_d > 0 && cfg->matrix_d <= AVX_FEC_MAX_COLS) ? cfg->matrix_d : 10;
        r->fec_cfg.matrix_l = (cfg->matrix_l > 0 && cfg->matrix_l <= AVX_FEC_MAX_ROWS) ? cfg->matrix_l : 5;
        r->fec_cfg.enable_row_fec = cfg->enable_row_fec;
    } else {
        memset(&r->fec_cfg, 0, sizeof(r->fec_cfg));
    }
    avx_mutex_unlock(&r->lock);
}

avx_udp_rudp_receiver_t* avx_udp_rudp_receiver_create(avx_socket_t sock) {
    if (sock == AVX_INVALID_SOCKET) return NULL;
    avx_udp_rudp_receiver_t* r = (avx_udp_rudp_receiver_t*)calloc(1, sizeof(avx_udp_rudp_receiver_t));
    if (!r) return NULL;
    r->sock = sock;
    r->srtt_ms = 20;
    r->rttvar_ms = 10;
    r->fec_cfg.enabled = true;
    r->fec_cfg.matrix_d = 10;
    r->fec_cfg.matrix_l = 5;
    r->fec_cfg.enable_row_fec = false;
    avx_mutex_init(&r->lock);
    return r;
}

void avx_udp_rudp_receiver_destroy(avx_udp_rudp_receiver_t* r) {
    if (!r) return;
    avx_mutex_lock(&r->lock);
    for (int i = 0; i < AVX_RUDP_RX_SLOTS; ++i) {
        rudp_rx_slot_free_fec(&r->slots[i]);
        if (r->slots[i].buf) {
            free(r->slots[i].buf);
            r->slots[i].buf = NULL;
            r->slots[i].buf_cap = 0;
        }
    }
    avx_crypto_wipe(&r->crypto);
    avx_mutex_unlock(&r->lock);
    avx_mutex_destroy(&r->lock);
    free(r);
}

static bool rudp_seq_is_newer(uint32_t a, uint32_t b) {
    uint32_t diff = (a - b) & 0x7FFFFFFFu;
    return diff != 0 && (diff & 0x40000000u) == 0;
}

static void rudp_trigger_nack_slot(avx_udp_rudp_receiver_t* r, avx_rudp_rx_slot_t* s,
                                   const void *sender_addr, bool scan_to_end) {
    if (!r || !s || !sender_addr || s->total_frags == 0 || s->completed) return;

    struct sockaddr_storage prepared;
    const struct sockaddr *dest_sa = NULL;
    socklen_t dest_len = avx_udp_prepare_dest_addr(r->sock, sender_addr, &prepared, &dest_sa);
    if (dest_len == 0 || !dest_sa) return;

    int64_t now_ns = avx_time_ns();

    /* Dynamic RTT-adaptive NACK cooldown (1.2 * srtt + 4 * rttvar) with 20ms floor and 500ms ceiling */
    int64_t cooldown_ns = AVX_RUDP_NACK_COOLDOWN_NS;
    if (r->srtt_ms > 0) {
        int64_t adaptive_ns = (int64_t)((1.2f * (float)r->srtt_ms + 4.0f * (float)r->rttvar_ms) * 1000000.0f);
        if (adaptive_ns > cooldown_ns) cooldown_ns = adaptive_ns;
        if (cooldown_ns > 500000000LL) cooldown_ns = 500000000LL;
    }

    if (s->last_nack_ns > 0 && (now_ns - s->last_nack_ns) < cooldown_ns) {
        return;
    }

    avx_udp_nack_t nack;
    memset(&nack, 0, sizeof(nack));
    nack.magic = AVX_UDP_NACK_MAGIC;
    nack.frame_seq = s->frame_seq;
    nack.echo_ts_ms = r->latest_sender_ts_ms;
    if (r->latest_sender_rx_ns > 0 && now_ns >= r->latest_sender_rx_ns) {
        nack.rx_delay_ms = (uint16_t)(((now_ns - r->latest_sender_rx_ns) / 1000000LL) & 0xFFFF);
    } else {
        nack.rx_delay_ms = 0;
    }

    uint16_t scan_limit = scan_to_end
        ? s->total_frags
        : ((s->highest_frag_seen + 1 < s->total_frags) ? (uint16_t)(s->highest_frag_seen + 1) : s->total_frags);

    for (uint16_t f = 0; f < scan_limit && nack.range_count < AVX_UDP_NACK_MAX_RANGES; ++f) {
        int byte_idx = f / 8;
        int bit_idx = f % 8;
        if (!(s->received_bitmap[byte_idx] & (1 << bit_idx))) {
            if (s->nack_retries[f] >= AVX_RUDP_MAX_RETRIES) {
                continue;
            }

            uint16_t range_start = f;
            uint16_t count = 0;
            while (f < scan_limit && !(s->received_bitmap[f / 8] & (1 << (f % 8))) &&
                   s->nack_retries[f] < AVX_RUDP_MAX_RETRIES) {
                s->nack_retries[f]++;
                count++;
                f++;
            }

            nack.ranges[nack.range_count].start = range_start;
            nack.ranges[nack.range_count].count = count;
            nack.range_count++;
        }
    }

    if (nack.range_count > 0) {
        sendto(r->sock, (const char*)&nack, sizeof(nack), 0, dest_sa, dest_len);
        s->last_nack_ns = now_ns;
    }
}

static void rudp_service_nacks(avx_udp_rudp_receiver_t* r, const void *sender_addr, bool poll_idle) {
    if (!r || !sender_addr) return;
    int64_t now_ns = avx_time_ns();

    /* Jitter-adaptive reorder grace period: max(3ms, rttvar) with 30ms ceiling */
    int64_t grace_ns = AVX_RUDP_REORDER_GRACE_NS;
    if (r->rttvar_ms > 0) {
        int64_t var_ns = (int64_t)r->rttvar_ms * 1000000LL;
        if (var_ns > grace_ns) grace_ns = var_ns;
        if (grace_ns > 30000000LL) grace_ns = 30000000LL;
    }

    bool has_newest[2] = { false, false };
    uint32_t newest_seq[2] = { 0, 0 };
    for (int i = 0; i < AVX_RUDP_RX_SLOTS; ++i) {
        avx_rudp_rx_slot_t *s = &r->slots[i];
        if (!s->active) continue;
        int chan = (int)((s->frame_seq >> 31) & 1u);
        uint32_t seq_num = s->frame_seq & 0x7FFFFFFFu;
        if (!has_newest[chan] || rudp_seq_is_newer(seq_num, newest_seq[chan])) {
            newest_seq[chan] = seq_num;
            has_newest[chan] = true;
        }
    }

    bool sent_any_nack = false;
    for (int i = 0; i < AVX_RUDP_RX_SLOTS; ++i) {
        avx_rudp_rx_slot_t *s = &r->slots[i];
        if (!s->active || s->completed || s->total_frags == 0 || s->received_frags >= s->total_frags) {
            continue;
        }
        if (s->fec) {
            rudp_try_fec_reconstruct(r, s);
            if (s->completed || s->received_frags >= s->total_frags) {
                continue;
            }
        }
        int chan = (int)((s->frame_seq >> 31) & 1u);
        uint32_t seq_num = s->frame_seq & 0x7FFFFFFFu;
        bool newer_arrived = has_newest[chan] && rudp_seq_is_newer(newest_seq[chan], seq_num);

        uint16_t seen_window = (uint16_t)(s->highest_frag_seen + 1);
        uint16_t missing_in_window = (seen_window > s->received_frags) ? (uint16_t)(seen_window - s->received_frags) : 0;

        if (missing_in_window == 0 && !newer_arrived && !poll_idle) {
            continue;
        }
        if (!poll_idle && !newer_arrived && missing_in_window <= 2 &&
            (now_ns - s->last_frag_ns) < grace_ns) {
            continue;
        }

        bool scan_to_end = newer_arrived || (poll_idle && (now_ns - s->last_frag_ns) >= grace_ns);
        int64_t pre_nack = s->last_nack_ns;
        rudp_trigger_nack_slot(r, s, sender_addr, scan_to_end);
        if (s->last_nack_ns > pre_nack) {
            sent_any_nack = true;
        }
    }

    /* Send periodic 24-byte feedback packet ('AKXU') every 100 ms if no NACKs were sent */
    if (!sent_any_nack && (now_ns - r->last_feedback_ns) >= 100000000LL) {
        struct sockaddr_storage prepared;
        const struct sockaddr *dest_sa = NULL;
        socklen_t dest_len = avx_udp_prepare_dest_addr(r->sock, sender_addr, &prepared, &dest_sa);
        if (dest_len > 0 && dest_sa) {
            avx_udp_feedback_t fb;
            memset(&fb, 0, sizeof(fb));
            fb.magic = AVX_UDP_ACK_MAGIC;
            fb.last_frame_seq = r->has_delivered[0] ? r->delivered_head[0] : 0;
            fb.echo_ts_ms = r->latest_sender_ts_ms;
            if (r->latest_sender_rx_ns > 0 && now_ns >= r->latest_sender_rx_ns) {
                fb.rx_delay_ms = (uint16_t)(((now_ns - r->latest_sender_rx_ns) / 1000000LL) & 0xFFFF);
            }
            fb.rx_frames = (uint32_t)(r->packets_recv / 64);
            fb.lost_packets = (uint32_t)r->packets_lost;
            fb.rtt_ms = (uint16_t)r->srtt_ms;
            fb.jitter_ms = (uint16_t)r->rttvar_ms;
            sendto(r->sock, (const char*)&fb, sizeof(fb), 0, dest_sa, dest_len);
            r->last_feedback_ns = now_ns;
        }
    }
}

static void rudp_mark_delivered(avx_udp_rudp_receiver_t* r, int chan, uint32_t seq_num) {
    if (!r->has_delivered[chan]) {
        r->has_delivered[chan] = true;
        r->delivered_head[chan] = seq_num;
        r->delivered_mask[chan] = 1ULL;
    } else if (rudp_seq_is_newer(seq_num, r->delivered_head[chan])) {
        uint32_t shift = (seq_num - r->delivered_head[chan]) & 0x7FFFFFFFu;
        r->delivered_mask[chan] = (shift >= 64u) ? 1ULL : ((r->delivered_mask[chan] << shift) | 1ULL);
        r->delivered_head[chan] = seq_num;
    } else {
        uint32_t back = (r->delivered_head[chan] - seq_num) & 0x7FFFFFFFu;
        if (back < 64u) {
            r->delivered_mask[chan] |= (1ULL << back);
        }
    }
}

static bool rudp_was_recently_delivered(const avx_udp_rudp_receiver_t* r, int chan, uint32_t seq_num) {
    if (!r->has_delivered[chan]) return false;
    if (rudp_seq_is_newer(seq_num, r->delivered_head[chan])) return false;
    uint32_t back = (r->delivered_head[chan] - seq_num) & 0x7FFFFFFFu;
    return (back < 64u) && ((r->delivered_mask[chan] & (1ULL << back)) != 0ULL);
}

static bool rudp_deliver_ready_slot(avx_udp_rudp_receiver_t* r, void *out_buf, size_t out_max,
                                    size_t *out_size, uint32_t *out_fourcc, int64_t *out_timecode) {
    int best_idx = -1;
    for (int i = 0; i < AVX_RUDP_RX_SLOTS; ++i) {
        if (!r->slots[i].active || !r->slots[i].completed) continue;
        if (best_idx < 0 || r->slots[i].first_rx_ns < r->slots[best_idx].first_rx_ns) {
            best_idx = i;
        }
    }
    if (best_idx < 0) return false;

    avx_rudp_rx_slot_t *s = &r->slots[best_idx];
    size_t copy_len = (s->assembled_bytes <= out_max) ? s->assembled_bytes : out_max;
    if (copy_len > 0 && s->buf) {
        memcpy(out_buf, s->buf, copy_len);
    }
    *out_size = copy_len;
    if (out_fourcc) *out_fourcc = s->fourcc;
    if (out_timecode) *out_timecode = s->timecode;

    int chan = (int)((s->frame_seq >> 31) & 1u);
    rudp_mark_delivered(r, chan, s->frame_seq & 0x7FFFFFFFu);

    s->active = false;
    s->completed = false;
    rudp_rx_slot_free_fec(s);
    return true;
}

static void rudp_process_fec_packet(avx_udp_rudp_receiver_t* r, const avx_udp_header_t *hdr,
                                    const uint8_t *pkt, size_t pkt_len, size_t out_max,
                                    int64_t now_ns) {
    if (!r->fec_cfg.enabled) return;

    bool is_encrypted = (hdr->reserved & AVX_UDP_FLAG_ENCRYPTED) != 0;
    if (r->crypto.enabled != is_encrypted) {
        return;
    }

    avx_fec_wire_header_t fec_hdr;
    const uint8_t *payload_ptr = NULL;
    uint8_t fec_dec_buf[sizeof(avx_fec_wire_header_t) + AVX_UDP_MAX_FRAG_SIZE];

    if (is_encrypted) {
        if (pkt_len < sizeof(avx_udp_header_t) + AVX_CRYPTO_OVERHEAD + sizeof(avx_fec_wire_header_t)) {
            return;
        }
        size_t dec_len = sizeof(fec_dec_buf);
        if (avx_crypto_decrypt_payload(&r->crypto, hdr->frame_seq, hdr->frag_idx, hdr->frag_count,
                                       hdr, sizeof(*hdr),
                                       pkt + sizeof(avx_udp_header_t),
                                       pkt_len - sizeof(avx_udp_header_t),
                                       fec_dec_buf, &dec_len) != 0 ||
            dec_len < sizeof(avx_fec_wire_header_t)) {
            return;
        }
        memcpy(&fec_hdr, fec_dec_buf, sizeof(fec_hdr));
        payload_ptr = fec_dec_buf + sizeof(avx_fec_wire_header_t);
        if (dec_len < sizeof(avx_fec_wire_header_t) + fec_hdr.parity_len) {
            return;
        }
    } else {
        if (pkt_len < sizeof(avx_udp_header_t) + sizeof(avx_fec_wire_header_t)) return;
        memcpy(&fec_hdr, pkt + sizeof(avx_udp_header_t), sizeof(fec_hdr));
        payload_ptr = pkt + sizeof(avx_udp_header_t) + sizeof(avx_fec_wire_header_t);
        if (pkt_len < sizeof(avx_udp_header_t) + sizeof(avx_fec_wire_header_t) + fec_hdr.parity_len) {
            return;
        }
    }

    if (fec_hdr.matrix_d == 0 || fec_hdr.matrix_d > AVX_FEC_MAX_COLS ||
        fec_hdr.matrix_l == 0 || fec_hdr.matrix_l > AVX_FEC_MAX_ROWS ||
        fec_hdr.block_frags == 0 || fec_hdr.block_frags > AVX_FEC_MAX_BLOCK_FRAGS ||
        fec_hdr.base_frag_idx >= AVX_UDP_MAX_FRAGS ||
        fec_hdr.parity_len == 0 || fec_hdr.parity_len > AVX_UDP_MAX_FRAG_SIZE) {
        return;
    }

    r->packets_recv++;
    r->latest_sender_ts_ms = hdr->send_ts_ms;
    r->latest_sender_rx_ns = now_ns;
    if (hdr->srtt_ms > 0) {
        if (r->srtt_ms == 0) {
            r->srtt_ms = hdr->srtt_ms;
            r->rttvar_ms = hdr->srtt_ms / 2;
        } else {
            r->srtt_ms = (7 * r->srtt_ms + hdr->srtt_ms) / 8;
        }
    }

    int chan = (int)((hdr->frame_seq >> 31) & 1u);
    uint32_t seq_num = hdr->frame_seq & 0x7FFFFFFFu;

    avx_rudp_rx_slot_t *s = NULL;
    int free_idx = -1;
    int oldest_incomplete_idx = -1;

    for (int i = 0; i < AVX_RUDP_RX_SLOTS; ++i) {
        if (r->slots[i].active && r->slots[i].frame_seq == hdr->frame_seq) {
            s = &r->slots[i];
            break;
        }
        if (!r->slots[i].active && free_idx < 0) {
            free_idx = i;
        } else if (r->slots[i].active && !r->slots[i].completed) {
            if (oldest_incomplete_idx < 0 ||
                r->slots[i].first_rx_ns < r->slots[oldest_incomplete_idx].first_rx_ns) {
                oldest_incomplete_idx = i;
            }
        }
    }

    if (!s) {
        if (rudp_was_recently_delivered(r, chan, seq_num)) {
            return;
        }
        int target_idx = (free_idx >= 0) ? free_idx : oldest_incomplete_idx;
        if (target_idx < 0) return;

        s = &r->slots[target_idx];
        rudp_rx_slot_free_fec(s);

        size_t need_cap = (size_t)hdr->frag_count * AVX_UDP_MAX_FRAG_SIZE;
        if (need_cap > out_max) need_cap = out_max;

        if (s->buf_cap < need_cap) {
            uint8_t *nb = (uint8_t*)realloc(s->buf, need_cap);
            if (!nb) return;
            s->buf = nb;
            s->buf_cap = need_cap;
        }

        s->active = true;
        s->completed = false;
        s->frame_seq = hdr->frame_seq;
        s->fourcc = fec_hdr.media_fourcc;
        s->timecode = hdr->timecode;
        s->total_frags = hdr->frag_count;
        s->received_frags = 0;
        s->highest_frag_seen = 0;
        s->first_rx_ns = now_ns;
        s->last_frag_ns = now_ns;
        s->last_nack_ns = 0;
        s->assembled_bytes = 0;
        s->fec = NULL;
        memset(s->received_bitmap, 0, sizeof(s->received_bitmap));
        memset(s->nack_retries, 0, sizeof(s->nack_retries));
    }

    if (s->completed) return;
    if (s->fourcc == 0 || s->fourcc == OAVX_FOURCC_FEC_COLUMN || s->fourcc == OAVX_FOURCC_FEC_ROW) {
        s->fourcc = fec_hdr.media_fourcc;
    }

    if (!s->fec) {
        s->fec = (avx_rudp_fec_frame_t*)calloc(1, sizeof(avx_rudp_fec_frame_t));
    }
    if (!s->fec) return;

    if (fec_hdr.block_idx >= s->fec->num_blocks) {
        int old_n = s->fec->num_blocks;
        int new_n = fec_hdr.block_idx + 1;
        avx_rudp_fec_block_t *nb = (avx_rudp_fec_block_t*)realloc(s->fec->blocks, (size_t)new_n * sizeof(avx_rudp_fec_block_t));
        if (!nb) return;
        memset(nb + old_n, 0, (size_t)(new_n - old_n) * sizeof(avx_rudp_fec_block_t));
        s->fec->blocks = nb;
        s->fec->num_blocks = new_n;
    }

    avx_rudp_fec_block_t *block = &s->fec->blocks[fec_hdr.block_idx];
    block->matrix_d = fec_hdr.matrix_d;
    block->matrix_l = fec_hdr.matrix_l;
    block->base_frag_idx = fec_hdr.base_frag_idx;
    block->block_frags = fec_hdr.block_frags;
    block->last_frag_len = fec_hdr.last_frag_len;

    if (hdr->fourcc == OAVX_FOURCC_FEC_COLUMN) {
        uint16_t j = fec_hdr.parity_idx;
        if (j < fec_hdr.matrix_d) {
            if (!block->col_parity[j]) {
                block->col_parity[j] = (uint8_t*)malloc(AVX_UDP_MAX_FRAG_SIZE);
            }
            if (block->col_parity[j]) {
                memcpy(block->col_parity[j], payload_ptr, fec_hdr.parity_len);
                block->col_len[j] = fec_hdr.parity_len;
                block->col_present[j] = true;
            }
        }
    } else if (hdr->fourcc == OAVX_FOURCC_FEC_ROW) {
        uint16_t i = fec_hdr.parity_idx;
        if (i < fec_hdr.matrix_l) {
            if (!block->row_parity[i]) {
                block->row_parity[i] = (uint8_t*)malloc(AVX_UDP_MAX_FRAG_SIZE);
            }
            if (block->row_parity[i]) {
                memcpy(block->row_parity[i], payload_ptr, fec_hdr.parity_len);
                block->row_len[i] = fec_hdr.parity_len;
                block->row_present[i] = true;
            }
        }
    }

    rudp_try_fec_reconstruct(r, s);
}

static void rudp_process_packet(avx_udp_rudp_receiver_t* r, const uint8_t *pkt, size_t pkt_len,
                                size_t out_max, uint32_t expected_magic, uint32_t compat_magic,
                                int64_t now_ns) {
    if (pkt_len < sizeof(avx_udp_header_t)) return;

    avx_udp_header_t hdr_val;
    memcpy(&hdr_val, pkt, sizeof(hdr_val));
    const avx_udp_header_t *hdr = &hdr_val;

    if ((hdr->magic != expected_magic && (!compat_magic || hdr->magic != compat_magic)) ||
        hdr->frag_count == 0 || hdr->frag_count > AVX_UDP_MAX_FRAGS) {
        return;
    }

    /* Branch immediately for FEC Parity Packets ('FEC1' and 'FEC2') */
    if (hdr->fourcc == OAVX_FOURCC_FEC_COLUMN || hdr->fourcc == OAVX_FOURCC_FEC_ROW) {
        rudp_process_fec_packet(r, hdr, pkt, pkt_len, out_max, now_ns);
        return;
    }

    bool is_encrypted = (hdr->reserved & AVX_UDP_FLAG_ENCRYPTED) != 0;
    if (r->crypto.enabled != is_encrypted) {
        return;
    }

    if (hdr->frag_idx >= hdr->frag_count ||
        hdr->payload_len > AVX_UDP_MAX_FRAG_SIZE) {
        return;
    }

    size_t expected_wire_len = sizeof(avx_udp_header_t) + hdr->payload_len + (is_encrypted ? AVX_CRYPTO_OVERHEAD : 0);
    if (pkt_len < expected_wire_len) {
        return;
    }

    size_t dest_offset = (size_t)hdr->frag_idx * AVX_UDP_MAX_FRAG_SIZE;
    if (dest_offset + hdr->payload_len > out_max) {
        return;
    }

    r->packets_recv++;
    r->latest_sender_ts_ms = hdr->send_ts_ms;
    r->latest_sender_rx_ns = now_ns;
    if (hdr->srtt_ms > 0) {
        if (r->srtt_ms == 0) {
            r->srtt_ms = hdr->srtt_ms;
            r->rttvar_ms = hdr->srtt_ms / 2;
        } else {
            r->srtt_ms = (7 * r->srtt_ms + hdr->srtt_ms) / 8;
        }
    }

    int chan = (int)((hdr->frame_seq >> 31) & 1u);
    uint32_t seq_num = hdr->frame_seq & 0x7FFFFFFFu;

    avx_rudp_rx_slot_t *s = NULL;
    int free_idx = -1;
    int oldest_incomplete_idx = -1;

    for (int i = 0; i < AVX_RUDP_RX_SLOTS; ++i) {
        if (r->slots[i].active && r->slots[i].frame_seq == hdr->frame_seq) {
            s = &r->slots[i];
            break;
        }
        if (!r->slots[i].active && free_idx < 0) {
            free_idx = i;
        } else if (r->slots[i].active && !r->slots[i].completed) {
            if (oldest_incomplete_idx < 0 ||
                r->slots[i].first_rx_ns < r->slots[oldest_incomplete_idx].first_rx_ns) {
                oldest_incomplete_idx = i;
            }
        }
    }

    if (!s) {
        /* Reject late duplicates of frames already delivered, without blocking older in-flight slots */
        if (rudp_was_recently_delivered(r, chan, seq_num)) {
            return;
        }
        int target_idx = (free_idx >= 0) ? free_idx : oldest_incomplete_idx;
        if (target_idx < 0) return;

        s = &r->slots[target_idx];
        rudp_rx_slot_free_fec(s);

        size_t need_cap = (size_t)hdr->frag_count * AVX_UDP_MAX_FRAG_SIZE;
        if (need_cap > out_max) need_cap = out_max;
        if (dest_offset + hdr->payload_len > need_cap) need_cap = dest_offset + hdr->payload_len;

        if (s->buf_cap < need_cap) {
            uint8_t *nb = (uint8_t*)realloc(s->buf, need_cap);
            if (!nb) return;
            s->buf = nb;
            s->buf_cap = need_cap;
        }

        s->active = true;
        s->completed = false;
        s->frame_seq = hdr->frame_seq;
        s->fourcc = hdr->fourcc;
        s->timecode = hdr->timecode;
        s->total_frags = hdr->frag_count;
        s->received_frags = 0;
        s->highest_frag_seen = 0;
        s->first_rx_ns = now_ns;
        s->last_frag_ns = now_ns;
        s->last_nack_ns = 0;
        s->assembled_bytes = 0;
        s->fec = NULL;
        memset(s->received_bitmap, 0, sizeof(s->received_bitmap));
        memset(s->nack_retries, 0, sizeof(s->nack_retries));
    }

    if (s->completed || hdr->frag_idx >= s->total_frags) {
        return;
    }

    int byte_idx = hdr->frag_idx / 8;
    int bit_idx = hdr->frag_idx % 8;
    if (!(s->received_bitmap[byte_idx] & (1 << bit_idx))) {
        if (dest_offset + hdr->payload_len > s->buf_cap) {
            return;
        }
        if (s->nack_retries[hdr->frag_idx] > 0) {
            r->packets_recovered++;
        }
        s->received_bitmap[byte_idx] |= (uint8_t)(1 << bit_idx);
        s->received_frags++;
        if (hdr->frag_idx > s->highest_frag_seen) {
            s->highest_frag_seen = hdr->frag_idx;
        }
        s->last_frag_ns = now_ns;

        uint8_t dec_payload[AVX_UDP_MAX_FRAG_SIZE];
        const uint8_t *plain_slice = pkt + sizeof(avx_udp_header_t);

        if (is_encrypted) {
            size_t dec_len = sizeof(dec_payload);
            if (avx_crypto_decrypt_payload(&r->crypto, hdr->frame_seq, hdr->frag_idx, hdr->frag_count,
                                           hdr, sizeof(*hdr),
                                           pkt + sizeof(avx_udp_header_t),
                                           hdr->payload_len + AVX_CRYPTO_OVERHEAD,
                                           dec_payload, &dec_len) != 0 || dec_len != hdr->payload_len) {
                return;
            }
            plain_slice = dec_payload;
        }

        if (hdr->payload_len > 0) {
            memcpy(s->buf + dest_offset, plain_slice, hdr->payload_len);
        }
        if (dest_offset + hdr->payload_len > s->assembled_bytes) {
            s->assembled_bytes = dest_offset + hdr->payload_len;
        }
        if (s->received_frags == s->total_frags) {
            s->completed = true;
        } else if (s->fec) {
            rudp_try_fec_reconstruct(r, s);
        }
    }
}

/*
 * Ingests datagrams via recvmmsg batching, tracks missing fragments with bitmask,
 * emits selective NACKs, and reassembles completed frames in monotonic sequence order.
 */
int avx_udp_rudp_recv_frame(avx_udp_rudp_receiver_t* r, const void *sender_addr,
                            void *out_buf, size_t out_max,
                            size_t *out_size, uint32_t *out_fourcc, int64_t *out_timecode,
                            int timeout_ms)
{
    if (!r || r->sock == AVX_INVALID_SOCKET || !out_buf || !out_size || !sender_addr) return -1;

    avx_mutex_lock(&r->lock);

    if (rudp_deliver_ready_slot(r, out_buf, out_max, out_size, out_fourcc, out_timecode)) {
        avx_mutex_unlock(&r->lock);
        return 0;
    }

    int64_t start_ms = avx_time_ms();
    int64_t deadline_ms = start_ms + (timeout_ms > 0 ? timeout_ms : 100);

    const oavx_wire_profile_t *prof = avx_wire_get_active_profile();
    uint32_t expected_magic = (prof && prof->magic_udp) ? prof->magic_udp : OAVX_UDP_MAGIC;
    uint32_t compat_magic = (prof) ? prof->compat_magic_udp : 0;

    while (1) {
        int64_t now_ms = avx_time_ms();
        if (now_ms >= deadline_ms) {
            avx_mutex_unlock(&r->lock);
            return -1;
        }

        int poll_timeout = (int)(deadline_ms - now_ms);
        if (poll_timeout > 5) poll_timeout = 5;

        avx_pollfd_t pfd = { .fd = r->sock, .events = POLLIN, .revents = 0 };
        avx_mutex_unlock(&r->lock);
        int pr = avx_poll(&pfd, 1, poll_timeout);
        avx_mutex_lock(&r->lock);

        if (pr <= 0 || !(pfd.revents & POLLIN)) {
            rudp_service_nacks(r, sender_addr, true);
            if (rudp_deliver_ready_slot(r, out_buf, out_max, out_size, out_fourcc, out_timecode)) {
                avx_mutex_unlock(&r->lock);
                return 0;
            }
            continue;
        }

        int64_t now_ns = avx_time_ns();
#if defined(AVX_HAS_MMSG)
        struct mmsghdr msgs[AVX_UDP_BATCH_SIZE];
        struct iovec iovs[AVX_UDP_BATCH_SIZE];
        uint8_t packet_bufs[AVX_UDP_BATCH_SIZE][2048];
        memset(msgs, 0, sizeof(msgs));

        for (int b = 0; b < AVX_UDP_BATCH_SIZE; ++b) {
            iovs[b].iov_base = packet_bufs[b];
            iovs[b].iov_len = sizeof(packet_bufs[b]);
            msgs[b].msg_hdr.msg_iov = &iovs[b];
            msgs[b].msg_hdr.msg_iovlen = 1;
        }

        int num_pkts = recvmmsg(r->sock, msgs, AVX_UDP_BATCH_SIZE, MSG_DONTWAIT, NULL);
        if (num_pkts <= 0) {
            if (num_pkts < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                avx_mutex_unlock(&r->lock);
                return -1;
            }
            ssize_t n = recv(r->sock, (char*)packet_bufs[0], sizeof(packet_bufs[0]), 0);
            if (n <= 0) continue;
            num_pkts = 1;
            msgs[0].msg_len = (unsigned int)n;
        }

        for (int p = 0; p < num_pkts; ++p) {
            rudp_process_packet(r, packet_bufs[p], (size_t)msgs[p].msg_len,
                                out_max, expected_magic, compat_magic, now_ns);
        }
#else
        uint8_t packet_buf[2048];
        ssize_t n = recv(r->sock, (char*)packet_buf, sizeof(packet_buf), 0);
        if (n > 0) {
            rudp_process_packet(r, packet_buf, (size_t)n,
                                out_max, expected_magic, compat_magic, now_ns);
        }
#endif

        rudp_service_nacks(r, sender_addr, false);
        if (rudp_deliver_ready_slot(r, out_buf, out_max, out_size, out_fourcc, out_timecode)) {
            avx_mutex_unlock(&r->lock);
            return 0;
        }
    }
}

bool avx_udp_rudp_sender_get_telemetry(const avx_udp_rudp_sender_t* s, avx_rudp_telemetry_t* out_telem) {
    if (!s || !out_telem) return false;
    avx_udp_rudp_sender_t* mut_s = (avx_udp_rudp_sender_t*)s;
    avx_mutex_lock(&mut_s->lock);
    out_telem->srtt_ms = s->srtt_ms;
    out_telem->rttvar_ms = s->rttvar_ms;
    out_telem->packets_sent = s->packets_sent;
    out_telem->packets_recv = s->packets_recv;
    out_telem->packets_retransmitted = s->packets_retransmitted;
    out_telem->packets_recovered = s->packets_recovered;
    out_telem->packets_lost = s->packets_lost;
    avx_mutex_unlock(&mut_s->lock);
    return true;
}

bool avx_udp_rudp_receiver_get_telemetry(const avx_udp_rudp_receiver_t* r, avx_rudp_telemetry_t* out_telem) {
    if (!r || !out_telem) return false;
    avx_udp_rudp_receiver_t* mut_r = (avx_udp_rudp_receiver_t*)r;
    avx_mutex_lock(&mut_r->lock);
    out_telem->srtt_ms = r->srtt_ms;
    out_telem->rttvar_ms = r->rttvar_ms;
    out_telem->packets_sent = 0;
    out_telem->packets_recv = r->packets_recv;
    out_telem->packets_retransmitted = 0;
    out_telem->packets_recovered = r->packets_recovered;
    out_telem->packets_lost = r->packets_lost;
    avx_mutex_unlock(&mut_r->lock);
    return true;
}
