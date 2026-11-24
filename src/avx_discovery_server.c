/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_discovery_server.c - Centralized discovery server & UDP relay
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avx_discovery_server.h"
#include "avx_internal.h"
#include "avx_udp.h"

typedef struct {
    struct sockaddr_storage addr;
    avx_socklen_t           addr_len;
    int64_t                 last_seen_ns;
} avx_relay_endpoint_t;

typedef struct {
    uint16_t             session_id;
    bool                 active;
    bool                 has_publisher;
    avx_relay_endpoint_t publisher;
    avx_relay_endpoint_t subscribers[AVX_RELAY_MAX_SUBSCRIBERS_PER_SESSION];
    size_t               num_subscribers;
    int64_t              last_active_ns;
} avx_relay_session_t;

struct avx_discovery_server_t {
    avx_socket_t listen_fd;
    uint16_t     port;
    char         bind_ip[64];
    char         public_ip[64];
    volatile bool running;
    avx_thread_t worker_thread;
    avx_mutex_t  lock;

    avx_server_source_entry_t sources[AVX_DISCOVERY_SERVER_MAX_SOURCES];
    size_t                    num_sources;

    avx_socket_t client_fds[AVX_DISCOVERY_SERVER_MAX_CLIENTS];
    size_t       num_clients;

    /* UDP Relay & STUN Reflection Subsystem */
    avx_socket_t        relay_sock;
    uint16_t            relay_port;
    avx_thread_t        relay_worker_thread;
    bool                relay_thread_started;
    avx_mutex_t         relay_lock;
    avx_relay_session_t sessions[AVX_RELAY_MAX_SESSIONS];
    avx_relay_stats_t   stats;
};

static bool sockaddr_matches(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    if (!a || !b) return false;
    if (a->ss_family != b->ss_family) return false;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *sa = (const struct sockaddr_in*)a;
        const struct sockaddr_in *sb = (const struct sockaddr_in*)b;
        return (sa->sin_port == sb->sin_port && sa->sin_addr.s_addr == sb->sin_addr.s_addr);
    } else if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sa = (const struct sockaddr_in6*)a;
        const struct sockaddr_in6 *sb = (const struct sockaddr_in6*)b;
        return (sa->sin6_port == sb->sin6_port && memcmp(&sa->sin6_addr, &sb->sin6_addr, sizeof(struct in6_addr)) == 0);
    }
    return false;
}

static avx_relay_session_t* find_or_create_relay_session(avx_discovery_server_t *srv, uint16_t session_id)
{
    if (session_id == 0) return NULL;
    int free_idx = -1;
    for (size_t i = 0; i < AVX_RELAY_MAX_SESSIONS; ++i) {
        if (srv->sessions[i].active && srv->sessions[i].session_id == session_id) {
            return &srv->sessions[i];
        }
        if (!srv->sessions[i].active && free_idx < 0) {
            free_idx = (int)i;
        }
    }
    if (free_idx >= 0) {
        avx_relay_session_t *sess = &srv->sessions[free_idx];
        memset(sess, 0, sizeof(*sess));
        sess->session_id = session_id;
        sess->active = true;
        sess->last_active_ns = avx_time_ns();
        return sess;
    }
    return NULL;
}

static void add_or_update_subscriber(avx_relay_session_t *sess, const struct sockaddr_storage *addr,
                                     avx_socklen_t len, int64_t now_ns)
{
    for (size_t i = 0; i < sess->num_subscribers; ++i) {
        if (sockaddr_matches(&sess->subscribers[i].addr, addr)) {
            sess->subscribers[i].last_seen_ns = now_ns;
            return;
        }
    }
    if (sess->num_subscribers < AVX_RELAY_MAX_SUBSCRIBERS_PER_SESSION) {
        size_t idx = sess->num_subscribers++;
        sess->subscribers[idx].addr = *addr;
        sess->subscribers[idx].addr_len = len;
        sess->subscribers[idx].last_seen_ns = now_ns;
    }
}

static void remove_subscriber(avx_relay_session_t *sess, const struct sockaddr_storage *addr)
{
    size_t w = 0;
    for (size_t i = 0; i < sess->num_subscribers; ++i) {
        if (!sockaddr_matches(&sess->subscribers[i].addr, addr)) {
            if (w != i) {
                sess->subscribers[w] = sess->subscribers[i];
            }
            w++;
        }
    }
    sess->num_subscribers = w;
}

static int find_subscriber_index(const avx_relay_session_t *sess, const struct sockaddr_storage *addr)
{
    for (size_t i = 0; i < sess->num_subscribers; ++i) {
        if (sockaddr_matches(&sess->subscribers[i].addr, addr)) {
            return (int)i;
        }
    }
    return -1;
}

