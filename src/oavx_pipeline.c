/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/oavx_pipeline.c - Unified media pipeline and stream API
 */

#include "openavx.h"
#include "avx_internal.h"
#include "avx_wire.h"
#include "avx_speedhq.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* 1. Context Lifecycle                                                      */
/* ------------------------------------------------------------------------- */

struct oavx_context {
    bool active;
    oavx_wire_profile_t wire_profile;
};

static int g_ctx_refcount = 0;
static avx_mutex_t g_ctx_lock = AVX_MUTEX_INITIALIZER;

/*
 * Initializes context with active wire profile. Refcounts global engine
 * and SIMD initialization; destroyed when last context is freed.
 */
OPENAVX_API oavx_context_t* oavx_context_create(void) {
    oavx_context_t *ctx = (oavx_context_t*)calloc(1, sizeof(oavx_context_t));
    if (!ctx) return NULL;

    avx_mutex_lock(&g_ctx_lock);
    if (g_ctx_refcount == 0) {
        if (!avx_engine_initialize()) {
            avx_mutex_unlock(&g_ctx_lock);
            free(ctx);
            return NULL;
        }
    }
    g_ctx_refcount++;
    avx_mutex_unlock(&g_ctx_lock);

    ctx->active = true;
    ctx->wire_profile = *avx_wire_get_active_profile();
    return ctx;
}

OPENAVX_API void oavx_context_destroy(oavx_context_t *ctx) {
    if (!ctx) return;
    ctx->active = false;
    free(ctx);

    avx_mutex_lock(&g_ctx_lock);
    g_ctx_refcount--;
    if (g_ctx_refcount <= 0) {
        g_ctx_refcount = 0;
        avx_engine_destroy();
    }
    avx_mutex_unlock(&g_ctx_lock);
}

OPENAVX_API const char* oavx_version(void) {
    return avx_engine_version();
}

OPENAVX_API bool oavx_is_supported_cpu(void) {
    return avx_engine_is_supported_cpu();
}

OPENAVX_API const char* oavx_status_to_string(oavx_status_t status) {
    switch (status) {
        case OAVX_SUCCESS:            return "Success";
        case OAVX_ERR_GENERIC:        return "Generic error";
        case OAVX_ERR_INVALID_ARG:    return "Invalid argument or null pointer";
        case OAVX_ERR_TIMEOUT:        return "Operation timed out";
        case OAVX_ERR_NOT_CONNECTED:  return "Transport not connected";
        case OAVX_ERR_QUEUE_FULL:     return "Pipeline queue full";
        case OAVX_ERR_UNSUPPORTED:    return "Feature or format unsupported";
        case OAVX_ERR_WOULD_BLOCK:    return "Operation would block";
        default:                      return "Unknown error code";
    }
}

OPENAVX_API void oavx_context_set_wire_profile(oavx_context_t *ctx, const oavx_wire_profile_t *profile) {
    if (ctx) {
        if (profile) {
            ctx->wire_profile = *profile;
        } else {
            ctx->wire_profile = *avx_wire_get_default_profile();
        }
    }
    avx_wire_set_active_profile(profile);
}

OPENAVX_API void oavx_context_get_wire_profile(oavx_context_t *ctx, oavx_wire_profile_t *out_profile) {
    if (!out_profile) return;
    if (ctx) {
        *out_profile = ctx->wire_profile;
    } else {
        *out_profile = *avx_wire_get_active_profile();
    }
}

/* ------------------------------------------------------------------------- */
/* 2. Unified Stream Pipeline                                                */
/* ------------------------------------------------------------------------- */

struct oavx_stream {
    oavx_direction_t        direction;
    avx_tx_instance_t       sender;
    avx_rx_instance_t       receiver;
    oavx_stream_config_t    config;
    char                    name[256];
    char                    endpoint[256];
    oavx_endpoint_t         ep;
};

/*
 * Factory that instantiates either a transmission pipeline (avx_tx) or
 * receiver pipeline (avx_rx) based on config->direction. Resolves transport
 * fallback (RUDP/UDP/TCP) from config or environment.
 */
