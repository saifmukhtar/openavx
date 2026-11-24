/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_video_codec.c - Video encoder/decoder registry & built-in SpeedHQ backends
 */

#include "avx_video_codec.h"
#include "avx_internal.h"
#include "avx_speedhq.h"
#include "avx_color.h"
#include <stdlib.h>
#include <string.h>

#define AVX_MAX_VIDEO_BACKENDS 16

/* ========================================================================= */
/* Internal Instance & Registry State                                        */
/* ========================================================================= */

struct avx_video_encoder_t {
    const avx_video_encoder_ops_t *ops;
    void                          *backend_ctx;
    avx_video_enc_cfg_t            cfg;
};

struct avx_video_decoder_t {
    const avx_video_decoder_ops_t *ops;
    void                          *backend_ctx;
    avx_video_dec_cfg_t            cfg;
    uint8_t                       *annexb_buf;
    size_t                         annexb_cap;
};

static avx_mutex_t s_vcodec_lock = AVX_MUTEX_INITIALIZER;
static bool        s_vcodec_initialized = false;

static const avx_video_encoder_ops_t *s_enc_backends[AVX_MAX_VIDEO_BACKENDS];
static int                            s_enc_count = 0;

static const avx_video_decoder_ops_t *s_dec_backends[AVX_MAX_VIDEO_BACKENDS];
static int                            s_dec_count = 0;

/* ========================================================================= */
/* Built-In SpeedHQ Encoder Backend                                          */
/* ========================================================================= */

typedef struct {
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint8_t  quality;
    uint8_t *uyvy_tmp;
    size_t   uyvy_tmp_cap;
    uint8_t *alpha_tmp;
    size_t   alpha_tmp_cap;
} avx_shq_enc_ctx_t;

static bool shq_enc_supports_fourcc(uint32_t target_fourcc) {
    return avx_is_speedhq_fourcc(target_fourcc);
}

static bool shq_fourcc_has_alpha(uint32_t fourcc) {
    return (fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ1 ||
            fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ3 ||
            fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ5 ||
            fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ7 ||
            fourcc == OAVX_FOURCC_VIDEO_TYPE_SHQ9);
}

static uint8_t shq_clamp_quality_or_zero(uint8_t q) {
    if (q == 0) return 0;
    if (q < 20) return 20;
    if (q > 98) return 98;
    return q;
}

static int shq_ensure_tmp(uint8_t **buf, size_t *cap, size_t needed) {
    if (*cap >= needed && *buf) return 0;
    uint8_t *nb = (uint8_t *)realloc(*buf, needed);
    if (!nb) return -1;
    *buf = nb;
    *cap = needed;
    return 0;
}

/*
 * Adapts source frame format (RGBA/BGRA/planar) to 4:2:2 UYVY and extracts
 * an optional alpha plane as required by the destination SpeedHQ subtype.
 */
