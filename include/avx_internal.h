/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_internal.h - Internal state and subsystem definitions
 */

#ifndef AVX_INTERNAL_H
#define AVX_INTERNAL_H

#include "openavx.h"
#include "avx_platform.h"
#include "avx_config.h"
#include "avx_wire.h"
#include "avx_udp.h"
#include "avx_audio_codec.h"
#include "avx_video_codec.h"

#define OPENAVX_VERSION "openavx v0.2.0"
#define AVX_MAX_SESSIONS 32
#define AVX_DEFAULT_PORT 7960
#define AVX_MDNS_PORT 5353
#define AVX_MDNS_MCAST_ADDR "224.0.0.251"

/* Video Dimension Bounds & Buffer Safety Limits */
#define AVX_MAX_VIDEO_WIDTH  16384
#define AVX_MAX_VIDEO_HEIGHT 16384
#define AVX_MAX_DECOMPRESSED_FRAME_SIZE (512ULL * 1024ULL * 1024ULL)

/* Native Internal Handles & Configuration */
typedef void* avx_find_instance_t;
typedef void* avx_tx_instance_t;
typedef void* avx_rx_instance_t;
#ifndef AVX_ROUTER_INSTANCE_T
#define AVX_ROUTER_INSTANCE_T
typedef void* avx_router_instance_t;
#endif
typedef void* avx_sync_instance_t;

typedef struct avx_find_config_t {
    bool                       show_local_sources;
    const char                *groups;
    const char                *extra_ips;
    const oavx_wire_profile_t *wire_profile;
} avx_find_config_t;

typedef struct avx_tx_config_t {
    const char                *name;
    const char                *groups;
    bool                       clock_video;
    bool                       clock_audio;
    const oavx_wire_profile_t *wire_profile;
    uint32_t                   max_connections;
    const char                *bind_ip;
    oavx_transport_e           transport;
    uint32_t                   video_codec_fourcc;
    uint32_t                   video_bitrate_kbps;
    uint32_t                   keyframe_interval;
    const char                *passphrase;
} avx_tx_config_t;

typedef struct avx_rx_config_t {
    oavx_endpoint_t             source_to_connect_to;
    oavx_color_format_e         color_format;
    oavx_bandwidth_e            bandwidth;
    bool                        allow_video_fields;
    const char                 *name;
    const oavx_wire_profile_t  *wire_profile;
    oavx_transport_e            transport;
    const char                 *passphrase;
} avx_rx_config_t;

typedef struct avx_router_config_t {
    const char *name;
    const char *groups;
} avx_router_config_t;

typedef struct avx_send_ctx_t avx_send_ctx_t;

/* Internal Subsystem Lifecycle */
OPENAVX_API bool avx_engine_initialize(void);
OPENAVX_API void avx_engine_destroy(void);
OPENAVX_API const char* avx_engine_version(void);
OPENAVX_API bool avx_engine_is_supported_cpu(void);

/* Transmitter Engine (src/avx_send.c) */
OPENAVX_API avx_tx_instance_t avx_tx_create(const avx_tx_config_t* settings);
OPENAVX_API void avx_tx_destroy(avx_tx_instance_t instance);
OPENAVX_API void avx_tx_send_video(avx_tx_instance_t instance, const oavx_video_frame_t* video_data);
OPENAVX_API void avx_tx_send_video_async(avx_tx_instance_t instance, const oavx_video_frame_t* video_data);
OPENAVX_API void avx_tx_send_audio(avx_tx_instance_t instance, const oavx_audio_frame_t* audio_data);
OPENAVX_API void avx_send_stop_listener(avx_send_ctx_t* slot);
void avx_tx_send_metadata(avx_tx_instance_t instance, const oavx_metadata_frame_t* metadata);
oavx_frame_type_t avx_tx_capture_metadata(avx_tx_instance_t instance, oavx_metadata_frame_t* metadata, uint32_t timeout_ms);
void avx_tx_free_metadata(avx_tx_instance_t instance, const oavx_metadata_frame_t* metadata);
bool avx_tx_get_tally(avx_tx_instance_t instance, oavx_tally_t* tally, uint32_t timeout_ms);
bool avx_tx_set_tally(avx_tx_instance_t instance, const oavx_tally_t* tally);
void avx_tx_clear_connection_metadata(avx_tx_instance_t instance);
void avx_tx_add_connection_metadata(avx_tx_instance_t instance, const oavx_metadata_frame_t* metadata);
void avx_tx_set_failover(avx_tx_instance_t instance, const oavx_endpoint_t* failover_source);
int avx_tx_get_connection_count(avx_tx_instance_t instance, uint32_t timeout_ms);
OPENAVX_API const oavx_endpoint_t* avx_tx_get_source(avx_tx_instance_t instance);
bool avx_tx_enable_rtsp_server(avx_tx_instance_t instance, uint16_t port, const char *path);
void avx_tx_disable_rtsp_server(avx_tx_instance_t instance);
uint16_t avx_tx_get_rtsp_port(avx_tx_instance_t instance);

