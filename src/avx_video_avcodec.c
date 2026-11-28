/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_video_avcodec.c - FFmpeg libavcodec Runtime-Loaded Video Decoder & Encoder Backend
 */

#include "avx_video_codec.h"
#include "avx_internal.h"
#include "avx_color.h"

#if defined(AVX_OS_LINUX) || defined(AVX_OS_WINDOWS)

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AVX_AV_CODEC_ID_H264 27
#define AVX_AV_CODEC_ID_HEVC 173
#define AVX_AV_CODEC_ID_AV1 226
#define AVX_AV_CODEC_ID_MJPEG 7
#define AVX_AV_NUM_DATA_POINTERS 8
#define AVX_AV_LOG_QUIET (-8)
#define AVX_AV_LOG_FATAL 8
#define AVX_AV_LOG_ERROR 16

typedef struct avx_av_packet_stub_t {
    void *buf;
    int64_t pts;
    int64_t dts;
    uint8_t *data;
    int size;
    int stream_index;
    int flags;
} avx_av_packet_stub_t;

typedef struct avx_av_frame_stub_t {
    uint8_t *data[AVX_AV_NUM_DATA_POINTERS];
    int linesize[AVX_AV_NUM_DATA_POINTERS];
    uint8_t **extended_data;
    int width;
    int height;
    int nb_samples;
    int format;
    int key_frame;
    int pict_type;
    int sar_num;
    int sar_den;
    int64_t pts;
} avx_av_frame_stub_t;

typedef unsigned (*fn_avcodec_version)(void);
typedef unsigned (*fn_avutil_version)(void);
typedef void *(*fn_avcodec_find_decoder)(int id);
typedef void *(*fn_avcodec_find_decoder_by_name)(const char *name);
typedef void *(*fn_avcodec_find_encoder)(int id);
typedef void *(*fn_avcodec_find_encoder_by_name)(const char *name);
typedef void *(*fn_avcodec_alloc_context3)(const void *codec);
typedef void (*fn_avcodec_free_context)(void **avctx);
typedef int (*fn_avcodec_open2)(void *avctx, const void *codec, void **options);
typedef int (*fn_avcodec_send_packet)(void *avctx, const void *pkt);
typedef int (*fn_avcodec_receive_frame)(void *avctx, void *frame);
typedef int (*fn_avcodec_send_frame)(void *avctx, const void *frame);
typedef int (*fn_avcodec_receive_packet)(void *avctx, void *pkt);
typedef void (*fn_avcodec_flush_buffers)(void *avctx);

typedef void *(*fn_av_packet_alloc)(void);
typedef void (*fn_av_packet_free)(void **pkt);
typedef int (*fn_av_new_packet)(void *pkt, int size);
typedef void (*fn_av_packet_unref)(void *pkt);

typedef void *(*fn_av_frame_alloc)(void);
typedef void (*fn_av_frame_free)(void **frame);
typedef void (*fn_av_frame_unref)(void *frame);
typedef int (*fn_av_frame_get_buffer)(void *frame, int align);
typedef int (*fn_av_get_pix_fmt)(const char *name);
typedef int (*fn_av_opt_set)(void *obj, const char *name, const char *val, int search_flags);
typedef int (*fn_av_opt_set_int)(void *obj, const char *name, int64_t val, int search_flags);
typedef void (*fn_av_log_set_level)(int level);

typedef struct {
    const char *codec_soname;
    const char *util_soname;
    unsigned codec_major;
    unsigned util_major;
} avx_ffmpeg_soname_pair_t;

#if defined(AVX_OS_WINDOWS)
static const avx_ffmpeg_soname_pair_t k_ffmpeg_pairs[] = {
    {"avcodec-62.dll", "avutil-60.dll", 62U, 60U}, /* FFmpeg 8.x */
    {"avcodec-61.dll", "avutil-59.dll", 61U, 59U}, /* FFmpeg 7.x */
    {"avcodec-60.dll", "avutil-58.dll", 60U, 58U}  /* FFmpeg 6.x */
};
#else
static const avx_ffmpeg_soname_pair_t k_ffmpeg_pairs[] = {
    {"libavcodec.so.62", "libavutil.so.60", 62U, 60U}, /* FFmpeg 8.x */
    {"libavcodec.so.61", "libavutil.so.59", 61U, 59U}, /* FFmpeg 7.x */
    {"libavcodec.so.60", "libavutil.so.58", 60U, 58U}  /* FFmpeg 6.x */
};
#endif

static struct {
    avx_mutex_t lock;
    bool probed;
    bool available;
    void *codec_handle;
    void *util_handle;

    fn_avcodec_version avcodec_version;
    fn_avcodec_find_decoder avcodec_find_decoder;
    fn_avcodec_find_decoder_by_name avcodec_find_decoder_by_name;
    fn_avcodec_find_encoder avcodec_find_encoder;
    fn_avcodec_find_encoder_by_name avcodec_find_encoder_by_name;
    fn_avcodec_alloc_context3 avcodec_alloc_context3;
    fn_avcodec_free_context avcodec_free_context;
    fn_avcodec_open2 avcodec_open2;
    fn_avcodec_send_packet avcodec_send_packet;
    fn_avcodec_receive_frame avcodec_receive_frame;
    fn_avcodec_send_frame avcodec_send_frame;
    fn_avcodec_receive_packet avcodec_receive_packet;
    fn_avcodec_flush_buffers avcodec_flush_buffers;
    fn_av_packet_alloc av_packet_alloc;
    fn_av_packet_free av_packet_free;
    fn_av_new_packet av_new_packet;
    fn_av_packet_unref av_packet_unref;

    fn_avutil_version avutil_version;
    fn_av_frame_alloc av_frame_alloc;
    fn_av_frame_free av_frame_free;
    fn_av_frame_unref av_frame_unref;
    fn_av_frame_get_buffer av_frame_get_buffer;
    fn_av_get_pix_fmt av_get_pix_fmt;
    fn_av_opt_set av_opt_set;
    fn_av_opt_set_int av_opt_set_int;
    fn_av_log_set_level av_log_set_level;

    int fmt_yuv420p;
    int fmt_yuvj420p;
    int fmt_nv12;
    int fmt_yuv422p;
    int fmt_yuvj422p;
    int fmt_yuv420p10le;
    int fmt_yuv422p10le;
} g_avcodec = {.lock = AVX_MUTEX_INITIALIZER,
               .probed = false,
               .available = false,
               .fmt_yuv420p = -1,
               .fmt_yuvj420p = -1,
               .fmt_nv12 = -1,
               .fmt_yuv422p = -1,
               .fmt_yuvj422p = -1,
               .fmt_yuv420p10le = -1,
               .fmt_yuv422p10le = -1};

