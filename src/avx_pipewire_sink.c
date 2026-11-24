/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_pipewire_sink.c - Native user-space PipeWire virtual camera sink
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "openavx.h"
#include "avx_pipewire_sink.h"
#include "avx_color.h"
#include "avx_platform.h"

#if defined(AVX_OS_LINUX)
#include <dlfcn.h>
#include <pthread.h>

/* Include PipeWire & SPA headers (either system or OpenAVX compat) */
#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>

/* Dynamic PipeWire library loader table */
typedef struct {
    void *handle;
    void (*pw_init)(int *argc, char **argv[]);
    void (*pw_deinit)(void);
    const char *(*pw_get_library_version)(void);
    struct pw_thread_loop *(*pw_thread_loop_new)(const char *name, const struct spa_dict *props);
    void (*pw_thread_loop_destroy)(struct pw_thread_loop *loop);
    struct pw_loop *(*pw_thread_loop_get_loop)(struct pw_thread_loop *loop);
    int (*pw_thread_loop_start)(struct pw_thread_loop *loop);
    void (*pw_thread_loop_stop)(struct pw_thread_loop *loop);
    void (*pw_thread_loop_lock)(struct pw_thread_loop *loop);
    void (*pw_thread_loop_unlock)(struct pw_thread_loop *loop);
    void (*pw_thread_loop_signal)(struct pw_thread_loop *loop, bool wait_for_accept);
    struct pw_properties *(*pw_properties_new)(const char *key, ...);
    int (*pw_properties_set)(struct pw_properties *properties, const char *key, const char *val);
    struct pw_stream *(*pw_stream_new_simple)(
        struct pw_loop *loop,
        const char *name,
        struct pw_properties *props,
        const struct pw_stream_events *events,
        void *data);
    void (*pw_stream_destroy)(struct pw_stream *stream);
    int (*pw_stream_connect)(
        struct pw_stream *stream,
        enum pw_direction direction,
        uint32_t target_id,
        enum pw_stream_flags flags,
        const struct spa_pod **params,
        uint32_t n_params);
    int (*pw_stream_disconnect)(struct pw_stream *stream);
    enum pw_stream_state (*pw_stream_get_state)(struct pw_stream *stream, const char **error);
    struct pw_buffer *(*pw_stream_dequeue_buffer)(struct pw_stream *stream);
    int (*pw_stream_queue_buffer)(struct pw_stream *stream, struct pw_buffer *buffer);
    int (*pw_stream_update_params)(struct pw_stream *stream, const struct spa_pod **params, uint32_t n_params);
} avx_pw_table_t;

static avx_pw_table_t g_pw;
static pthread_once_t g_pw_init_once = PTHREAD_ONCE_INIT;
static bool g_pw_loaded = false;

/* Dynamically binds libpipewire-0.3 symbols at runtime without compile-time hard linking. */
static void avx_pipewire_do_init(void) {
    g_pw.handle = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!g_pw.handle) {
        g_pw.handle = dlopen("libpipewire-0.3.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_pw.handle) {
        g_pw_loaded = false;
        return;
    }

    #define LOAD_SYM(name) do { \
        g_pw.name = dlsym(g_pw.handle, #name); \
        if (!g_pw.name) { \
            dlclose(g_pw.handle); \
            g_pw.handle = NULL; \
            g_pw_loaded = false; \
            return; \
        } \
    } while(0)

    LOAD_SYM(pw_init);
    LOAD_SYM(pw_deinit);
    LOAD_SYM(pw_get_library_version);
    LOAD_SYM(pw_thread_loop_new);
    LOAD_SYM(pw_thread_loop_destroy);
    LOAD_SYM(pw_thread_loop_get_loop);
    LOAD_SYM(pw_thread_loop_start);
    LOAD_SYM(pw_thread_loop_stop);
    LOAD_SYM(pw_thread_loop_lock);
    LOAD_SYM(pw_thread_loop_unlock);
    LOAD_SYM(pw_thread_loop_signal);
    LOAD_SYM(pw_properties_new);
    LOAD_SYM(pw_properties_set);
    LOAD_SYM(pw_stream_new_simple);
    LOAD_SYM(pw_stream_destroy);
    LOAD_SYM(pw_stream_connect);
    LOAD_SYM(pw_stream_disconnect);
    LOAD_SYM(pw_stream_get_state);
    LOAD_SYM(pw_stream_dequeue_buffer);
    LOAD_SYM(pw_stream_queue_buffer);
    LOAD_SYM(pw_stream_update_params);
    #undef LOAD_SYM

    g_pw.pw_init(NULL, NULL);
    g_pw_loaded = true;
}