static int shq_prepare_uyvy_input(avx_shq_enc_ctx_t *ctx,
                                  const oavx_video_frame_t *in_frame,
                                  uint32_t w, uint32_t h,
                                  const uint8_t **out_uyvy, size_t *out_uyvy_stride,
                                  const uint8_t **out_alpha, size_t *out_alpha_stride)
{
    uint32_t src_fourcc = (uint32_t)in_frame->fourcc;
    bool want_alpha = shq_fourcc_has_alpha(ctx->fourcc);

    if (src_fourcc == 0 || avx_is_speedhq_fourcc(src_fourcc) ||
        src_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY ||
        src_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
        size_t s0 = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w * 2;
        const uint8_t *alpha = in_frame->planes[1];
        if (!alpha && (want_alpha || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA)) {
            alpha = in_frame->planes[0] + s0 * (size_t)h;
        }
        *out_uyvy = in_frame->planes[0];
        *out_uyvy_stride = s0;
        *out_alpha = alpha;
        *out_alpha_stride = in_frame->strides[1] ? in_frame->strides[1] : (size_t)w;
        return 0;
    }

    size_t uyvy_bytes = (size_t)w * (size_t)h * 2;
    if (shq_ensure_tmp(&ctx->uyvy_tmp, &ctx->uyvy_tmp_cap, uyvy_bytes) != 0) {
        return -1;
    }
    *out_uyvy = ctx->uyvy_tmp;
    *out_uyvy_stride = (size_t)w * 2;
    *out_alpha = NULL;
    *out_alpha_stride = 0;

    bool is_rgba = (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX);
    bool is_bgra = (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX);
    if (is_rgba || is_bgra) {
        size_t s0 = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w * 4;
        if (s0 < (size_t)w * 4) return -1;

        if (s0 == (size_t)w * 4) {
            if (is_rgba) avx_color_rgba_to_uyvy(in_frame->planes[0], ctx->uyvy_tmp, (int)w, (int)h);
            else         avx_color_bgra_to_uyvy(in_frame->planes[0], ctx->uyvy_tmp, (int)w, (int)h);
        } else {
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t *src_row = in_frame->planes[0] + (size_t)y * s0;
                uint8_t *dst_row = ctx->uyvy_tmp + (size_t)y * (size_t)w * 2;
                if (is_rgba) avx_color_rgba_to_uyvy(src_row, dst_row, (int)w, 1);
                else         avx_color_bgra_to_uyvy(src_row, dst_row, (int)w, 1);
            }
        }

        if (want_alpha && (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA ||
                           src_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA)) {
            size_t alpha_bytes = (size_t)w * (size_t)h;
            if (shq_ensure_tmp(&ctx->alpha_tmp, &ctx->alpha_tmp_cap, alpha_bytes) != 0) {
                return -1;
            }
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t *src_row = in_frame->planes[0] + (size_t)y * s0;
                uint8_t *dst_a = ctx->alpha_tmp + (size_t)y * (size_t)w;
                for (uint32_t x = 0; x < w; ++x) {
                    dst_a[x] = src_row[x * 4 + 3];
                }
            }
            *out_alpha = ctx->alpha_tmp;
            *out_alpha_stride = (size_t)w;
        }
        return 0;
    }

    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
        size_t s0 = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w;
        size_t s1 = in_frame->strides[1] ? in_frame->strides[1] : (size_t)w;
        const uint8_t *uv = in_frame->planes[1] ? in_frame->planes[1] : (in_frame->planes[0] + s0 * (size_t)h);
        avx_color_nv12_to_uyvy(in_frame->planes[0], s0, uv, s1,
                               ctx->uyvy_tmp, (size_t)w * 2, (int)w, (int)h);
        return 0;
    }

    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_I420 || src_fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
        size_t s0 = in_frame->strides[0] ? in_frame->strides[0] : (size_t)w;
        size_t cw = (size_t)((w + 1) / 2);
        size_t ch = (size_t)((h + 1) / 2);
        size_t s1 = in_frame->strides[1] ? in_frame->strides[1] : cw;
        size_t s2 = in_frame->strides[2] ? in_frame->strides[2] : cw;
        const uint8_t *p1 = in_frame->planes[1] ? in_frame->planes[1] : (in_frame->planes[0] + s0 * (size_t)h);
        const uint8_t *p2 = in_frame->planes[2] ? in_frame->planes[2] : (p1 + s1 * ch);
        if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_I420) {
            avx_color_i420_to_uyvy(in_frame->planes[0], s0, p1, s1, p2, s2,
                                   ctx->uyvy_tmp, (size_t)w * 2, (int)w, (int)h);
        } else {
            avx_color_yv12_to_uyvy(in_frame->planes[0], s0, p1, s1, p2, s2,
                                   ctx->uyvy_tmp, (size_t)w * 2, (int)w, (int)h);
        }
        return 0;
    }

    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
        avx_color_p216_to_uyvy(in_frame->planes[0], ctx->uyvy_tmp, (int)w, (int)h);
        return 0;
    }

    return -1;
}