/* Dynamically probes and binds libavcodec and libavutil across versions 6.x, 7.x, 8.x via dlopen. */
static bool probe_avcodec_library(void) {
    avx_mutex_lock(&g_avcodec.lock);
    if (g_avcodec.probed) {
        bool ok = g_avcodec.available;
        avx_mutex_unlock(&g_avcodec.lock);
        return ok;
    }
    g_avcodec.probed = true;

    for (size_t i = 0; i < sizeof(k_ffmpeg_pairs) / sizeof(k_ffmpeg_pairs[0]); ++i) {
        const avx_ffmpeg_soname_pair_t *pair = &k_ffmpeg_pairs[i];
        void *h_util = avx_dlopen(pair->util_soname);
        if (!h_util) {
            continue;
        }

        void *h_codec = avx_dlopen(pair->codec_soname);
        if (!h_codec) {
            avx_dlclose(h_util);
            continue;
        }

        fn_avutil_version p_util_ver = (fn_avutil_version)avx_dlsym(h_util, "avutil_version");
        fn_avcodec_version p_codec_ver = (fn_avcodec_version)avx_dlsym(h_codec, "avcodec_version");
        if (!p_util_ver || !p_codec_ver) {
            avx_dlclose(h_codec);
            avx_dlclose(h_util);
            continue;
        }

        unsigned util_major = p_util_ver() >> 16;
        unsigned codec_major = p_codec_ver() >> 16;
        if (util_major != pair->util_major || codec_major != pair->codec_major) {
            avx_dlclose(h_codec);
            avx_dlclose(h_util);
            continue;
        }

        g_avcodec.avcodec_version = p_codec_ver;
        g_avcodec.avcodec_find_decoder = (fn_avcodec_find_decoder)avx_dlsym(h_codec, "avcodec_find_decoder");
        g_avcodec.avcodec_find_decoder_by_name =
            (fn_avcodec_find_decoder_by_name)avx_dlsym(h_codec, "avcodec_find_decoder_by_name");
        g_avcodec.avcodec_find_encoder = (fn_avcodec_find_encoder)avx_dlsym(h_codec, "avcodec_find_encoder");
        g_avcodec.avcodec_find_encoder_by_name =
            (fn_avcodec_find_encoder_by_name)avx_dlsym(h_codec, "avcodec_find_encoder_by_name");
        g_avcodec.avcodec_alloc_context3 = (fn_avcodec_alloc_context3)avx_dlsym(h_codec, "avcodec_alloc_context3");
        g_avcodec.avcodec_free_context = (fn_avcodec_free_context)avx_dlsym(h_codec, "avcodec_free_context");
        g_avcodec.avcodec_open2 = (fn_avcodec_open2)avx_dlsym(h_codec, "avcodec_open2");
        g_avcodec.avcodec_send_packet = (fn_avcodec_send_packet)avx_dlsym(h_codec, "avcodec_send_packet");
        g_avcodec.avcodec_receive_frame = (fn_avcodec_receive_frame)avx_dlsym(h_codec, "avcodec_receive_frame");
        g_avcodec.avcodec_send_frame = (fn_avcodec_send_frame)avx_dlsym(h_codec, "avcodec_send_frame");
        g_avcodec.avcodec_receive_packet = (fn_avcodec_receive_packet)avx_dlsym(h_codec, "avcodec_receive_packet");
        g_avcodec.avcodec_flush_buffers = (fn_avcodec_flush_buffers)avx_dlsym(h_codec, "avcodec_flush_buffers");
        g_avcodec.av_packet_alloc = (fn_av_packet_alloc)avx_dlsym(h_codec, "av_packet_alloc");
        g_avcodec.av_packet_free = (fn_av_packet_free)avx_dlsym(h_codec, "av_packet_free");
        g_avcodec.av_new_packet = (fn_av_new_packet)avx_dlsym(h_codec, "av_new_packet");
        g_avcodec.av_packet_unref = (fn_av_packet_unref)avx_dlsym(h_codec, "av_packet_unref");

        g_avcodec.avutil_version = p_util_ver;
        g_avcodec.av_frame_alloc = (fn_av_frame_alloc)avx_dlsym(h_util, "av_frame_alloc");
        g_avcodec.av_frame_free = (fn_av_frame_free)avx_dlsym(h_util, "av_frame_free");
        g_avcodec.av_frame_unref = (fn_av_frame_unref)avx_dlsym(h_util, "av_frame_unref");
        g_avcodec.av_frame_get_buffer = (fn_av_frame_get_buffer)avx_dlsym(h_util, "av_frame_get_buffer");
        g_avcodec.av_get_pix_fmt = (fn_av_get_pix_fmt)avx_dlsym(h_util, "av_get_pix_fmt");
        g_avcodec.av_opt_set = (fn_av_opt_set)avx_dlsym(h_util, "av_opt_set");
        g_avcodec.av_opt_set_int = (fn_av_opt_set_int)avx_dlsym(h_util, "av_opt_set_int");
        g_avcodec.av_log_set_level = (fn_av_log_set_level)avx_dlsym(h_util, "av_log_set_level");

        if (!g_avcodec.avcodec_find_decoder || !g_avcodec.avcodec_alloc_context3 || !g_avcodec.avcodec_free_context ||
            !g_avcodec.avcodec_open2 || !g_avcodec.avcodec_send_packet || !g_avcodec.avcodec_receive_frame ||
            !g_avcodec.av_packet_alloc || !g_avcodec.av_packet_free || !g_avcodec.av_new_packet ||
            !g_avcodec.av_packet_unref || !g_avcodec.av_frame_alloc || !g_avcodec.av_frame_free ||
            !g_avcodec.av_frame_unref || !g_avcodec.av_get_pix_fmt) {
            avx_dlclose(h_codec);
            avx_dlclose(h_util);
            continue;
        }

        if (g_avcodec.av_log_set_level) {
            g_avcodec.av_log_set_level(AVX_AV_LOG_FATAL);
        }

        g_avcodec.fmt_yuv420p = g_avcodec.av_get_pix_fmt("yuv420p");
        g_avcodec.fmt_yuvj420p = g_avcodec.av_get_pix_fmt("yuvj420p");
        g_avcodec.fmt_nv12 = g_avcodec.av_get_pix_fmt("nv12");
        g_avcodec.fmt_yuv422p = g_avcodec.av_get_pix_fmt("yuv422p");
        g_avcodec.fmt_yuvj422p = g_avcodec.av_get_pix_fmt("yuvj422p");
        g_avcodec.fmt_yuv420p10le = g_avcodec.av_get_pix_fmt("yuv420p10le");
        g_avcodec.fmt_yuv422p10le = g_avcodec.av_get_pix_fmt("yuv422p10le");

        if (g_avcodec.fmt_yuv420p < 0 && g_avcodec.fmt_nv12 < 0) {
            avx_dlclose(h_codec);
            avx_dlclose(h_util);
            continue;
        }

        g_avcodec.codec_handle = h_codec;
        g_avcodec.util_handle = h_util;
        g_avcodec.available = true;
        break;
    }

    bool ok = g_avcodec.available;
    avx_mutex_unlock(&g_avcodec.lock);
    return ok;
}

typedef struct avx_avcodec_dec_ctx_t {
    void *avctx;
    void *pkt;
    void *frame;
    uint32_t src_fourcc;
    uint8_t *frame_buf;
    size_t frame_buf_size;
} avx_avcodec_dec_ctx_t;