static void clean_expired_relay_entries(avx_discovery_server_t *srv, int64_t now_ns)
{
    const int64_t timeout_ns = 30000000000LL; /* 30 seconds idle timeout */
    for (size_t i = 0; i < AVX_RELAY_MAX_SESSIONS; ++i) {
        avx_relay_session_t *sess = &srv->sessions[i];
        if (!sess->active) continue;

        if (sess->has_publisher && (now_ns - sess->publisher.last_seen_ns) > timeout_ns) {
            sess->has_publisher = false;
        }

        size_t w = 0;
        for (size_t s = 0; s < sess->num_subscribers; ++s) {
            if ((now_ns - sess->subscribers[s].last_seen_ns) <= timeout_ns) {
                if (w != s) {
                    sess->subscribers[w] = sess->subscribers[s];
                }
                w++;
            }
        }
        sess->num_subscribers = w;

        if (!sess->has_publisher && sess->num_subscribers == 0 && (now_ns - sess->last_active_ns) > timeout_ns) {
            sess->active = false;
        }
    }
}

static void remove_client_and_sources(avx_discovery_server_t *srv, avx_socket_t fd)
{
    /* Remove any sources owned by this FD */
    size_t w = 0;
    for (size_t i = 0; i < srv->num_sources; ++i) {
        if (srv->sources[i].owner_fd != fd) {
            if (w != i) {
                srv->sources[w] = srv->sources[i];
            }
            w++;
        }
    }
    srv->num_sources = w;

    /* Remove client FD */
    w = 0;
    for (size_t i = 0; i < srv->num_clients; ++i) {
        if (srv->client_fds[i] != fd) {
            srv->client_fds[w++] = srv->client_fds[i];
        }
    }
    srv->num_clients = w;
    avx_socket_close(fd);
}