OPENAVX_API oavx_stream_t* oavx_stream_open(oavx_context_t *ctx, const oavx_stream_config_t *config) {
    if (!config) return NULL;

    const oavx_wire_profile_t *wp = config->wire_profile ? config->wire_profile
                                  : (ctx ? &ctx->wire_profile : avx_wire_get_active_profile());

    oavx_stream_t *s = (oavx_stream_t*)calloc(1, sizeof(oavx_stream_t));
    if (!s) return NULL;

    s->direction = config->direction;
    s->config = *config;
    s->config.wire_profile = wp;

    if (config->name) {
        strncpy(s->name, config->name, sizeof(s->name) - 1);
        s->config.name = s->name;
    }
    if (config->endpoint) {
        strncpy(s->endpoint, config->endpoint, sizeof(s->endpoint) - 1);
        s->config.endpoint = s->endpoint;
    }
    s->ep.name = s->name;
    s->ep.address = s->endpoint;
    s->ep.port = 0;
    s->ep.group = config->groups;

    if (config->direction == OAVX_DIR_TRANSMIT) {
        avx_tx_config_t sc;
        memset(&sc, 0, sizeof(sc));
        sc.name = s->name[0] ? s->name : "OpenAVX Stream";
        sc.groups = config->groups;
        sc.clock_video = config->clock_sync;
        sc.clock_audio = config->clock_sync;
        sc.wire_profile = wp;
        sc.max_connections = config->max_connections;
        sc.bind_ip = config->bind_ip;
        sc.transport = config->transport;
        sc.video_codec_fourcc = config->video_codec_fourcc;
        sc.video_bitrate_kbps = config->video_bitrate_kbps;
        sc.keyframe_interval  = config->keyframe_interval;
        sc.passphrase         = config->passphrase;
        if (sc.transport == 0) {
            const avx_config_t *cfg = avx_config_get();
            if (cfg) {
                if (strcasecmp(cfg->transport, "rudp") == 0) sc.transport = OAVX_TRANSPORT_RUDP;
                else if (strcasecmp(cfg->transport, "udp") == 0) sc.transport = OAVX_TRANSPORT_UDP;
            }
        }

        s->sender = avx_tx_create(&sc);
        if (!s->sender) {
            free(s);
            return NULL;
        }
        const oavx_endpoint_t *src_info = avx_tx_get_source(s->sender);
        if (src_info) {
            s->ep = *src_info;
        }
    } else if (config->direction == OAVX_DIR_RECEIVE) {
        avx_rx_config_t rc;
        memset(&rc, 0, sizeof(rc));
        rc.color_format = config->color_format;
        rc.bandwidth = config->bandwidth;
        rc.name = s->name[0] ? s->name : "OpenAVX Receiver";
        rc.wire_profile = wp;
        rc.transport = config->transport;
        rc.passphrase = config->passphrase;
        if (rc.transport == 0) {
            const avx_config_t *cfg = avx_config_get();
            if (cfg) {
                if (strcasecmp(cfg->transport, "rudp") == 0) rc.transport = OAVX_TRANSPORT_RUDP;
                else if (strcasecmp(cfg->transport, "udp") == 0) rc.transport = OAVX_TRANSPORT_UDP;
            }
        }

        if (s->endpoint[0]) {
            rc.source_to_connect_to.name = s->name;
            rc.source_to_connect_to.address = s->endpoint;
        }

        s->receiver = avx_rx_create(&rc);
        if (!s->receiver) {
            free(s);
            return NULL;
        }
    } else {
        free(s);
        return NULL;
    }

    return s;
}

OPENAVX_API void oavx_stream_close(oavx_stream_t *stream) {
    if (!stream) return;
    if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        avx_tx_destroy(stream->sender);
        stream->sender = NULL;
    } else if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        avx_rx_destroy(stream->receiver);
        stream->receiver = NULL;
    }
    free(stream);
}

