/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_socket.c - Socket helpers, interface resolution, and pcap utilities
 */

#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include "avx_platform.h"
#include "avx_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(AVX_OS_POSIX)
    #include <netinet/tcp.h>
    #include <sys/ioctl.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #include <netdb.h>
    #if defined(__linux__)
        #include <linux/sockios.h>
    #endif
#endif

/* Resolves network interface name (e.g. eth0) or IP literal to active unicast IPv4/IPv6 address. */
bool avx_net_resolve_interface_or_ip(const char* iface_or_ip, char* out_ip, size_t out_len) {
    if (!iface_or_ip || iface_or_ip[0] == '\0' || !out_ip || out_len == 0) return false;

    /* 1. Direct IPv4 literal validation */
    struct in_addr dummy4;
    if (inet_pton(AF_INET, iface_or_ip, &dummy4) == 1) {
        strncpy(out_ip, iface_or_ip, out_len - 1);
        out_ip[out_len - 1] = '\0';
        return true;
    }

    /* 2. Direct IPv6 literal validation (handles "[::1]" or "::1") */
    const char* ip_ptr = iface_or_ip;
    char unbracketed[128];
    if (iface_or_ip[0] == '[') {
        const char* end = strchr(iface_or_ip, ']');
        if (end) {
            size_t len = (size_t)(end - (iface_or_ip + 1));
            if (len >= sizeof(unbracketed)) len = sizeof(unbracketed) - 1;
            strncpy(unbracketed, iface_or_ip + 1, len);
            unbracketed[len] = '\0';
            ip_ptr = unbracketed;
        }
    }

    struct in6_addr dummy6;
    if (inet_pton(AF_INET6, ip_ptr, &dummy6) == 1) {
        strncpy(out_ip, ip_ptr, out_len - 1);
        out_ip[out_len - 1] = '\0';
        return true;
    }

    /* 3. Match by interface name (e.g., "eth0", "lo", "enp47s0", "wlp46s0") */
#if defined(AVX_OS_WINDOWS) || defined(_WIN32)
    if (strcmp(iface_or_ip, "lo") == 0 || strcasecmp(iface_or_ip, "loopback") == 0 || strcasecmp(iface_or_ip, "localhost") == 0) {
        strncpy(out_ip, "127.0.0.1", out_len - 1);
        out_ip[out_len - 1] = '\0';
        return true;
    }
#endif
#if defined(AVX_OS_POSIX)
    struct ifaddrs *ifaddr = NULL, *ifa = NULL;
    if (getifaddrs(&ifaddr) == -1) {
        return false;
    }

    bool found = false;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || (ifa->ifa_addr->sa_family != AF_INET && ifa->ifa_addr->sa_family != AF_INET6)) {
            continue;
        }
        bool match = (strcmp(ifa->ifa_name, iface_or_ip) == 0);
#if defined(AVX_OS_MACOS) || defined(AVX_OS_BSD)
        if (!match && strcmp(iface_or_ip, "lo") == 0 && strcmp(ifa->ifa_name, "lo0") == 0) {
            match = true;
        }
#endif
        if (match) {
            if (ifa->ifa_addr->sa_family == AF_INET) {
                struct sockaddr_in *s4 = (struct sockaddr_in*)ifa->ifa_addr;
                if (inet_ntop(AF_INET, &s4->sin_addr, out_ip, (socklen_t)out_len) != NULL) {
                    found = true;
                    break;
                }
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                struct sockaddr_in6 *s6 = (struct sockaddr_in6*)ifa->ifa_addr;
                if (!IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr)) {
                    if (inet_ntop(AF_INET6, &s6->sin6_addr, out_ip, (socklen_t)out_len) != NULL) {
                        found = true;
                        break;
                    }
                }
            }
        }
    }
    freeifaddrs(ifaddr);
    return found;
#else
    return false;
#endif
}