static void handle_client_message(avx_discovery_server_t *srv, avx_socket_t fd, const char *msg)
{
    /* 1. REGISTER handler */
    if (strncmp(msg, "REGISTER", 8) == 0) {
        /* Format: REGISTER name|url|groups[|wan_endpoint] */
        const char *p = msg + 8;
        while (*p == ' ') p++;

        char name[256] = {0}, url[256] = {0}, groups[256] = {0}, wan_endpoint[64] = {0};
        const char *pipe1 = strchr(p, '|');
        if (pipe1) {
            size_t nlen = (size_t)(pipe1 - p);
            if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
            strncpy(name, p, nlen);

            const char *pipe2 = strchr(pipe1 + 1, '|');
            if (pipe2) {
                size_t ulen = (size_t)(pipe2 - (pipe1 + 1));
                if (ulen >= sizeof(url)) ulen = sizeof(url) - 1;
                strncpy(url, pipe1 + 1, ulen);

                const char *pipe3 = strchr(pipe2 + 1, '|');
                if (pipe3) {
                    size_t glen = (size_t)(pipe3 - (pipe2 + 1));
                    if (glen >= sizeof(groups)) glen = sizeof(groups) - 1;
                    strncpy(groups, pipe2 + 1, glen);
                    strncpy(wan_endpoint, pipe3 + 1, sizeof(wan_endpoint) - 1);
                } else {
                    strncpy(groups, pipe2 + 1, sizeof(groups) - 1);
                }
            } else {
                strncpy(url, pipe1 + 1, sizeof(url) - 1);
                strcpy(groups, "public");
            }
        }

        /* Determine peer's reflexive address via TCP socket */
        char peer_ip[64] = "127.0.0.1";
        uint16_t peer_port = 0;
        struct sockaddr_storage peer_sa;
        avx_socklen_t peer_len = sizeof(peer_sa);
        if (getpeername(fd, (struct sockaddr*)&peer_sa, &peer_len) == 0) {
            if (peer_sa.ss_family == AF_INET) {
                struct sockaddr_in *sin4 = (struct sockaddr_in*)&peer_sa;
                inet_ntop(AF_INET, &sin4->sin_addr, peer_ip, sizeof(peer_ip));
                peer_port = ntohs(sin4->sin_port);
            } else if (peer_sa.ss_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)&peer_sa;
                inet_ntop(AF_INET6, &sin6->sin6_addr, peer_ip, sizeof(peer_ip));
                peer_port = ntohs(sin6->sin6_port);
            }
        }

        bool registered = false;
        if (name[0] && url[0]) {
            avx_mutex_lock(&srv->lock);
            if (srv->num_sources < AVX_DISCOVERY_SERVER_MAX_SOURCES) {
                /* Update existing or add new */
                bool found = false;
                for (size_t i = 0; i < srv->num_sources; ++i) {
                    if (strcmp(srv->sources[i].name, name) == 0) {
                        snprintf(srv->sources[i].url, sizeof(srv->sources[i].url), "%s", url);
                        snprintf(srv->sources[i].groups, sizeof(srv->sources[i].groups), "%s", groups);
                        if (wan_endpoint[0]) {
                            snprintf(srv->sources[i].public_ip, sizeof(srv->sources[i].public_ip), "%s", wan_endpoint);
                        } else {
                            snprintf(srv->sources[i].public_ip, sizeof(srv->sources[i].public_ip), "%s", peer_ip);
                            srv->sources[i].public_port = peer_port;
                        }
                        srv->sources[i].owner_fd = fd;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    size_t idx = srv->num_sources++;
                    snprintf(srv->sources[idx].name, sizeof(srv->sources[idx].name), "%s", name);
                    snprintf(srv->sources[idx].url, sizeof(srv->sources[idx].url), "%s", url);
                    snprintf(srv->sources[idx].groups, sizeof(srv->sources[idx].groups), "%s", groups);
                    if (wan_endpoint[0]) {
                        snprintf(srv->sources[idx].public_ip, sizeof(srv->sources[idx].public_ip), "%s", wan_endpoint);
                    } else {
                        snprintf(srv->sources[idx].public_ip, sizeof(srv->sources[idx].public_ip), "%s", peer_ip);
                        srv->sources[idx].public_port = peer_port;
                    }
                    srv->sources[idx].owner_fd = fd;
                }
                registered = true;
            }
            avx_mutex_unlock(&srv->lock);
        }

        if (registered) {
            send(fd, "OK\n", 3, MSG_NOSIGNAL);
        }
    }
    /* 2. QUERY handler */
    else if (strncmp(msg, "QUERY", 5) == 0) {
        char resp[4096];
        int offset = 0;

        avx_mutex_lock(&srv->lock);
        offset = snprintf(resp, sizeof(resp), "SOURCES %zu\n", srv->num_sources);
        for (size_t i = 0; i < srv->num_sources; ++i) {
            int len = snprintf(resp + offset, sizeof(resp) - (size_t)offset,
                               "%s|%s\n", srv->sources[i].name, srv->sources[i].url);
            if (len > 0) offset += len;
        }
        avx_mutex_unlock(&srv->lock);

        if (offset > 0) {
            send(fd, resp, (size_t)offset, MSG_NOSIGNAL);
        }
    }
    /* 3. RESOLVE handler: returns stream local URL and WAN reflexive endpoint */
    else if (strncmp(msg, "RESOLVE", 7) == 0) {
        const char *name = msg + 7;
        while (*name == ' ') name++;

        avx_mutex_lock(&srv->lock);
        int found_idx = -1;
        for (size_t i = 0; i < srv->num_sources; ++i) {
            if (strcmp(srv->sources[i].name, name) == 0) {
                found_idx = (int)i;
                break;
            }
        }

        char resp[1024];
        if (found_idx >= 0) {
            snprintf(resp, sizeof(resp), "RESOLVED %s|%s|%s:%u\n",
                     srv->sources[found_idx].name,
                     srv->sources[found_idx].url,
                     srv->sources[found_idx].public_ip,
                     srv->sources[found_idx].public_port);
        } else {
            snprintf(resp, sizeof(resp), "NOT_FOUND %s\n", name);
        }
        avx_mutex_unlock(&srv->lock);

        send(fd, resp, strlen(resp), MSG_NOSIGNAL);
    }
    /* 4. PUNCH_REQ handler: signaling for P2P NAT hole punching */
    else if (strncmp(msg, "PUNCH_REQ", 9) == 0) {
        const char *p = msg + 9;
        while (*p == ' ') p++;

        char sname[256] = {0}, client_cand[128] = {0};
        const char *pipe = strchr(p, '|');
        if (pipe) {
            size_t nlen = (size_t)(pipe - p);
            if (nlen >= sizeof(sname)) nlen = sizeof(sname) - 1;
            strncpy(sname, p, nlen);
            strncpy(client_cand, pipe + 1, sizeof(client_cand) - 1);
        } else {
            strncpy(sname, p, sizeof(sname) - 1);
        }

        avx_mutex_lock(&srv->lock);
        int found_idx = -1;
        for (size_t i = 0; i < srv->num_sources; ++i) {
            if (strcmp(srv->sources[i].name, sname) == 0) {
                found_idx = (int)i;
                break;
            }
        }

        if (found_idx >= 0) {
            avx_socket_t owner_fd = srv->sources[found_idx].owner_fd;
            char notify[512];
            snprintf(notify, sizeof(notify), "PUNCH_PEER %s|%s\n", sname, client_cand);
            send(owner_fd, notify, strlen(notify), MSG_NOSIGNAL);

            char resp[512];
            snprintf(resp, sizeof(resp), "PUNCH_ACK %s|%s:%u\n", sname,
                     srv->sources[found_idx].public_ip,
                     srv->sources[found_idx].public_port);
            avx_mutex_unlock(&srv->lock);
            send(fd, resp, strlen(resp), MSG_NOSIGNAL);
        } else {
            avx_mutex_unlock(&srv->lock);
            send(fd, "NOT_FOUND\n", 10, MSG_NOSIGNAL);
        }
    }
}