static void *find_decoder_for_fourcc(uint32_t fourcc) {
    if (!probe_avcodec_library()) {
        return NULL;
    }

    static const char *k_h264_dec_cands[] = {"h264", NULL};
    static const char *k_hevc_dec_cands[] = {"hevc", NULL};
    static const char *k_av1_dec_cands[] = {"libdav1d", "av1", "libaom-av1", NULL};
    static const char *k_mjpeg_dec_cands[] = {"mjpeg", NULL};

    const char **cands = NULL;
    int fallback_codec_id = 0;

    if (fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) {
        cands = k_h264_dec_cands;
        fallback_codec_id = AVX_AV_CODEC_ID_H264;
    } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || fourcc == 0x43564568U) {
        cands = k_hevc_dec_cands;
        fallback_codec_id = AVX_AV_CODEC_ID_HEVC;
    } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
        cands = k_av1_dec_cands;
        fallback_codec_id = AVX_AV_CODEC_ID_AV1;
    } else if (fourcc == 0x47504A4D || fourcc == 0x4A50474D) {
        cands = k_mjpeg_dec_cands;
        fallback_codec_id = AVX_AV_CODEC_ID_MJPEG;
    } else {
        return NULL;
    }

    if (cands && g_avcodec.avcodec_find_decoder_by_name) {
        for (int i = 0; cands[i]; ++i) {
            void *dec = g_avcodec.avcodec_find_decoder_by_name(cands[i]);
            if (dec) return dec;
        }
    }

    if (fallback_codec_id != 0 && g_avcodec.avcodec_find_decoder) {
        return g_avcodec.avcodec_find_decoder(fallback_codec_id);
    }
    return NULL;
}

static bool avcodec_dec_supports_fourcc(uint32_t fourcc) {
    return find_decoder_for_fourcc(fourcc) != NULL;
}

static void avcodec_dec_destroy(void *backend_ctx) {
    avx_avcodec_dec_ctx_t *ctx = (avx_avcodec_dec_ctx_t *)backend_ctx;
    if (!ctx) return;

    if (ctx->avctx && g_avcodec.avcodec_free_context) {
        g_avcodec.avcodec_free_context(&ctx->avctx);
    }
    if (ctx->pkt && g_avcodec.av_packet_free) {
        g_avcodec.av_packet_free(&ctx->pkt);
    }
    if (ctx->frame && g_avcodec.av_frame_free) {
        g_avcodec.av_frame_free(&ctx->frame);
    }
    free(ctx->frame_buf);
    free(ctx);
}

/* Allocates FFmpeg AVCodecContext configured for ultra-low latency slice-threaded decoding. */
static int avcodec_dec_create(const avx_video_dec_cfg_t *cfg, void **backend_ctx) {
    if (!cfg || !backend_ctx) return -1;
    if (!probe_avcodec_library()) return -1;

    void *codec = find_decoder_for_fourcc(cfg->fourcc);
    if (!codec) return -1;

    void *avctx = g_avcodec.avcodec_alloc_context3(codec);
    if (!avctx) return -1;

    /* Low-delay slice threading prevents multi-frame buffering latency */
    if (g_avcodec.av_opt_set) {
        g_avcodec.av_opt_set(avctx, "flags", "+low_delay", 0);
        if (cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 || cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC) {
            g_avcodec.av_opt_set(avctx, "thread_type", "slice", 0);
        }
    }
    if (g_avcodec.av_opt_set_int) {
        g_avcodec.av_opt_set_int(avctx, "threads", 2, 0);
    }

    if (g_avcodec.avcodec_open2(avctx, codec, NULL) < 0) {
        g_avcodec.avcodec_free_context(&avctx);
        return -1;
    }

    avx_avcodec_dec_ctx_t *ctx = (avx_avcodec_dec_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        g_avcodec.avcodec_free_context(&avctx);
        return -1;
    }

    ctx->avctx = avctx;
    ctx->src_fourcc = cfg->fourcc;
    ctx->pkt = g_avcodec.av_packet_alloc();
    ctx->frame = g_avcodec.av_frame_alloc();
    if (!ctx->pkt || !ctx->frame) {
        avcodec_dec_destroy(ctx);
        return -1;
    }

    *backend_ctx = ctx;
    return 0;
}

static int avcodec_ensure_frame_buf(avx_avcodec_dec_ctx_t *ctx, uint64_t needed_u64) {
    if (needed_u64 == 0 || needed_u64 > AVX_MAX_DECOMPRESSED_FRAME_SIZE) {
        return -1;
    }
    size_t needed = (size_t)needed_u64;
    if (ctx->frame_buf_size < needed) {
        uint8_t *nb = (uint8_t *)realloc(ctx->frame_buf, needed);
        if (!nb) return -1;
        ctx->frame_buf = nb;
        ctx->frame_buf_size = needed;
    }
    return 0;
}