/* Receiver Engine (src/avx_recv.c & src/avx_recv_ex.c) */
OPENAVX_API avx_rx_instance_t avx_rx_create(const avx_rx_config_t* settings);
OPENAVX_API void avx_rx_destroy(avx_rx_instance_t instance);
OPENAVX_API void avx_rx_connect(avx_rx_instance_t instance, const oavx_endpoint_t* source);
OPENAVX_API oavx_frame_type_t avx_rx_receive(
    avx_rx_instance_t instance,
    oavx_video_frame_t* video_data,
    oavx_audio_frame_t* audio_data,
    oavx_metadata_frame_t* metadata,
    uint32_t timeout_ms);
void avx_rx_free_video(avx_rx_instance_t instance, const oavx_video_frame_t* video_data);
void avx_rx_free_audio(avx_rx_instance_t instance, const oavx_audio_frame_t* audio_data);
void avx_rx_free_metadata(avx_rx_instance_t instance, const oavx_metadata_frame_t* metadata);
bool avx_rx_get_performance(avx_rx_instance_t instance, oavx_performance_t* total, oavx_performance_t* dropped);
bool avx_rx_get_queue(avx_rx_instance_t instance, oavx_queue_t* total);
void avx_rx_clear_connection_metadata(avx_rx_instance_t instance);
void avx_rx_add_connection_metadata(avx_rx_instance_t instance, const oavx_metadata_frame_t* metadata);
bool avx_rx_send_metadata(avx_rx_instance_t instance, const oavx_metadata_frame_t* metadata);
bool avx_rx_set_tally(avx_rx_instance_t instance, const oavx_tally_t* tally);
bool avx_rx_get_tally(avx_rx_instance_t instance, oavx_tally_t* tally, uint32_t timeout_ms);
int avx_rx_get_connection_count(avx_rx_instance_t instance);
const char* avx_rx_get_web_control(avx_rx_instance_t instance);

/* PTZ & Remote Recording Controls (src/avx_recv_ex.c) */
bool avx_rx_ptz_zoom(avx_rx_instance_t instance, float zoom_val);
bool avx_rx_ptz_zoom_speed(avx_rx_instance_t instance, float zoom_speed);
bool avx_rx_ptz_pan_tilt(avx_rx_instance_t instance, float pan_val, float tilt_val);
bool avx_rx_ptz_pan_tilt_speed(avx_rx_instance_t instance, float pan_speed, float tilt_speed);
bool avx_rx_ptz_store_preset(avx_rx_instance_t instance, int preset_no);
bool avx_rx_ptz_recall_preset(avx_rx_instance_t instance, int preset_no, float speed);
bool avx_rx_ptz_auto_focus(avx_rx_instance_t instance);
bool avx_rx_ptz_focus(avx_rx_instance_t instance, float focus_val);
bool avx_rx_ptz_focus_speed(avx_rx_instance_t instance, float focus_speed);
bool avx_rx_ptz_white_balance_auto(avx_rx_instance_t instance);
bool avx_rx_ptz_white_balance_indoor(avx_rx_instance_t instance);
bool avx_rx_ptz_white_balance_outdoor(avx_rx_instance_t instance);
bool avx_rx_ptz_white_balance_oneshot(avx_rx_instance_t instance);
bool avx_rx_ptz_white_balance_manual(avx_rx_instance_t instance, float red, float blue);
bool avx_rx_ptz_exposure_auto(avx_rx_instance_t instance);
bool avx_rx_ptz_exposure_manual(avx_rx_instance_t instance, float iris, float gain, float shutter);
bool avx_rx_recording_start(avx_rx_instance_t instance, const char* filename_hint);
bool avx_rx_recording_stop(avx_rx_instance_t instance);
bool avx_rx_recording_set_audio_level(avx_rx_instance_t instance, float level_db);
bool avx_rx_recording_is_recording(avx_rx_instance_t instance);
const char* avx_rx_recording_get_filename(avx_rx_instance_t instance);
const char* avx_rx_recording_get_error(avx_rx_instance_t instance);
void avx_rx_set_failover(avx_rx_instance_t instance, const oavx_endpoint_t* failover_source);
bool avx_rx_is_on_failover(avx_rx_instance_t instance);
void avx_rx_set_failover_timeout(avx_rx_instance_t instance, uint32_t timeout_ms);
void avx_rx_set_auto_revert(avx_rx_instance_t instance, bool auto_revert, uint32_t hold_time_ms);