/* TCP server loop handling client registration, source resolution, and NAT punch requests across threads. */
static void* discovery_server_worker(void *arg)
{
    avx_discovery_server_t *srv = (avx_discovery_server_t*)arg;

    while (srv->running) {
        avx_pollfd_t fds[AVX_DISCOVERY_SERVER_MAX_CLIENTS + 1];
        fds[0].fd = srv->listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        avx_mutex_lock(&srv->lock);
        size_t n_clients = srv->num_clients;
        for (size_t i = 0; i < n_clients; ++i) {
            fds[i + 1].fd = srv->client_fds[i];
            fds[i + 1].events = POLLIN;
            fds[i + 1].revents = 0;
        }
        avx_mutex_unlock(&srv->lock);

        int pr = avx_poll(fds, (avx_nfds_t)(n_clients + 1), 50);
        if (pr <= 0) continue;

        /* Check new connections */
        if (fds[0].revents & POLLIN) {
            struct sockaddr_storage client_addr;
            avx_socklen_t client_len = sizeof(client_addr);
            avx_socket_t client_fd = accept(srv->listen_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd != AVX_INVALID_SOCKET) {
                avx_socket_set_nosigpipe(client_fd);
                avx_mutex_lock(&srv->lock);
                if (srv->num_clients < AVX_DISCOVERY_SERVER_MAX_CLIENTS) {
                    srv->client_fds[srv->num_clients++] = client_fd;
                } else {
                    avx_socket_close(client_fd);
                }
                avx_mutex_unlock(&srv->lock);
            }
        }

        /* Check active clients */
        for (size_t i = 0; i < n_clients; ++i) {
            if (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[1024];
                ssize_t n = recv(fds[i + 1].fd, buf, sizeof(buf) - 1, 0);
                if (n <= 0) {
                    avx_mutex_lock(&srv->lock);
                    remove_client_and_sources(srv, fds[i + 1].fd);
                    avx_mutex_unlock(&srv->lock);
                } else {
                    buf[n] = '\0';
                    char *saveptr = NULL;
                    char *line = strtok_r(buf, "\r\n", &saveptr);
                    while (line) {
                        handle_client_message(srv, fds[i + 1].fd, line);
                        line = strtok_r(NULL, "\r\n", &saveptr);
                    }
                }
            }
        }
    }
    return NULL;
}