OPENAVX_API oavx_status_t oavx_stream_send_video(oavx_stream_t *stream, const oavx_video_frame_t *frame) {
    if (!stream || !frame) return OAVX_ERR_INVALID_ARG;
    if (stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return OAVX_ERR_NOT_CONNECTED;
    avx_tx_send_video(stream->sender, frame);
    return OAVX_SUCCESS;
}

OPENAVX_API oavx_status_t oavx_stream_send_audio(oavx_stream_t *stream, const oavx_audio_frame_t *frame) {
    if (!stream || !frame) return OAVX_ERR_INVALID_ARG;
    if (stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return OAVX_ERR_NOT_CONNECTED;
    avx_tx_send_audio(stream->sender, frame);
    return OAVX_SUCCESS;
}

OPENAVX_API oavx_status_t oavx_stream_set_audio_compression(oavx_stream_t *stream, oavx_audio_compression_mode_e mode, int target_bitrate_kbps) {
    if (!stream) return OAVX_ERR_INVALID_ARG;
    if (stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return OAVX_ERR_NOT_CONNECTED;
    avx_tx_set_audio_compression(stream->sender, mode, target_bitrate_kbps);
    return OAVX_SUCCESS;
}

OPENAVX_API oavx_status_t oavx_stream_set_video_compression(oavx_stream_t *stream, uint32_t target_fourcc, uint32_t bitrate_kbps) {
    if (!stream) return OAVX_ERR_INVALID_ARG;
    if (stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return OAVX_ERR_NOT_CONNECTED;
    avx_tx_set_video_compression(stream->sender, target_fourcc, bitrate_kbps);
    return OAVX_SUCCESS;
}

OPENAVX_API uint32_t oavx_stream_get_video_compression(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return 0;
    return avx_tx_get_video_compression(stream->sender);
}

OPENAVX_API oavx_status_t oavx_stream_set_video_quality(oavx_stream_t *stream, uint8_t quality) {
    if (!stream) return OAVX_ERR_INVALID_ARG;
    if (stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return OAVX_ERR_NOT_CONNECTED;
    avx_tx_set_video_quality(stream->sender, quality);
    return OAVX_SUCCESS;
}

OPENAVX_API uint8_t oavx_stream_get_video_quality(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return 0;
    return avx_tx_get_video_quality(stream->sender);
}

OPENAVX_API oavx_status_t oavx_stream_enable_rtsp_server(oavx_stream_t *stream, uint16_t port, const char *path) {
    if (!stream) return OAVX_ERR_INVALID_ARG;
    if (stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return OAVX_ERR_NOT_CONNECTED;
    return avx_tx_enable_rtsp_server(stream->sender, port, path) ? OAVX_SUCCESS : OAVX_ERR_GENERIC;
}

OPENAVX_API void oavx_stream_disable_rtsp_server(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return;
    avx_tx_disable_rtsp_server(stream->sender);
}

OPENAVX_API uint16_t oavx_stream_get_rtsp_port(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return 0;
    return avx_tx_get_rtsp_port(stream->sender);
}

OPENAVX_API int oavx_stream_get_rtsp_client_count(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return 0;
    return avx_tx_get_rtsp_client_count(stream->sender);
}

OPENAVX_API oavx_status_t oavx_stream_enable_web_preview(oavx_stream_t *stream, uint16_t port) {
    if (!stream) return OAVX_ERR_INVALID_ARG;
    if (stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return OAVX_ERR_NOT_CONNECTED;
    return avx_tx_enable_web_preview(stream->sender, port) ? OAVX_SUCCESS : OAVX_ERR_GENERIC;
}

OPENAVX_API void oavx_stream_disable_web_preview(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return;
    avx_tx_disable_web_preview(stream->sender);
}

OPENAVX_API uint16_t oavx_stream_get_web_preview_port(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return 0;
    return avx_tx_get_web_preview_port(stream->sender);
}

OPENAVX_API const char* oavx_stream_get_web_preview_url(oavx_stream_t *stream, char *out_url, size_t max_len) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return NULL;
    return avx_tx_get_web_preview_url(stream->sender, out_url, max_len);
}

OPENAVX_API int oavx_stream_get_web_client_count(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return 0;
    return avx_tx_get_web_client_count(stream->sender);
}

OPENAVX_API oavx_status_t oavx_stream_send_metadata(oavx_stream_t *stream, const oavx_metadata_frame_t *meta) {
    if (!stream || !meta) return OAVX_ERR_INVALID_ARG;
    if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        avx_tx_send_metadata(stream->sender, meta);
        return OAVX_SUCCESS;
    } else if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        return avx_rx_send_metadata(stream->receiver, meta) ? OAVX_SUCCESS : OAVX_ERR_GENERIC;
    }
    return OAVX_ERR_NOT_CONNECTED;
}

/*
 * Polls receiver pipeline for next ready frame up to timeout_ms.
 * Caller must release returned buffers via oavx_stream_release_*.
 */
OPENAVX_API oavx_frame_type_t oavx_stream_receive(
    oavx_stream_t *stream,
    oavx_video_frame_t *v,
    oavx_audio_frame_t *a,
    oavx_metadata_frame_t *m,
    uint32_t timeout_ms)
{
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) {
        return OAVX_FRAME_TYPE_ERROR;
    }
    return avx_rx_receive(stream->receiver, v, a, m, timeout_ms);
}

/* Returns zero-copy video buffer back to receiver ring buffer pool. */
OPENAVX_API void oavx_stream_release_video(oavx_stream_t *stream, const oavx_video_frame_t *frame) {
    if (!stream || !stream->receiver || !frame) return;
    avx_rx_free_video(stream->receiver, frame);
}

/* Returns audio frame buffer to receiver pool. */
OPENAVX_API void oavx_stream_release_audio(oavx_stream_t *stream, const oavx_audio_frame_t *frame) {
    if (!stream || !stream->receiver || !frame) return;
    avx_rx_free_audio(stream->receiver, frame);
}

OPENAVX_API void oavx_stream_release_metadata(oavx_stream_t *stream, const oavx_metadata_frame_t *meta) {
    if (!stream || !meta) return;
    if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        avx_tx_free_metadata(stream->sender, meta);
    } else if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        avx_rx_free_metadata(stream->receiver, meta);
    }
}

OPENAVX_API bool oavx_stream_set_ptz(oavx_stream_t *stream, const oavx_ptz_state_t *ptz) {
    if (!stream || !stream->receiver || !ptz) return false;
    bool ok = true;
    if (ptz->zoom != 0.0f) {
        ok &= avx_rx_ptz_zoom(stream->receiver, ptz->zoom);
    }
    if (ptz->pan != 0.0f || ptz->tilt != 0.0f) {
        ok &= avx_rx_ptz_pan_tilt(stream->receiver, ptz->pan, ptz->tilt);
    }
    if (ptz->focus != 0.0f) {
        ok &= avx_rx_ptz_focus(stream->receiver, ptz->focus);
    }
    return ok;
}

OPENAVX_API bool oavx_stream_set_tally(oavx_stream_t *stream, const oavx_tally_t *tally) {
    if (!stream || !tally) return false;
    if (stream->receiver) {
        return avx_rx_set_tally(stream->receiver, tally);
    }
    if (stream->sender) {
        return avx_tx_set_tally(stream->sender, tally);
    }
    return false;
}

OPENAVX_API bool oavx_stream_get_tally(oavx_stream_t *stream, oavx_tally_t *out_tally, uint32_t timeout_ms) {
    if (!stream || !out_tally) return false;
    if (stream->sender) {
        return avx_tx_get_tally(stream->sender, out_tally, timeout_ms);
    }
    if (stream->receiver) {
        return avx_rx_get_tally(stream->receiver, out_tally, timeout_ms);
    }
    return false;
}

OPENAVX_API bool oavx_stream_record_trigger(oavx_stream_t *stream, const char *action, const char *filename_hint) {
    if (!stream || !stream->receiver || !action) return false;
    if (strcmp(action, "start") == 0) {
        return avx_rx_recording_start(stream->receiver, filename_hint);
    } else if (strcmp(action, "stop") == 0) {
        return avx_rx_recording_stop(stream->receiver);
    }
    return false;
}

OPENAVX_API bool oavx_stream_record_status(oavx_stream_t *stream, bool *is_recording, const char **filename, const char **error) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return false;
    if (is_recording) *is_recording = avx_rx_recording_is_recording(stream->receiver);
    if (filename) *filename = avx_rx_recording_get_filename(stream->receiver);
    if (error) *error = avx_rx_recording_get_error(stream->receiver);
    return true;
}