/* Discovers local default outbound IP by querying kernel routing table via unconnected UDP probe. */
bool avx_net_get_default_ip(char* out_ip, size_t out_len) {
    if (!out_ip || out_len == 0) return false;
    avx_platform_sockets_init();

    /* 1. Query OS routing table using dummy UDP socket connect to 8.8.8.8 */
    avx_socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock != AVX_INVALID_SOCKET) {
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(53);
        if (inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr) > 0) {
            if (connect(sock, (struct sockaddr*)&dest, sizeof(dest)) == 0) {
                struct sockaddr_in local;
                avx_socklen_t len = sizeof(local);
                if (getsockname(sock, (struct sockaddr*)&local, &len) == 0) {
                    if (local.sin_addr.s_addr != 0 && local.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                        if (inet_ntop(AF_INET, &local.sin_addr, out_ip, (socklen_t)out_len) != NULL) {
                            avx_socket_close(sock);
                            return true;
                        }
                    }
                }
            }
        }
        avx_socket_close(sock);
    }

    /* 2. Fallback: Scan active interfaces via getifaddrs() */
#if defined(AVX_OS_POSIX)
    struct ifaddrs *ifaddr = NULL, *ifa = NULL;
    if (getifaddrs(&ifaddr) == 0) {
        char candidate[64] = {0};
        bool candidate_is_virtual = true;

        for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;

            struct sockaddr_in *s4 = (struct sockaddr_in*)ifa->ifa_addr;
            char cur_ip[64];
            if (inet_ntop(AF_INET, &s4->sin_addr, cur_ip, sizeof(cur_ip)) != NULL) {
                bool is_virtual = (strncmp(ifa->ifa_name, "docker", 6) == 0 ||
                                   strncmp(ifa->ifa_name, "br-", 3) == 0 ||
                                   strncmp(ifa->ifa_name, "veth", 4) == 0);
                if (candidate[0] == '\0' || (candidate_is_virtual && !is_virtual)) {
                    snprintf(candidate, sizeof(candidate), "%s", cur_ip);
                    candidate_is_virtual = is_virtual;
                    if (!is_virtual) break;
                }
            }
        }
        freeifaddrs(ifaddr);

        if (candidate[0] != '\0') {
            snprintf(out_ip, out_len, "%s", candidate);
            return true;
        }
    }
#endif

    /* 3. Fallback to loopback if completely offline / isolated */
    snprintf(out_ip, out_len, "127.0.0.1");
    return true;
}

/*
 * On POSIX, SO_REUSEADDR allows re-binding over TIME_WAIT without hijacking
 * active listeners (which would require SO_REUSEPORT). On Windows Winsock2,
 * SO_REUSEADDR allows multiple active listeners on the same IP:port, breaking
 * dynamic port fallback across co-located transmitters; SO_EXCLUSIVEADDRUSE
 * enforces exclusive port ownership per active listener.
 */
static void avx_socket_set_server_bind_opts(avx_socket_t fd) {
    int opt = 1;
#if defined(AVX_OS_WINDOWS)
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&opt, sizeof(opt));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#endif
}

/* Binds non-blocking TCP server socket with exclusive/reuse bind opts and tuned buffers. */
avx_socket_t avx_socket_create_tcp_server_bind(const char* bind_ip, uint16_t port) {
    avx_platform_sockets_init();
    int buf_size = 16 * 1024 * 1024; /* 16 MB buffer for video streaming */

    /* Case A: Explicit IPv6 literal (e.g. "::1" or "2001:...") */
    struct in6_addr in6;
    if (bind_ip && bind_ip[0] != '\0' && inet_pton(AF_INET6, bind_ip, &in6) == 1) {
        avx_socket_t fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

        avx_socket_set_server_bind_opts(fd);
        avx_socket_set_nosigpipe(fd);
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        addr6.sin6_addr = in6;

        if (bind(fd, (struct sockaddr*)&addr6, sizeof(addr6)) < 0 || listen(fd, 8) < 0) {
            avx_socket_close(fd);
            return AVX_INVALID_SOCKET;
        }
        return fd;
    }

    /* Case B: Explicit IPv4 literal (e.g. "192.168.1.100" or "127.0.0.1") */
    struct in_addr in4;
    if (bind_ip && bind_ip[0] != '\0' && strcmp(bind_ip, "0.0.0.0") != 0 && strcmp(bind_ip, "::") != 0) {
        if (inet_pton(AF_INET, bind_ip, &in4) == 1) {
            avx_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

            avx_socket_set_server_bind_opts(fd);
            avx_socket_set_nosigpipe(fd);
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));

            struct sockaddr_in addr4;
            memset(&addr4, 0, sizeof(addr4));
            addr4.sin_family = AF_INET;
            addr4.sin_port = htons(port);
            addr4.sin_addr = in4;

            if (bind(fd, (struct sockaddr*)&addr4, sizeof(addr4)) < 0 || listen(fd, 8) < 0) {
                avx_socket_close(fd);
                return AVX_INVALID_SOCKET;
            }
            return fd;
        }
    }

    /* Case C: Wildcard Dual-Stack Binding (bind_ip is NULL, "", "0.0.0.0", or "::") */
    /* Attempt AF_INET6 socket with IPV6_V6ONLY = 0 (dual-stack: accepts both IPv4 and IPv6) */
    avx_socket_t fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd != AVX_INVALID_SOCKET) {
        int no = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&no, sizeof(no));
        avx_socket_set_server_bind_opts(fd);
        avx_socket_set_nosigpipe(fd);
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));

        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        addr6.sin6_addr = in6addr_any;

        if (bind(fd, (struct sockaddr*)&addr6, sizeof(addr6)) == 0 && listen(fd, 8) == 0) {
            return fd;
        }
        avx_socket_close(fd);
    }

    /* Fallback to AF_INET if dual-stack is unsupported by OS */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

    avx_socket_set_server_bind_opts(fd);
    avx_socket_set_nosigpipe(fd);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));

    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(port);
    addr4.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr4, sizeof(addr4)) < 0 || listen(fd, 8) < 0) {
        avx_socket_close(fd);
        return AVX_INVALID_SOCKET;
    }
    return fd;
}