/* High-throughput UDP worker handling STUN reflection, session registration, and cross-subnet packet forwarding. */
static void* discovery_relay_worker(void *arg)
{
    avx_discovery_server_t *srv = (avx_discovery_server_t*)arg;
    int64_t last_clean_ns = avx_time_ns();

    while (srv->running) {
        avx_pollfd_t pfd;
        pfd.fd = srv->relay_sock;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = avx_poll(&pfd, 1, 20);
        int64_t now_ns = avx_time_ns();

        if (now_ns - last_clean_ns > 2000000000LL) {
            avx_mutex_lock(&srv->relay_lock);
            clean_expired_relay_entries(srv, now_ns);
            avx_mutex_unlock(&srv->relay_lock);
            last_clean_ns = now_ns;
        }

        if (pr <= 0 || !(pfd.revents & POLLIN)) continue;

        uint8_t buf[2048];
        struct sockaddr_storage from_addr;
        avx_socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(srv->relay_sock, (char*)buf, sizeof(buf), 0,
                             (struct sockaddr*)&from_addr, &from_len);
        if (n <= 0) continue;

        /* Branch A: STUN Reflexive Echo Request */
        if ((size_t)n >= sizeof(avx_stun_msg_t)) {
            const avx_stun_msg_t *stun_req = (const avx_stun_msg_t*)buf;
            if (stun_req->magic == AVX_STUN_MAGIC) {
                avx_stun_msg_t stun_resp;
                memset(&stun_resp, 0, sizeof(stun_resp));
                stun_resp.magic = AVX_STUN_MAGIC;
                stun_resp.cookie = stun_req->cookie;
                if (from_addr.ss_family == AF_INET) {
                    struct sockaddr_in *sin4 = (struct sockaddr_in*)&from_addr;
                    stun_resp.family = AF_INET;
                    stun_resp.mapped_port = sin4->sin_port;
                    memcpy(stun_resp.mapped_ip, &sin4->sin_addr.s_addr, 4);
                } else if (from_addr.ss_family == AF_INET6) {
                    struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)&from_addr;
                    stun_resp.family = AF_INET6;
                    stun_resp.mapped_port = sin6->sin6_port;
                    memcpy(stun_resp.mapped_ip, &sin6->sin6_addr, 16);
                }
                sendto(srv->relay_sock, (const char*)&stun_resp, sizeof(stun_resp), 0,
                       (struct sockaddr*)&from_addr, from_len);
                continue;
            }
        }

        /* Branch B: Encapsulated Relay Datagram (with avx_relay_header_t) */
        if ((size_t)n >= sizeof(avx_relay_header_t)) {
            const avx_relay_header_t *rh = (const avx_relay_header_t*)buf;
            if (rh->magic == AVX_RELAY_MAGIC) {
                uint16_t sid = rh->session_id;

                avx_mutex_lock(&srv->relay_lock);
                avx_relay_session_t *sess = find_or_create_relay_session(srv, sid);
                if (!sess) {
                    avx_mutex_unlock(&srv->relay_lock);
                    continue;
                }
                sess->last_active_ns = now_ns;

                /* Session Registration / Bind */
                if (rh->flags & AVX_RELAY_FLAG_BIND) {
                    if (rh->channel == AVX_RELAY_CHANNEL_MEDIA) {
                        sess->publisher.addr = from_addr;
                        sess->publisher.addr_len = from_len;
                        sess->publisher.last_seen_ns = now_ns;
                        sess->has_publisher = true;
                    } else {
                        add_or_update_subscriber(sess, &from_addr, from_len, now_ns);
                    }

                    /* Reply with ACK */
                    avx_relay_header_t ack = *rh;
                    ack.flags = AVX_RELAY_FLAG_ACK;
                    sendto(srv->relay_sock, (const char*)&ack, sizeof(ack), 0,
                           (struct sockaddr*)&from_addr, from_len);
                    avx_mutex_unlock(&srv->relay_lock);
                    continue;
                }

                /* Session Leave */
                if (rh->flags & AVX_RELAY_FLAG_LEAVE) {
                    if (rh->channel == AVX_RELAY_CHANNEL_MEDIA) {
                        sess->has_publisher = false;
                    } else {
                        remove_subscriber(sess, &from_addr);
                    }
                    avx_mutex_unlock(&srv->relay_lock);
                    continue;
                }

                /* Data Forwarding: strip relay header and forward inner payload */
                const uint8_t *payload = buf + sizeof(avx_relay_header_t);
                size_t payload_len = (size_t)n - sizeof(avx_relay_header_t);

                if (rh->channel == AVX_RELAY_CHANNEL_MEDIA) {
                    sess->publisher.addr = from_addr;
                    sess->publisher.addr_len = from_len;
                    sess->publisher.last_seen_ns = now_ns;
                    sess->has_publisher = true;

                    for (size_t s = 0; s < sess->num_subscribers; ++s) {
                        sendto(srv->relay_sock, (const char*)payload, payload_len, 0,
                               (struct sockaddr*)&sess->subscribers[s].addr, sess->subscribers[s].addr_len);
                        srv->stats.packets_relayed++;
                        srv->stats.bytes_relayed += payload_len;
                    }
                } else if (rh->channel == AVX_RELAY_CHANNEL_CONTROL) {
                    add_or_update_subscriber(sess, &from_addr, from_len, now_ns);
                    if (sess->has_publisher) {
                        sendto(srv->relay_sock, (const char*)payload, payload_len, 0,
                               (struct sockaddr*)&sess->publisher.addr, sess->publisher.addr_len);
                        srv->stats.packets_relayed++;
                        srv->stats.bytes_relayed += payload_len;
                    }
                }
                avx_mutex_unlock(&srv->relay_lock);
                continue;
            }
        }

        /* Branch C: Transparent Raw OpenAVX Datagram (without relay header) */
        avx_mutex_lock(&srv->relay_lock);
        for (size_t i = 0; i < AVX_RELAY_MAX_SESSIONS; ++i) {
            avx_relay_session_t *sess = &srv->sessions[i];
            if (!sess->active) continue;

            if (sess->has_publisher && sockaddr_matches(&sess->publisher.addr, &from_addr)) {
                /* Packet from Publisher -> fan out to all subscribers */
                sess->publisher.last_seen_ns = now_ns;
                sess->last_active_ns = now_ns;
                for (size_t s = 0; s < sess->num_subscribers; ++s) {
                    sendto(srv->relay_sock, (const char*)buf, (size_t)n, 0,
                           (struct sockaddr*)&sess->subscribers[s].addr, sess->subscribers[s].addr_len);
                    srv->stats.packets_relayed++;
                    srv->stats.bytes_relayed += (size_t)n;
                }
                break;
            }

            int sub_idx = find_subscriber_index(sess, &from_addr);
            if (sub_idx >= 0) {
                /* Packet from Subscriber -> route to publisher */
                sess->subscribers[sub_idx].last_seen_ns = now_ns;
                sess->last_active_ns = now_ns;
                if (sess->has_publisher) {
                    sendto(srv->relay_sock, (const char*)buf, (size_t)n, 0,
                           (struct sockaddr*)&sess->publisher.addr, sess->publisher.addr_len);
                    srv->stats.packets_relayed++;
                    srv->stats.bytes_relayed += (size_t)n;
                }
                break;
            }
        }
        avx_mutex_unlock(&srv->relay_lock);
    }
    return NULL;
}

