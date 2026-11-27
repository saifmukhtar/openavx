/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_udp.h - UDP datagram transport and fragmentation
 */

#ifndef AVX_UDP_H
#define AVX_UDP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "avx_platform.h"
#include "openavx.h"
#include "avx_fec.h"
#include "avx_crypto.h"

#define OAVX_UDP_MAGIC 0x55585641 /* Native OpenAVX UDP datagram magic ('AVXU') */
#define AVX_UDP_MAGIC OAVX_UDP_MAGIC
#define OAVX_UDP_NACK_MAGIC 0x55584E41 /* Native OpenAVX UDP NACK datagram magic ('ANXU') */
#define AVX_UDP_NACK_MAGIC OAVX_UDP_NACK_MAGIC
#define OAVX_UDP_ACK_MAGIC 0x55584B41 /* Native OpenAVX UDP ACK/Feedback datagram magic ('AKXU') */
#define AVX_UDP_ACK_MAGIC OAVX_UDP_ACK_MAGIC
#define AVX_UDP_MAX_FRAG_SIZE 1400
#define AVX_UDP_MAX_FRAGS 65535
#define AVX_UDP_NACK_MAX_RANGES 8

/* UDP Header Flags (stored in avx_udp_header_t.reserved) */
#define AVX_UDP_FLAG_ENCRYPTED 0x0001

