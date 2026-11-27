/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_send_client.c - Client network connections, listener daemon, and async TX/RX workers
 */

#include "avx_send_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

bool avx_socket_send_all(avx_socket_t fd, const void* buf, size_t len) {
    if (fd == AVX_INVALID_SOCKET || !buf || len == 0) return true;
    const char* ptr = (const char*)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = send(fd, ptr, remaining, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        ptr += n;
        remaining -= (size_t)n;
    }
    return true;
}

bool avx_send_video_datagram(
    avx_send_ctx_t *slot,
    avx_client_slot_t *client,
    const avx_tx_video_slot_t *vslot,
    bool use_rudp)
{
    size_t max_udp_payload = (size_t)AVX_UDP_MAX_FRAGS * (size_t)AVX_UDP_MAX_FRAG_SIZE;
    size_t total_sz = vslot->header_len + vslot->payload_size;
    if (total_sz > max_udp_payload) {
        return false;
    }

    uint32_t seq = ((uint32_t)client->video_packets_sent) & 0x7FFFFFFFu;
    uint32_t fourcc = 0;
    int64_t tc = 0;
    if (vslot->header_len >= 32) {
        memcpy(&fourcc, vslot->header + 12, sizeof(fourcc));
        memcpy(&tc, vslot->header + 24, sizeof(tc));
    }

    uint8_t *bundle = (uint8_t*)malloc(total_sz);
    if (!bundle) return false;

    memcpy(bundle, vslot->header, vslot->header_len);
    if (vslot->payload_size > 0 && vslot->payload) {
        memcpy(bundle + vslot->header_len, vslot->payload, vslot->payload_size);
    }

    int rc = use_rudp
        ? avx_udp_rudp_send_frame(slot->rudp_sender, &client->udp_dest_addr, seq, fourcc, tc, bundle, total_sz)
        : avx_udp_send_frame(slot->udp_sock, &client->udp_dest_addr, seq, fourcc, tc, bundle, total_sz);
    free(bundle);
    return (rc == 0);
}

void avx_send_recalculate_tally_locked(avx_send_ctx_t* slot) {
    uint8_t aggregate_flags = OAVX_TALLY_NONE;
    for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
        if (slot->clients[i].fd != AVX_INVALID_SOCKET) {
            aggregate_flags |= slot->clients[i].tally.flags;
        }
    }
    if (slot->tally.flags != aggregate_flags) {
        slot->tally.flags = aggregate_flags;
        avx_cond_broadcast(&slot->tally_cond);
        if (slot->web_server) {
            avx_web_server_broadcast_tally((avx_web_server_t*)slot->web_server, &slot->tally);
        }
    }
}

void avx_send_remove_client_locked(avx_send_ctx_t* slot, int index) {
    if (index < 0 || index >= AVX_MAX_CLIENTS) return;
    if (slot->clients[index].fd != AVX_INVALID_SOCKET) {
        avx_socket_t old_fd = slot->clients[index].fd;
        shutdown(old_fd, AVX_SHUT_RDWR);
        slot->clients[index].tally.flags = OAVX_TALLY_NONE;
        slot->clients[index].connected_time_ns = 0;
        if (slot->active_clients > 0) {
            slot->active_clients--;
        }

        /* Clear fd and signal client's async tx_worker under tx_lock; defer close until workers exit */
        avx_mutex_lock(&slot->clients[index].tx_lock);
        slot->clients[index].fd = AVX_INVALID_SOCKET;
        slot->clients[index].tx_running = false;
        avx_cond_broadcast(&slot->clients[index].tx_cond);
        avx_mutex_unlock(&slot->clients[index].tx_lock);

        if (slot->clients[index].pending_close_fd == AVX_INVALID_SOCKET) {
            slot->clients[index].pending_close_fd = old_fd;
        } else {
            avx_socket_close(old_fd);
        }
    }

    if (slot->clients[index].is_wan_registered) {
        slot->clients[index].is_wan_registered = false;
        slot->clients[index].is_wan_client = false;
        if (slot->wan_client_count > 0) slot->wan_client_count--;
        if (slot->wan_client_count == 0 && slot->wan_encoder) {
            avx_video_encoder_destroy(slot->wan_encoder);
            slot->wan_encoder = NULL;
            slot->wan_enc_width = 0;
            slot->wan_enc_height = 0;
            slot->wan_enc_fourcc = 0;
        }
    }

    /* Update legacy alias */
    slot->tcp_client_fd = AVX_INVALID_SOCKET;
    for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
        if (slot->clients[i].fd != AVX_INVALID_SOCKET) {
            slot->tcp_client_fd = slot->clients[i].fd;
            break;
        }
    }
    avx_send_recalculate_tally_locked(slot);
}

/* Background worker servicing RUDP retransmission NACKs and driving periodic rate control ticks. */
void* avx_send_nack_service_worker(void* arg) {
    avx_send_ctx_t* slot = (avx_send_ctx_t*)arg;
    if (!slot) return NULL;
    int64_t last_rc_tick_ms = 0;
    while (1) {
        avx_mutex_lock(&slot->lock);
        bool keep_running = (slot->running && slot->nack_thread_running);
        avx_mutex_unlock(&slot->lock);
        if (!keep_running) break;
        if (slot->rudp_sender) {
            avx_udp_rudp_sender_service_nacks(slot->rudp_sender);
        }
        int64_t now_ms = avx_time_ns() / 1000000LL;
        if (now_ms - last_rc_tick_ms >= 50) {
            avx_send_profiles_tick(slot, now_ms);
            last_rc_tick_ms = now_ms;
        }
        avx_sleep_ms(1);
    }
    return NULL;
}