avx_discovery_server_t* avx_discovery_server_create_ex(const avx_discovery_server_config_t *cfg)
{
    avx_platform_sockets_init();
    avx_discovery_server_t *srv = calloc(1, sizeof(avx_discovery_server_t));
    if (!srv) return NULL;

    srv->port = (cfg && cfg->tcp_port > 0) ? cfg->tcp_port : AVX_DISCOVERY_SERVER_DEFAULT_PORT;
    if (cfg && cfg->relay_udp_port > 0) {
        srv->relay_port = cfg->relay_udp_port;
    } else {
        srv->relay_port = (srv->port == AVX_DISCOVERY_SERVER_DEFAULT_PORT) ?
                           AVX_RELAY_SERVER_DEFAULT_PORT : (uint16_t)(srv->port + 1);
    }
    bool enable_relay = cfg ? cfg->enable_relay : true;

    if (cfg && cfg->bind_ip && cfg->bind_ip[0]) {
        strncpy(srv->bind_ip, cfg->bind_ip, sizeof(srv->bind_ip) - 1);
    } else {
        strcpy(srv->bind_ip, "0.0.0.0");
    }

    if (cfg && cfg->public_ip && cfg->public_ip[0]) {
        strncpy(srv->public_ip, cfg->public_ip, sizeof(srv->public_ip) - 1);
    }

    srv->listen_fd = avx_socket_create_tcp_server_bind(srv->bind_ip, srv->port);
    if (srv->listen_fd == AVX_INVALID_SOCKET) {
        free(srv);
        return NULL;
    }

    if (enable_relay) {
        srv->relay_sock = avx_udp_create_socket_bind(srv->bind_ip, srv->relay_port, true);
        if (srv->relay_sock == AVX_INVALID_SOCKET) {
            avx_socket_close(srv->listen_fd);
            free(srv);
            return NULL;
        }
    } else {
        srv->relay_sock = AVX_INVALID_SOCKET;
    }

    avx_mutex_init(&srv->lock);
    avx_mutex_init(&srv->relay_lock);
    srv->running = true;

    avx_thread_create(&srv->worker_thread, discovery_server_worker, srv);
    if (srv->relay_sock != AVX_INVALID_SOCKET) {
        avx_thread_create(&srv->relay_worker_thread, discovery_relay_worker, srv);
        srv->relay_thread_started = true;
    }

    return srv;
}

avx_discovery_server_t* avx_discovery_server_create(const char *bind_ip, uint16_t port)
{
    avx_discovery_server_config_t cfg = {
        .bind_ip = bind_ip,
        .tcp_port = port,
        .relay_udp_port = 0,
        .enable_relay = true,
        .public_ip = NULL
    };
    return avx_discovery_server_create_ex(&cfg);
}

void avx_discovery_server_destroy(avx_discovery_server_t *srv)
{
    if (!srv) return;

    srv->running = false;
    avx_thread_join(srv->worker_thread);
    if (srv->relay_thread_started) {
        avx_thread_join(srv->relay_worker_thread);
        srv->relay_thread_started = false;
    }

    avx_mutex_lock(&srv->lock);
    for (size_t i = 0; i < srv->num_clients; ++i) {
        avx_socket_close(srv->client_fds[i]);
    }
    srv->num_clients = 0;
    if (srv->listen_fd != AVX_INVALID_SOCKET) {
        avx_socket_close(srv->listen_fd);
        srv->listen_fd = AVX_INVALID_SOCKET;
    }
    avx_mutex_unlock(&srv->lock);
    avx_mutex_destroy(&srv->lock);

    avx_mutex_lock(&srv->relay_lock);
    if (srv->relay_sock != AVX_INVALID_SOCKET) {
        avx_socket_close(srv->relay_sock);
        srv->relay_sock = AVX_INVALID_SOCKET;
    }
    avx_mutex_unlock(&srv->relay_lock);
    avx_mutex_destroy(&srv->relay_lock);

    free(srv);
}

bool avx_relay_server_get_stats(avx_discovery_server_t *srv, avx_relay_stats_t *out_stats)
{
    if (!srv || !out_stats) return false;
    avx_mutex_lock(&srv->relay_lock);
    uint32_t active_sessions = 0;
    uint32_t active_subscribers = 0;
    for (size_t i = 0; i < AVX_RELAY_MAX_SESSIONS; ++i) {
        if (srv->sessions[i].active) {
            active_sessions++;
            active_subscribers += (uint32_t)srv->sessions[i].num_subscribers;
        }
    }
    out_stats->active_sessions = active_sessions;
    out_stats->active_subscribers = active_subscribers;
    out_stats->packets_relayed = srv->stats.packets_relayed;
    out_stats->bytes_relayed = srv->stats.bytes_relayed;
    avx_mutex_unlock(&srv->relay_lock);
    return true;
}

int avx_discovery_client_register(const char *server_ip, uint16_t server_port,
                                  const char *name, const char *url, const char *groups)
{
    if (!server_ip || !name || !url) return -1;

    uint16_t port = server_port > 0 ? server_port : AVX_DISCOVERY_SERVER_DEFAULT_PORT;
    avx_socket_t sock = avx_socket_connect_tcp(server_ip, port);
    if (sock == AVX_INVALID_SOCKET) return -1;

    char req[512];
    snprintf(req, sizeof(req), "REGISTER %s|%s|%s\n", name, url, groups ? groups : "public");
    send(sock, req, strlen(req), MSG_NOSIGNAL);

    char resp[64];
    ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
    if (n > 0) {
        resp[n] = '\0';
        if (strncmp(resp, "OK", 2) == 0) {
            return (int)sock; /* Return active socket handle to keep registration alive */
        }
    }

    avx_socket_close(sock);
    return -1;
}