static bool avx_pipewire_ensure_loaded(void) {
    pthread_once(&g_pw_init_once, avx_pipewire_do_init);
    return g_pw_loaded;
}

/* PipeWire virtual camera sink internal state */
struct avx_pipewire_sink_t {
    struct pw_thread_loop *loop;
    struct pw_stream      *stream;
    struct spa_hook        stream_listener;
    
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t format; /* SPA_VIDEO_FORMAT_YUY2 or SPA_VIDEO_FORMAT_UYVY */
    
    bool is_active;
    bool is_streaming;
    
    /* Conversion buffer for UYVY <-> YUY2 */
    uint8_t *conv_buf;
    size_t   conv_buf_cap;
    
    avx_mutex_t lock;
};

/* Fast SIMD / bitwise conversion of UYVY (U Y0 V Y1) to YUYV (Y0 U Y1 V) */
static void uyvy_to_yuyv_convert(const uint8_t *src, uint8_t *dst, size_t pixel_count)
{
    size_t word_count = pixel_count / 2;
    const uint32_t *src32 = (const uint32_t*)src;
    uint32_t *dst32 = (uint32_t*)dst;

    for (size_t i = 0; i < word_count; i++) {
        uint32_t in = src32[i];
        uint32_t u  = (in & 0x000000FFU);
        uint32_t y0 = (in & 0x0000FF00U) >> 8;
        uint32_t v  = (in & 0x00FF0000U) >> 16;
        uint32_t y1 = (in & 0xFF000000U) >> 24;
        dst32[i] = y0 | (u << 8) | (y1 << 16) | (v << 24);
    }
}

/* PipeWire Stream Event Callbacks */
static void on_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    avx_pipewire_sink_t *sink = (avx_pipewire_sink_t*)data;
    (void)old;
    if (!sink) return;

    if (state == PW_STREAM_STATE_STREAMING) {
        sink->is_streaming = true;
        sink->is_active = true;
        oavx_log_write(OAVX_LOG_INFO, "pipewire", "Virtual camera stream active and streaming");
    } else if (state == PW_STREAM_STATE_PAUSED) {
        sink->is_streaming = false;
        sink->is_active = true;
        oavx_log_write(OAVX_LOG_DEBUG, "pipewire", "Virtual camera stream paused (ready)");
    } else if (state == PW_STREAM_STATE_ERROR) {
        sink->is_streaming = false;
        sink->is_active = false;
        oavx_log_write(OAVX_LOG_ERROR, "pipewire", "Virtual camera stream error: %s", error ? error : "unknown");
    } else {
        sink->is_streaming = false;
        sink->is_active = false;
    }
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    avx_pipewire_sink_t *sink = (avx_pipewire_sink_t*)data;
    if (!sink || !param || id != SPA_PARAM_Format) return;

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0) return;

    sink->width = info.size.width;
    sink->height = info.size.height;
    if (info.framerate.denom > 0) {
        sink->fps_num = info.framerate.num;
        sink->fps_den = info.framerate.denom;
    }
    sink->format = info.format;

    oavx_log_write(OAVX_LOG_INFO, "pipewire", "Negotiated format: %ux%u @ %u/%u fps (SPA format: %u)",
        sink->width, sink->height, sink->fps_num, sink->fps_den, sink->format);
}

static const struct pw_stream_events k_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
};

bool avx_pipewire_is_available(void)
{
    return avx_pipewire_ensure_loaded();
}