/* Discovery Engine (src/avx_find.c) */
OPENAVX_API avx_find_instance_t avx_find_create(const avx_find_config_t* settings);
OPENAVX_API void avx_find_destroy(avx_find_instance_t instance);
OPENAVX_API const oavx_endpoint_t* avx_find_get_sources(avx_find_instance_t instance, uint32_t* count);
OPENAVX_API bool avx_find_wait(avx_find_instance_t instance, uint32_t timeout_ms);

/* Matrix Router (src/avx_routing.c) */
avx_router_instance_t avx_router_create(const avx_router_config_t* settings);
void avx_router_destroy(avx_router_instance_t instance);
bool avx_router_route(avx_router_instance_t instance, const oavx_endpoint_t* source);
bool avx_router_clear(avx_router_instance_t instance);
oavx_endpoint_t avx_router_get_source(avx_router_instance_t instance);

/* Frame Synchronizer (src/avx_framesync.c) */
OPENAVX_API avx_sync_instance_t avx_sync_create(avx_rx_instance_t receiver);
OPENAVX_API void avx_sync_destroy(avx_sync_instance_t instance);
void avx_sync_capture_video(avx_sync_instance_t instance, oavx_video_frame_t* frame, int field_type);
void avx_sync_free_video(avx_sync_instance_t instance, oavx_video_frame_t* frame);
void avx_sync_capture_audio(avx_sync_instance_t instance, oavx_audio_frame_t* frame, int sample_rate, int channels, int frames);
void avx_sync_free_audio(avx_sync_instance_t instance, oavx_audio_frame_t* frame);
int avx_sync_audio_queue_depth(avx_sync_instance_t instance);

/* Format Utilities (src/avx_utilities.c) */
void avx_util_audio_to_interleaved_16s(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_16s_t* dst);
void avx_util_audio_from_interleaved_16s(const oavx_audio_frame_interleaved_16s_t* src, oavx_audio_frame_t* dst);
void avx_util_audio_to_interleaved_32f(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_32f_t* dst);
void avx_util_audio_from_interleaved_32f(const oavx_audio_frame_interleaved_32f_t* src, oavx_audio_frame_t* dst);
void avx_util_audio_to_interleaved_32s(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_32s_t* dst);
void avx_util_audio_from_interleaved_32s(const oavx_audio_frame_interleaved_32s_t* src, oavx_audio_frame_t* dst);
void avx_util_V210_to_P216(const oavx_video_frame_t* src, oavx_video_frame_t* dst);
void avx_util_P216_to_V210(const oavx_video_frame_t* src, oavx_video_frame_t* dst);

/* Internal Finder Session */
typedef struct avx_find_ctx_t {
    bool in_use;
    bool show_local_sources;
    char groups[256];
    char extra_ips[256];
    avx_mutex_t lock;
    avx_socket_t mdns_sock;
    uint32_t num_sources;
    oavx_endpoint_t sources[64];
    char source_names[64][256];
    char source_urls[64][256];
    oavx_wire_profile_t wire_profile;
    volatile int refresh_in_progress;
    int64_t last_refresh_ns;
} avx_find_ctx_t;

/* Telemetry Window & Dynamics Tracker */
typedef struct avx_telemetry_tracker_t {
    uint64_t total_bytes;
    int64_t  last_window_time_ns;
    uint64_t window_bytes;
    uint32_t window_frames;
    float    instant_fps;
    float    instant_mbps;

    int64_t  last_frame_arrival_ns;
    float    inter_frame_jitter_ms;

    uint32_t last_fourcc;
    uint32_t last_width;
    uint32_t last_height;
} avx_telemetry_tracker_t;

/* Multi-Client Fan-Out Limits & State */
#define AVX_MAX_CLIENTS 64
#define AVX_DEFAULT_MAX_CLIENTS 32
#define AVX_TX_VIDEO_POOL_SIZE 4
#define AVX_CLIENT_QUEUE_DEPTH 16