/* Converts decoded AVFrame pixel formats (NV12, I420, P010) into OpenAVX destination layout. */
static int avcodec_copy_out_frame(avx_avcodec_dec_ctx_t *ctx, oavx_video_frame_t *out_frame) {
    const avx_av_frame_stub_t *f = (const avx_av_frame_stub_t *)ctx->frame;
    if (f->width <= 0 || f->height <= 0 || f->width > AVX_MAX_VIDEO_WIDTH || f->height > AVX_MAX_VIDEO_HEIGHT) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    uint32_t w = (uint32_t)f->width;
    uint32_t h = (uint32_t)f->height;
    int fmt = f->format;

    if (fmt >= 0 && (fmt == g_avcodec.fmt_yuv420p || fmt == g_avcodec.fmt_yuvj420p)) {
        uint32_t cw = (w + 1U) / 2U;
        uint32_t ch = (h + 1U) / 2U;
        if (!f->data[0] || !f->data[1] || !f->data[2] || f->linesize[0] < (int)w || f->linesize[1] < (int)cw ||
            f->linesize[2] < (int)cw) {
            return AVX_VIDEO_CODEC_ERROR;
        }
        uint64_t needed = (uint64_t)w * h + 2ULL * (uint64_t)cw * ch;
        if (avcodec_ensure_frame_buf(ctx, needed) != 0) {
            return AVX_VIDEO_CODEC_ERROR;
        }

        for (uint32_t y = 0; y < h; ++y) {
            memcpy(ctx->frame_buf + (size_t)y * w, f->data[0] + (size_t)y * (size_t)f->linesize[0], w);
        }
        uint8_t *dst_u = ctx->frame_buf + (size_t)w * h;
        for (uint32_t y = 0; y < ch; ++y) {
            memcpy(dst_u + (size_t)y * cw, f->data[1] + (size_t)y * (size_t)f->linesize[1], cw);
        }
        uint8_t *dst_v = dst_u + (size_t)cw * ch;
        for (uint32_t y = 0; y < ch; ++y) {
            memcpy(dst_v + (size_t)y * cw, f->data[2] + (size_t)y * (size_t)f->linesize[2], cw);
        }

        out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_I420;
        out_frame->width = w;
        out_frame->height = h;
        out_frame->planes[0] = ctx->frame_buf;
        out_frame->strides[0] = w;
        out_frame->planes[1] = dst_u;
        out_frame->strides[1] = cw;
        out_frame->planes[2] = dst_v;
        out_frame->strides[2] = cw;
        out_frame->planes[3] = NULL;
        out_frame->strides[3] = 0;
        return AVX_VIDEO_CODEC_OK;
    }

    if (fmt >= 0 && fmt == g_avcodec.fmt_nv12) {
        uint32_t ch = (h + 1U) / 2U;
        if (!f->data[0] || !f->data[1] || f->linesize[0] < (int)w || f->linesize[1] < (int)w) {
            return AVX_VIDEO_CODEC_ERROR;
        }
        uint64_t needed = (uint64_t)w * h + (uint64_t)w * ch;
        if (avcodec_ensure_frame_buf(ctx, needed) != 0) {
            return AVX_VIDEO_CODEC_ERROR;
        }

        for (uint32_t y = 0; y < h; ++y) {
            memcpy(ctx->frame_buf + (size_t)y * w, f->data[0] + (size_t)y * (size_t)f->linesize[0], w);
        }
        uint8_t *dst_uv = ctx->frame_buf + (size_t)w * h;
        for (uint32_t y = 0; y < ch; ++y) {
            memcpy(dst_uv + (size_t)y * w, f->data[1] + (size_t)y * (size_t)f->linesize[1], w);
        }

        out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_NV12;
        out_frame->width = w;
        out_frame->height = h;
        out_frame->planes[0] = ctx->frame_buf;
        out_frame->strides[0] = w;
        out_frame->planes[1] = dst_uv;
        out_frame->strides[1] = w;
        out_frame->planes[2] = NULL;
        out_frame->strides[2] = 0;
        out_frame->planes[3] = NULL;
        out_frame->strides[3] = 0;
        return AVX_VIDEO_CODEC_OK;
    }

    if (fmt >= 0 && (fmt == g_avcodec.fmt_yuv422p || fmt == g_avcodec.fmt_yuvj422p)) {
        uint32_t cw = (w + 1U) / 2U;
        if (!f->data[0] || !f->data[1] || !f->data[2] || f->linesize[0] < (int)w || f->linesize[1] < (int)cw ||
            f->linesize[2] < (int)cw) {
            return AVX_VIDEO_CODEC_ERROR;
        }
        uint64_t needed = (uint64_t)w * h * 2ULL;
        if (avcodec_ensure_frame_buf(ctx, needed) != 0) {
            return AVX_VIDEO_CODEC_ERROR;
        }

        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t *sy = f->data[0] + (size_t)y * (size_t)f->linesize[0];
            const uint8_t *su = f->data[1] + (size_t)y * (size_t)f->linesize[1];
            const uint8_t *sv = f->data[2] + (size_t)y * (size_t)f->linesize[2];
            uint8_t *dst = ctx->frame_buf + (size_t)y * (size_t)w * 2ULL;
            uint32_t x = 0;
            for (; x + 1U < w; x += 2U) {
                uint32_t cx = x >> 1;
                dst[x * 2U + 0] = su[cx];
                dst[x * 2U + 1] = sy[x];
                dst[x * 2U + 2] = sv[cx];
                dst[x * 2U + 3] = sy[x + 1U];
            }
            if (x < w) {
                uint32_t cx = x >> 1;
                dst[x * 2U + 0] = su[cx];
                dst[x * 2U + 1] = sy[x];
            }
        }

        out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY;
        out_frame->width = w;
        out_frame->height = h;
        out_frame->planes[0] = ctx->frame_buf;
        out_frame->strides[0] = (size_t)w * 2ULL;
        out_frame->planes[1] = NULL;
        out_frame->strides[1] = 0;
        out_frame->planes[2] = NULL;
        out_frame->strides[2] = 0;
        out_frame->planes[3] = NULL;
        out_frame->strides[3] = 0;
        return AVX_VIDEO_CODEC_OK;
    }

    if (fmt >= 0 && (fmt == g_avcodec.fmt_yuv420p10le || fmt == g_avcodec.fmt_yuv422p10le)) {
        bool is_420 = (fmt == g_avcodec.fmt_yuv420p10le);
        uint32_t cw = (w + 1U) / 2U;
        if (!f->data[0] || !f->data[1] || !f->data[2] || f->linesize[0] < (int)(w * 2U) ||
            f->linesize[1] < (int)(cw * 2U) || f->linesize[2] < (int)(cw * 2U)) {
            return AVX_VIDEO_CODEC_ERROR;
        }
        uint64_t needed = (uint64_t)w * (uint64_t)h * 4ULL;
        if (avcodec_ensure_frame_buf(ctx, needed) != 0) {
            return AVX_VIDEO_CODEC_ERROR;
        }

        uint32_t half_w = w / 2U;
        for (uint32_t y = 0; y < h; ++y) {
            const uint16_t *sy = (const uint16_t *)(const void *)(f->data[0] + (size_t)y * (size_t)f->linesize[0]);
            uint16_t *dy = (uint16_t *)(void *)(ctx->frame_buf + (size_t)y * (size_t)w * 2ULL);
            for (uint32_t x = 0; x < w; ++x) {
                dy[x] = (uint16_t)(sy[x] << 6);
            }

            uint32_t cy = is_420 ? (y >> 1) : y;
            const uint16_t *su = (const uint16_t *)(const void *)(f->data[1] + (size_t)cy * (size_t)f->linesize[1]);
            const uint16_t *sv = (const uint16_t *)(const void *)(f->data[2] + (size_t)cy * (size_t)f->linesize[2]);
            uint16_t *duv = (uint16_t *)(void *)(ctx->frame_buf + (size_t)w * h * 2ULL + (size_t)y * (size_t)w * 2ULL);
            for (uint32_t cx = 0; cx < half_w; ++cx) {
                duv[2U * cx + 0U] = (uint16_t)(su[cx] << 6);
                duv[2U * cx + 1U] = (uint16_t)(sv[cx] << 6);
            }
            if ((w & 1U) != 0U && half_w < cw) {
                duv[2U * half_w] = (uint16_t)(su[half_w] << 6);
            }
        }

        out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_P216;
        out_frame->width = w;
        out_frame->height = h;
        out_frame->planes[0] = ctx->frame_buf;
        out_frame->strides[0] = (size_t)w * 2ULL;
        out_frame->planes[1] = ctx->frame_buf + (size_t)w * h * 2ULL;
        out_frame->strides[1] = (size_t)w * 2ULL;
        out_frame->planes[2] = NULL;
        out_frame->strides[2] = 0;
        out_frame->planes[3] = NULL;
        out_frame->strides[3] = 0;
        return AVX_VIDEO_CODEC_OK;
    }

    return AVX_VIDEO_CODEC_ERROR;
}

static bool avx_pkt_has_slice_nal(uint32_t fourcc, const uint8_t *data, size_t size) {
    if (!data || size < 4) return false;

    if (fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
        size_t pos = 0;
        if (size >= 4 && data[0] == 0 && data[1] == 0) {
            if (data[2] == 1)
                pos = 3;
            else if (data[2] == 0 && data[3] == 1)
                pos = 4;
        }
        while (pos < size) {
            uint8_t hdr = data[pos++];
            int obu_type = (hdr >> 3) & 0x0F;
            if (obu_type == 3 || obu_type == 4 || obu_type == 6) {
                return true;
            }
            if (hdr & 0x04) { /* extension flag */
                if (pos >= size) break;
                pos++;
            }
            if (hdr & 0x02) { /* has size field (LEB128) */
                size_t val = 0, shift = 0;
                while (pos < size) {
                    uint8_t b = data[pos++];
                    val |= (size_t)(b & 0x7F) << shift;
                    if (!(b & 0x80)) break;
                    shift += 7;
                }
                pos += val;
            } else {
                break;
            }
        }
        return false;
    }

    bool is_hevc = (fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || fourcc == 0x43564568U);
    size_t pos = 0;
    while (pos + 3 <= size) {
        size_t sc_len = 0;
        if (data[pos] == 0 && data[pos + 1] == 0) {
            if (data[pos + 2] == 1)
                sc_len = 3;
            else if (pos + 4 <= size && data[pos + 2] == 0 && data[pos + 3] == 1)
                sc_len = 4;
        }
        if (sc_len > 0) {
            pos += sc_len;
            if (pos >= size) break;
            if (is_hevc) {
                uint8_t nal_type = (data[pos] >> 1) & 0x3F;
                if (nal_type <= 31) return true;
            } else {
                uint8_t nal_type = data[pos] & 0x1F;
                if (nal_type >= 1 && nal_type <= 5) return true;
            }
        } else {
            pos++;
        }
    }
    return false;
}