#ifdef __cplusplus
extern "C" {
#endif

AVX_PACKED_START
typedef struct {
    uint32_t magic;       /* AVX_UDP_MAGIC */
    uint32_t frame_seq;   /* Frame sequence counter */
    uint16_t frag_idx;    /* Fragment index (0 .. frag_count-1) */
    uint16_t frag_count;  /* Total fragments in frame */
    uint32_t fourcc;      /* Video/Audio FourCC */
    int64_t timecode;     /* Presentation timecode */
    uint16_t payload_len; /* Length of payload in this fragment */
    uint16_t send_ts_ms;  /* Sender transmission clock (ms mod 65536) */
    uint16_t srtt_ms;     /* Sender current smoothed RTT in ms */
    uint16_t reserved;    /* Reserved / alignment (32-byte header) */
} AVX_PACKED avx_udp_header_t;

typedef struct {
    uint16_t start; /* First missing fragment index */
    uint16_t count; /* Number of consecutive missing fragments */
} AVX_PACKED avx_udp_nack_range_t;

typedef struct {
    uint32_t magic;       /* AVX_UDP_NACK_MAGIC */
    uint32_t frame_seq;   /* Frame sequence counter needing retransmission */
    uint16_t range_count; /* Number of missing ranges in this NACK (1..8) */
    uint16_t echo_ts_ms;  /* Echo of sender latest send_ts_ms */
    uint16_t rx_delay_ms; /* Time elapsed at receiver since receiving echo_ts_ms */
    uint16_t reserved;    /* Reserved / alignment */
    avx_udp_nack_range_t ranges[AVX_UDP_NACK_MAX_RANGES];
} AVX_PACKED avx_udp_nack_t;

typedef struct {
    uint32_t magic;          /* AVX_UDP_ACK_MAGIC */
    uint32_t last_frame_seq; /* Highest delivered frame sequence */
    uint16_t echo_ts_ms;     /* Echoed sender timestamp */
    uint16_t rx_delay_ms;    /* Receiver turnaround delay */
    uint32_t rx_frames;      /* Delivered frames count */
    uint32_t lost_packets;   /* Lost packets count */
    uint16_t rtt_ms;         /* Receiver estimated RTT in ms */
    uint16_t jitter_ms;      /* Receiver estimated jitter in ms */
} AVX_PACKED avx_udp_feedback_t;
AVX_PACKED_END

/* RUDP Telemetry statistics */
typedef struct {
    uint32_t srtt_ms;               /* Smoothed RTT in milliseconds */
    uint32_t rttvar_ms;             /* RTT variance in milliseconds */
    uint64_t packets_sent;          /* Total UDP datagrams sent */
    uint64_t packets_recv;          /* Total UDP datagrams received */
    uint64_t packets_retransmitted; /* Packets retransmitted via NACK */
    uint64_t packets_recovered;     /* Lost packets recovered via NACK */
    uint64_t packets_lost;          /* Permanently lost packets */
} avx_rudp_telemetry_t;

/* Opaque RUDP Session Handles */
typedef struct avx_udp_rudp_sender_t avx_udp_rudp_sender_t;
typedef struct avx_udp_rudp_receiver_t avx_udp_rudp_receiver_t;

/**
 * Creates an RUDP sender session with a circulating retransmission ring buffer.
 * max_cached_frames: Number of previous frames to retain for retransmission (e.g. 8).
 */
OPENAVX_API avx_udp_rudp_sender_t *avx_udp_rudp_sender_create(avx_socket_t sock, int max_cached_frames);
OPENAVX_API void avx_udp_rudp_sender_destroy(avx_udp_rudp_sender_t *s);

/**
 * Sends a frame via RUDP:
 * Fragments and transmits datagrams, archives the frame into the retransmission ring,
 * and processes any pending incoming NACK datagrams on the sender socket.
 * dest: Destination address pointer (struct sockaddr_in*, sockaddr_in6*, or sockaddr_storage*).
 */
OPENAVX_API int avx_udp_rudp_send_frame(avx_udp_rudp_sender_t *s, const void *dest, uint32_t frame_seq, uint32_t fourcc,
                                        int64_t timecode, const void *payload, size_t payload_len);

/**
 * Services incoming NACK packets on the sender socket and retransmits missing slices.
 * Returns number of retransmitted fragments (>=0), or -1 on error.
 */
OPENAVX_API int avx_udp_rudp_sender_service_nacks(avx_udp_rudp_sender_t *s);

/**
 * Creates an RUDP receiver session.
 */
OPENAVX_API avx_udp_rudp_receiver_t *avx_udp_rudp_receiver_create(avx_socket_t sock);
OPENAVX_API void avx_udp_rudp_receiver_destroy(avx_udp_rudp_receiver_t *r);

/**
 * Receives and reassembles a frame via RUDP:
 * Gathers incoming datagrams, detects packet loss gaps, automatically transmits Selective
 * NACK datagrams back to sender_addr to request retransmission, and delivers complete frame.
 * sender_addr: Sender address pointer (struct sockaddr_in*, sockaddr_in6*, or sockaddr_storage*).
 */
OPENAVX_API int avx_udp_rudp_recv_frame(avx_udp_rudp_receiver_t *r, const void *sender_addr, void *out_buf,
                                        size_t out_max, size_t *out_size, uint32_t *out_fourcc, int64_t *out_timecode,
                                        int timeout_ms);

/**
 * Configures forward error correction (FEC) on the RUDP sender.
 * If cfg is NULL or cfg->enabled is false, FEC transmission is disabled.
 */
OPENAVX_API void avx_udp_rudp_sender_set_fec(avx_udp_rudp_sender_t *s, const avx_fec_config_t *cfg);

/**
 * Configures forward error correction (FEC) on the RUDP receiver.
 * If cfg is NULL or cfg->enabled is false, FEC reconstruction is disabled.
 */
OPENAVX_API void avx_udp_rudp_receiver_set_fec(avx_udp_rudp_receiver_t *r, const avx_fec_config_t *cfg);

/**
 * Configures native end-to-end encryption (ChaCha20-Poly1305) on the RUDP sender.
 * If passphrase is NULL or empty, encryption is disabled.
 */
OPENAVX_API int avx_udp_rudp_sender_set_passphrase(avx_udp_rudp_sender_t *s, const char *passphrase,
                                                   const char *stream_name);

/**
 * Configures native end-to-end encryption (ChaCha20-Poly1305) on the RUDP receiver.
 * If passphrase is NULL or empty, decryption is disabled.
 */
OPENAVX_API int avx_udp_rudp_receiver_set_passphrase(avx_udp_rudp_receiver_t *r, const char *passphrase,
                                                     const char *stream_name);

/**
 * Retrieves current RUDP sender telemetry statistics (RTT, variance, packet counters).
 */
OPENAVX_API bool avx_udp_rudp_sender_get_telemetry(const avx_udp_rudp_sender_t *s, avx_rudp_telemetry_t *out_telem);

/**
 * Retrieves current RUDP receiver telemetry statistics (RTT, variance, packet counters).
 */
OPENAVX_API bool avx_udp_rudp_receiver_get_telemetry(const avx_udp_rudp_receiver_t *r, avx_rudp_telemetry_t *out_telem);

/**
 * Creates a non-blocking dual-stack UDP socket.
 * If port > 0, binds to port (for receivers/listeners).
 * Defaults to dual-stack IPv4/IPv6 wildcard binding with IPv4 fallback.
 */
OPENAVX_API avx_socket_t avx_udp_create_socket(uint16_t port, bool reuse);

/**
 * Creates a UDP socket bound to a specific IP / interface address and port.
 * If bind_ip is NULL, "", "0.0.0.0", or "::", performs a dual-stack wildcard bind.
 */
OPENAVX_API avx_socket_t avx_udp_create_socket_bind(const char *bind_ip, uint16_t port, bool reuse);

/**
 * Joins a multicast group (IPv4 IGMP or IPv6 MLD).
 * mcast_addr: IPv4 multicast ("224.0.0.0" - "239.255.255.255") or IPv6 ("ff00::/8") address.
 * iface_or_ip: Optional interface name (e.g. "eth0", "lo") or local IP, or NULL for default.
 * Returns true on success, false on error.
 */
OPENAVX_API bool avx_udp_join_multicast(avx_socket_t sock, const char *mcast_addr, const char *iface_or_ip);

/**
 * Leaves a multicast group.
 * Returns true on success, false on error.
 */
OPENAVX_API bool avx_udp_leave_multicast(avx_socket_t sock, const char *mcast_addr, const char *iface_or_ip);

/**
 * Sets multicast TTL (IPv4) or Hop Limit (IPv6).
 */
OPENAVX_API bool avx_udp_set_multicast_ttl(avx_socket_t sock, int ttl);

/**
 * Sets multicast loopback (whether sent multicast packets loop back to local sockets).
 */
OPENAVX_API bool avx_udp_set_multicast_loop(avx_socket_t sock, bool loop);

/**
 * Fragment and send an entire frame over UDP datagrams.
 * dest: Destination address pointer (struct sockaddr_in*, sockaddr_in6*, or sockaddr_storage*).
 */
OPENAVX_API int avx_udp_send_frame(avx_socket_t sock, const void *dest, uint32_t frame_seq, uint32_t fourcc,
                                   int64_t timecode, const void *payload, size_t payload_len);

/**
 * Receive and reassemble fragments into a complete frame.
 * Returns 0 on success, -1 on timeout/error.
 */
OPENAVX_API int avx_udp_recv_frame(avx_socket_t sock, void *out_buf, size_t out_max, size_t *out_size,
                                   uint32_t *out_fourcc, int64_t *out_timecode, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AVX_UDP_H */