/* Shared Ref-Counted Frame Slot for Video */
typedef struct avx_tx_video_slot_t {
    uint8_t  header[64];
    size_t   header_len;
    uint8_t *metadata_buf;
    size_t   metadata_len;
    uint8_t  metadata_hdr[64];
    size_t   metadata_hdr_len;
    uint8_t *payload;
    size_t   payload_size;
    size_t   payload_capacity;
    int      refcount;
    bool     in_flight;
    int64_t  pts_ns;
} avx_tx_video_slot_t;

/* Shared Packet for Audio & Metadata */
typedef struct avx_tx_shared_pkt_t {
    uint8_t *data;
    size_t   size;
    int      refcount;
} avx_tx_shared_pkt_t;

typedef enum avx_queue_item_type_e {
    AVX_QUEUE_ITEM_NONE = 0,
    AVX_QUEUE_ITEM_VIDEO,
    AVX_QUEUE_ITEM_AUDIO,
    AVX_QUEUE_ITEM_METADATA
} avx_queue_item_type_e;

typedef struct avx_queue_item_t {
    avx_queue_item_type_e type;
    int video_pool_idx;
    avx_tx_shared_pkt_t *shared_pkt;
} avx_queue_item_t;

struct avx_send_ctx_t;

typedef struct avx_client_slot_t {
    avx_socket_t      fd;
    struct sockaddr_storage addr;
    oavx_tally_t      tally;
    int64_t           connected_time_ns;
    uint64_t          dropped_frames;

    /* Per-Client Async TX Queue */
    struct avx_send_ctx_t *parent_ctx;
    int               client_index;
    avx_thread_t      tx_thread;
    bool              tx_thread_active;
    avx_mutex_t       tx_lock;
    avx_cond_t        tx_cond;
    avx_queue_item_t  queue[AVX_CLIENT_QUEUE_DEPTH];
    int               queue_head;
    int               queue_tail;
    int               queue_count;
    int               video_queued;
    bool              tx_running;

    /* Congestion & Bitrate Adaptation */
    uint32_t          congestion_level; /* 0 = Normal (full fps), 1 = Moderate (30fps), 2 = Severe (15fps) */
    uint64_t          frame_seq;        /* Monotonic video frame presentation sequence */
    uint64_t          audio_packets_sent;
    uint64_t          video_packets_sent;
    uint64_t          tcp_bytes_submitted;     /* Cumulative wire bytes written to client TCP socket */
    uint64_t          last_acked_bytes;        /* Monotonic TCP ACKed byte progress estimate */
    uint64_t          rate_window_start_acked; /* ACKed byte baseline at rate_window_start_ns */
    uint64_t          rate_window_start_sub;   /* Submitted byte baseline at rate_window_start_ns */
    int64_t           rate_window_start_ns;    /* Monotonic timestamp of current rate window */
    bool              rate_window_throttled;   /* True if any cadence > 1 occurred in window */
    float             tx_bytes_per_sec;        /* Smoothed ACKed TCP throughput (bytes/sec), < 0 before warmup */
    float             last_rtt_ms;             /* Cached kernel TCP RTT (ms) */
    int64_t           last_rtt_sample_ns;      /* Monotonic timestamp of last TCP RTT sample */

    /* Transport Negotiation */
    oavx_transport_e  transport;
    struct sockaddr_storage udp_dest_addr;
    bool              has_udp_dest;

    /* WAN Simulcast & Codec Negotiation */
    bool              is_wan_client;
    bool              is_wan_registered;
    uint32_t          negotiated_codec_fourcc;
} avx_client_slot_t;

/* Internal Sender Session */
struct avx_send_ctx_t {
    bool in_use;
    char name[256];
    char groups[256];
    bool clock_video;
    bool clock_audio;
    avx_socket_t tcp_listen_fd;
    avx_socket_t tcp_client_fd; /* Legacy alias for first active client */
    avx_client_slot_t clients[AVX_MAX_CLIENTS];
    int max_clients;
    int active_clients;
    uint16_t port;
    avx_mutex_t lock;
    avx_cond_t tally_cond;
    avx_thread_t listen_thread;
    avx_thread_t client_rx_thread;
    volatile bool client_rx_running;
    volatile bool running;
    oavx_tally_t tally;
    int64_t video_frames_sent;
    int64_t audio_frames_sent;
    int64_t metadata_frames_sent;
    uint8_t* compressed_buf;
    size_t compressed_buf_size;
    avx_tx_video_slot_t video_pool[AVX_TX_VIDEO_POOL_SIZE];
    int video_pool_next;
    avx_socket_t discovery_server_sock;
    avx_socket_t discovery_server_socks[8];
    int num_discovery_server_socks;
    char conn_metadata[16][1024];
    size_t num_conn_metadata;
    char last_metadata[4096];
    bool has_new_metadata;
    oavx_endpoint_t source_info;
    char url[256];
    char bind_ip[64];
    char advertised_ip[64];
    int64_t last_mdns_broadcast_ns;
    oavx_wire_profile_t wire_profile;
    avx_telemetry_tracker_t telem;