void avx_client_queue_drain(avx_send_ctx_t* slot, avx_client_slot_t* client) {
    if (!slot || !client) return;
    avx_queue_item_t local_items[AVX_CLIENT_QUEUE_DEPTH];
    int count = 0;

    avx_mutex_lock(&client->tx_lock);
    while (client->queue_count > 0 && count < AVX_CLIENT_QUEUE_DEPTH) {
        local_items[count++] = client->queue[client->queue_head];
        client->queue[client->queue_head].type = AVX_QUEUE_ITEM_NONE;
        client->queue[client->queue_head].shared_pkt = NULL;
        client->queue_head = (client->queue_head + 1) % AVX_CLIENT_QUEUE_DEPTH;
        client->queue_count--;
    }
    client->video_queued = 0;
    avx_mutex_unlock(&client->tx_lock);

    if (count > 0) {
        avx_mutex_lock(&slot->lock);
        for (int i = 0; i < count; ++i) {
            if (local_items[i].type == AVX_QUEUE_ITEM_VIDEO) {
                if (local_items[i].shared_pkt) {
                    avx_tx_release_shared_pkt_locked(local_items[i].shared_pkt);
                } else {
                    int p_idx = local_items[i].video_pool_idx;
                    if (p_idx >= 0 && p_idx < AVX_TX_VIDEO_POOL_SIZE) {
                        if (slot->video_pool[p_idx].refcount > 0) {
                            slot->video_pool[p_idx].refcount--;
                        }
                    }
                }
            } else if (local_items[i].type == AVX_QUEUE_ITEM_AUDIO || local_items[i].type == AVX_QUEUE_ITEM_METADATA) {
                if (local_items[i].shared_pkt) {
                    avx_tx_release_shared_pkt_locked(local_items[i].shared_pkt);
                }
            }
        }
        avx_mutex_unlock(&slot->lock);
    }
}