/* Feeds Annex-B NAL packets into libavcodec and drains completed picture frames. */
static int avcodec_dec_decode(void *backend_ctx, const uint8_t *pkt_data, size_t pkt_size,
                              oavx_video_frame_t *out_frame) {
    avx_avcodec_dec_ctx_t *ctx = (avx_avcodec_dec_ctx_t *)backend_ctx;
    if (!ctx || !pkt_data || pkt_size == 0 || pkt_size > (size_t)INT_MAX || !out_frame) {
        return AVX_VIDEO_CODEC_ERROR;
    }
    if (!ctx->avctx || !ctx->pkt || !ctx->frame) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    g_avcodec.av_packet_unref(ctx->pkt);
    if (g_avcodec.av_new_packet(ctx->pkt, (int)pkt_size) < 0) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    avx_av_packet_stub_t *pkt_stub = (avx_av_packet_stub_t *)ctx->pkt;
    if (!pkt_stub->data || pkt_stub->size < (int)pkt_size) {
        g_avcodec.av_packet_unref(ctx->pkt);
        return AVX_VIDEO_CODEC_ERROR;
    }
    memcpy(pkt_stub->data, pkt_data, pkt_size);

    int send_rc = g_avcodec.avcodec_send_packet(ctx->avctx, ctx->pkt);
    if (send_rc == -EAGAIN) {
        /* Input queue full: drain one decoded frame first, then retry send */
        int drain_rc = g_avcodec.avcodec_receive_frame(ctx->avctx, ctx->frame);
        if (drain_rc == 0) {
            int copy_rc = avcodec_copy_out_frame(ctx, out_frame);
            g_avcodec.av_frame_unref(ctx->frame);
            if (copy_rc != AVX_VIDEO_CODEC_OK) {
                g_avcodec.av_packet_unref(ctx->pkt);
                return copy_rc;
            }
            (void)g_avcodec.avcodec_send_packet(ctx->avctx, ctx->pkt);
            g_avcodec.av_packet_unref(ctx->pkt);
            return AVX_VIDEO_CODEC_OK;
        }
    }

    g_avcodec.av_packet_unref(ctx->pkt);
    bool has_slice = avx_pkt_has_slice_nal(ctx->src_fourcc, pkt_data, pkt_size);
    if (send_rc < 0 && send_rc != -EAGAIN) {
        if (!has_slice) {
            /* Parameter/priming packet consumed by decoder without slice/frame */
            return AVX_VIDEO_CODEC_NEED_MORE_DATA;
        }
        return AVX_VIDEO_CODEC_ERROR;
    }

    int recv_rc = g_avcodec.avcodec_receive_frame(ctx->avctx, ctx->frame);
    if (recv_rc == -EAGAIN) {
        return AVX_VIDEO_CODEC_NEED_MORE_DATA;
    }
    if (recv_rc < 0) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    int copy_rc = avcodec_copy_out_frame(ctx, out_frame);
    g_avcodec.av_frame_unref(ctx->frame);
    return copy_rc;
}

static void avcodec_dec_reset(void *backend_ctx) {
    avx_avcodec_dec_ctx_t *ctx = (avx_avcodec_dec_ctx_t *)backend_ctx;
    if (!ctx) return;

    if (ctx->avctx && g_avcodec.avcodec_flush_buffers) {
        g_avcodec.avcodec_flush_buffers(ctx->avctx);
    }
    if (ctx->pkt && g_avcodec.av_packet_unref) {
        g_avcodec.av_packet_unref(ctx->pkt);
    }
    if (ctx->frame && g_avcodec.av_frame_unref) {
        g_avcodec.av_frame_unref(ctx->frame);
    }
}

static const avx_video_decoder_ops_t s_avcodec_decoder_ops = {.name = "libavcodec-dlopen",
                                                              .priority = 50,
                                                              .is_hardware = false,
                                                              .supports_fourcc = avcodec_dec_supports_fourcc,
                                                              .create = avcodec_dec_create,
                                                              .decode = avcodec_dec_decode,
                                                              .reset = avcodec_dec_reset,
                                                              .destroy = avcodec_dec_destroy};

const avx_video_decoder_ops_t *avx_video_avcodec_get_ops(void) {
    return &s_avcodec_decoder_ops;
}

/* ========================================================================= */
/* Video Encoder Backend Implementation                                      */
/* ========================================================================= */

typedef struct avx_avcodec_enc_ctx_t {
    void *avctx;
    void *pkt;
    void *frame;
    uint32_t target_fourcc;
    uint32_t width;
    uint32_t height;
    int32_t fps_num;
    int32_t fps_den;
    uint32_t bitrate_kbps;
    uint8_t quality;
    int64_t pts_counter;
    bool force_idr_next;
    char encoder_name[64];
    bool is_hardware;
    int enc_pix_fmt;
    uint8_t *tmp_buf;
    size_t tmp_cap;
} avx_avcodec_enc_ctx_t;

static const char *k_h264_enc_candidates[] = {"h264_nvenc", "h264_qsv", "h264_amf", "libx264", NULL};

static const char *k_hevc_enc_candidates[] = {"hevc_nvenc", "hevc_qsv", "hevc_amf", "libx265", NULL};

static const char *k_av1_enc_candidates[] = {"av1_nvenc", "av1_qsv", "av1_amf", "libsvtav1", "libaom-av1", NULL};

static bool avcodec_enc_has_support_locked(void) {
    return (g_avcodec.available && g_avcodec.avcodec_find_encoder && g_avcodec.avcodec_send_frame &&
            g_avcodec.avcodec_receive_packet && g_avcodec.av_frame_get_buffer && g_avcodec.fmt_yuv420p >= 0);
}

static bool avcodec_enc_supports_fourcc(uint32_t target_fourcc) {
    if (target_fourcc != OAVX_FOURCC_VIDEO_TYPE_H264 && target_fourcc != OAVX_FOURCC_VIDEO_TYPE_HEVC &&
        target_fourcc != OAVX_FOURCC_VIDEO_TYPE_AV01) {
        return false;
    }
    if (!probe_avcodec_library()) {
        return false;
    }
    if (!avcodec_enc_has_support_locked()) {
        return false;
    }
    const char **cands = NULL;
    int fallback_id = 0;
    if (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) {
        cands = k_h264_enc_candidates;
        fallback_id = AVX_AV_CODEC_ID_H264;
    } else if (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC) {
        cands = k_hevc_enc_candidates;
        fallback_id = AVX_AV_CODEC_ID_HEVC;
    } else if (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
        cands = k_av1_enc_candidates;
        fallback_id = AVX_AV_CODEC_ID_AV1;
    }
    if (cands && g_avcodec.avcodec_find_encoder_by_name) {
        for (int i = 0; cands[i]; ++i) {
            if (g_avcodec.avcodec_find_encoder_by_name(cands[i])) {
                return true;
            }
        }
    }
    return (fallback_id != 0 && g_avcodec.avcodec_find_encoder && g_avcodec.avcodec_find_encoder(fallback_id) != NULL);
}