    /* Failover Redundancy */
    bool has_failover;
    oavx_endpoint_t failover_source;
    char failover_name[256];
    char failover_url[256];

    /* Datagram / RUDP Transport Subsystem */
    oavx_transport_e transport;
    avx_socket_t udp_sock;
    uint16_t udp_port;
    avx_udp_rudp_sender_t *rudp_sender;
    avx_thread_t nack_thread;
    bool nack_thread_active;
    volatile bool nack_thread_running;

    /* Adaptive Bitrate & Dynamic FEC Rate Controller */
    struct avx_rate_controller_t *rate_ctl;

    /* High-Efficiency Audio Compression Subsystem */
    oavx_audio_compression_mode_e audio_comp_mode;
    int                           audio_target_bitrate_kbps;
    avx_audio_encoder_t          *audio_encoder;
    bool                          audio_comp_active;
    int                           audio_enc_channels;
    int                           audio_enc_sample_rate;

    /* Per-Stream Video Encoder Subsystem */
    avx_video_encoder_t          *video_encoder;
    uint32_t                      video_enc_fourcc;
    uint32_t                      video_enc_width;
    uint32_t                      video_enc_height;
    uint8_t                       video_quality; /* 0 = global default, 20..98 = per-stream SpeedHQ quality */
    uint32_t                      video_codec_fourcc;
    uint32_t                      video_bitrate_kbps;
    uint32_t                      keyframe_interval;

    /* On-Demand Lazy Hardware Simulcast (LAN vs WAN) */
    int                           wan_client_count;
    uint32_t                      wan_codec_fourcc;
    avx_video_encoder_t          *wan_encoder;
    uint32_t                      wan_enc_fourcc;
    uint32_t                      wan_enc_width;
    uint32_t                      wan_enc_height;
    uint32_t                      wan_enc_bitrate_kbps;

    /* Native RFC 2326 RTSP 1.0 Server */
    struct avx_rtsp_server_t     *rtsp_server;

    /* Embedded HTTP / fMP4 Web Preview Server */
    struct avx_web_server_t      *web_server;
};

/* Internal Receiver Session */
typedef struct avx_recv_ctx_t {
    bool in_use;
    oavx_endpoint_t target_source;
    char target_name[256];
    char target_url[256];
    oavx_color_format_e color_format;
    oavx_bandwidth_e bandwidth;
    bool allow_video_fields;
    char recv_name[256];
    avx_socket_t tcp_conn_fd;
    avx_mutex_t lock;
    bool connected;
    uint8_t* video_buf;
    size_t video_buf_size;
    uint8_t* decompressed_buf;
    size_t decompressed_buf_size;
    uint8_t* converted_buf;
    size_t converted_buf_size;
    float* audio_buf;
    size_t audio_buf_size;
    char* metadata_buf;
    size_t metadata_buf_size;
    char last_video_metadata[4096];
    char conn_metadata[16][1024];
    size_t num_conn_metadata;
    oavx_performance_t perf;
    oavx_queue_t queue;

    /* PTZ, Recording, and Web Control Extensions */
    bool ptz_supported;
    bool recording_supported;
    bool is_recording;
    char current_recording_filename[256];
    char current_recording_error[256];
    char web_control_url[256];
    oavx_wire_profile_t wire_profile;
    avx_telemetry_tracker_t telem;
    oavx_tally_t tally;
    avx_cond_t tally_cond;

    /* Failover Redundancy Engine */
    bool has_failover;
    bool is_on_failover;
    char primary_name[256];
    char primary_url[256];
    char failover_name[256];
    char failover_url[256];
    int64_t last_frame_recv_ns;
    uint32_t failover_timeout_ms;
    bool auto_revert;
    uint32_t revert_hold_ms;
    int64_t primary_online_since_ns;
    int64_t last_probe_time_ns;
    int64_t last_connect_attempt_ns;
    avx_socket_t pending_conn_fd;
    int64_t pending_conn_start_ns;
    bool offline_mode;
    volatile bool decoding_video;

    /* Datagram / RUDP Transport Subsystem */
    oavx_transport_e transport;
    avx_socket_t udp_sock;
    uint16_t udp_port;
    avx_udp_rudp_receiver_t *rudp_receiver;
    struct sockaddr_storage sender_udp_addr;
    bool has_sender_udp;

    /* High-Efficiency Audio Decompression Subsystem */
    avx_audio_decoder_t          *audio_decoder;
    uint32_t                      audio_decoder_fourcc;
    int                           audio_decoder_channels;
    int                           audio_decoder_rate;

    /* Per-Stream Video Decompression Subsystem */
    avx_video_decoder_t          *video_decoder;
    uint32_t                      video_decoder_fourcc;
    uint32_t                      video_decoder_width;
    uint32_t                      video_decoder_height;
} avx_recv_ctx_t;