/* Per-client worker transmitting queued video, audio, and metadata packets across TCP/UDP/RUDP. */
void* avx_send_client_tx_worker(void* arg) {
    avx_client_slot_t* client = (avx_client_slot_t*)arg;
    if (!client) return NULL;
    avx_send_ctx_t* slot = client->parent_ctx;
    int c_idx = client->client_index;

    while (1) {
        avx_mutex_lock(&client->tx_lock);
        while (client->tx_running && client->queue_count == 0) {
            avx_cond_wait(&client->tx_cond, &client->tx_lock);
        }

        if (!client->tx_running && client->queue_count == 0) {
            avx_mutex_unlock(&client->tx_lock);
            break;
        }

        avx_queue_item_t item = client->queue[client->queue_head];
        client->queue[client->queue_head].type = AVX_QUEUE_ITEM_NONE;
        client->queue[client->queue_head].shared_pkt = NULL;
        client->queue_head = (client->queue_head + 1) % AVX_CLIENT_QUEUE_DEPTH;
        client->queue_count--;
        if (item.type == AVX_QUEUE_ITEM_VIDEO) {
            if (client->video_queued > 0) client->video_queued--;
        }

        avx_socket_t fd = client->fd;
        avx_mutex_unlock(&client->tx_lock);

        bool send_ok = true;
        uint64_t tcp_sent_bytes = 0;
        if (fd != AVX_INVALID_SOCKET) {
            if (item.type == AVX_QUEUE_ITEM_VIDEO) {
                if (item.shared_pkt && item.shared_pkt->data && item.shared_pkt->size > 0) {
                    /* WAN Compressed Simulcast Frame (H.264 / HEVC / AV1) */
                    if (client->transport == OAVX_TRANSPORT_RUDP && slot->rudp_sender && client->has_udp_dest) {
                        uint32_t seq = ((uint32_t)client->video_packets_sent) & 0x7FFFFFFFu;
                        uint32_t fourcc = 0;
                        int64_t tc = 0;
                        if (item.shared_pkt->size >= 32) {
                            memcpy(&fourcc, item.shared_pkt->data + 12, sizeof(fourcc));
                            memcpy(&tc, item.shared_pkt->data + 24, sizeof(tc));
                        }
                        if (avx_udp_rudp_send_frame(slot->rudp_sender, &client->udp_dest_addr,
                                                    seq, fourcc, tc,
                                                    item.shared_pkt->data, item.shared_pkt->size) != 0) {
                            send_ok = false;
                        }
                    } else if (client->transport == OAVX_TRANSPORT_UDP && slot->udp_sock != AVX_INVALID_SOCKET && client->has_udp_dest) {
                        uint32_t seq = ((uint32_t)client->video_packets_sent) & 0x7FFFFFFFu;
                        uint32_t fourcc = 0;
                        int64_t tc = 0;
                        if (item.shared_pkt->size >= 32) {
                            memcpy(&fourcc, item.shared_pkt->data + 12, sizeof(fourcc));
                            memcpy(&tc, item.shared_pkt->data + 24, sizeof(tc));
                        }
                        if (avx_udp_send_frame(slot->udp_sock, &client->udp_dest_addr,
                                               seq, fourcc, tc,
                                               item.shared_pkt->data, item.shared_pkt->size) != 0) {
                            send_ok = false;
                        }
                    } else {
                        if (!avx_socket_send_all(fd, item.shared_pkt->data, item.shared_pkt->size)) {
                            send_ok = false;
                        } else {
                            tcp_sent_bytes += (uint64_t)item.shared_pkt->size;
                        }
                    }
                } else {
                    int p_idx = item.video_pool_idx;
                    if (p_idx >= 0 && p_idx < AVX_TX_VIDEO_POOL_SIZE) {
                        avx_tx_video_slot_t *vslot = &slot->video_pool[p_idx];
                        if (client->transport == OAVX_TRANSPORT_RUDP && slot->rudp_sender && client->has_udp_dest) {
                            send_ok = avx_send_video_datagram(slot, client, vslot, true);
                        } else if (client->transport == OAVX_TRANSPORT_UDP && slot->udp_sock != AVX_INVALID_SOCKET && client->has_udp_dest) {
                            send_ok = avx_send_video_datagram(slot, client, vslot, false);
                        } else {
                            if (vslot->metadata_buf && vslot->metadata_len > 0) {
                                if (!avx_socket_send_all(fd, vslot->metadata_hdr, vslot->metadata_hdr_len) ||
                                    !avx_socket_send_all(fd, vslot->metadata_buf, vslot->metadata_len)) {
                                    send_ok = false;
                                } else {
                                    tcp_sent_bytes += (uint64_t)vslot->metadata_hdr_len + (uint64_t)vslot->metadata_len;
                                }
                            }
                            if (send_ok) {
                                if (!avx_socket_send_all(fd, vslot->header, vslot->header_len) ||
                                    !avx_socket_send_all(fd, vslot->payload, vslot->payload_size)) {
                                    send_ok = false;
                                } else {
                                    tcp_sent_bytes += (uint64_t)vslot->header_len + (uint64_t)vslot->payload_size;
                                }
                            }
                        }
                    }
                }
            } else if (item.type == AVX_QUEUE_ITEM_AUDIO || item.type == AVX_QUEUE_ITEM_METADATA) {
                if (item.shared_pkt && item.shared_pkt->data && item.shared_pkt->size > 0) {
                    if (item.type == AVX_QUEUE_ITEM_AUDIO && client->transport == OAVX_TRANSPORT_RUDP && slot->rudp_sender && client->has_udp_dest) {
                        uint32_t seq = 0x80000000u | (((uint32_t)client->audio_packets_sent) & 0x7FFFFFFFu);
                        uint32_t udp_fourcc = OAVX_FOURCC_AUDIO_TYPE_FLTP;
                        if (item.shared_pkt->size >= 8) {
                            uint32_t pkt_magic = 0;
                            memcpy(&pkt_magic, item.shared_pkt->data, 4);
                            if (pkt_magic == OAVX_WIRE_MAGIC_AUDIO_COMP) {
                                memcpy(&udp_fourcc, item.shared_pkt->data + 4, 4);
                            }
                        }
                        if (avx_udp_rudp_send_frame(slot->rudp_sender, &client->udp_dest_addr,
                                                    seq, udp_fourcc, 0,
                                                    item.shared_pkt->data, item.shared_pkt->size) != 0) {
                            send_ok = false;
                        }
                    } else if (item.type == AVX_QUEUE_ITEM_AUDIO && client->transport == OAVX_TRANSPORT_UDP && slot->udp_sock != AVX_INVALID_SOCKET && client->has_udp_dest) {
                        uint32_t seq = 0x80000000u | (((uint32_t)client->audio_packets_sent) & 0x7FFFFFFFu);
                        uint32_t udp_fourcc = OAVX_FOURCC_AUDIO_TYPE_FLTP;
                        if (item.shared_pkt->size >= 8) {
                            uint32_t pkt_magic = 0;
                            memcpy(&pkt_magic, item.shared_pkt->data, 4);
                            if (pkt_magic == OAVX_WIRE_MAGIC_AUDIO_COMP) {
                                memcpy(&udp_fourcc, item.shared_pkt->data + 4, 4);
                            }
                        }
                        if (avx_udp_send_frame(slot->udp_sock, &client->udp_dest_addr,
                                               seq, udp_fourcc, 0,
                                               item.shared_pkt->data, item.shared_pkt->size) != 0) {
                            send_ok = false;
                        }
                    } else {
                        if (!avx_socket_send_all(fd, item.shared_pkt->data, item.shared_pkt->size)) {
                            send_ok = false;
                        } else {
                            tcp_sent_bytes += (uint64_t)item.shared_pkt->size;
                        }
                    }
                }
            }
        } else {
            send_ok = false;
        }

        /* Post-send: release refcounts under slot->lock */
        avx_mutex_lock(&slot->lock);

        if (item.type == AVX_QUEUE_ITEM_VIDEO) {
            if (item.shared_pkt) {
                avx_tx_release_shared_pkt_locked(item.shared_pkt);
            } else {
                int p_idx = item.video_pool_idx;
                if (p_idx >= 0 && p_idx < AVX_TX_VIDEO_POOL_SIZE) {
                    if (slot->video_pool[p_idx].refcount > 0) {
                        slot->video_pool[p_idx].refcount--;
                    }
                }
            }
        } else if (item.type == AVX_QUEUE_ITEM_AUDIO || item.type == AVX_QUEUE_ITEM_METADATA) {
            if (item.shared_pkt) {
                avx_tx_release_shared_pkt_locked(item.shared_pkt);
            }
        }

        if (!send_ok) {
            if (slot->clients[c_idx].fd == fd) {
                avx_send_remove_client_locked(slot, c_idx);
            }
            avx_mutex_unlock(&slot->lock);
            break;
        }

        avx_mutex_unlock(&slot->lock);

        if (tcp_sent_bytes > 0) {
            avx_mutex_lock(&client->tx_lock);
            client->tcp_bytes_submitted += tcp_sent_bytes;
            avx_mutex_unlock(&client->tx_lock);
        }
    }

    /* Drain any remaining queue items upon thread exit */
    avx_client_queue_drain(slot, client);
    avx_mutex_lock(&slot->lock);
    client->tx_exited = true;
    avx_mutex_unlock(&slot->lock);
    return NULL;
}