OPENAVX_API bool oavx_stream_record_set_audio_level(oavx_stream_t *stream, float level_db) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return false;
    return avx_rx_recording_set_audio_level(stream->receiver, level_db);
}

/* ------------------------------------------------------------------------- */
/* 3. Discovery Grid                                                         */
/* ------------------------------------------------------------------------- */

struct oavx_discovery {
    avx_find_instance_t finder;
};

OPENAVX_API oavx_discovery_t* oavx_discovery_create(oavx_context_t *ctx, const char *groups) {
    avx_find_config_t fc;
    memset(&fc, 0, sizeof(fc));
    fc.groups = groups;
    fc.show_local_sources = true;
    fc.wire_profile = ctx ? &ctx->wire_profile : avx_wire_get_active_profile();

    avx_find_instance_t fi = avx_find_create(&fc);
    if (!fi) return NULL;

    oavx_discovery_t *d = (oavx_discovery_t*)calloc(1, sizeof(oavx_discovery_t));
    if (!d) {
        avx_find_destroy(fi);
        return NULL;
    }
    d->finder = fi;
    return d;
}

OPENAVX_API void oavx_discovery_destroy(oavx_discovery_t *disc) {
    if (!disc) return;
    if (disc->finder) {
        avx_find_destroy(disc->finder);
        disc->finder = NULL;
    }
    free(disc);
}