static void avcodec_enc_destroy(void *backend_ctx) {
    avx_avcodec_enc_ctx_t *ctx = (avx_avcodec_enc_ctx_t *)backend_ctx;
    if (!ctx) return;

    if (ctx->avctx && g_avcodec.avcodec_send_frame) {
        g_avcodec.avcodec_send_frame(ctx->avctx, NULL);
    }
    if (ctx->pkt && g_avcodec.av_packet_free) {
        g_avcodec.av_packet_free(&ctx->pkt);
    }
    if (ctx->frame && g_avcodec.av_frame_free) {
        g_avcodec.av_frame_free(&ctx->frame);
    }
    if (ctx->avctx && g_avcodec.avcodec_free_context) {
        g_avcodec.avcodec_free_context(&ctx->avctx);
    }
    if (ctx->tmp_buf) {
        free(ctx->tmp_buf);
    }
    free(ctx);
}

/* Allocates and tunes FFmpeg hardware/software encoder (NVENC, QSV, x264) for zero-latency streaming. */
static int avcodec_enc_create(const avx_video_enc_cfg_t *cfg, void **backend_ctx) {
    if (!cfg || !backend_ctx || cfg->width == 0 || cfg->height == 0) {
        return AVX_VIDEO_CODEC_ERROR;
    }
    *backend_ctx = NULL;

    if (!probe_avcodec_library() || !avcodec_enc_has_support_locked()) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    const char **cands = NULL;
    int fallback_codec_id = 0;
    if (cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) {
        cands = k_h264_enc_candidates;
        fallback_codec_id = AVX_AV_CODEC_ID_H264;
    } else if (cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC) {
        cands = k_hevc_enc_candidates;
        fallback_codec_id = AVX_AV_CODEC_ID_HEVC;
    } else if (cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
        cands = k_av1_enc_candidates;
        fallback_codec_id = AVX_AV_CODEC_ID_AV1;
    } else {
        return AVX_VIDEO_CODEC_ERROR;
    }

    void *enc = NULL;
    void *avctx = NULL;
    char chosen_name[64] = "unknown";
    bool is_hw = false;

    int fps_num = cfg->fps_num > 0 ? cfg->fps_num : 30;
    int fps_den = cfg->fps_den > 0 ? cfg->fps_den : 1;
    char tb_str[64], fr_str[64], size_str[64];
    snprintf(tb_str, sizeof(tb_str), "%d/%d", fps_den, fps_num);
    snprintf(fr_str, sizeof(fr_str), "%d/%d", fps_num, fps_den);
    snprintf(size_str, sizeof(size_str), "%ux%u", (unsigned)cfg->width, (unsigned)cfg->height);

    uint32_t br_kbps = cfg->bitrate_kbps;
    if (br_kbps == 0) {
        uint64_t pixels = (uint64_t)cfg->width * cfg->height;
        if (pixels <= 1280 * 720) {
            br_kbps = 4000;
        } else if (pixels <= 1920 * 1080) {
            br_kbps = 8000;
        } else {
            br_kbps = 20000;
        }
        if (cfg->quality > 0) {
            br_kbps = (uint32_t)((uint64_t)br_kbps * cfg->quality / 50ULL);
            if (br_kbps < 500) br_kbps = 500;
        }
    }
    uint32_t gop = cfg->keyframe_interval > 0 ? cfg->keyframe_interval : 60;

    bool want_10bit =
        ((cfg->src_fourcc == OAVX_FOURCC_VIDEO_TYPE_P216 || cfg->src_fourcc == OAVX_FOURCC_VIDEO_TYPE_PA16) &&
         (cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) &&
         g_avcodec.fmt_yuv420p10le >= 0);
    int chosen_pix_fmt = want_10bit ? g_avcodec.fmt_yuv420p10le : g_avcodec.fmt_yuv420p;

    for (int pass = 0; pass < (want_10bit ? 2 : 1) && !avctx; ++pass) {
        int try_pix_fmt = (pass == 0) ? chosen_pix_fmt : g_avcodec.fmt_yuv420p;
        int try_fallback_id = fallback_codec_id;
        int cand_idx = 0;
        while (1) {
            const char *cand_name = (cands && cands[cand_idx]) ? cands[cand_idx] : NULL;
            if (cand_name) {
                cand_idx++;
                if (!g_avcodec.avcodec_find_encoder_by_name) continue;
                enc = g_avcodec.avcodec_find_encoder_by_name(cand_name);
                if (!enc) continue;
                snprintf(chosen_name, sizeof(chosen_name), "%s", cand_name);
            } else if (try_fallback_id != 0 && g_avcodec.avcodec_find_encoder) {
                enc = g_avcodec.avcodec_find_encoder(try_fallback_id);
                try_fallback_id = 0;
                if (!enc) break;
                snprintf(chosen_name, sizeof(chosen_name), "default");
            } else {
                break;
            }

            avctx = g_avcodec.avcodec_alloc_context3(enc);
            if (!avctx) continue;

            g_avcodec.av_opt_set(avctx, "video_size", size_str, 0);
            g_avcodec.av_opt_set_int(avctx, "pixel_format", try_pix_fmt, 0);
            g_avcodec.av_opt_set(avctx, "time_base", tb_str, 0);
            g_avcodec.av_opt_set(avctx, "framerate", fr_str, 0);
            g_avcodec.av_opt_set_int(avctx, "b", (int64_t)br_kbps * 1000LL, 0);
            g_avcodec.av_opt_set_int(avctx, "g", gop, 0);
            g_avcodec.av_opt_set_int(avctx, "bf", 0, 0);

            if (strstr(chosen_name, "nvenc")) {
                is_hw = true;
                g_avcodec.av_opt_set(avctx, "preset", "p1", 1);
                g_avcodec.av_opt_set(avctx, "tune", "ull", 1);
                g_avcodec.av_opt_set_int(avctx, "zerolatency", 1, 1);
                g_avcodec.av_opt_set_int(avctx, "delay", 0, 1);
            } else if (strstr(chosen_name, "qsv")) {
                is_hw = true;
                g_avcodec.av_opt_set(avctx, "preset", "veryfast", 1);
                g_avcodec.av_opt_set_int(avctx, "async_depth", 1, 1);
            } else if (strstr(chosen_name, "amf")) {
                is_hw = true;
                g_avcodec.av_opt_set(avctx, "usage", "lowlatency", 1);
                g_avcodec.av_opt_set(avctx, "quality", "speed", 1);
            } else {
                is_hw = false;
                g_avcodec.av_opt_set(avctx, "preset", "ultrafast", 1);
                g_avcodec.av_opt_set(avctx, "tune", "zerolatency", 1);
            }
            g_avcodec.av_opt_set(avctx, "forced-idr", "1", 1);

            int open_rc = g_avcodec.avcodec_open2(avctx, enc, NULL);
            if (open_rc == 0) {
                chosen_pix_fmt = try_pix_fmt;
                oavx_log_write(OAVX_LOG_INFO, "avcodec", "Selected %s %s encoder for %ux%u @ %u kbps (fmt=%d)",
                               is_hw ? "Hardware" : "Software", chosen_name, (unsigned)cfg->width,
                               (unsigned)cfg->height, (unsigned)br_kbps, chosen_pix_fmt);
                break;
            }

            /* If opening this candidate failed (e.g. resolution or 10-bit not supported), try next */
            g_avcodec.avcodec_free_context(&avctx);
            avctx = NULL;
            enc = NULL;
        }
    }

    if (!avctx || !enc) {
        oavx_log_write(OAVX_LOG_ERROR, "avcodec", "Failed to initialize any encoder candidate for FourCC %08X",
                       cfg->fourcc);
        return AVX_VIDEO_CODEC_ERROR;
    }

    avx_avcodec_enc_ctx_t *ctx = (avx_avcodec_enc_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        g_avcodec.avcodec_free_context(&avctx);
        return AVX_VIDEO_CODEC_ERROR;
    }

    ctx->avctx = avctx;
    ctx->target_fourcc = cfg->fourcc;
    ctx->width = cfg->width;
    ctx->height = cfg->height;
    ctx->fps_num = fps_num;
    ctx->fps_den = fps_den;
    ctx->bitrate_kbps = cfg->bitrate_kbps;
    ctx->quality = cfg->quality;
    ctx->pts_counter = 0;
    ctx->force_idr_next = true;
    snprintf(ctx->encoder_name, sizeof(ctx->encoder_name), "%s (%s)", chosen_name, is_hw ? "Hardware" : "Software");
    ctx->is_hardware = is_hw;
    ctx->enc_pix_fmt = chosen_pix_fmt;

    ctx->frame = g_avcodec.av_frame_alloc();
    if (!ctx->frame) {
        avcodec_enc_destroy(ctx);
        return AVX_VIDEO_CODEC_ERROR;
    }

    avx_av_frame_stub_t *f = (avx_av_frame_stub_t *)ctx->frame;
    f->width = (int)cfg->width;
    f->height = (int)cfg->height;
    f->format = ctx->enc_pix_fmt;

    int buf_rc = g_avcodec.av_frame_get_buffer(ctx->frame, 32);
    if (buf_rc < 0) {
        avcodec_enc_destroy(ctx);
        return AVX_VIDEO_CODEC_ERROR;
    }

    ctx->pkt = g_avcodec.av_packet_alloc();
    if (!ctx->pkt) {
        avcodec_enc_destroy(ctx);
        return AVX_VIDEO_CODEC_ERROR;
    }

    *backend_ctx = ctx;
    return AVX_VIDEO_CODEC_OK;
}