/* Internal Routing Session */
typedef struct avx_routing_ctx_t {
    bool in_use;
    avx_tx_instance_t p_send;
    avx_rx_instance_t p_recv;
    oavx_endpoint_t current_source;
    char current_name[256];
    char current_url[256];
    bool has_source;
    oavx_tally_t last_tally;
    avx_mutex_t lock;
    avx_cond_t source_cond;
    avx_thread_t worker_thread;
    bool worker_running;
    bool stop_requested;
} avx_routing_ctx_t;

/* Internal Global State */
typedef struct avx_global_state_t {
    avx_mutex_t global_lock;
    bool initialized;
    avx_find_ctx_t find_slots[AVX_MAX_SESSIONS];
    avx_send_ctx_t send_slots[AVX_MAX_SESSIONS];
    avx_recv_ctx_t recv_slots[AVX_MAX_SESSIONS];
    avx_routing_ctx_t routing_slots[AVX_MAX_SESSIONS];
} avx_global_state_t;

extern avx_global_state_t g_avx_state;

/* Internal Helper Functions */
avx_socket_t avx_socket_create_tcp_server(uint16_t port);
avx_socket_t avx_socket_create_tcp_server_bind(const char* bind_ip, uint16_t port);
avx_socket_t avx_socket_connect_tcp(const char* host, uint16_t port);
avx_socket_t avx_socket_connect_tcp_timeout(const char* host, uint16_t port, uint32_t timeout_ms);
avx_socket_t avx_socket_start_nonblocking_connect(const char* host, uint16_t port, bool* connected_now);
avx_socket_t avx_socket_create_mdns_multicast(void);

/* Multi-NIC & Network Interface Helpers */
bool avx_net_resolve_interface_or_ip(const char* iface_or_ip, char* out_ip, size_t out_len);
bool avx_net_get_default_ip(char* out_ip, size_t out_len);

/* Packet Tracing & Zero-Sudo PCAP Generation */
void avx_trace_packet(const char* direction, const char* tag, const void* data, size_t len);
void avx_pcap_init(const char* filepath);
void avx_pcap_write_packet(const void* data, size_t len);
void avx_pcap_write_udp(uint32_t src_ip_net, uint16_t src_port_host, uint32_t dst_ip_net, uint16_t dst_port_host, const void* payload, size_t len);
void avx_pcap_write_tcp(uint32_t src_ip_net, uint16_t src_port_host, uint32_t dst_ip_net, uint16_t dst_port_host, uint32_t seq, uint32_t ack, uint8_t flags, const void* payload, size_t len);
void avx_pcap_close(void);

/* Sender Listener Thread */
bool avx_send_start_listener(avx_send_ctx_t* send);
void avx_send_stop_listener(avx_send_ctx_t* send);

/* Telemetry & Kernel Socket Diagnostics */
OPENAVX_API bool avx_rx_get_telemetry(avx_rx_instance_t p_instance, oavx_telemetry_t *out);
OPENAVX_API bool avx_tx_get_telemetry(avx_tx_instance_t p_instance, oavx_telemetry_t *out);
void avx_socket_get_diagnostics(avx_socket_t sock, float *out_rtt_ms, uint32_t *out_retrans, uint32_t *out_send_q, uint32_t *out_recv_q);