OPENAVX_API size_t oavx_discovery_get_sources(oavx_discovery_t *disc, oavx_endpoint_t *out_sources, size_t max_count) {
    if (!disc || !disc->finder || !out_sources || max_count == 0) return 0;
    uint32_t count = 0;
    const oavx_endpoint_t *srcs = avx_find_get_sources(disc->finder, &count);
    if (!srcs || count == 0) return 0;

    size_t to_copy = count < max_count ? count : max_count;
    for (size_t i = 0; i < to_copy; ++i) {
        out_sources[i] = srcs[i];
    }
    return to_copy;
}

OPENAVX_API bool oavx_discovery_wait(oavx_discovery_t *disc, uint32_t timeout_ms) {
    if (!disc || !disc->finder) return false;
    return avx_find_wait(disc->finder, timeout_ms);
}

OPENAVX_API int oavx_discovery_get_fd(oavx_discovery_t *disc) {
    if (!disc || !disc->finder) return -1;
    avx_find_ctx_t *fc = (avx_find_ctx_t*)disc->finder;
    if (fc->mdns_sock != AVX_INVALID_SOCKET) {
        return (int)fc->mdns_sock;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* 4. Stream Connection & Advanced Endpoint Controls                         */
/* ------------------------------------------------------------------------- */

OPENAVX_API void oavx_stream_connect(oavx_stream_t *stream, const oavx_endpoint_t *endpoint) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return;
    avx_rx_connect(stream->receiver, endpoint);
}

OPENAVX_API const oavx_endpoint_t* oavx_stream_get_endpoint(oavx_stream_t *stream) {
    if (!stream) return NULL;
    if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        const oavx_endpoint_t *src_info = avx_tx_get_source(stream->sender);
        if (src_info) stream->ep = *src_info;
    }
    return &stream->ep;
}

OPENAVX_API oavx_frame_type_t oavx_stream_capture_metadata(oavx_stream_t *stream, oavx_metadata_frame_t *meta, uint32_t timeout_ms) {
    if (!stream || stream->direction != OAVX_DIR_TRANSMIT || !stream->sender) return OAVX_FRAME_TYPE_ERROR;
    return avx_tx_capture_metadata(stream->sender, meta, timeout_ms);
}

OPENAVX_API void oavx_stream_clear_connection_metadata(oavx_stream_t *stream) {
    if (!stream) return;
    if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        avx_tx_clear_connection_metadata(stream->sender);
    } else if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        avx_rx_clear_connection_metadata(stream->receiver);
    }
}

OPENAVX_API void oavx_stream_add_connection_metadata(oavx_stream_t *stream, const oavx_metadata_frame_t *meta) {
    if (!stream || !meta) return;
    if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        avx_tx_add_connection_metadata(stream->sender, meta);
    } else if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        avx_rx_add_connection_metadata(stream->receiver, meta);
    }
}

OPENAVX_API void oavx_stream_set_failover(oavx_stream_t *stream, const oavx_endpoint_t *failover_endpoint) {
    if (!stream) return;
    if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        avx_tx_set_failover(stream->sender, failover_endpoint);
    } else if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        avx_rx_set_failover(stream->receiver, failover_endpoint);
    }
}

OPENAVX_API bool oavx_stream_is_on_failover(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return false;
    return avx_rx_is_on_failover(stream->receiver);
}