/* Background worker polling client sockets and parsing incoming wire commands (PTZ, tally, ping). */
void* avx_send_client_rx_worker(void* arg) {
    avx_send_ctx_t* slot = (avx_send_ctx_t*)arg;

    while (1) {
        avx_pollfd_t pfds[AVX_MAX_CLIENTS];
        int client_indices[AVX_MAX_CLIENTS];
        avx_socket_t reap_fds[AVX_MAX_CLIENTS];
        int nreap = 0;
        int nfds = 0;

        avx_mutex_lock(&slot->lock);
        bool keep_running = (slot->running && slot->client_rx_running);
        for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
            if (slot->clients[i].fd == AVX_INVALID_SOCKET &&
                slot->clients[i].pending_close_fd != AVX_INVALID_SOCKET &&
                slot->clients[i].tx_exited) {
                reap_fds[nreap++] = slot->clients[i].pending_close_fd;
                slot->clients[i].pending_close_fd = AVX_INVALID_SOCKET;
            }
            if (keep_running && slot->clients[i].fd != AVX_INVALID_SOCKET) {
                pfds[nfds].fd = slot->clients[i].fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                client_indices[nfds] = i;
                nfds++;
            }
        }
        avx_mutex_unlock(&slot->lock);

        for (int r = 0; r < nreap; ++r) {
            avx_socket_close(reap_fds[r]);
        }
        if (!keep_running) break;

        if (nfds == 0) {
            avx_sleep_ms(20);
            continue;
        }

        int pr = avx_poll(pfds, nfds, 100);
        if (pr <= 0) continue;

        for (int p = 0; p < nfds; ++p) {
            if (pfds[p].revents == 0) continue;

            int c_idx = client_indices[p];
            avx_socket_t fd = pfds[p].fd;

            if (pfds[p].revents & (POLLHUP | POLLERR)) {
                avx_mutex_lock(&slot->lock);
                if (slot->clients[c_idx].fd == fd) {
                    avx_send_remove_client_locked(slot, c_idx);
                }
                avx_mutex_unlock(&slot->lock);
                continue;
            }

            if (pfds[p].revents & POLLIN) {
                uint32_t magic = 0;
                ssize_t n = recv(fd, (char*)&magic, sizeof(magic), MSG_WAITALL);
                if (n <= 0) {
                    avx_mutex_lock(&slot->lock);
                    if (slot->clients[c_idx].fd == fd) {
                        avx_send_remove_client_locked(slot, c_idx);
                    }
                    avx_mutex_unlock(&slot->lock);
                    continue;
                }

                if (magic == slot->wire_profile.magic_metadata ||
                    (slot->wire_profile.compat_magic_metadata && magic == slot->wire_profile.compat_magic_metadata)) {
                    AVX_PACKED_START
                    struct {
                        int64_t  timecode;
                        uint32_t payload_len;
                    } AVX_PACKED mhdr;
                    AVX_PACKED_END

                    if (recv(fd, (char*)&mhdr, sizeof(mhdr), MSG_WAITALL) != (ssize_t)sizeof(mhdr)) {
                        avx_mutex_lock(&slot->lock);
                        if (slot->clients[c_idx].fd == fd) {
                            avx_send_remove_client_locked(slot, c_idx);
                        }
                        avx_mutex_unlock(&slot->lock);
                        continue;
                    }

                    if (mhdr.payload_len > 0 && mhdr.payload_len < 65536) {
                        char *mbuf = (char*)malloc(mhdr.payload_len + 1);
                        if (!mbuf) continue;
                        if (recv(fd, mbuf, mhdr.payload_len, MSG_WAITALL) != (ssize_t)mhdr.payload_len) {
                            free(mbuf);
                            avx_mutex_lock(&slot->lock);
                            if (slot->clients[c_idx].fd == fd) {
                                avx_send_remove_client_locked(slot, c_idx);
                            }
                            avx_mutex_unlock(&slot->lock);
                            continue;
                        }
                        mbuf[mhdr.payload_len] = '\0';

                        avx_trace_packet("[RX]", "METADATA (REVERSE)", mbuf, mhdr.payload_len);

                        avx_mutex_lock(&slot->lock);
                        if (slot->clients[c_idx].fd == fd) {
                            /* Check for Tally XML */
                            if (strstr(mbuf, slot->wire_profile.tally_tag) != NULL || strstr(mbuf, "tally") != NULL) {
                                const char *prog = strstr(mbuf, "on_program=\"");
                                const char *prev = strstr(mbuf, "on_preview=\"");
                                const char *rec  = strstr(mbuf, "on_recording=\"");
                                if (!rec) rec = strstr(mbuf, "recording=\"");
                                slot->clients[c_idx].tally.flags = OAVX_TALLY_NONE;
                                if (prog && strncmp(prog + 12, "true", 4) == 0) {
                                    slot->clients[c_idx].tally.flags |= OAVX_TALLY_PROGRAM;
                                }
                                if (prev && strncmp(prev + 12, "true", 4) == 0) {
                                    slot->clients[c_idx].tally.flags |= OAVX_TALLY_PREVIEW;
                                }
                                if (rec) {
                                    const char *val = strchr(rec, '"');
                                    if (val && strncmp(val + 1, "true", 4) == 0) {
                                        slot->clients[c_idx].tally.flags |= OAVX_TALLY_RECORDING;
                                    }
                                }
                                avx_send_recalculate_tally_locked(slot);
                            }

                            /* Check for Transport negotiation XML from client */
                            if (strstr(mbuf, "transport") != NULL) {
                                const char *m_attr = strstr(mbuf, "mode=\"");
                                if (m_attr) {
                                    if (strncmp(m_attr + 6, "rudp", 4) == 0) slot->clients[c_idx].transport = OAVX_TRANSPORT_RUDP;
                                    else if (strncmp(m_attr + 6, "udp", 3) == 0) slot->clients[c_idx].transport = OAVX_TRANSPORT_UDP;
                                    else if (strncmp(m_attr + 6, "tcp", 3) == 0) slot->clients[c_idx].transport = OAVX_TRANSPORT_TCP;
                                }
                                const char *p_attr = strstr(mbuf, "port=\"");
                                if (p_attr) {
                                    int cp = atoi(p_attr + 6);
                                    if (cp > 0 && cp <= 65535) {
                                        if (slot->clients[c_idx].udp_dest_addr.ss_family == AF_INET6) {
                                            ((struct sockaddr_in6*)&slot->clients[c_idx].udp_dest_addr)->sin6_port = htons((uint16_t)cp);
                                            slot->clients[c_idx].has_udp_dest = true;
                                        } else if (slot->clients[c_idx].udp_dest_addr.ss_family == AF_INET) {
                                            ((struct sockaddr_in*)&slot->clients[c_idx].udp_dest_addr)->sin_port = htons((uint16_t)cp);
                                            slot->clients[c_idx].has_udp_dest = true;
                                        }
                                    }
                                }

                                const char *c_attr = strstr(mbuf, "codecs=\"");
                                if (c_attr) {
                                    char c_val[64];
                                    c_val[0] = '\0';
                                    const char *q_end = strchr(c_attr + 8, '"');
                                    if (q_end) {
                                        size_t q_len = (size_t)(q_end - (c_attr + 8));
                                        if (q_len >= sizeof(c_val)) q_len = sizeof(c_val) - 1;
                                        memcpy(c_val, c_attr + 8, q_len);
                                        c_val[q_len] = '\0';
                                        slot->clients[c_idx].negotiated_codec_fourcc = avx_negotiate_best_codec(c_val);
                                    }
                                }

                                const char *prof_attr = strstr(mbuf, "profile=\"");
                                bool force_wan = false;
                                bool force_lan = false;
                                if (prof_attr) {
                                    if (strncmp(prof_attr + 9, "wan", 3) == 0) force_wan = true;
                                    else if (strncmp(prof_attr + 9, "lan", 3) == 0) force_lan = true;
                                }

                                bool is_wan = false;
                                if (force_wan) {
                                    is_wan = true;
                                } else if (force_lan) {
                                    is_wan = false;
                                } else {
                                    is_wan = !avx_is_lan_sockaddr(&slot->clients[c_idx].addr);
                                }

                                if (is_wan && !slot->clients[c_idx].is_wan_registered) {
                                    slot->clients[c_idx].is_wan_client = true;
                                    slot->clients[c_idx].is_wan_registered = true;
                                    slot->wan_client_count++;
                                    if (slot->wan_codec_fourcc == 0 || slot->clients[c_idx].negotiated_codec_fourcc != 0) {
                                        slot->wan_codec_fourcc = slot->clients[c_idx].negotiated_codec_fourcc
                                            ? slot->clients[c_idx].negotiated_codec_fourcc
                                            : OAVX_FOURCC_VIDEO_TYPE_H264;
                                    }
                                    if (slot->wan_encoder) {
                                        avx_video_encoder_request_keyframe(slot->wan_encoder);
                                    }
                                } else if (!is_wan && slot->clients[c_idx].is_wan_registered) {
                                    slot->clients[c_idx].is_wan_client = false;
                                    slot->clients[c_idx].is_wan_registered = false;
                                    if (slot->wan_client_count > 0) slot->wan_client_count--;
                                    if (slot->wan_client_count == 0 && slot->wan_encoder) {
                                        avx_video_encoder_destroy(slot->wan_encoder);
                                        slot->wan_encoder = NULL;
                                        slot->wan_enc_width = 0;
                                        slot->wan_enc_height = 0;
                                        slot->wan_enc_fourcc = 0;
                                    }
                                    if (slot->video_encoder) {
                                        avx_video_encoder_request_keyframe(slot->video_encoder);
                                    }
                                }
                            }

                            /* Store last metadata */
                            snprintf(slot->last_metadata, sizeof(slot->last_metadata), "%s", mbuf);
                            slot->has_new_metadata = true;
                        }
                        avx_mutex_unlock(&slot->lock);

                        free(mbuf);
                    }
                }
            }
        }
    }
    return NULL;
}