avx_socket_t avx_socket_create_tcp_server(uint16_t port) {
    return avx_socket_create_tcp_server_bind(NULL, port);
}

static struct addrinfo* avx_socket_resolve_tcp(const char* host, uint16_t port) {
    if (!host || host[0] == '\0') return NULL;

    /* Strip brackets if host is formatted as "[::1]" -> "::1" */
    char clean_host[128];
    const char* h = host;
    if (h[0] == '[') {
        h++;
        size_t len = strlen(h);
        if (len > 0 && h[len - 1] == ']') {
            len--;
        }
        if (len >= sizeof(clean_host)) len = sizeof(clean_host) - 1;
        strncpy(clean_host, h, len);
        clean_host[len] = '\0';
        h = clean_host;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     /* IPv4 or IPv6 dual-stack */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    if (getaddrinfo(h, port_str, &hints, &res) != 0 || !res) {
        return NULL;
    }
    return res;
}

static avx_socket_t avx_socket_open_tuned_tcp(const struct addrinfo *rp, bool nonblocking) {
    avx_platform_sockets_init();
    avx_socket_t fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

    avx_socket_set_nosigpipe(fd);

    int buf_size = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));
    if (nonblocking) {
        avx_socket_set_nonblocking(fd, true);
    }
    return fd;
}

avx_socket_t avx_socket_start_nonblocking_connect(const char* host, uint16_t port, bool* connected_now) {
    if (connected_now) *connected_now = false;
    struct addrinfo *res = avx_socket_resolve_tcp(host, port);
    if (!res) return AVX_INVALID_SOCKET;

    avx_socket_t fd = AVX_INVALID_SOCKET;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        fd = avx_socket_open_tuned_tcp(rp, true);
        if (fd == AVX_INVALID_SOCKET) continue;

        int ret = connect(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen);
        if (ret == 0) {
            avx_socket_set_nonblocking(fd, false);
            avx_socket_set_keepalive(fd, 2, 1, 3);
            avx_socket_set_tcp_nodelay(fd, true);
            if (connected_now) *connected_now = true;
            break;
        }
#if defined(AVX_OS_WINDOWS)
        int err = WSAGetLastError();
        bool in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        bool in_progress = (errno == EINPROGRESS);
#endif
        if (in_progress) {
            break;
        }

        avx_socket_close(fd);
        fd = AVX_INVALID_SOCKET;
    }

    freeaddrinfo(res);
    return fd;
}