OPENAVX_API void oavx_stream_set_failover_timeout(oavx_stream_t *stream, uint32_t timeout_ms) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return;
    avx_rx_set_failover_timeout(stream->receiver, timeout_ms);
}

OPENAVX_API void oavx_stream_set_auto_revert(oavx_stream_t *stream, bool auto_revert, uint32_t hold_time_ms) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return;
    avx_rx_set_auto_revert(stream->receiver, auto_revert, hold_time_ms);
}

OPENAVX_API int oavx_stream_get_connection_count(oavx_stream_t *stream, uint32_t timeout_ms) {
    if (!stream) return 0;
    if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        return avx_tx_get_connection_count(stream->sender, timeout_ms);
    } else if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        (void)timeout_ms;
        return avx_rx_get_connection_count(stream->receiver);
    }
    return 0;
}

OPENAVX_API bool oavx_stream_get_performance(oavx_stream_t *stream, oavx_performance_t *total, oavx_performance_t *dropped) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return false;
    return avx_rx_get_performance(stream->receiver, total, dropped);
}

OPENAVX_API bool oavx_stream_get_queue(oavx_stream_t *stream, oavx_queue_t *total) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return false;
    return avx_rx_get_queue(stream->receiver, total);
}

OPENAVX_API bool oavx_stream_get_telemetry(oavx_stream_t *stream, oavx_telemetry_t *out_telemetry) {
    if (!stream || !out_telemetry) return false;
    if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        return avx_rx_get_telemetry(stream->receiver, out_telemetry);
    } else if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        return avx_tx_get_telemetry(stream->sender, out_telemetry);
    }
    return false;
}

OPENAVX_API size_t oavx_telemetry_to_json(const oavx_telemetry_t *telem, char *out_json, size_t max_len) {
    if (!telem || !out_json || max_len == 0) return 0;

    char fourcc_str[16] = "NONE";
    if (telem->fourcc != 0) {
        uint32_t fc = telem->fourcc;
        char c0 = (char)(fc & 0xFF);
        char c1 = (char)((fc >> 8) & 0xFF);
        char c2 = (char)((fc >> 16) & 0xFF);
        char c3 = (char)((fc >> 24) & 0xFF);
        if (c0 >= 32 && c0 <= 126 && c1 >= 32 && c1 <= 126 &&
            c2 >= 32 && c2 <= 126 && c3 >= 32 && c3 <= 126) {
            fourcc_str[0] = c0;
            fourcc_str[1] = c1;
            fourcc_str[2] = c2;
            fourcc_str[3] = c3;
            fourcc_str[4] = '\0';
        } else {
            snprintf(fourcc_str, sizeof(fourcc_str), "0x%08X", fc);
        }
    }

    int ret = snprintf(out_json, max_len,
        "{\n"
        "  \"connected\": %s,\n"
        "  \"endpoint\": \"%s\",\n"
        "  \"format\": {\n"
        "    \"fourcc\": \"%s\",\n"
        "    \"width\": %u,\n"
        "    \"height\": %u\n"
        "  },\n"
        "  \"media\": {\n"
        "    \"video_frames\": %lld,\n"
        "    \"audio_frames\": %lld,\n"
        "    \"metadata_frames\": %lld,\n"
        "    \"dropped_frames\": %lld,\n"
        "    \"fps\": %.2f\n"
        "  },\n"
        "  \"transport\": {\n"
        "    \"bitrate_mbps\": %.2f,\n"
        "    \"total_bytes\": %llu,\n"
        "    \"jitter_ms\": %.3f,\n"
        "    \"congestion_level\": %u\n"
        "  },\n"
        "  \"kernel\": {\n"
        "    \"rtt_ms\": %.2f,\n"
        "    \"retransmissions\": %u,\n"
        "    \"send_queue_bytes\": %u,\n"
        "    \"recv_queue_bytes\": %u\n"
        "  }\n"
        "}",
        telem->is_connected ? "true" : "false",
        telem->remote_endpoint,
        fourcc_str,
        telem->width,
        telem->height,
        (long long)telem->video_frames_total,
        (long long)telem->audio_frames_total,
        (long long)telem->metadata_frames_total,
        (long long)telem->dropped_frames_total,
        (double)telem->fps,
        (double)telem->bitrate_mbps,
        (unsigned long long)telem->total_wire_bytes,
        (double)telem->inter_frame_jitter_ms,
        telem->congestion_level,
        (double)telem->kernel_rtt_ms,
        telem->tcp_retransmissions,
        telem->socket_send_q_bytes,
        telem->socket_recv_q_bytes
    );

    if (ret < 0) return 0;
    if ((size_t)ret >= max_len) {
        out_json[max_len - 1] = '\0';
        return max_len - 1;
    }
    return (size_t)ret;
}