/* TCP listener thread accepting incoming connections and performing initial client handshake. */
void* avx_send_listen_worker(void* arg) {
    avx_send_ctx_t* slot = (avx_send_ctx_t*)arg;

    while (1) {
        avx_socket_t listen_fd;
        avx_mutex_lock(&slot->lock);
        bool keep_running = slot->running;
        listen_fd = slot->tcp_listen_fd;
        int64_t now_ns = avx_time_ns();
        bool do_mdns = (keep_running &&
                        (slot->last_mdns_broadcast_ns == 0 ||
                         (now_ns - slot->last_mdns_broadcast_ns) > 30000000000LL));
        if (do_mdns) {
            slot->last_mdns_broadcast_ns = now_ns;
        }
        avx_mutex_unlock(&slot->lock);

        if (listen_fd == AVX_INVALID_SOCKET || !keep_running) {
            break;
        }

        if (do_mdns) {
            avx_mdns_broadcast_source_ex(slot->name, slot->port, slot->advertised_ip);
        }

        avx_pollfd_t pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = avx_poll(&pfd, 1, 50);
        if (pr <= 0) {
            continue;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            break;
        }

        if (!(pfd.revents & POLLIN)) {
            continue;
        }

        struct sockaddr_storage client_addr;
        avx_socklen_t client_len = sizeof(client_addr);
        avx_socket_t client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd == AVX_INVALID_SOCKET) {
            avx_mutex_lock(&slot->lock);
            bool still_running = slot->running;
            avx_mutex_unlock(&slot->lock);
            if (!still_running) break;
            avx_sleep_ms(10);
            continue;
        }

        avx_socket_set_nonblocking(client_fd, false);
        avx_socket_set_nosigpipe(client_fd);
        avx_socket_set_keepalive(client_fd, 2, 1, 3);
        avx_socket_set_tcp_nodelay(client_fd, true);
        int buf_size = 16 * 1024 * 1024;
        setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buf_size, sizeof(buf_size));
        setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size));

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 250000; /* 250 ms send timeout to prevent blocking indefinitely */
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        avx_mutex_lock(&slot->lock);

        /* Check if we reached max simultaneous clients */
        if (slot->active_clients >= slot->max_clients) {
            avx_mutex_unlock(&slot->lock);
            avx_socket_close(client_fd);
            continue;
        }

        /* Find free client slot */
        int assigned_slot = -1;
        avx_thread_t prev_thread = 0;
        bool join_prev = false;

        for (int i = 0; i < slot->max_clients && i < AVX_MAX_CLIENTS; ++i) {
            if (slot->clients[i].fd == AVX_INVALID_SOCKET) {
                assigned_slot = i;
                if (slot->clients[i].tx_thread_active) {
                    prev_thread = slot->clients[i].tx_thread;
                    join_prev = true;
                    slot->clients[i].tx_thread_active = false;
                    slot->clients[i].tx_thread = 0;
                }
                break;
            }
        }

        if (assigned_slot < 0) {
            avx_mutex_unlock(&slot->lock);
            avx_socket_close(client_fd);
            continue;
        }

        /* If previous thread was active on this slot, signal it to stop */
        if (join_prev) {
            avx_mutex_lock(&slot->clients[assigned_slot].tx_lock);
            slot->clients[assigned_slot].tx_running = false;
            avx_cond_broadcast(&slot->clients[assigned_slot].tx_cond);
            avx_mutex_unlock(&slot->clients[assigned_slot].tx_lock);
        }

        avx_mutex_unlock(&slot->lock);

        /* Join previous thread completely before reusing this slot */
        if (join_prev) {
            avx_thread_join(prev_thread);
            avx_client_queue_drain(slot, &slot->clients[assigned_slot]);
        }

        /* Now acquire lock and safely initialize the slot for the new client */
        avx_mutex_lock(&slot->lock);
        if (!slot->running) {
            avx_mutex_unlock(&slot->lock);
            avx_socket_close(client_fd);
            break;
        }

        if (slot->clients[assigned_slot].pending_close_fd != AVX_INVALID_SOCKET) {
            avx_socket_close(slot->clients[assigned_slot].pending_close_fd);
            slot->clients[assigned_slot].pending_close_fd = AVX_INVALID_SOCKET;
        }
        slot->clients[assigned_slot].fd = client_fd;
        slot->clients[assigned_slot].addr = client_addr;
        slot->clients[assigned_slot].connected_time_ns = avx_time_ns();
        slot->clients[assigned_slot].dropped_frames = 0;
        slot->clients[assigned_slot].tally.flags = OAVX_TALLY_NONE;
        slot->clients[assigned_slot].transport = slot->transport;
        slot->clients[assigned_slot].has_udp_dest = false;
        memset(&slot->clients[assigned_slot].udp_dest_addr, 0, sizeof(slot->clients[assigned_slot].udp_dest_addr));
        slot->clients[assigned_slot].udp_dest_addr = client_addr;
        if (client_addr.ss_family == AF_INET) {
            ((struct sockaddr_in*)&slot->clients[assigned_slot].udp_dest_addr)->sin_port = 0;
        } else if (client_addr.ss_family == AF_INET6) {
            ((struct sockaddr_in6*)&slot->clients[assigned_slot].udp_dest_addr)->sin6_port = 0;
        }
        slot->active_clients++;

        if (slot->tcp_client_fd == AVX_INVALID_SOCKET) {
            slot->tcp_client_fd = client_fd;
        }

        bool is_wan = !avx_is_lan_sockaddr(&client_addr);
        slot->clients[assigned_slot].is_wan_client = is_wan;
        slot->clients[assigned_slot].is_wan_registered = is_wan;
        slot->clients[assigned_slot].negotiated_codec_fourcc = OAVX_FOURCC_VIDEO_TYPE_H264;
        if (is_wan) {
            slot->wan_client_count++;
            if (slot->wan_codec_fourcc == 0) slot->wan_codec_fourcc = OAVX_FOURCC_VIDEO_TYPE_H264;
        }

        /* Reset per-client queue and enable queueing */
        avx_mutex_lock(&slot->clients[assigned_slot].tx_lock);
        slot->clients[assigned_slot].queue_head = 0;
        slot->clients[assigned_slot].queue_tail = 0;
        slot->clients[assigned_slot].queue_count = 0;
        slot->clients[assigned_slot].video_queued = 0;
        slot->clients[assigned_slot].congestion_level = 0;
        slot->clients[assigned_slot].frame_seq = 0;
        slot->clients[assigned_slot].audio_packets_sent = 0;
        slot->clients[assigned_slot].video_packets_sent = 0;
        slot->clients[assigned_slot].tcp_bytes_submitted = 0;
        slot->clients[assigned_slot].last_acked_bytes = 0;
        slot->clients[assigned_slot].rate_window_start_acked = 0;
        slot->clients[assigned_slot].rate_window_start_sub = 0;
        slot->clients[assigned_slot].rate_window_start_ns = 0;
        slot->clients[assigned_slot].rate_window_throttled = false;
        slot->clients[assigned_slot].tx_bytes_per_sec = -1.0f;
        slot->clients[assigned_slot].last_rtt_ms = 0.0f;
        slot->clients[assigned_slot].last_rtt_sample_ns = 0;
        slot->clients[assigned_slot].tx_running = true;
        slot->clients[assigned_slot].tx_exited = false;
        avx_mutex_unlock(&slot->clients[assigned_slot].tx_lock);

        /* Start reverse reader worker if not running */
        if (!slot->client_rx_running) {
            slot->client_rx_running = true;
            avx_thread_create(&slot->client_rx_thread, avx_send_client_rx_worker, slot);
        }

        /* Request immediate keyframe from video encoders so new client receives immediate IDR */
        if (slot->video_encoder) {
            avx_video_encoder_request_keyframe(slot->video_encoder);
        }
        if (slot->wan_encoder) {
            avx_video_encoder_request_keyframe(slot->wan_encoder);
        }

        avx_mutex_unlock(&slot->lock);

        /* Send standard wire protocol XML handshake */
        const char *t_str = (slot->transport == OAVX_TRANSPORT_RUDP) ? "rudp" :
                            (slot->transport == OAVX_TRANSPORT_UDP ? "udp" : "tcp");
        char xml[512];
        int xml_len = snprintf(xml, sizeof(xml),
            "<%s name=\"%s\" port=\"%u\" version=\"0.2.2\">\n"
            "  <capabilities video=\"1\" audio=\"1\" metadata=\"1\" transport=\"%s\" udp_port=\"%u\"/>\n"
            "</%s>\n",
            slot->wire_profile.handshake_tag, slot->name, slot->port,
            t_str, slot->udp_port, slot->wire_profile.handshake_tag);

        uint32_t payload_len = (uint32_t)xml_len;
        uint64_t init_tcp_bytes = 0;
        ssize_t n_hdr = send(client_fd, (const char*)&payload_len, sizeof(payload_len), MSG_NOSIGNAL);
        if (n_hdr > 0) init_tcp_bytes += (uint64_t)n_hdr;
        ssize_t n_xml = send(client_fd, xml, payload_len, MSG_NOSIGNAL);
        if (n_xml > 0) init_tcp_bytes += (uint64_t)n_xml;

        avx_trace_packet("[TX]", "AVX TCP HANDSHAKE", xml, payload_len);
        uint8_t *handshake_pkt = (uint8_t*)malloc(4 + payload_len);
        if (handshake_pkt) {
            memcpy(handshake_pkt, &payload_len, 4);
            memcpy(handshake_pkt + 4, xml, payload_len);
            avx_pcap_write_tcp(htonl(0x7F000001), slot->port, htonl(0x7F000001), 54321, 1, 1, 0x18, handshake_pkt, 4 + payload_len);
            free(handshake_pkt);
        }

        /* Send any queued connection metadata */
        avx_mutex_lock(&slot->lock);
        if (!slot->running) {
            avx_send_remove_client_locked(slot, assigned_slot);
            avx_mutex_unlock(&slot->lock);
            break;
        }

        for (size_t m = 0; m < slot->num_conn_metadata; ++m) {
            size_t mlen = strlen(slot->conn_metadata[m]);
            AVX_PACKED_START
            struct {
                uint32_t magic;
                int64_t  timecode;
                uint32_t payload_len;
            } AVX_PACKED mhdr;
            AVX_PACKED_END
            mhdr.magic = slot->wire_profile.magic_metadata;
            mhdr.timecode = 0;
            mhdr.payload_len = (uint32_t)mlen;
            ssize_t nm1 = send(client_fd, (const char*)&mhdr, sizeof(mhdr), MSG_NOSIGNAL);
            if (nm1 > 0) init_tcp_bytes += (uint64_t)nm1;
            ssize_t nm2 = send(client_fd, slot->conn_metadata[m], mlen, MSG_NOSIGNAL);
            if (nm2 > 0) init_tcp_bytes += (uint64_t)nm2;
        }

        if (init_tcp_bytes > 0 && slot->clients[assigned_slot].fd == client_fd) {
            avx_mutex_lock(&slot->clients[assigned_slot].tx_lock);
            slot->clients[assigned_slot].tcp_bytes_submitted += init_tcp_bytes;
            avx_mutex_unlock(&slot->clients[assigned_slot].tx_lock);
        }

        /* Launch worker thread to pump async TX queue */
        if (avx_thread_create(&slot->clients[assigned_slot].tx_thread,
                              avx_send_client_tx_worker,
                              &slot->clients[assigned_slot]) == 0) {
            slot->clients[assigned_slot].tx_thread_active = true;
        } else {
            slot->clients[assigned_slot].tx_thread_active = false;
            slot->clients[assigned_slot].tx_exited = true;
        }

        avx_mutex_unlock(&slot->lock);
    }
    return NULL;
}