/* Initiates non-blocking TCP connection with polling deadline to prevent thread hangs. */
avx_socket_t avx_socket_connect_tcp_timeout(const char* host, uint16_t port, uint32_t timeout_ms) {
    struct addrinfo *res = avx_socket_resolve_tcp(host, port);
    if (!res) return AVX_INVALID_SOCKET;
    struct addrinfo *rp = NULL;

    avx_socket_t fd = AVX_INVALID_SOCKET;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = avx_socket_open_tuned_tcp(rp, timeout_ms > 0);
        if (fd == AVX_INVALID_SOCKET) continue;

        if (timeout_ms > 0) {
            int ret = connect(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen);
            if (ret == 0) {
                avx_socket_set_nonblocking(fd, false);
                avx_socket_set_keepalive(fd, 2, 1, 3);
                avx_socket_set_tcp_nodelay(fd, true);
                break; /* Successfully connected! */
            }

#if defined(AVX_OS_WINDOWS)
            int err = WSAGetLastError();
            bool in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
            if (in_progress) {
                fd_set wset, eset;
                FD_ZERO(&wset);
                FD_ZERO(&eset);
                FD_SET(fd, &wset);
                FD_SET(fd, &eset);
                struct timeval tv;
                tv.tv_sec = (long)(timeout_ms / 1000);
                tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
                int sret = select((int)fd + 1, NULL, &wset, &eset, &tv);
                if (sret > 0 && FD_ISSET(fd, &wset) && !FD_ISSET(fd, &eset)) {
                    int sock_err = 0;
                    socklen_t elen = sizeof(sock_err);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&sock_err, &elen) == 0 && sock_err == 0) {
                        avx_socket_set_nonblocking(fd, false);
                        avx_socket_set_keepalive(fd, 2, 1, 3);
                        avx_socket_set_tcp_nodelay(fd, true);
                        break;
                    }
                }
            }
#else
            int err = errno;
            bool in_progress = (err == EINPROGRESS);
            if (in_progress) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                int pret = poll(&pfd, 1, (int)timeout_ms);
                if (pret > 0 && (pfd.revents & POLLOUT) && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    int sock_err = 0;
                    socklen_t elen = sizeof(sock_err);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &elen) == 0 && sock_err == 0) {
                        avx_socket_set_nonblocking(fd, false);
                        avx_socket_set_keepalive(fd, 2, 1, 3);
                        avx_socket_set_tcp_nodelay(fd, true);
                        break;
                    }
                }
            }
#endif
        } else {
            if (connect(fd, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
                avx_socket_set_keepalive(fd, 2, 1, 3);
                avx_socket_set_tcp_nodelay(fd, true);
                break; /* Successfully connected! */
            }
        }

        avx_socket_close(fd);
        fd = AVX_INVALID_SOCKET;
    }

    freeaddrinfo(res);
    return fd;
}

avx_socket_t avx_socket_connect_tcp(const char* host, uint16_t port) {
    return avx_socket_connect_tcp_timeout(host, port, 3000);
}

avx_socket_t avx_socket_create_mdns_multicast(void) {
    avx_platform_sockets_init();
    avx_socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == AVX_INVALID_SOCKET) return AVX_INVALID_SOCKET;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    avx_socket_set_nosigpipe(fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(AVX_MDNS_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        avx_socket_close(fd);
        return AVX_INVALID_SOCKET;
    }

    /* Join multicast group 224.0.0.251 */
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, AVX_MDNS_MCAST_ADDR, &mreq.imr_multiaddr.s_addr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));

    return fd;
}

/* Zero-Sudo PCAP File Generation (Standard Ethernet DLT_EN10MB = 1) */
static FILE* g_pcap_file = NULL;
static avx_mutex_t g_pcap_lock = AVX_MUTEX_INITIALIZER;
static uint16_t g_pcap_ip_id = 1;

AVX_PACKED_START
struct AVX_PACKED pcap_hdr_s {
    uint32_t magic_number;   /* 0xa1b2c3d4 */
    uint16_t version_major;  /* 2 */
    uint16_t version_minor;  /* 4 */
    int32_t  thiszone;       /* 0 */
    uint32_t sigfigs;        /* 0 */
    uint32_t snaplen;        /* 65535 */
    uint32_t network;        /* 1 = DLT_EN10MB (Standard Ethernet) */
};

struct AVX_PACKED pcaprec_hdr_s {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};
AVX_PACKED_END

static uint16_t avx_pcap_ip_checksum(const void* buf, size_t len) {
    const uint16_t* p = (const uint16_t*)buf;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const uint8_t*)p;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