/* Compute BDP- and frame-aware TCP TIOCOUTQ congestion thresholds */
static inline void avx_send_compute_tcp_thresholds(
    size_t frame_wire_bytes,
    float measured_tx_bytes_per_sec,
    float target_fps,
    float kernel_rtt_ms,
    uint64_t *out_warn_bytes,
    uint64_t *out_severe_bytes
) {
    uint64_t warn_thresh = 500000ULL;    /* 500 KB low-BDP baseline */
    uint64_t severe_thresh = 2000000ULL; /* 2 MB low-BDP baseline */

    uint64_t frame_warn = (uint64_t)frame_wire_bytes * 2ULL;
    uint64_t frame_sev  = (uint64_t)frame_wire_bytes * 4ULL;
    if (frame_warn > warn_thresh) warn_thresh = frame_warn;
    if (frame_sev > severe_thresh) severe_thresh = frame_sev;

    float fps = (target_fps > 0.0f && target_fps <= 240.0f) ? target_fps : 60.0f;
    if (target_fps > 240.0f) fps = 240.0f;

    /* Negative sentinel means initial rate window has not closed yet: bootstrap from offered rate */
    float effective_bps = (measured_tx_bytes_per_sec >= 0.0f && measured_tx_bytes_per_sec < 1.0e11f)
                        ? measured_tx_bytes_per_sec
                        : ((float)frame_wire_bytes * fps);

    if (kernel_rtt_ms > 0.0f && kernel_rtt_ms < 60000.0f &&
        effective_bps > 0.0f && effective_bps < 1.0e11f) {
        double bdp_bytes = (double)effective_bps * ((double)kernel_rtt_ms / 1000.0);
        uint64_t bdp_warn = (uint64_t)(bdp_bytes * 1.5) + (uint64_t)frame_wire_bytes;
        uint64_t bdp_sev  = (uint64_t)(bdp_bytes * 3.0) + (uint64_t)frame_wire_bytes * 2ULL;
        if (bdp_warn > warn_thresh) warn_thresh = bdp_warn;
        if (bdp_sev > severe_thresh) severe_thresh = bdp_sev;
    }

    if (severe_thresh <= warn_thresh) {
        severe_thresh = warn_thresh * 2ULL;
    }

    if (out_warn_bytes) *out_warn_bytes = warn_thresh;
    if (out_severe_bytes) *out_severe_bytes = severe_thresh;
}

/* Update per-client TCP ACKed throughput and evaluate dynamic congestion level & frame cadence */
static inline uint32_t avx_send_eval_tcp_congestion(
    avx_client_slot_t *client,
    int64_t now_ns,
    size_t frame_wire_bytes,
    float target_fps,
    int unsent_bytes,
    int v_queued,
    float rtt_ms,
    uint32_t *out_cadence
) {
    uint64_t unsent_u64 = (unsent_bytes > 0) ? (uint64_t)unsent_bytes : 0ULL;

    uint64_t warn_thresh = 500000ULL;
    uint64_t severe_thresh = 2000000ULL;
    avx_send_compute_tcp_thresholds(frame_wire_bytes,
                                    client->tx_bytes_per_sec,
                                    target_fps,
                                    rtt_ms,
                                    &warn_thresh,
                                    &severe_thresh);

    uint64_t raw_acked = (client->tcp_bytes_submitted > unsent_u64)
                       ? (client->tcp_bytes_submitted - unsent_u64)
                       : 0ULL;
    uint64_t acked_now = (raw_acked > client->last_acked_bytes)
                       ? raw_acked
                       : client->last_acked_bytes;
    client->last_acked_bytes = acked_now;

    if (client->rate_window_start_ns == 0) {
        client->rate_window_start_ns = now_ns;
        client->rate_window_start_acked = acked_now;
        client->rate_window_start_sub = client->tcp_bytes_submitted;
        client->rate_window_throttled = false;
    } else {
        int64_t min_win_ns = 200000000LL; /* 200 ms baseline sample window */
        if (rtt_ms > 50.0f && rtt_ms < 125.0f) {
            min_win_ns = (int64_t)((double)rtt_ms * 4.0 * 1000000.0);
        } else if (rtt_ms >= 125.0f) {
            min_win_ns = 500000000LL; /* 500 ms cap */
        }

        int64_t elapsed_ns = now_ns - client->rate_window_start_ns;
        if (elapsed_ns >= min_win_ns) {
            uint64_t delta_sub = client->tcp_bytes_submitted - client->rate_window_start_sub;
            uint64_t delta_acked = acked_now - client->rate_window_start_acked;

            if (delta_sub > 0 || unsent_u64 >= 65536ULL) {
                double elapsed_sec = (double)elapsed_ns / 1000000000.0;
                float sample_bps = (float)((double)delta_acked / elapsed_sec);
                bool queue_pressured = (v_queued > 0 || unsent_u64 >= warn_thresh);

                /* Avoid ratcheting BDP down merely because temporal decimation self-throttled cleanly */
                if (!client->rate_window_throttled || queue_pressured ||
                    client->tx_bytes_per_sec < 0.0f || sample_bps > client->tx_bytes_per_sec) {
                    if (client->tx_bytes_per_sec < 0.0f) {
                        client->tx_bytes_per_sec = sample_bps;
                    } else {
                        client->tx_bytes_per_sec = 0.5f * client->tx_bytes_per_sec + 0.5f * sample_bps;
                    }
                    avx_send_compute_tcp_thresholds(frame_wire_bytes,
                                                    client->tx_bytes_per_sec,
                                                    target_fps,
                                                    rtt_ms,
                                                    &warn_thresh,
                                                    &severe_thresh);
                }
            }

            client->rate_window_start_ns = now_ns;
            client->rate_window_start_acked = acked_now;
            client->rate_window_start_sub = client->tcp_bytes_submitted;
            client->rate_window_throttled = false;
        }
    }

    uint32_t level = 0;
    uint32_t cadence = 1;
    if (v_queued >= 2 || unsent_u64 >= severe_thresh) {
        cadence = 4;
        level = 2;
        client->rate_window_throttled = true;
    } else if (v_queued >= 1 || unsent_u64 >= warn_thresh) {
        cadence = 2;
        level = 1;
        client->rate_window_throttled = true;
    }

    if (out_cadence) *out_cadence = cadence;
    return level;
}