/* Spawns PipeWire thread loop and registers virtual camera stream with SPA video POD format negotiation. */
avx_pipewire_sink_t *avx_pipewire_sink_create(const avx_pipewire_sink_config_t *config)
{
    if (!avx_pipewire_ensure_loaded()) {
        oavx_log_write(OAVX_LOG_WARN, "pipewire", "PipeWire library (libpipewire-0.3.so.0) not found on system");
        return NULL;
    }

    avx_pipewire_sink_t *sink = (avx_pipewire_sink_t*)calloc(1, sizeof(avx_pipewire_sink_t));
    if (!sink) return NULL;

    sink->width = (config && config->width > 0) ? config->width : 1920;
    sink->height = (config && config->height > 0) ? config->height : 1080;
    sink->fps_num = (config && config->fps_num > 0) ? config->fps_num : 30;
    sink->fps_den = (config && config->fps_den > 0) ? config->fps_den : 1;
    sink->format = SPA_VIDEO_FORMAT_YUY2;

    sink->conv_buf_cap = (size_t)sink->width * sink->height * 4;
    sink->conv_buf = (uint8_t*)malloc(sink->conv_buf_cap);
    if (!sink->conv_buf) {
        free(sink);
        return NULL;
    }
    avx_mutex_init(&sink->lock);

    sink->loop = g_pw.pw_thread_loop_new("openavx-virtcam-loop", NULL);
    if (!sink->loop) {
        oavx_log_write(OAVX_LOG_ERROR, "pipewire", "Failed to create PipeWire thread loop");
        free(sink->conv_buf);
        free(sink);
        return NULL;
    }

    const char *node_name = (config && config->node_name) ? config->node_name : "OpenAVX Virtual Camera";
    const char *node_desc = (config && config->node_description) ? config->node_description : "OpenAVX Virtual Camera Feed";

    /* Create PipeWire node properties conforming to standard Linux video capture specifications */
    struct pw_properties *props = g_pw.pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Camera",
        "media.class", "Video/Source",
        "node.name", node_name,
        "node.description", node_desc,
        "node.nick", "OpenAVX Cam",
        NULL
    );

    sink->stream = g_pw.pw_stream_new_simple(
        g_pw.pw_thread_loop_get_loop(sink->loop),
        "openavx-camera-stream",
        props,
        &k_stream_events,
        sink
    );

    if (!sink->stream) {
        oavx_log_write(OAVX_LOG_ERROR, "pipewire", "Failed to create PipeWire stream");
        g_pw.pw_thread_loop_destroy(sink->loop);
        free(sink->conv_buf);
        free(sink);
        return NULL;
    }

    /* Build supported format parameters: YUY2 (primary) and UYVY */
    uint8_t b1_buf[1024];
    uint8_t b2_buf[1024];
    struct spa_pod_builder b1 = SPA_POD_BUILDER_INIT(b1_buf, sizeof(b1_buf));
    struct spa_pod_builder b2 = SPA_POD_BUILDER_INIT(b2_buf, sizeof(b2_buf));

    struct spa_video_info_raw info_yuy2;
    spa_zero(info_yuy2);
    info_yuy2.format = SPA_VIDEO_FORMAT_YUY2;
    info_yuy2.size.width = sink->width;
    info_yuy2.size.height = sink->height;
    info_yuy2.framerate.num = sink->fps_num;
    info_yuy2.framerate.denom = sink->fps_den;

    struct spa_video_info_raw info_uyvy;
    spa_zero(info_uyvy);
    info_uyvy.format = SPA_VIDEO_FORMAT_UYVY;
    info_uyvy.size.width = sink->width;
    info_uyvy.size.height = sink->height;
    info_uyvy.framerate.num = sink->fps_num;
    info_uyvy.framerate.denom = sink->fps_den;

    const struct spa_pod *params[2];
    params[0] = spa_format_video_raw_build(&b1, SPA_PARAM_EnumFormat, &info_yuy2);
    params[1] = spa_format_video_raw_build(&b2, SPA_PARAM_EnumFormat, &info_uyvy);

    g_pw.pw_thread_loop_lock(sink->loop);
    int rc = g_pw.pw_stream_connect(
        sink->stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
        params,
        2
    );
    g_pw.pw_thread_loop_unlock(sink->loop);

    if (rc < 0) {
        oavx_log_write(OAVX_LOG_ERROR, "pipewire", "pw_stream_connect failed (code: %d)", rc);
        g_pw.pw_stream_destroy(sink->stream);
        g_pw.pw_thread_loop_destroy(sink->loop);
        free(sink->conv_buf);
        free(sink);
        return NULL;
    }

    if (g_pw.pw_thread_loop_start(sink->loop) < 0) {
        oavx_log_write(OAVX_LOG_ERROR, "pipewire", "Failed to start PipeWire thread loop");
        g_pw.pw_stream_destroy(sink->stream);
        g_pw.pw_thread_loop_destroy(sink->loop);
        free(sink->conv_buf);
        free(sink);
        return NULL;
    }

    oavx_log_write(OAVX_LOG_INFO, "pipewire", "Initialized user-space virtual camera '%s' (PipeWire %s)",
        node_name, g_pw.pw_get_library_version());
    return sink;
}