void avx_pcap_init(const char* filepath) {
    avx_mutex_lock(&g_pcap_lock);
    if (!g_pcap_file && filepath) {
        FILE *check = fopen(filepath, "rb");
        bool has_header = false;
        if (check) {
            uint32_t magic = 0;
            if (fread(&magic, sizeof(magic), 1, check) == 1 && magic == 0xa1b2c3d4) {
                has_header = true;
            }
            fclose(check);
        }

        if (has_header) {
            g_pcap_file = fopen(filepath, "ab");
        } else {
            g_pcap_file = fopen(filepath, "wb");
            if (g_pcap_file) {
                struct pcap_hdr_s hdr = {
                    .magic_number = 0xa1b2c3d4,
                    .version_major = 2,
                    .version_minor = 4,
                    .thiszone = 0,
                    .sigfigs = 0,
                    .snaplen = 65535,
                    .network = 1 /* DLT_EN10MB */
                };
                fwrite(&hdr, sizeof(hdr), 1, g_pcap_file);
                fflush(g_pcap_file);
            }
        }
    }
    avx_mutex_unlock(&g_pcap_lock);
}

/* Synthesizes raw Ethernet/IPv4/UDP packets for Wireshark PCAP capture of live stream traffic. */
void avx_pcap_write_udp(uint32_t src_ip_net, uint16_t src_port_host, uint32_t dst_ip_net, uint16_t dst_port_host, const void* payload, size_t len) {
    avx_mutex_lock(&g_pcap_lock);
    if (!g_pcap_file) {
        const char *pcap_env = getenv("OPENAVX_DUMP_PCAP");
        if (pcap_env && pcap_env[0] != '\0') {
            avx_mutex_unlock(&g_pcap_lock);
            avx_pcap_init(pcap_env);
            avx_mutex_lock(&g_pcap_lock);
        }
        if (!g_pcap_file) {
            avx_mutex_unlock(&g_pcap_lock);
            return;
        }
    }

    size_t clamped_len = len > 65000 ? 65000 : len;
    size_t total_frame_len = 14 + 20 + 8 + clamped_len;
    uint8_t *frame = (uint8_t*)malloc(total_frame_len);
    if (!frame) {
        avx_mutex_unlock(&g_pcap_lock);
        return;
    }

    /* 1. Ethernet Header (14 bytes) */
    if (dst_ip_net == htonl(0xE00000FB)) {
        uint8_t mcast_mac[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};
        memcpy(frame, mcast_mac, 6);
    } else {
        uint8_t dst_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
        memcpy(frame, dst_mac, 6);
    }
    uint8_t src_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(frame + 6, src_mac, 6);
    uint16_t ethertype = htons(0x0800);
    memcpy(frame + 12, &ethertype, 2);

    /* 2. IPv4 Header (20 bytes) */
    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0x00;
    uint16_t ip_len = htons((uint16_t)(20 + 8 + clamped_len));
    memcpy(ip + 2, &ip_len, 2);
    uint16_t ip_id = htons(g_pcap_ip_id++);
    memcpy(ip + 4, &ip_id, 2);
    uint16_t flags_frag = htons(0x4000);
    memcpy(ip + 6, &flags_frag, 2);
    ip[8] = 64;
    ip[9] = 17; /* UDP */
    uint16_t zero_csum = 0;
    memcpy(ip + 10, &zero_csum, 2);
    memcpy(ip + 12, &src_ip_net, 4);
    memcpy(ip + 16, &dst_ip_net, 4);
    uint16_t csum = avx_pcap_ip_checksum(ip, 20);
    memcpy(ip + 10, &csum, 2);

    /* 3. UDP Header (8 bytes) */
    uint8_t *udp = frame + 14 + 20;
    uint16_t sp = htons(src_port_host);
    uint16_t dp = htons(dst_port_host);
    uint16_t ulen = htons((uint16_t)(8 + clamped_len));
    memcpy(udp, &sp, 2);
    memcpy(udp + 2, &dp, 2);
    memcpy(udp + 4, &ulen, 2);
    memcpy(udp + 6, &zero_csum, 2);

    /* 4. Payload */
    if (payload && clamped_len > 0) {
        memcpy(frame + 14 + 20 + 8, payload, clamped_len);
    }

    struct timeval tv;
    avx_gettimeofday(&tv, NULL);
    struct pcaprec_hdr_s rec = {
        .ts_sec = (uint32_t)tv.tv_sec,
        .ts_usec = (uint32_t)tv.tv_usec,
        .incl_len = (uint32_t)total_frame_len,
        .orig_len = (uint32_t)(14 + 20 + 8 + len)
    };
    fwrite(&rec, sizeof(rec), 1, g_pcap_file);
    fwrite(frame, total_frame_len, 1, g_pcap_file);
    fflush(g_pcap_file);
    free(frame);
    avx_mutex_unlock(&g_pcap_lock);
}