int avx_discovery_client_query(const char *server_ip, uint16_t server_port,
                               char names[][256], char urls[][256], size_t max_sources)
{
    if (!server_ip || !names || !urls || max_sources == 0) return -1;

    uint16_t port = server_port > 0 ? server_port : AVX_DISCOVERY_SERVER_DEFAULT_PORT;
    avx_socket_t sock = avx_socket_connect_tcp(server_ip, port);
    if (sock == AVX_INVALID_SOCKET) return -1;

    send(sock, "QUERY\n", 6, MSG_NOSIGNAL);

    char resp[4096];
    ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
    avx_socket_close(sock);

    if (n <= 0) return 0;
    resp[n] = '\0';

    int count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(resp, "\r\n", &saveptr);
    if (!line || strncmp(line, "SOURCES", 7) != 0) return 0;

    line = strtok_r(NULL, "\r\n", &saveptr);
    while (line && (size_t)count < max_sources) {
        char *pipe = strchr(line, '|');
        if (pipe) {
            *pipe = '\0';
            snprintf(names[count], sizeof(names[count]), "%s", line);
            snprintf(urls[count], sizeof(urls[count]), "%s", pipe + 1);
            count++;
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    return count;
}

int avx_discovery_client_resolve(const char *server_ip, uint16_t server_port,
                                 const char *name, char *out_url, size_t out_url_max,
                                 char *out_wan_addr, size_t out_wan_max)
{
    if (!server_ip || !name) return -1;

    uint16_t port = server_port > 0 ? server_port : AVX_DISCOVERY_SERVER_DEFAULT_PORT;
    avx_socket_t sock = avx_socket_connect_tcp(server_ip, port);
    if (sock == AVX_INVALID_SOCKET) return -1;

    char req[512];
    snprintf(req, sizeof(req), "RESOLVE %s\n", name);
    send(sock, req, strlen(req), MSG_NOSIGNAL);

    char resp[512];
    ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
    avx_socket_close(sock);

    if (n <= 0) return -1;
    resp[n] = '\0';

    if (strncmp(resp, "RESOLVED", 8) != 0) return -1;

    char *saveptr = NULL;
    char *line = strtok_r(resp, "\r\n", &saveptr);
    if (!line) return -1;

    /* Format: RESOLVED name|url|wan_addr */
    char *p = line + 8;
    while (*p == ' ') p++;

    char *pipe1 = strchr(p, '|');
    if (!pipe1) return -1;
    char *pipe2 = strchr(pipe1 + 1, '|');

    if (out_url && out_url_max > 0) {
        size_t ulen = pipe2 ? (size_t)(pipe2 - (pipe1 + 1)) : strlen(pipe1 + 1);
        if (ulen >= out_url_max) ulen = out_url_max - 1;
        strncpy(out_url, pipe1 + 1, ulen);
        out_url[ulen] = '\0';
    }

    if (pipe2 && out_wan_addr && out_wan_max > 0) {
        strncpy(out_wan_addr, pipe2 + 1, out_wan_max - 1);
        out_wan_addr[out_wan_max - 1] = '\0';
    }

    return 0;
}

int avx_discovery_client_request_punch(const char *server_ip, uint16_t server_port,
                                       const char *name, const char *local_candidate,
                                       char *out_peer_candidate, size_t out_cand_max)
{
    if (!server_ip || !name || !local_candidate) return -1;

    uint16_t port = server_port > 0 ? server_port : AVX_DISCOVERY_SERVER_DEFAULT_PORT;
    avx_socket_t sock = avx_socket_connect_tcp(server_ip, port);
    if (sock == AVX_INVALID_SOCKET) return -1;

    char req[512];
    snprintf(req, sizeof(req), "PUNCH_REQ %s|%s\n", name, local_candidate);
    send(sock, req, strlen(req), MSG_NOSIGNAL);

    char resp[512];
    ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
    avx_socket_close(sock);

    if (n <= 0) return -1;
    resp[n] = '\0';

    if (strncmp(resp, "PUNCH_ACK", 9) != 0) return -1;

    char *saveptr = NULL;
    char *line = strtok_r(resp, "\r\n", &saveptr);
    if (!line) return -1;

    /* Format: PUNCH_ACK name|peer_candidate */
    char *p = line + 9;
    while (*p == ' ') p++;

    char *pipe = strchr(p, '|');
    if (!pipe) return -1;

    if (out_peer_candidate && out_cand_max > 0) {
        strncpy(out_peer_candidate, pipe + 1, out_cand_max - 1);
        out_peer_candidate[out_cand_max - 1] = '\0';
    }

    return 0;
}

/* Queries discovery server STUN reflection probe to discover local socket's public NAT reflexive IP and port. */
int avx_discovery_client_get_reflexive_addr_sock(avx_socket_t sock,
                                                 const char *server_ip, uint16_t udp_port,
                                                 char *out_ip, uint16_t *out_port)
{
    if (!avx_socket_is_valid(sock) || !server_ip || udp_port == 0) return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", udp_port);
    if (getaddrinfo(server_ip, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    uint32_t cookie = (uint32_t)(avx_time_ns() & 0xFFFFFFFFu);
    if (cookie == 0) cookie = 1;

    avx_stun_msg_t req;
    memset(&req, 0, sizeof(req));
    req.magic = AVX_STUN_MAGIC;
    req.cookie = cookie;

    bool success = false;
    for (int retry = 0; retry < 3 && !success; ++retry) {
        sendto(sock, (const char*)&req, sizeof(req), 0,
               res->ai_addr, (avx_socklen_t)res->ai_addrlen);

        avx_pollfd_t pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = avx_poll(&pfd, 1, 200);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            avx_stun_msg_t resp;
            struct sockaddr_storage from;
            avx_socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(sock, (char*)&resp, sizeof(resp), 0,
                                 (struct sockaddr*)&from, &flen);
            if (n >= (ssize_t)sizeof(resp) &&
                resp.magic == AVX_STUN_MAGIC &&
                resp.cookie == cookie) {

                if (out_port) {
                    *out_port = ntohs(resp.mapped_port);
                }
                if (out_ip) {
                    if (resp.family == AF_INET6) {
                        inet_ntop(AF_INET6, resp.mapped_ip, out_ip, 64);
                    } else {
                        inet_ntop(AF_INET, resp.mapped_ip, out_ip, 64);
                    }
                }
                success = true;
                break;
            }
        }
    }

    freeaddrinfo(res);
    return success ? 0 : -1;
}

int avx_discovery_client_get_reflexive_addr(const char *server_ip, uint16_t udp_port,
                                            char *out_ip, uint16_t *out_port)
{
    avx_socket_t sock = avx_udp_create_socket(0, false);
    if (sock == AVX_INVALID_SOCKET) return -1;

    int ret = avx_discovery_client_get_reflexive_addr_sock(sock, server_ip, udp_port, out_ip, out_port);
    avx_socket_close(sock);
    return ret;
}

int avx_relay_client_bind_session(avx_socket_t sock, const char *relay_ip, uint16_t relay_port,
                                  uint32_t session_id, bool is_publisher)
{
    if (!avx_socket_is_valid(sock) || !relay_ip || session_id == 0) return -1;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", relay_port);
    if (getaddrinfo(relay_ip, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    avx_relay_header_t bind_hdr;
    memset(&bind_hdr, 0, sizeof(bind_hdr));
    bind_hdr.magic = AVX_RELAY_MAGIC;
    bind_hdr.session_id = (uint16_t)session_id;
    bind_hdr.channel = is_publisher ? AVX_RELAY_CHANNEL_MEDIA : AVX_RELAY_CHANNEL_CONTROL;
    bind_hdr.flags = AVX_RELAY_FLAG_BIND;

    bool acked = false;
    for (int retry = 0; retry < 3 && !acked; ++retry) {
        sendto(sock, (const char*)&bind_hdr, sizeof(bind_hdr), 0,
               res->ai_addr, (avx_socklen_t)res->ai_addrlen);

        avx_pollfd_t pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = avx_poll(&pfd, 1, 150);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            avx_relay_header_t ack;
            struct sockaddr_storage from;
            avx_socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(sock, (char*)&ack, sizeof(ack), 0,
                                 (struct sockaddr*)&from, &flen);
            if (n >= (ssize_t)sizeof(ack) &&
                ack.magic == AVX_RELAY_MAGIC &&
                ack.session_id == (uint16_t)session_id &&
                (ack.flags & AVX_RELAY_FLAG_ACK)) {
                acked = true;
                break;
            }
        }
    }

    freeaddrinfo(res);
    return 0; /* Successfully transmitted bind sequence */
}

/* Prefixes 4-byte session header to UDP packet and dispatches via discovery server relay. */
int avx_relay_send_packet(avx_socket_t sock, const struct sockaddr *relay_addr, avx_socklen_t addr_len,
                          uint32_t session_id, uint8_t channel,
                          const void *payload, size_t payload_len)
{
    if (!avx_socket_is_valid(sock) || !relay_addr || !payload || payload_len == 0) return -1;

    avx_relay_header_t hdr;
    hdr.magic = AVX_RELAY_MAGIC;
    hdr.session_id = (uint16_t)session_id;
    hdr.channel = channel;
    hdr.flags = AVX_RELAY_FLAG_DATA;

    if (payload_len + sizeof(hdr) <= 2048) {
        uint8_t pkt[2048];
        memcpy(pkt, &hdr, sizeof(hdr));
        memcpy(pkt + sizeof(hdr), payload, payload_len);
        ssize_t sent = sendto(sock, (const char*)pkt, sizeof(hdr) + payload_len, 0,
                              relay_addr, (avx_socklen_t)addr_len);
        return (sent > 0) ? (int)sent : -1;
    } else {
        uint8_t *pkt = malloc(sizeof(hdr) + payload_len);
        if (!pkt) return -1;
        memcpy(pkt, &hdr, sizeof(hdr));
        memcpy(pkt + sizeof(hdr), payload, payload_len);
        ssize_t sent = sendto(sock, (const char*)pkt, sizeof(hdr) + payload_len, 0,
                              relay_addr, (avx_socklen_t)addr_len);
        free(pkt);
        return (sent > 0) ? (int)sent : -1;
    }
}