void avx_pipewire_sink_destroy(avx_pipewire_sink_t *sink)
{
    if (!sink) return;

    if (sink->loop) {
        g_pw.pw_thread_loop_stop(sink->loop);
    }
    if (sink->stream) {
        g_pw.pw_stream_destroy(sink->stream);
        sink->stream = NULL;
    }
    if (sink->loop) {
        g_pw.pw_thread_loop_destroy(sink->loop);
        sink->loop = NULL;
    }

    avx_mutex_destroy(&sink->lock);
    if (sink->conv_buf) {
        free(sink->conv_buf);
        sink->conv_buf = NULL;
    }
    free(sink);
}

/* Dequeues PipeWire shared memory buffer, writes converted video frame, and queues for client consumption. */
bool avx_pipewire_sink_push_frame(
    avx_pipewire_sink_t *sink,
    const uint8_t *frame_data,
    size_t frame_size,
    uint32_t width,
    uint32_t height,
    uint32_t fourcc)
{
    if (!sink || !sink->stream || !frame_data || frame_size == 0) return false;

    g_pw.pw_thread_loop_lock(sink->loop);
    struct pw_buffer *b = g_pw.pw_stream_dequeue_buffer(sink->stream);
    if (!b) {
        g_pw.pw_thread_loop_unlock(sink->loop);
        return false;
    }

    struct spa_buffer *buf = b->buffer;
    if (!buf || buf->n_datas == 0 || !buf->datas[0].data) {
        g_pw.pw_stream_queue_buffer(sink->stream, b);
        g_pw.pw_thread_loop_unlock(sink->loop);
        return false;
    }

    size_t target_stride = width * 2;
    size_t target_size = target_stride * height;

    if (buf->datas[0].maxsize < target_size) {
        g_pw.pw_stream_queue_buffer(sink->stream, b);
        g_pw.pw_thread_loop_unlock(sink->loop);
        return false;
    }

    uint8_t *dst = (uint8_t*)buf->datas[0].data;

    if (sink->format == SPA_VIDEO_FORMAT_YUY2) {
        if (fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
            uyvy_to_yuyv_convert(frame_data, dst, (size_t)width * height);
        } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX) {
            if (sink->conv_buf_cap >= target_size * 2) {
                uint8_t *tmp_uyvy = sink->conv_buf;
                avx_color_rgba_to_uyvy(frame_data, tmp_uyvy, (int)width, (int)height);
                uyvy_to_yuyv_convert(tmp_uyvy, dst, (size_t)width * height);
            }
        } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA || fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
            if (sink->conv_buf_cap >= target_size * 2) {
                uint8_t *tmp_uyvy = sink->conv_buf;
                avx_color_bgra_to_uyvy(frame_data, tmp_uyvy, (int)width, (int)height);
                uyvy_to_yuyv_convert(tmp_uyvy, dst, (size_t)width * height);
            }
        } else {
            /* Direct raw copy fallback */
            memcpy(dst, frame_data, frame_size < target_size ? frame_size : target_size);
        }
    } else {
        /* Direct UYVY or generic match */
        if (fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
            memcpy(dst, frame_data, target_size);
        } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA) {
            avx_color_rgba_to_uyvy(frame_data, dst, (int)width, (int)height);
        } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA) {
            avx_color_bgra_to_uyvy(frame_data, dst, (int)width, (int)height);
        } else {
            memcpy(dst, frame_data, frame_size < target_size ? frame_size : target_size);
        }
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->size = target_size;
    buf->datas[0].chunk->stride = target_stride;

    g_pw.pw_stream_queue_buffer(sink->stream, b);
    g_pw.pw_thread_loop_unlock(sink->loop);
    return true;
}

bool avx_pipewire_sink_is_active(const avx_pipewire_sink_t *sink)
{
    return sink ? (sink->is_active || sink->is_streaming) : false;
}

#else /* Non-Linux or systems without PipeWire support */

bool avx_pipewire_is_available(void) { return false; }
/* Spawns PipeWire thread loop and registers virtual camera stream with SPA video POD format negotiation. */
avx_pipewire_sink_t *avx_pipewire_sink_create(const avx_pipewire_sink_config_t *config) { (void)config; return NULL; }
void avx_pipewire_sink_destroy(avx_pipewire_sink_t *sink) { (void)sink; }
/* Dequeues PipeWire shared memory buffer, writes converted video frame, and queues for client consumption. */
bool avx_pipewire_sink_push_frame(avx_pipewire_sink_t *sink, const uint8_t *f, size_t s, uint32_t w, uint32_t h, uint32_t fc) {
    (void)sink; (void)f; (void)s; (void)w; (void)h; (void)fc; return false;
}
bool avx_pipewire_sink_is_active(const avx_pipewire_sink_t *sink) { (void)sink; return false; }

#endif