void avx_pcap_write_tcp(uint32_t src_ip_net, uint16_t src_port_host, uint32_t dst_ip_net, uint16_t dst_port_host, uint32_t seq, uint32_t ack, uint8_t flags, const void* payload, size_t len) {
    avx_mutex_lock(&g_pcap_lock);
    if (!g_pcap_file) {
        const char *pcap_env = getenv("OPENAVX_DUMP_PCAP");
        if (pcap_env && pcap_env[0] != '\0') {
            avx_mutex_unlock(&g_pcap_lock);
            avx_pcap_init(pcap_env);
            avx_mutex_lock(&g_pcap_lock);
        }
        if (!g_pcap_file) {
            avx_mutex_unlock(&g_pcap_lock);
            return;
        }
    }

    size_t clamped_len = len > 65000 ? 65000 : len;
    size_t total_frame_len = 14 + 20 + 20 + clamped_len;
    uint8_t *frame = (uint8_t*)malloc(total_frame_len);
    if (!frame) {
        avx_mutex_unlock(&g_pcap_lock);
        return;
    }

    /* 1. Ethernet Header (14 bytes) */
    uint8_t dst_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    uint8_t src_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, src_mac, 6);
    uint16_t ethertype = htons(0x0800);
    memcpy(frame + 12, &ethertype, 2);

    /* 2. IPv4 Header (20 bytes) */
    uint8_t *ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0x00;
    uint16_t ip_len = htons((uint16_t)(20 + 20 + clamped_len));
    memcpy(ip + 2, &ip_len, 2);
    uint16_t ip_id = htons(g_pcap_ip_id++);
    memcpy(ip + 4, &ip_id, 2);
    uint16_t flags_frag = htons(0x4000);
    memcpy(ip + 6, &flags_frag, 2);
    ip[8] = 64;
    ip[9] = 6; /* TCP */
    uint16_t zero_csum = 0;
    memcpy(ip + 10, &zero_csum, 2);
    memcpy(ip + 12, &src_ip_net, 4);
    memcpy(ip + 16, &dst_ip_net, 4);
    uint16_t csum = avx_pcap_ip_checksum(ip, 20);
    memcpy(ip + 10, &csum, 2);

    /* 3. TCP Header (20 bytes) */
    uint8_t *tcp = frame + 14 + 20;
    uint16_t sp = htons(src_port_host);
    uint16_t dp = htons(dst_port_host);
    uint32_t s = htonl(seq);
    uint32_t a = htonl(ack);
    uint16_t win = htons(65535);
    memcpy(tcp, &sp, 2);
    memcpy(tcp + 2, &dp, 2);
    memcpy(tcp + 4, &s, 4);
    memcpy(tcp + 8, &a, 4);
    tcp[12] = 0x50; /* Data offset 20 bytes */
    tcp[13] = flags;
    memcpy(tcp + 14, &win, 2);
    memcpy(tcp + 16, &zero_csum, 2);
    memcpy(tcp + 18, &zero_csum, 2);

    /* 4. Payload */
    if (payload && clamped_len > 0) {
        memcpy(frame + 14 + 20 + 20, payload, clamped_len);
    }

    struct timeval tv;
    avx_gettimeofday(&tv, NULL);
    struct pcaprec_hdr_s rec = {
        .ts_sec = (uint32_t)tv.tv_sec,
        .ts_usec = (uint32_t)tv.tv_usec,
        .incl_len = (uint32_t)total_frame_len,
        .orig_len = (uint32_t)(14 + 20 + 20 + len)
    };
    fwrite(&rec, sizeof(rec), 1, g_pcap_file);
    fwrite(frame, total_frame_len, 1, g_pcap_file);
    fflush(g_pcap_file);
    free(frame);
    avx_mutex_unlock(&g_pcap_lock);
}

void avx_pcap_write_packet(const void* data, size_t len) {
    avx_pcap_write_udp(htonl(0x7F000001), AVX_DEFAULT_PORT, htonl(0x7F000001), AVX_DEFAULT_PORT, data, len);
}