/* Audio & Video Compression Control */
void avx_tx_set_audio_compression(avx_tx_instance_t p_instance, oavx_audio_compression_mode_e mode, int target_bitrate_kbps);
void avx_tx_set_video_quality(avx_tx_instance_t p_instance, uint8_t quality);
uint8_t avx_tx_get_video_quality(avx_tx_instance_t p_instance);
void avx_tx_set_video_compression(avx_tx_instance_t p_instance, uint32_t target_fourcc, uint32_t bitrate_kbps);
uint32_t avx_tx_get_video_compression(avx_tx_instance_t p_instance);

/* Multi-plane video frame setup */
void avx_setup_raw_video_planes(oavx_video_frame_t *vf, uint8_t *payload, size_t payload_len);

/* mDNS Broadcast */
int avx_mdns_broadcast_source(const char* name, uint16_t port);
int avx_mdns_broadcast_source_ex(const char* name, uint16_t port, const char* bind_ip);

/* Native RFC 2326 RTSP 1.0 Server */
bool avx_tx_enable_rtsp_server(avx_tx_instance_t instance, uint16_t port, const char *path);
void avx_tx_disable_rtsp_server(avx_tx_instance_t instance);
uint16_t avx_tx_get_rtsp_port(avx_tx_instance_t instance);
int avx_tx_get_rtsp_client_count(avx_tx_instance_t instance);

/* Embedded HTTP / fMP4 Web Preview Server */
bool avx_tx_enable_web_preview(avx_tx_instance_t instance, uint16_t port);
void avx_tx_disable_web_preview(avx_tx_instance_t instance);
uint16_t avx_tx_get_web_preview_port(avx_tx_instance_t instance);
const char* avx_tx_get_web_preview_url(avx_tx_instance_t instance, char *out_url, size_t max_len);
int avx_tx_get_web_client_count(avx_tx_instance_t instance);

/* Adaptive Bitrate & Rate Controller */
struct avx_rate_controller_t* avx_tx_get_rate_controller(avx_tx_instance_t p_instance);

/* On-Demand Hardware Simulcast Diagnostics & In-Flight Profile Migration */
OPENAVX_API int avx_tx_get_wan_client_count(avx_tx_instance_t p_instance);
OPENAVX_API uint32_t avx_tx_get_wan_codec_fourcc(avx_tx_instance_t p_instance);
OPENAVX_API bool avx_tx_is_wan_encoder_active(avx_tx_instance_t p_instance);
OPENAVX_API bool avx_tx_set_client_profile(avx_tx_instance_t p_instance, int client_idx, const char *profile);
OPENAVX_API bool avx_rx_request_profile(avx_rx_instance_t p_instance, const char *profile);

#endif /* AVX_INTERNAL_H */