static int avcodec_enc_prepare_input(avx_avcodec_enc_ctx_t *ctx, const oavx_video_frame_t *in_frame) {
    if (!ctx || !in_frame || !ctx->frame) return -1;
    uint32_t w = ctx->width;
    uint32_t h = ctx->height;
    avx_av_frame_stub_t *f = (avx_av_frame_stub_t *)ctx->frame;

    uint32_t src_fourcc = (uint32_t)in_frame->fourcc;
    if (src_fourcc == 0 || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
        size_t s0 = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w * 2;
        avx_color_uyvy_to_i420(in_frame->planes[0], s0, f->data[0], (size_t)f->linesize[0], f->data[1],
                               (size_t)f->linesize[1], f->data[2], (size_t)f->linesize[2], (int)w, (int)h);
        return 0;
    }

    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA) {
        size_t s0 = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w * 4;
        avx_color_rgba_to_i420(in_frame->planes[0], s0, f->data[0], (size_t)f->linesize[0], f->data[1],
                               (size_t)f->linesize[1], f->data[2], (size_t)f->linesize[2], (int)w, (int)h);
        return 0;
    }

    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA) {
        size_t s0 = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w * 4;
        if (s0 < (size_t)w * 4) return -1;
        size_t needed = (size_t)w * (size_t)h * 2;
        if (ctx->tmp_cap < needed) {
            uint8_t *nb = (uint8_t *)realloc(ctx->tmp_buf, needed);
            if (!nb) return -1;
            ctx->tmp_buf = nb;
            ctx->tmp_cap = needed;
        }
        if (s0 == (size_t)w * 4) {
            avx_color_bgra_to_uyvy(in_frame->planes[0], ctx->tmp_buf, (int)w, (int)h);
        } else {
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t *src_row = in_frame->planes[0] + (size_t)y * s0;
                uint8_t *dst_row = ctx->tmp_buf + (size_t)y * (size_t)w * 2;
                avx_color_bgra_to_uyvy(src_row, dst_row, (int)w, 1);
            }
        }
        avx_color_uyvy_to_i420(ctx->tmp_buf, (size_t)w * 2, f->data[0], (size_t)f->linesize[0], f->data[1],
                               (size_t)f->linesize[1], f->data[2], (size_t)f->linesize[2], (int)w, (int)h);
        return 0;
    }

    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_P216 || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_PA16) {
        if (f->format == g_avcodec.fmt_yuv420p10le && g_avcodec.fmt_yuv420p10le >= 0) {
            size_t ys_bytes = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w * 2;
            size_t uvs_bytes = in_frame->strides[1] ? in_frame->strides[1] : ys_bytes;
            const uint8_t *src_y_base = in_frame->planes[0];
            const uint8_t *src_uv_base =
                in_frame->planes[1] ? in_frame->planes[1] : (src_y_base + ys_bytes * (size_t)h);

            for (uint32_t r = 0; r < h; ++r) {
                const uint16_t *sy = (const uint16_t *)(src_y_base + r * ys_bytes);
                uint16_t *dy = (uint16_t *)(f->data[0] + r * (size_t)f->linesize[0]);
                for (uint32_t c = 0; c < w; ++c) {
                    dy[c] = (uint16_t)(sy[c] >> 6);
                }
            }
            for (uint32_t r = 0; r < h / 2; ++r) {
                const uint16_t *suv0 = (const uint16_t *)(src_uv_base + (2 * r) * uvs_bytes);
                const uint16_t *suv1 = (const uint16_t *)(src_uv_base + (2 * r + 1) * uvs_bytes);
                uint16_t *du = (uint16_t *)(f->data[1] + r * (size_t)f->linesize[1]);
                uint16_t *dv = (uint16_t *)(f->data[2] + r * (size_t)f->linesize[2]);
                for (uint32_t c = 0; c < w / 2; ++c) {
                    uint32_t u = (((uint32_t)suv0[c * 2] + (uint32_t)suv1[c * 2] + 1U) >> 1) >> 6;
                    uint32_t v = (((uint32_t)suv0[c * 2 + 1] + (uint32_t)suv1[c * 2 + 1] + 1U) >> 1) >> 6;
                    du[c] = (uint16_t)u;
                    dv[c] = (uint16_t)v;
                }
            }
            return 0;
        }
        size_t needed = (size_t)w * (size_t)h * 2;
        if (ctx->tmp_cap < needed) {
            uint8_t *nb = (uint8_t *)realloc(ctx->tmp_buf, needed);
            if (!nb) return -1;
            ctx->tmp_buf = nb;
            ctx->tmp_cap = needed;
        }
        avx_color_p216_to_uyvy(in_frame->planes[0], ctx->tmp_buf, (int)w, (int)h);
        avx_color_uyvy_to_i420(ctx->tmp_buf, (size_t)w * 2, f->data[0], (size_t)f->linesize[0], f->data[1],
                               (size_t)f->linesize[1], f->data[2], (size_t)f->linesize[2], (int)w, (int)h);
        return 0;
    }

    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
        size_t ys = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w;
        size_t uvs = in_frame->strides[1] ? in_frame->strides[1] : (size_t)w;
        const uint8_t *src_y = in_frame->planes[0];
        const uint8_t *src_uv = in_frame->planes[1];
        if (!src_uv) src_uv = src_y + ys * (size_t)h;

        for (uint32_t r = 0; r < h; ++r) {
            memcpy(f->data[0] + r * (size_t)f->linesize[0], src_y + r * ys, w);
        }
        for (uint32_t r = 0; r < h / 2; ++r) {
            const uint8_t *row_uv = src_uv + r * uvs;
            uint8_t *dst_u = f->data[1] + r * (size_t)f->linesize[1];
            uint8_t *dst_v = f->data[2] + r * (size_t)f->linesize[2];
            for (uint32_t c = 0; c < w / 2; ++c) {
                dst_u[c] = row_uv[c * 2 + 0];
                dst_v[c] = row_uv[c * 2 + 1];
            }
        }
        return 0;
    }

    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_I420) {
        size_t ys = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w;
        size_t us = in_frame->strides[1] ? in_frame->strides[1] : ((size_t)w / 2);
        size_t vs = in_frame->strides[2] ? in_frame->strides[2] : ((size_t)w / 2);
        const uint8_t *src_y = in_frame->planes[0];
        const uint8_t *src_u = in_frame->planes[1];
        const uint8_t *src_v = in_frame->planes[2];
        if (!src_u) src_u = src_y + ys * (size_t)h;
        if (!src_v) src_v = src_u + us * ((size_t)h / 2);

        for (uint32_t r = 0; r < h; ++r) {
            memcpy(f->data[0] + r * (size_t)f->linesize[0], src_y + r * ys, w);
        }
        for (uint32_t r = 0; r < h / 2; ++r) {
            memcpy(f->data[1] + r * (size_t)f->linesize[1], src_u + r * us, w / 2);
            memcpy(f->data[2] + r * (size_t)f->linesize[2], src_v + r * vs, w / 2);
        }
        return 0;
    }

    return -1;
}