void avx_pcap_close(void) {
    avx_mutex_lock(&g_pcap_lock);
    if (g_pcap_file) {
        fclose(g_pcap_file);
        g_pcap_file = NULL;
    }
    avx_mutex_unlock(&g_pcap_lock);
}

/* Live Terminal Packet Tracer */
void avx_trace_packet(const char* direction, const char* tag, const void* data, size_t len) {
    const char* trace_env = getenv("OPENAVX_PACKET_TRACE");
    if (!trace_env || strcmp(trace_env, "0") == 0) return;

    bool use_color = false;
#if defined(AVX_OS_POSIX)
    use_color = (isatty(fileno(stderr)) != 0);
#endif

    if (use_color) {
        fprintf(stderr, "\033[1;36m[openavx:wire] [debug]\033[0m %s %s (%zu bytes)\n", direction, tag, len);
    } else {
        fprintf(stderr, "[openavx:wire] [debug] %s %s (%zu bytes)\n", direction, tag, len);
    }

    const uint8_t* p = (const uint8_t*)data;
    size_t preview_len = len > 64 ? 64 : len;

    fprintf(stderr, "  hex : ");
    for (size_t i = 0; i < preview_len; ++i) {
        fprintf(stderr, "%02X ", p[i]);
        if ((i + 1) % 16 == 0 && (i + 1) < preview_len) fprintf(stderr, "\n        ");
    }
    if (len > 64) fprintf(stderr, "... (+%zu bytes)", len - 64);
    fputc('\n', stderr);

    /* Print ASCII if printable characters detected */
    bool has_ascii = false;
    for (size_t i = 0; i < preview_len; ++i) {
        if (p[i] >= 32 && p[i] <= 126) { has_ascii = true; break; }
    }
    if (has_ascii) {
        fprintf(stderr, "  asc : \"");
        for (size_t i = 0; i < preview_len; ++i) {
            fputc((p[i] >= 32 && p[i] <= 126) ? p[i] : '.', stderr);
        }
        fprintf(stderr, "\"\n");
    }
    fflush(stderr);
}

/* Reads kernel socket diagnostics (TCP_INFO / queue ioctls) for RTT, retransmissions, and buffer backlog. */
void avx_socket_get_diagnostics(avx_socket_t sock, float *out_rtt_ms, uint32_t *out_retrans, uint32_t *out_send_q, uint32_t *out_recv_q) {
    if (out_rtt_ms) *out_rtt_ms = 0.0f;
    if (out_retrans) *out_retrans = 0;
    if (out_send_q) *out_send_q = 0;
    if (out_recv_q) *out_recv_q = 0;

    if (!avx_socket_is_valid(sock)) return;

#if defined(AVX_OS_LINUX)
    struct tcp_info info;
    socklen_t len = sizeof(info);
    if (getsockopt(sock, IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
        if (out_rtt_ms) *out_rtt_ms = (float)info.tcpi_rtt / 1000.0f;
        if (out_retrans) *out_retrans = info.tcpi_total_retrans;
    }
    int send_q = 0;
    if (ioctl(sock, SIOCOUTQ, &send_q) == 0 && send_q > 0) {
        if (out_send_q) *out_send_q = (uint32_t)send_q;
    }
    int recv_q = 0;
    if (ioctl(sock, FIONREAD, &recv_q) == 0 && recv_q > 0) {
        if (out_recv_q) *out_recv_q = (uint32_t)recv_q;
    }
#elif defined(AVX_OS_MACOS)
    #if defined(TCP_CONNECTION_INFO)
    struct tcp_connection_info info;
    socklen_t len = sizeof(info);
    if (getsockopt(sock, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &len) == 0) {
        if (out_rtt_ms) *out_rtt_ms = (float)info.tcpi_srtt;
        if (out_retrans) *out_retrans = (uint32_t)info.tcpi_txretransmitpackets;
    }
    #endif
    int recv_q = 0;
    if (ioctl(sock, FIONREAD, &recv_q) == 0 && recv_q > 0) {
        if (out_recv_q) *out_recv_q = (uint32_t)recv_q;
    }
#elif defined(AVX_OS_WINDOWS)
    u_long recv_q = 0;
    if (ioctlsocket(sock, FIONREAD, &recv_q) == 0) {
        if (out_recv_q) *out_recv_q = (uint32_t)recv_q;
    }
#endif
}