bool avx_send_start_listener(avx_send_ctx_t* send_ctx) {
    if (!send_ctx) return false;

    const char *bind_ip = (send_ctx->bind_ip[0] != '\0') ? send_ctx->bind_ip : NULL;
    send_ctx->tcp_listen_fd = avx_socket_create_tcp_server_bind(bind_ip, send_ctx->port);
    if (send_ctx->tcp_listen_fd == AVX_INVALID_SOCKET) {
        /* Try dynamic port fallback */
        uint16_t base_port = send_ctx->port ? send_ctx->port : OAVX_WIRE_DEFAULT_PORT;
        for (uint16_t alt_port = base_port + 1; alt_port <= base_port + 10; ++alt_port) {
            send_ctx->tcp_listen_fd = avx_socket_create_tcp_server_bind(bind_ip, alt_port);
            if (send_ctx->tcp_listen_fd != AVX_INVALID_SOCKET) {
                send_ctx->port = alt_port;
                break;
            }
        }
    }

    if (send_ctx->tcp_listen_fd == AVX_INVALID_SOCKET) return false;

    avx_socket_set_nonblocking(send_ctx->tcp_listen_fd, true);

    if (avx_thread_create(&send_ctx->listen_thread, avx_send_listen_worker, send_ctx) != 0) {
        avx_socket_close(send_ctx->tcp_listen_fd);
        send_ctx->tcp_listen_fd = AVX_INVALID_SOCKET;
        return false;
    }

    return true;
}