static int shq_enc_create(const avx_video_enc_cfg_t *cfg, void **backend_ctx) {
    if (!cfg || !backend_ctx || !avx_is_speedhq_fourcc(cfg->fourcc)) return -1;
    if (cfg->width == 0 || cfg->height == 0 ||
        cfg->width > AVX_MAX_VIDEO_WIDTH || cfg->height > AVX_MAX_VIDEO_HEIGHT) {
        return -1;
    }

    avx_speedhq_init();

    avx_shq_enc_ctx_t *ctx = (avx_shq_enc_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    ctx->fourcc = cfg->fourcc;
    ctx->width = cfg->width;
    ctx->height = cfg->height;
    ctx->quality = shq_clamp_quality_or_zero(cfg->quality);
    *backend_ctx = ctx;
    return 0;
}

/* Compresses raw UYVY/alpha frame into SpeedHQ bitstream slices with quality quantization. */
static int shq_enc_encode(void *backend_ctx,
                          const oavx_video_frame_t *in_frame,
                          uint8_t *out_buf, size_t out_cap,
                          size_t *out_size, bool *out_is_keyframe) {
    avx_shq_enc_ctx_t *ctx = (avx_shq_enc_ctx_t *)backend_ctx;
    if (!ctx || !in_frame || !in_frame->planes[0] || !out_buf || !out_size) return -1;

    uint32_t w = in_frame->width ? in_frame->width : ctx->width;
    uint32_t h = in_frame->height ? in_frame->height : ctx->height;
    if (w == 0 || h == 0 || w > AVX_MAX_VIDEO_WIDTH || h > AVX_MAX_VIDEO_HEIGHT) return -1;

    const uint8_t *uyvy_ptr = NULL;
    const uint8_t *alpha_ptr = NULL;
    size_t uyvy_stride = 0;
    size_t alpha_stride = 0;
    if (shq_prepare_uyvy_input(ctx, in_frame, w, h,
                               &uyvy_ptr, &uyvy_stride,
                               &alpha_ptr, &alpha_stride) != 0) {
        return -1;
    }

    int rc = avx_speedhq_encode_strided_ex(ctx->fourcc, (int)w, (int)h,
                                           uyvy_ptr, uyvy_stride,
                                           alpha_ptr, alpha_stride,
                                           ctx->quality,
                                           out_buf, out_cap, out_size);
    if (rc == 0 && out_is_keyframe) {
        *out_is_keyframe = true; /* SpeedHQ is intra-frame only */
    }
    return rc;
}

static int shq_enc_set_quality(void *backend_ctx, uint8_t quality) {
    avx_shq_enc_ctx_t *ctx = (avx_shq_enc_ctx_t *)backend_ctx;
    if (!ctx) return -1;
    ctx->quality = shq_clamp_quality_or_zero(quality);
    return 0;
}

static void shq_enc_request_keyframe(void *backend_ctx) {
    (void)backend_ctx; /* Intra-frame codec: every frame is already a keyframe */
}

static void shq_enc_destroy(void *backend_ctx) {
    avx_shq_enc_ctx_t *ctx = (avx_shq_enc_ctx_t *)backend_ctx;
    if (!ctx) return;
    free(ctx->uyvy_tmp);
    free(ctx->alpha_tmp);
    free(ctx);
}

static const avx_video_encoder_ops_t s_shq_encoder_ops = {
    .name = "speedhq-cpu",
    .priority = 50,
    .is_hardware = false,
    .supports_fourcc = shq_enc_supports_fourcc,
    .create = shq_enc_create,
    .encode = shq_enc_encode,
    .set_quality = shq_enc_set_quality,
    .request_keyframe = shq_enc_request_keyframe,
    .destroy = shq_enc_destroy
};

/* ========================================================================= */
/* Built-In SpeedHQ Decoder Backend                                          */
/* ========================================================================= */

typedef struct {
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint8_t *buf;
    size_t   buf_size;
} avx_shq_dec_ctx_t;

static bool shq_dec_supports_fourcc(uint32_t src_fourcc) {
    return avx_is_speedhq_fourcc(src_fourcc);
}

static int shq_dec_create(const avx_video_dec_cfg_t *cfg, void **backend_ctx) {
    if (!cfg || !backend_ctx || !avx_is_speedhq_fourcc(cfg->fourcc)) return -1;
    if (cfg->width == 0 || cfg->height == 0 ||
        cfg->width > AVX_MAX_VIDEO_WIDTH || cfg->height > AVX_MAX_VIDEO_HEIGHT) {
        return -1;
    }

    avx_speedhq_init();

    avx_shq_dec_ctx_t *ctx = (avx_shq_dec_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    ctx->fourcc = cfg->fourcc;
    ctx->width = cfg->width;
    ctx->height = cfg->height;
    *backend_ctx = ctx;
    return 0;
}

/* Decompresses SpeedHQ bitstream slices into raw UYVY/alpha output frame. */
static int shq_dec_decode(void *backend_ctx,
                          const uint8_t *pkt_data, size_t pkt_size,
                          oavx_video_frame_t *out_frame) {
    avx_shq_dec_ctx_t *ctx = (avx_shq_dec_ctx_t *)backend_ctx;
    if (!ctx || !pkt_data || pkt_size == 0 || !out_frame) return -1;

    uint64_t needed_u64 = (uint64_t)ctx->width * (uint64_t)ctx->height * 3ULL;
    if (ctx->width == 0 || ctx->height == 0 ||
        ctx->width > AVX_MAX_VIDEO_WIDTH || ctx->height > AVX_MAX_VIDEO_HEIGHT ||
        needed_u64 > AVX_MAX_DECOMPRESSED_FRAME_SIZE) {
        return -1;
    }

    size_t needed = (size_t)needed_u64;
    if (ctx->buf_size < needed) {
        uint8_t *nb = (uint8_t *)realloc(ctx->buf, needed);
        if (!nb) return -1;
        ctx->buf = nb;
        ctx->buf_size = needed;
    }

    bool has_alpha = false;
    uint8_t *alpha_plane = ctx->buf + (size_t)ctx->width * (size_t)ctx->height * 2;
    if (avx_speedhq_decode_ex(ctx->fourcc, (int)ctx->width, (int)ctx->height,
                              pkt_data, pkt_size,
                              ctx->buf, alpha_plane, &has_alpha) != 0) {
        return -1;
    }

    out_frame->width = ctx->width;
    out_frame->height = ctx->height;
    out_frame->fourcc = has_alpha ? OAVX_FOURCC_VIDEO_TYPE_UYVA : OAVX_FOURCC_VIDEO_TYPE_UYVY;
    out_frame->planes[0] = ctx->buf;
    out_frame->strides[0] = (size_t)ctx->width * 2;
    if (has_alpha) {
        out_frame->planes[1] = alpha_plane;
        out_frame->strides[1] = (size_t)ctx->width;
    } else {
        out_frame->planes[1] = NULL;
        out_frame->strides[1] = 0;
    }
    out_frame->planes[2] = NULL;
    out_frame->strides[2] = 0;
    out_frame->planes[3] = NULL;
    out_frame->strides[3] = 0;
    return 0;
}

static void shq_dec_reset(void *backend_ctx) {
    (void)backend_ctx; /* Stateless between intra frames */
}

static void shq_dec_destroy(void *backend_ctx) {
    avx_shq_dec_ctx_t *ctx = (avx_shq_dec_ctx_t *)backend_ctx;
    if (!ctx) return;
    free(ctx->buf);
    free(ctx);
}

static const avx_video_decoder_ops_t s_shq_decoder_ops = {
    .name = "speedhq-cpu",
    .priority = 50,
    .is_hardware = false,
    .supports_fourcc = shq_dec_supports_fourcc,
    .create = shq_dec_create,
    .decode = shq_dec_decode,
    .reset = shq_dec_reset,
    .destroy = shq_dec_destroy
};

/* ========================================================================= */
/* Registry & Selection Helpers                                              */
/* ========================================================================= */

const avx_video_decoder_ops_t *avx_video_v4l2_get_ops(void);
const avx_video_decoder_ops_t *avx_video_avcodec_get_ops(void);
const avx_video_decoder_ops_t *avx_video_mf_get_ops(void);
const avx_video_decoder_ops_t *avx_video_vt_get_ops(void);
const avx_video_encoder_ops_t *avx_video_vt_get_encoder_ops(void);
const avx_video_encoder_ops_t *avx_video_avcodec_get_encoder_ops(void);

static void register_dec_backend_locked(const avx_video_decoder_ops_t *ops) {
    if (!ops || s_dec_count >= AVX_MAX_VIDEO_BACKENDS) return;
    for (int i = 0; i < s_dec_count; ++i) {
        if (s_dec_backends[i] == ops) return;
    }
    s_dec_backends[s_dec_count++] = ops;
}

static void register_enc_backend_locked(const avx_video_encoder_ops_t *ops) {
    if (!ops || s_enc_count >= AVX_MAX_VIDEO_BACKENDS) return;
    for (int i = 0; i < s_enc_count; ++i) {
        if (s_enc_backends[i] == ops) return;
    }
    s_enc_backends[s_enc_count++] = ops;
}

static void avx_video_codec_register_builtins_locked(void) {
    if (s_vcodec_initialized) return;

    avx_speedhq_init();

    register_enc_backend_locked(&s_shq_encoder_ops);
    register_enc_backend_locked(avx_video_vt_get_encoder_ops());
    register_enc_backend_locked(avx_video_avcodec_get_encoder_ops());

    register_dec_backend_locked(&s_shq_decoder_ops);
    register_dec_backend_locked(avx_video_v4l2_get_ops());
    register_dec_backend_locked(avx_video_avcodec_get_ops());
    register_dec_backend_locked(avx_video_mf_get_ops());
    register_dec_backend_locked(avx_video_vt_get_ops());

    s_vcodec_initialized = true;
}

bool avx_video_codec_init(void) {
    avx_mutex_lock(&s_vcodec_lock);
    avx_video_codec_register_builtins_locked();
    avx_mutex_unlock(&s_vcodec_lock);
    return true;
}

void avx_video_codec_destroy(void) {
    avx_mutex_lock(&s_vcodec_lock);
    s_enc_count = 0;
    s_dec_count = 0;
    s_vcodec_initialized = false;
    avx_mutex_unlock(&s_vcodec_lock);

    avx_speedhq_destroy();
}

bool avx_video_encoder_register(const avx_video_encoder_ops_t *ops) {
    if (!ops || !ops->supports_fourcc || !ops->create || !ops->encode || !ops->destroy) {
        return false;
    }
    avx_mutex_lock(&s_vcodec_lock);
    avx_video_codec_register_builtins_locked();
    for (int i = 0; i < s_enc_count; ++i) {
        if (s_enc_backends[i] == ops) {
            avx_mutex_unlock(&s_vcodec_lock);
            return true;
        }
    }
    if (s_enc_count >= AVX_MAX_VIDEO_BACKENDS) {
        avx_mutex_unlock(&s_vcodec_lock);
        return false;
    }
    s_enc_backends[s_enc_count++] = ops;
    avx_mutex_unlock(&s_vcodec_lock);
    return true;
}

bool avx_video_decoder_register(const avx_video_decoder_ops_t *ops) {
    if (!ops || !ops->supports_fourcc || !ops->create || !ops->decode || !ops->destroy) {
        return false;
    }
    avx_mutex_lock(&s_vcodec_lock);
    avx_video_codec_register_builtins_locked();
    for (int i = 0; i < s_dec_count; ++i) {
        if (s_dec_backends[i] == ops) {
            avx_mutex_unlock(&s_vcodec_lock);
            return true;
        }
    }
    if (s_dec_count >= AVX_MAX_VIDEO_BACKENDS) {
        avx_mutex_unlock(&s_vcodec_lock);
        return false;
    }
    s_dec_backends[s_dec_count++] = ops;
    avx_mutex_unlock(&s_vcodec_lock);
    return true;
}

static const avx_video_encoder_ops_t *find_encoder_backend_locked(uint32_t target_fourcc) {
    const avx_video_encoder_ops_t *best = NULL;
    for (int i = 0; i < s_enc_count; ++i) {
        const avx_video_encoder_ops_t *ops = s_enc_backends[i];
        if (!ops || !ops->supports_fourcc(target_fourcc)) continue;
        if (!best || ops->priority > best->priority) {
            best = ops;
        }
    }
    return best;
}

static const avx_video_decoder_ops_t *find_decoder_backend_locked(uint32_t src_fourcc) {
    const avx_video_decoder_ops_t *best = NULL;
    for (int i = 0; i < s_dec_count; ++i) {
        const avx_video_decoder_ops_t *ops = s_dec_backends[i];
        if (!ops || !ops->supports_fourcc(src_fourcc)) continue;
        if (!best || ops->priority > best->priority) {
            best = ops;
        }
    }
    return best;
}

bool avx_video_encoder_has_backend(uint32_t target_fourcc) {
    avx_mutex_lock(&s_vcodec_lock);
    avx_video_codec_register_builtins_locked();
    bool ok = (find_encoder_backend_locked(target_fourcc) != NULL);
    avx_mutex_unlock(&s_vcodec_lock);
    return ok;
}

bool avx_video_decoder_has_backend(uint32_t src_fourcc) {
    avx_mutex_lock(&s_vcodec_lock);
    avx_video_codec_register_builtins_locked();
    bool ok = (find_decoder_backend_locked(src_fourcc) != NULL);
    avx_mutex_unlock(&s_vcodec_lock);
    return ok;
}

/* ========================================================================= */
/* Per-Stream Video Encoder API                                              */
/* ========================================================================= */

/* Probes and initializes the highest-priority encoder backend supporting the requested FourCC. */
avx_video_encoder_t *avx_video_encoder_create(const avx_video_enc_cfg_t *cfg) {
    if (!cfg || cfg->fourcc == 0) return NULL;

    avx_mutex_lock(&s_vcodec_lock);
    avx_video_codec_register_builtins_locked();

    const avx_video_encoder_ops_t *cands[AVX_MAX_VIDEO_BACKENDS];
    int n_cands = 0;
    for (int i = 0; i < s_enc_count; ++i) {
        if (s_enc_backends[i] && s_enc_backends[i]->supports_fourcc(cfg->fourcc)) {
            cands[n_cands++] = s_enc_backends[i];
        }
    }
    avx_mutex_unlock(&s_vcodec_lock);

    /* Sort candidate pointers descending by priority without mutating registry */
    for (int i = 0; i < n_cands; ++i) {
        for (int j = i + 1; j < n_cands; ++j) {
            if (cands[j]->priority > cands[i]->priority) {
                const avx_video_encoder_ops_t *tmp = cands[i];
                cands[i] = cands[j];
                cands[j] = tmp;
            }
        }
    }

    for (int i = 0; i < n_cands; ++i) {
        void *backend_ctx = NULL;
        if (cands[i]->create(cfg, &backend_ctx) == 0 && backend_ctx) {
            avx_video_encoder_t *enc = (avx_video_encoder_t *)calloc(1, sizeof(*enc));
            if (!enc) {
                cands[i]->destroy(backend_ctx);
                return NULL;
            }
            enc->ops = cands[i];
            enc->backend_ctx = backend_ctx;
            enc->cfg = *cfg;
            return enc;
        }
    }
    return NULL;
}

void avx_video_encoder_destroy(avx_video_encoder_t *enc) {
    if (!enc) return;
    if (enc->ops && enc->ops->destroy && enc->backend_ctx) {
        enc->ops->destroy(enc->backend_ctx);
    }
    free(enc);
}

int avx_video_encoder_encode(avx_video_encoder_t *enc,
                             const oavx_video_frame_t *in_frame,
                             uint8_t *out_buf, size_t out_cap,
                             size_t *out_size, bool *out_is_keyframe) {
    if (!enc || !enc->ops || !enc->ops->encode) return -1;
    return enc->ops->encode(enc->backend_ctx, in_frame, out_buf, out_cap, out_size, out_is_keyframe);
}

int avx_video_encoder_set_quality(avx_video_encoder_t *enc, uint8_t quality) {
    if (!enc || !enc->ops) return -1;
    enc->cfg.quality = quality;
    if (enc->ops->set_quality) {
        return enc->ops->set_quality(enc->backend_ctx, quality);
    }
    return 0;
}

void avx_video_encoder_request_keyframe(avx_video_encoder_t *enc) {
    if (!enc || !enc->ops || !enc->ops->request_keyframe) return;
    enc->ops->request_keyframe(enc->backend_ctx);
}

uint32_t avx_video_encoder_get_fourcc(const avx_video_encoder_t *enc) {
    return enc ? enc->cfg.fourcc : 0;
}

const char *avx_video_encoder_get_backend_name(const avx_video_encoder_t *enc) {
    if (!enc || !enc->ops) return "none";
    if (enc->ops->get_name && enc->backend_ctx) {
        return enc->ops->get_name(enc->backend_ctx);
    }
    return enc->ops->name ? enc->ops->name : "none";
}

/* ========================================================================= */
/* Per-Stream Video Decoder API                                              */
/* ========================================================================= */

/* Probes and initializes the highest-priority decoder backend supporting the requested FourCC. */
avx_video_decoder_t *avx_video_decoder_create(const avx_video_dec_cfg_t *cfg) {
    if (!cfg || cfg->fourcc == 0) return NULL;

    avx_mutex_lock(&s_vcodec_lock);
    avx_video_codec_register_builtins_locked();

    const avx_video_decoder_ops_t *cands[AVX_MAX_VIDEO_BACKENDS];
    int n_cands = 0;
    for (int i = 0; i < s_dec_count; ++i) {
        if (s_dec_backends[i] && s_dec_backends[i]->supports_fourcc(cfg->fourcc)) {
            cands[n_cands++] = s_dec_backends[i];
        }
    }
    avx_mutex_unlock(&s_vcodec_lock);

    for (int i = 0; i < n_cands; ++i) {
        for (int j = i + 1; j < n_cands; ++j) {
            if (cands[j]->priority > cands[i]->priority) {
                const avx_video_decoder_ops_t *tmp = cands[i];
                cands[i] = cands[j];
                cands[j] = tmp;
            }
        }
    }

    for (int i = 0; i < n_cands; ++i) {
        void *backend_ctx = NULL;
        if (cands[i]->create(cfg, &backend_ctx) == 0 && backend_ctx) {
            avx_video_decoder_t *dec = (avx_video_decoder_t *)calloc(1, sizeof(*dec));
            if (!dec) {
                cands[i]->destroy(backend_ctx);
                return NULL;
            }
            dec->ops = cands[i];
            dec->backend_ctx = backend_ctx;
            dec->cfg = *cfg;
            return dec;
        }
    }
    return NULL;
}

void avx_video_decoder_destroy(avx_video_decoder_t *dec) {
    if (!dec) return;
    if (dec->ops && dec->ops->destroy && dec->backend_ctx) {
        dec->ops->destroy(dec->backend_ctx);
    }
    free(dec->annexb_buf);
    free(dec);
}

static bool is_avcc_bitstream(const uint8_t *data, size_t size, bool is_hevc) {
    if (!data || size < 5) return false;
    /* If already Annex-B with 3-byte or 4-byte start code, not AVCC */
    if (data[0] == 0 && data[1] == 0 &&
        (data[2] == 1 || (data[2] == 0 && size >= 4 && data[3] == 1))) {
        return false;
    }

    size_t pos = 0;
    int nal_count = 0;
    while (pos + 4 <= size) {
        uint32_t nlen = ((uint32_t)data[pos] << 24) |
                        ((uint32_t)data[pos + 1] << 16) |
                        ((uint32_t)data[pos + 2] << 8) |
                        (uint32_t)data[pos + 3];
        if (nlen == 0 || pos + 4 + nlen > size) {
            return false;
        }
        uint8_t nh = data[pos + 4];
        if (nh & 0x80) {
            return false; /* forbidden_zero_bit must be 0 in H.264 and HEVC */
        }
        if (!is_hevc) {
            uint8_t ntype = nh & 0x1F;
            if (ntype == 0 || ntype > 23) return false;
        } else {
            uint8_t ntype = (nh >> 1) & 0x3F;
            if (ntype > 40) return false;
        }
        pos += 4 + nlen;
        nal_count++;
    }
    return (pos == size && nal_count > 0);
}

/* Decodes compressed video packet, converting Annex-B start codes to AVCC/HVCC if required by backend. */
int avx_video_decoder_decode(avx_video_decoder_t *dec,
                             const uint8_t *pkt_data, size_t pkt_size,
                             oavx_video_frame_t *out_frame) {
    if (!dec || !dec->ops || !dec->ops->decode) return -1;
    if (!pkt_data || pkt_size == 0 || !out_frame) return -1;

    const uint8_t *send_data = pkt_data;
    size_t send_size = pkt_size;

    bool is_h264 = (dec->cfg.fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 || dec->cfg.fourcc == 0x34363268U);
    bool is_hevc = (dec->cfg.fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || dec->cfg.fourcc == 0x43564568U);

    if ((is_h264 || is_hevc) && is_avcc_bitstream(pkt_data, pkt_size, is_hevc)) {
        if (dec->annexb_cap < pkt_size) {
            uint8_t *nb = (uint8_t *)realloc(dec->annexb_buf, pkt_size);
            if (nb) {
                dec->annexb_buf = nb;
                dec->annexb_cap = pkt_size;
            }
        }
        if (dec->annexb_buf && dec->annexb_cap >= pkt_size) {
            memcpy(dec->annexb_buf, pkt_data, pkt_size);
            size_t pos = 0;
            while (pos + 4 <= pkt_size) {
                uint32_t nlen = ((uint32_t)dec->annexb_buf[pos] << 24) |
                                ((uint32_t)dec->annexb_buf[pos + 1] << 16) |
                                ((uint32_t)dec->annexb_buf[pos + 2] << 8) |
                                (uint32_t)dec->annexb_buf[pos + 3];
                dec->annexb_buf[pos + 0] = 0x00;
                dec->annexb_buf[pos + 1] = 0x00;
                dec->annexb_buf[pos + 2] = 0x00;
                dec->annexb_buf[pos + 3] = 0x01;
                pos += 4 + nlen;
            }
            send_data = dec->annexb_buf;
            send_size = pkt_size;
        }
    }

    return dec->ops->decode(dec->backend_ctx, send_data, send_size, out_frame);
}

void avx_video_decoder_reset(avx_video_decoder_t *dec) {
    if (!dec || !dec->ops || !dec->ops->reset) return;
    dec->ops->reset(dec->backend_ctx);
}

uint32_t avx_video_decoder_get_fourcc(const avx_video_decoder_t *dec) {
    return dec ? dec->cfg.fourcc : 0;
}

const char *avx_video_decoder_get_backend_name(const avx_video_decoder_t *dec) {
    return (dec && dec->ops && dec->ops->name) ? dec->ops->name : "none";
}