/* Submits raw frame to libavcodec encoder and drains Annex-B NAL stream packet. */
static int avcodec_enc_encode(void *backend_ctx, const oavx_video_frame_t *in_frame, uint8_t *out_buf, size_t out_cap,
                              size_t *out_size, bool *out_is_keyframe) {
    avx_avcodec_enc_ctx_t *ctx = (avx_avcodec_enc_ctx_t *)backend_ctx;
    if (!ctx || !in_frame || !out_buf || !out_size || !out_is_keyframe) {
        return AVX_VIDEO_CODEC_ERROR;
    }
    *out_size = 0;
    *out_is_keyframe = false;

    if (in_frame->width != ctx->width || in_frame->height != ctx->height) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    if (avcodec_enc_prepare_input(ctx, in_frame) != 0) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    avx_av_frame_stub_t *f = (avx_av_frame_stub_t *)ctx->frame;
    f->pts = ctx->pts_counter++;
    if (ctx->force_idr_next) {
        f->key_frame = 1;
        f->pict_type = 1; /* AV_PICTURE_TYPE_I */
        ctx->force_idr_next = false;
    } else {
        f->key_frame = 0;
        f->pict_type = 0; /* AV_PICTURE_TYPE_NONE */
    }

    int send_rc = g_avcodec.avcodec_send_frame(ctx->avctx, ctx->frame);
    if (send_rc < 0) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    g_avcodec.av_packet_unref(ctx->pkt);
    int recv_rc = g_avcodec.avcodec_receive_packet(ctx->avctx, ctx->pkt);
    if (recv_rc == 0) {
        avx_av_packet_stub_t *pkt = (avx_av_packet_stub_t *)ctx->pkt;
        if (pkt->size < 0 || (size_t)pkt->size > out_cap) {
            g_avcodec.av_packet_unref(ctx->pkt);
            return AVX_VIDEO_CODEC_ERROR;
        }
        memcpy(out_buf, pkt->data, (size_t)pkt->size);
        *out_size = (size_t)pkt->size;
        *out_is_keyframe = ((pkt->flags & 1) != 0);
        g_avcodec.av_packet_unref(ctx->pkt);
        return AVX_VIDEO_CODEC_OK;
    } else if (recv_rc == -11 || recv_rc == -EAGAIN) {
        *out_size = 0;
        *out_is_keyframe = false;
        return AVX_VIDEO_CODEC_OK;
    }

    return AVX_VIDEO_CODEC_ERROR;
}

static int avcodec_enc_set_quality(void *backend_ctx, uint8_t quality) {
    avx_avcodec_enc_ctx_t *ctx = (avx_avcodec_enc_ctx_t *)backend_ctx;
    if (!ctx) return AVX_VIDEO_CODEC_ERROR;
    ctx->quality = quality;
    if (ctx->bitrate_kbps == 0 && quality > 0) {
        uint64_t pixels = (uint64_t)ctx->width * ctx->height;
        uint32_t base_kbps = (pixels <= 1280 * 720) ? 4000 : (pixels <= 1920 * 1080 ? 8000 : 20000);
        uint32_t br_kbps = (uint32_t)((uint64_t)base_kbps * quality / 50ULL);
        if (br_kbps < 500) br_kbps = 500;
        if (g_avcodec.av_opt_set_int) {
            g_avcodec.av_opt_set_int(ctx->avctx, "b", (int64_t)br_kbps * 1000LL, 0);
        }
    }
    return AVX_VIDEO_CODEC_OK;
}

static void avcodec_enc_request_keyframe(void *backend_ctx) {
    avx_avcodec_enc_ctx_t *ctx = (avx_avcodec_enc_ctx_t *)backend_ctx;
    if (ctx) {
        ctx->force_idr_next = true;
    }
}

static const char *avcodec_enc_get_name(void *backend_ctx) {
    avx_avcodec_enc_ctx_t *ctx = (avx_avcodec_enc_ctx_t *)backend_ctx;
    return (ctx && ctx->encoder_name[0]) ? ctx->encoder_name : "libavcodec-dlopen";
}

static const avx_video_encoder_ops_t s_avcodec_encoder_ops = {.name = "libavcodec-dlopen",
                                                              .priority = 85,
                                                              .is_hardware = true,
                                                              .supports_fourcc = avcodec_enc_supports_fourcc,
                                                              .create = avcodec_enc_create,
                                                              .encode = avcodec_enc_encode,
                                                              .set_quality = avcodec_enc_set_quality,
                                                              .request_keyframe = avcodec_enc_request_keyframe,
                                                              .get_name = avcodec_enc_get_name,
                                                              .destroy = avcodec_enc_destroy};

const avx_video_encoder_ops_t *avx_video_avcodec_get_encoder_ops(void) {
    return &s_avcodec_encoder_ops;
}

#else

const avx_video_decoder_ops_t *avx_video_avcodec_get_ops(void) {
    return NULL;
}

const avx_video_encoder_ops_t *avx_video_avcodec_get_encoder_ops(void) {
    return NULL;
}

#endif /* AVX_OS_LINUX || AVX_OS_WINDOWS */