OPENAVX_API void avx_send_stop_listener(avx_send_ctx_t* send_ctx) {
    if (!send_ctx) return;
    avx_socket_t close_client_fds[AVX_MAX_CLIENTS];
    avx_socket_t close_listen_fd = AVX_INVALID_SOCKET;
    for (int i = 0; i < AVX_MAX_CLIENTS; ++i) close_client_fds[i] = AVX_INVALID_SOCKET;

    avx_mutex_lock(&send_ctx->lock);
    send_ctx->running = false;
    send_ctx->client_rx_running = false;
    send_ctx->nack_thread_running = false;

    /* Shutdown active client sockets and clear fds; close only after workers join */
    for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
        avx_mutex_lock(&send_ctx->clients[i].tx_lock);
        if (send_ctx->clients[i].fd != AVX_INVALID_SOCKET) {
            close_client_fds[i] = send_ctx->clients[i].fd;
            shutdown(close_client_fds[i], AVX_SHUT_RDWR);
            send_ctx->clients[i].fd = AVX_INVALID_SOCKET;
        }
        send_ctx->clients[i].tx_running = false;
        avx_cond_broadcast(&send_ctx->clients[i].tx_cond);
        avx_mutex_unlock(&send_ctx->clients[i].tx_lock);
    }
    send_ctx->active_clients = 0;
    send_ctx->tcp_client_fd = AVX_INVALID_SOCKET;
    if (send_ctx->tcp_listen_fd != AVX_INVALID_SOCKET) {
        close_listen_fd = send_ctx->tcp_listen_fd;
        shutdown(close_listen_fd, AVX_SHUT_RDWR);
        send_ctx->tcp_listen_fd = AVX_INVALID_SOCKET;
    }
    avx_mutex_unlock(&send_ctx->lock);

    /* Join all active client tx worker threads */
    for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
        if (send_ctx->clients[i].tx_thread_active) {
            avx_thread_join(send_ctx->clients[i].tx_thread);
            send_ctx->clients[i].tx_thread_active = false;
            send_ctx->clients[i].tx_thread = 0;
        }
        avx_client_queue_drain(send_ctx, &send_ctx->clients[i]);
    }

    if (send_ctx->listen_thread) {
        avx_thread_join(send_ctx->listen_thread);
        send_ctx->listen_thread = 0;
    }
    if (send_ctx->client_rx_thread) {
        avx_thread_join(send_ctx->client_rx_thread);
        send_ctx->client_rx_thread = 0;
    }
    if (send_ctx->nack_thread_active) {
        avx_thread_join(send_ctx->nack_thread);
        send_ctx->nack_thread_active = false;
        send_ctx->nack_thread = 0;
    }

    if (close_listen_fd != AVX_INVALID_SOCKET) {
        avx_socket_close(close_listen_fd);
    }
    for (int i = 0; i < AVX_MAX_CLIENTS; ++i) {
        if (close_client_fds[i] != AVX_INVALID_SOCKET) {
            avx_socket_close(close_client_fds[i]);
        }
        if (send_ctx->clients[i].pending_close_fd != AVX_INVALID_SOCKET) {
            avx_socket_close(send_ctx->clients[i].pending_close_fd);
            send_ctx->clients[i].pending_close_fd = AVX_INVALID_SOCKET;
        }
    }
}