OPENAVX_API const char* oavx_stream_get_web_control(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return NULL;
    return avx_rx_get_web_control(stream->receiver);
}

OPENAVX_API void oavx_speedhq_set_quality(uint8_t quality) {
    avx_speedhq_set_quality(quality);
}

OPENAVX_API uint8_t oavx_speedhq_get_quality(void) {
    return avx_speedhq_get_quality();
}

/* Returns underlying socket descriptor (UDP or TCP) for external poll/epoll loops. */
OPENAVX_API int oavx_stream_get_fd(oavx_stream_t *stream) {
    if (!stream) return -1;
    if (stream->direction == OAVX_DIR_RECEIVE && stream->receiver) {
        avx_recv_ctx_t *rx = (avx_recv_ctx_t*)stream->receiver;
        if ((rx->transport == OAVX_TRANSPORT_RUDP || rx->transport == OAVX_TRANSPORT_UDP) &&
            rx->udp_sock != AVX_INVALID_SOCKET) {
            return (int)rx->udp_sock;
        }
        if (rx->tcp_conn_fd != AVX_INVALID_SOCKET) {
            return (int)rx->tcp_conn_fd;
        }
    } else if (stream->direction == OAVX_DIR_TRANSMIT && stream->sender) {
        avx_send_ctx_t *tx = (avx_send_ctx_t*)stream->sender;
        if ((tx->transport == OAVX_TRANSPORT_RUDP || tx->transport == OAVX_TRANSPORT_UDP) &&
            tx->udp_sock != AVX_INVALID_SOCKET) {
            return (int)tx->udp_sock;
        }
        if (tx->tcp_listen_fd != AVX_INVALID_SOCKET) {
            return (int)tx->tcp_listen_fd;
        } else if (tx->tcp_client_fd != AVX_INVALID_SOCKET) {
            return (int)tx->tcp_client_fd;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* 5. Dynamic Virtual Matrix Router                                          */
/* ------------------------------------------------------------------------- */

struct oavx_router {
    avx_router_instance_t instance;
};

OPENAVX_API oavx_router_t* oavx_router_create(oavx_context_t *ctx, const char *name, const char *groups) {
    (void)ctx;
    avx_router_config_t r = { .name = name, .groups = groups };
    avx_router_instance_t ri = avx_router_create(&r);
    if (!ri) return NULL;
    oavx_router_t *router = (oavx_router_t*)calloc(1, sizeof(oavx_router_t));
    if (!router) {
        avx_router_destroy(ri);
        return NULL;
    }
    router->instance = ri;
    return router;
}

OPENAVX_API void oavx_router_destroy(oavx_router_t *router) {
    if (!router) return;
    if (router->instance) {
        avx_router_destroy(router->instance);
        router->instance = NULL;
    }
    free(router);
}

/* Seamlessly switches downstream receiver to target endpoint without pipeline reinitialization. */
OPENAVX_API bool oavx_router_route(oavx_router_t *router, const oavx_endpoint_t *source) {
    if (!router || !router->instance) return false;
    return avx_router_route(router->instance, source);
}

OPENAVX_API bool oavx_router_clear(oavx_router_t *router) {
    if (!router || !router->instance) return false;
    return avx_router_clear(router->instance);
}

OPENAVX_API oavx_endpoint_t oavx_router_get_source(oavx_router_t *router) {
    if (!router || !router->instance) {
        oavx_endpoint_t empty = {0};
        return empty;
    }
    return avx_router_get_source(router->instance);
}

/* ------------------------------------------------------------------------- */
/* 6. Media Clock & Frame Synchronizer                                       */
/* ------------------------------------------------------------------------- */

struct oavx_framesync {
    avx_sync_instance_t instance;
};

OPENAVX_API oavx_framesync_t* oavx_framesync_create(oavx_stream_t *stream) {
    if (!stream || stream->direction != OAVX_DIR_RECEIVE || !stream->receiver) return NULL;
    avx_sync_instance_t fsi = avx_sync_create(stream->receiver);
    if (!fsi) return NULL;
    oavx_framesync_t *fs = (oavx_framesync_t*)calloc(1, sizeof(oavx_framesync_t));
    if (!fs) {
        avx_sync_destroy(fsi);
        return NULL;
    }
    fs->instance = fsi;
    return fs;
}

OPENAVX_API void oavx_framesync_destroy(oavx_framesync_t *fs) {
    if (!fs) return;
    if (fs->instance) {
        avx_sync_destroy(fs->instance);
        fs->instance = NULL;
    }
    free(fs);
}

/*
 * Locks to hardware/media clock and captures freshest video frame,
 * synthesizing or dropping fields according to field_type.
 */
OPENAVX_API void oavx_framesync_capture_video(oavx_framesync_t *fs, oavx_video_frame_t *frame, int field_type) {
    if (!fs || !fs->instance || !frame) return;
    avx_sync_capture_video(fs->instance, frame, field_type);
}

OPENAVX_API void oavx_framesync_free_video(oavx_framesync_t *fs, oavx_video_frame_t *frame) {
    if (!fs || !fs->instance || !frame) return;
    avx_sync_free_video(fs->instance, frame);
}

/* Pulls resampled audio samples matching target rate and channel layout. */
OPENAVX_API void oavx_framesync_capture_audio(oavx_framesync_t *fs, oavx_audio_frame_t *frame, uint32_t sample_rate, uint16_t channels, uint32_t num_samples) {
    if (!fs || !fs->instance || !frame) return;
    avx_sync_capture_audio(fs->instance, frame, sample_rate, channels, num_samples);
}

OPENAVX_API void oavx_framesync_free_audio(oavx_framesync_t *fs, oavx_audio_frame_t *frame) {
    if (!fs || !fs->instance || !frame) return;
    avx_sync_free_audio(fs->instance, frame);
}

OPENAVX_API int oavx_framesync_audio_queue_depth(oavx_framesync_t *fs) {
    if (!fs || !fs->instance) return 0;
    return avx_sync_audio_queue_depth(fs->instance);
}

/* ------------------------------------------------------------------------- */
/* 7. Media Utilities                                                        */
/* ------------------------------------------------------------------------- */

OPENAVX_API void oavx_util_audio_to_interleaved_16s(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_16s_t* dst) {
    avx_util_audio_to_interleaved_16s(src, dst);
}

OPENAVX_API void oavx_util_audio_from_interleaved_16s(const oavx_audio_frame_interleaved_16s_t* src, oavx_audio_frame_t* dst) {
    avx_util_audio_from_interleaved_16s(src, dst);
}

OPENAVX_API void oavx_util_audio_to_interleaved_32f(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_32f_t* dst) {
    avx_util_audio_to_interleaved_32f(src, dst);
}

OPENAVX_API void oavx_util_audio_from_interleaved_32f(const oavx_audio_frame_interleaved_32f_t* src, oavx_audio_frame_t* dst) {
    avx_util_audio_from_interleaved_32f(src, dst);
}

OPENAVX_API void oavx_util_audio_to_interleaved_32s(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_32s_t* dst) {
    avx_util_audio_to_interleaved_32s(src, dst);
}

OPENAVX_API void oavx_util_audio_from_interleaved_32s(const oavx_audio_frame_interleaved_32s_t* src, oavx_audio_frame_t* dst) {
    avx_util_audio_from_interleaved_32s(src, dst);
}

/* Unpacks 10-bit packed V210 (3 samples per 32-bit word) to planar 16-bit P216 via SIMD. */
OPENAVX_API void oavx_util_V210_to_P216(const oavx_video_frame_t* src, oavx_video_frame_t* dst) {
    avx_util_V210_to_P216(src, dst);
}

/* Packs planar 16-bit P216 to 10-bit packed V210 words via SIMD. */
OPENAVX_API void oavx_util_P216_to_V210(const oavx_video_frame_t* src, oavx_video_frame_t* dst) {
    avx_util_P216_to_V210(src, dst);
}
