/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_speedhq_test.c - SpeedHQ Intra-DCT verification and benchmark
 *
 * External FFmpeg interoperability is exercised through a local static test
 * helper that dlopen()s libavcodec at runtime.  libopenavx.so exports no
 * SpeedHQ avcodec wrappers; this file is the only consumer of that path.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <stdint.h>
#include <stdbool.h>

#include "avx_platform.h"
#include "avx_speedhq.h"

/* =========================================================================
 * Local test-only helper: probe libavcodec for SpeedHQ encoder/decoder.
 * None of this logic is exported by libopenavx.so.
 * ========================================================================= */

/* Minimal struct stubs matching libavcodec binary layout */
typedef struct {
    const void *av_class;
    int log_level_offset;
    int codec_type;
    const void *codec;
    int codec_id;
    unsigned int codec_tag;
    void *priv_data;
    void *internal;
    void *opaque;
    int64_t bit_rate;
    int flags;
    int flags2;
    uint8_t *extradata;
    int extradata_size;
    int time_base_num;
    int time_base_den;
    int pkt_timebase_num;
    int pkt_timebase_den;
    int framerate_num;
    int framerate_den;
    int delay;
    int width;
    int height;
    int coded_width;
    int coded_height;
    int sample_aspect_ratio_num;
    int sample_aspect_ratio_den;
    int pix_fmt;
} TestCodecCtxStub;

typedef struct {
    uint8_t *data[8];
    int linesize[8];
    uint8_t **extended_data;
    int width;
    int height;
    int nb_samples;
    int format;
} TestFrameStub;

typedef struct {
    void *buf;
    int64_t pts;
    int64_t dts;
    uint8_t *data;
    int size;
} TestPktStub;

typedef void *(*fn_find_enc)(const char *);
typedef void *(*fn_find_dec)(const char *);
typedef void *(*fn_alloc_ctx)(const void *);
typedef void (*fn_free_ctx)(void **);
typedef int (*fn_open2)(void *, const void *, void **);
typedef void *(*fn_pkt_alloc)(void);
typedef void (*fn_pkt_free)(void **);
typedef void *(*fn_frm_alloc)(void);
typedef void (*fn_frm_free)(void **);
typedef int (*fn_frm_getbuf)(void *, int);
typedef int (*fn_send_frame)(void *, const void *);
typedef int (*fn_recv_pkt)(void *, void *);
typedef int (*fn_send_pkt)(void *, const void *);
typedef int (*fn_recv_frame)(void *, void *);
typedef int (*fn_opt_set)(void *, const char *, const char *, int);
typedef int (*fn_opt_set_int)(void *, const char *, int64_t, int);

static struct {
    void *h_codec;
    void *h_util;
    bool ready;
    const void *shq_encoder;
    const void *shq_decoder;
    fn_find_enc find_encoder;
    fn_find_dec find_decoder;
    fn_alloc_ctx alloc_ctx;
    fn_free_ctx free_ctx;
    fn_open2 open2;
    fn_pkt_alloc pkt_alloc;
    fn_pkt_free pkt_free;
    fn_frm_alloc frm_alloc;
    fn_frm_free frm_free;
    fn_frm_getbuf frm_getbuf;
    fn_send_frame send_frame;
    fn_recv_pkt recv_pkt;
    fn_send_pkt send_pkt;
    fn_recv_frame recv_frame;
    fn_opt_set opt_set;
    fn_opt_set_int opt_set_int;
} s_ffshq = {0};

static bool test_ffmpeg_shq_available(void) {
#ifdef _WIN32
    return false;
#else
    if (s_ffshq.ready) return true;

    static const char *s_codec_libs[] = {"libavcodec.so.62",
                                         "libavcodec.so.61",
                                         "libavcodec.so.60",
                                         "libavcodec.so.59",
                                         "libavcodec.so.58",
                                         "libavcodec.so",
                                         NULL};
    static const char *s_util_libs[] = {"libavutil.so.60",
                                        "libavutil.so.59",
                                        "libavutil.so.58",
                                        "libavutil.so.57",
                                        "libavutil.so.56",
                                        "libavutil.so",
                                        NULL};

    for (int i = 0; s_util_libs[i]; ++i) {
        s_ffshq.h_util = dlopen(s_util_libs[i], RTLD_LAZY | RTLD_GLOBAL);
        if (s_ffshq.h_util) break;
    }
    for (int i = 0; s_codec_libs[i]; ++i) {
        s_ffshq.h_codec = dlopen(s_codec_libs[i], RTLD_LAZY | RTLD_GLOBAL);
        if (s_ffshq.h_codec) break;
    }
    if (!s_ffshq.h_codec) return false;

    void *util = s_ffshq.h_util ? s_ffshq.h_util : s_ffshq.h_codec;

#define SYM(h, t, name) ((t)dlsym((h), (name)))
    s_ffshq.find_encoder = SYM(s_ffshq.h_codec, fn_find_enc, "avcodec_find_encoder_by_name");
    s_ffshq.find_decoder = SYM(s_ffshq.h_codec, fn_find_dec, "avcodec_find_decoder_by_name");
    s_ffshq.alloc_ctx = SYM(s_ffshq.h_codec, fn_alloc_ctx, "avcodec_alloc_context3");
    s_ffshq.free_ctx = SYM(s_ffshq.h_codec, fn_free_ctx, "avcodec_free_context");
    s_ffshq.open2 = SYM(s_ffshq.h_codec, fn_open2, "avcodec_open2");
    s_ffshq.pkt_alloc = SYM(s_ffshq.h_codec, fn_pkt_alloc, "av_packet_alloc");
    s_ffshq.pkt_free = SYM(s_ffshq.h_codec, fn_pkt_free, "av_packet_free");
    s_ffshq.send_frame = SYM(s_ffshq.h_codec, fn_send_frame, "avcodec_send_frame");
    s_ffshq.recv_pkt = SYM(s_ffshq.h_codec, fn_recv_pkt, "avcodec_receive_packet");
    s_ffshq.send_pkt = SYM(s_ffshq.h_codec, fn_send_pkt, "avcodec_send_packet");
    s_ffshq.recv_frame = SYM(s_ffshq.h_codec, fn_recv_frame, "avcodec_receive_frame");
    s_ffshq.frm_alloc = SYM(util, fn_frm_alloc, "av_frame_alloc");
    s_ffshq.frm_free = SYM(util, fn_frm_free, "av_frame_free");
    s_ffshq.frm_getbuf = SYM(util, fn_frm_getbuf, "av_frame_get_buffer");
    s_ffshq.opt_set = SYM(util, fn_opt_set, "av_opt_set");
    s_ffshq.opt_set_int = SYM(util, fn_opt_set_int, "av_opt_set_int");
#undef SYM

    if (!s_ffshq.find_encoder || !s_ffshq.find_decoder || !s_ffshq.alloc_ctx || !s_ffshq.free_ctx || !s_ffshq.open2 ||
        !s_ffshq.pkt_alloc || !s_ffshq.pkt_free || !s_ffshq.send_frame || !s_ffshq.recv_pkt || !s_ffshq.send_pkt ||
        !s_ffshq.recv_frame || !s_ffshq.frm_alloc || !s_ffshq.frm_free || !s_ffshq.frm_getbuf)
        return false;

    s_ffshq.shq_encoder = s_ffshq.find_encoder("speedhq");
    s_ffshq.shq_decoder = s_ffshq.find_decoder("speedhq");

    if (!s_ffshq.shq_encoder || !s_ffshq.shq_decoder) return false;

    s_ffshq.ready = true;
    return true;
#endif
}

/*
 * Encode packed UYVY to SpeedHQ-2 (SHQ2) via libavcodec.
 * Returns 0 on success, -1 on failure.
 */
static int test_ffmpeg_encode_shq(int width, int height, const uint8_t *uyvy, uint8_t *out, size_t out_max,
                                  size_t *out_size) {
    if (!s_ffshq.ready) return -1;

    void *ctx = s_ffshq.alloc_ctx(s_ffshq.shq_encoder);
    if (!ctx) return -1;

    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%dx%d", width, height);
    if (s_ffshq.opt_set) {
        s_ffshq.opt_set(ctx, "video_size", size_str, 0);
        s_ffshq.opt_set(ctx, "time_base", "1/60", 0);
        s_ffshq.opt_set(ctx, "framerate", "60/1", 0);
    }
    if (s_ffshq.opt_set_int) {
        s_ffshq.opt_set_int(ctx, "pixel_format", 4, 0);
        s_ffshq.opt_set_int(ctx, "codec_tag", 0x32514853, 0);
    }

    if (s_ffshq.open2(ctx, s_ffshq.shq_encoder, NULL) < 0) {
        s_ffshq.free_ctx(&ctx);
        return -1;
    }

    void *frame = s_ffshq.frm_alloc();
    if (!frame) {
        s_ffshq.free_ctx(&ctx);
        return -1;
    }

    TestFrameStub *f = (TestFrameStub *)frame;
    f->width = width;
    f->height = height;
    f->format = 4; /* YUV422P */
    if (s_ffshq.frm_getbuf(frame, 32) < 0) {
        s_ffshq.frm_free(&frame);
        s_ffshq.free_ctx(&ctx);
        return -1;
    }

    /* Convert packed UYVY → planar YUV422P */
    for (int y = 0; y < height; ++y) {
        uint8_t *y_line = f->data[0] + (size_t)y * f->linesize[0];
        uint8_t *u_line = f->data[1] + (size_t)y * f->linesize[1];
        uint8_t *v_line = f->data[2] + (size_t)y * f->linesize[2];
        const uint8_t *src = uyvy + (size_t)y * ((size_t)width * 2);
        for (int x = 0; x < width / 2; ++x) {
            u_line[x] = src[4 * x + 0];
            y_line[2 * x + 0] = src[4 * x + 1];
            v_line[x] = src[4 * x + 2];
            y_line[2 * x + 1] = src[4 * x + 3];
        }
    }

    void *pkt = s_ffshq.pkt_alloc();
    int ret = -1;
    if (pkt) {
        if (s_ffshq.send_frame(ctx, frame) >= 0 && s_ffshq.recv_pkt(ctx, pkt) >= 0) {
            TestPktStub *p = (TestPktStub *)pkt;
            if ((size_t)p->size <= out_max) {
                memcpy(out, p->data, (size_t)p->size);
                *out_size = (size_t)p->size;
                ret = 0;
            }
        }
        s_ffshq.pkt_free(&pkt);
    }

    s_ffshq.frm_free(&frame);
    s_ffshq.free_ctx(&ctx);
    return ret;
}

/*
 * Decode a SpeedHQ-2 bitstream to packed UYVY via libavcodec.
 * Returns 0 on success, -1 on failure.
 */
static int test_ffmpeg_decode_shq(int width, int height, const uint8_t *shq, size_t shq_size, uint8_t *uyvy_out) {
    if (!s_ffshq.ready) return -1;

    void *ctx = s_ffshq.alloc_ctx(s_ffshq.shq_decoder);
    if (!ctx) return -1;

    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%dx%d", width, height);
    if (s_ffshq.opt_set) {
        s_ffshq.opt_set(ctx, "video_size", size_str, 0);
    }
    if (s_ffshq.opt_set_int) {
        s_ffshq.opt_set_int(ctx, "codec_tag", 0x32514853, 0);
    }

    if (s_ffshq.open2(ctx, s_ffshq.shq_decoder, NULL) < 0) {
        s_ffshq.free_ctx(&ctx);
        return -1;
    }

    void *pkt = s_ffshq.pkt_alloc();
    if (!pkt) {
        s_ffshq.free_ctx(&ctx);
        return -1;
    }

    TestPktStub *p = (TestPktStub *)pkt;
    p->data = (uint8_t *)shq;
    p->size = (int)shq_size;

    int ret = -1;
    if (s_ffshq.send_pkt(ctx, pkt) >= 0) {
        void *frame = s_ffshq.frm_alloc();
        if (frame && s_ffshq.recv_frame(ctx, frame) >= 0) {
            TestFrameStub *f = (TestFrameStub *)frame;
            int fw = f->width ? f->width : width;
            int fh = f->height ? f->height : height;
            /* SpeedHQ decodes to YUV422P (format 4) */
            for (int y = 0; y < fh; ++y) {
                uint8_t *dst = uyvy_out + (size_t)y * ((size_t)fw * 2);
                const uint8_t *yl = f->data[0] + (size_t)y * f->linesize[0];
                const uint8_t *ul = f->data[1] + (size_t)y * f->linesize[1];
                const uint8_t *vl = f->data[2] + (size_t)y * f->linesize[2];
                for (int x = 0; x < fw / 2; ++x) {
                    dst[4 * x + 0] = ul[x];
                    dst[4 * x + 1] = yl[2 * x + 0];
                    dst[4 * x + 2] = vl[x];
                    dst[4 * x + 3] = yl[2 * x + 1];
                }
            }
            ret = 0;
        }
        if (frame) s_ffshq.frm_free(&frame);
    }

    p->data = NULL;
    p->size = 0;
    s_ffshq.pkt_free(&pkt);
    s_ffshq.free_ctx(&ctx);
    return ret;
}

static void test_ffmpeg_destroy(void) {
#ifndef _WIN32
    if (s_ffshq.h_codec) {
        dlclose(s_ffshq.h_codec);
        s_ffshq.h_codec = NULL;
    }
    if (s_ffshq.h_util) {
        dlclose(s_ffshq.h_util);
        s_ffshq.h_util = NULL;
    }
#endif
    s_ffshq.ready = false;
}

static double get_time_ms(void) {
    return (double)avx_time_ns() / 1000000.0;
}

static void generate_test_uyvy(int width, int height, uint8_t *buf) {
    for (int y = 0; y < height; ++y) {
        uint8_t *line = buf + (size_t)y * ((size_t)width * 2);
        for (int x = 0; x < width / 2; ++x) {
            uint8_t u = (uint8_t)((x * 255) / (width / 2));
            uint8_t y0 = (uint8_t)((y * 255) / height);
            uint8_t v = (uint8_t)(128);
            uint8_t y1 = (uint8_t)(((y + 1) * 255) / (height + 1));

            line[4 * x + 0] = u;
            line[4 * x + 1] = y0;
            line[4 * x + 2] = v;
            line[4 * x + 3] = y1;
        }
    }
}

static void generate_checkerboard_uyvy(int width, int height, uint8_t *buf) {
    for (int y = 0; y < height; ++y) {
        uint8_t *line = buf + (size_t)y * ((size_t)width * 2);
        for (int x = 0; x < width / 2; ++x) {
            int check0 = (((x * 2) / 8) + (y / 8)) & 1;
            int check1 = ((((x * 2) + 1) / 8) + (y / 8)) & 1;
            line[4 * x + 0] = 128;
            line[4 * x + 1] = check0 ? 235 : 16;
            line[4 * x + 2] = 128;
            line[4 * x + 3] = check1 ? 235 : 16;
        }
    }
}

static void compute_uyvy_psnr(int width, int height, const uint8_t *ref, const uint8_t *test, double *out_y_psnr,
                              double *out_uv_psnr, int *out_max_diff) {
    double y_mse = 0.0;
    double uv_mse = 0.0;
    int max_d = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width / 2; ++x) {
            size_t idx = ((size_t)y * (width / 2) + x) * 4;
            int du = abs((int)ref[idx + 0] - (int)test[idx + 0]);
            int dy0 = abs((int)ref[idx + 1] - (int)test[idx + 1]);
            int dv = abs((int)ref[idx + 2] - (int)test[idx + 2]);
            int dy1 = abs((int)ref[idx + 3] - (int)test[idx + 3]);

            if (du > max_d) max_d = du;
            if (dy0 > max_d) max_d = dy0;
            if (dv > max_d) max_d = dv;
            if (dy1 > max_d) max_d = dy1;

            y_mse += (double)(dy0 * dy0 + dy1 * dy1);
            uv_mse += (double)(du * du + dv * dv);
        }
    }
    y_mse /= (double)(width * height);
    uv_mse /= (double)(width * height);
    *out_y_psnr = (y_mse > 0.0) ? (10.0 * log10(255.0 * 255.0 / y_mse)) : 99.0;
    *out_uv_psnr = (uv_mse > 0.0) ? (10.0 * log10(255.0 * 255.0 / uv_mse)) : 99.0;
    *out_max_diff = max_d;
}

typedef struct {
    int width;
    int height;
    const uint8_t *src_uyvy;
    const uint8_t *ref_shq;
    size_t ref_size;
} ShqMtStressArg;

static void *shq_mt_stress_worker(void *arg) {
    ShqMtStressArg *a = (ShqMtStressArg *)arg;
    size_t raw_size = (size_t)a->width * (size_t)a->height * 2;
    uint8_t *enc = (uint8_t *)malloc(raw_size);
    uint8_t *dec = (uint8_t *)malloc(raw_size);
    assert(enc && dec);

    for (int iter = 0; iter < 6; ++iter) {
        size_t esize = 0;
        int r = avx_speedhq_encode(a->width, a->height, a->src_uyvy, enc, raw_size, &esize);
        assert(r == 0 && esize == a->ref_size);
        assert(memcmp(enc, a->ref_shq, esize) == 0);

        memset(dec, 0, raw_size);
        r = avx_speedhq_decode(a->width, a->height, enc, esize, dec);
        assert(r == 0);
        if (iter == 0) {
            double yp = 0, uvp = 0;
            int md = 0;
            compute_uyvy_psnr(a->width, a->height, a->src_uyvy, dec, &yp, &uvp, &md);
            assert(yp >= 35.0 && uvp >= 35.0 && md <= 10);
        }
    }

    free(enc);
    free(dec);
    return NULL;
}

int main(void) {
    printf("\n=== openavx SpeedHQ-2 Intra-DCT Codec Verification ===\n");

    const int width = 1920;
    const int height = 1080;
    const size_t raw_size = (size_t)width * height * 2; /* 4.14 MB for 1080p UYVY */

    uint8_t *src_uyvy = malloc(raw_size);
    uint8_t *dst_uyvy = malloc(raw_size);
    size_t max_shq_size = raw_size;
    uint8_t *shq_buf = malloc(max_shq_size);

    assert(src_uyvy && dst_uyvy && shq_buf);
    generate_test_uyvy(width, height, src_uyvy);

    /* 1. Initialize Codec */
    printf("[*] 1. Initializing Native Standalone SpeedHQ subsystem...\n");
    bool init_ok = avx_speedhq_init();
    assert(init_ok);

    bool has_simd = avx_speedhq_has_acceleration();
    printf("    -> Native Hardware SIMD Acceleration: %s\n",
           has_simd ? "ENABLED (AVX2/NEON active)" : "DISABLED (Pure C99 mode)");

    /* 2. Standalone Native SpeedHQ Encode & Decode Test */
    printf("[*] 2. Testing Standalone Native SpeedHQ Encoder & Decoder (1080p)...\n");
    size_t native_size = 0;
    double t0 = get_time_ms();
    int ret = avx_speedhq_encode(width, height, src_uyvy, shq_buf, max_shq_size, &native_size);
    double t1 = get_time_ms();

    assert(ret == 0);
    assert(native_size > 64);
    printf("    [+] Native Encoded 1080p frame: %zu bytes (compression ratio: %.1f:1) in %.2f ms\n", native_size,
           (double)raw_size / native_size, t1 - t0);

    /* Verify header */
    assert(shq_buf[0] == 96);                                      /* Quality byte */
    assert(shq_buf[1] == 4 && shq_buf[2] == 0 && shq_buf[3] == 0); /* Progressive offset 4 */

    memset(dst_uyvy, 0, raw_size);
    double t2 = get_time_ms();
    ret = avx_speedhq_decode(width, height, shq_buf, native_size, dst_uyvy);
    double t3 = get_time_ms();

    assert(ret == 0);
    printf("    [+] Native Decoded 1080p frame in %.2f ms\n", t3 - t2);

    /* Verify Reconstruction Quality (PSNR / MSE) */
    double y_psnr = 0, uv_psnr = 0;
    int max_diff = 0;
    compute_uyvy_psnr(width, height, src_uyvy, dst_uyvy, &y_psnr, &uv_psnr, &max_diff);
    printf("    [+] Native Quality: Y-PSNR=%.2f dB, UV-PSNR=%.2f dB (max diff=%d)\n", y_psnr, uv_psnr, max_diff);
    assert(y_psnr >= 35.0);
    assert(uv_psnr >= 35.0);
    assert(max_diff <= 10);

    /* 3. Probe External Reference Codec (libavcodec SpeedHQ) for Interoperability Verification */
    bool has_ffmpeg = test_ffmpeg_shq_available();
    printf("[*] 3. Probing External Reference Codec (libavcodec SpeedHQ)...\n");
    printf("    -> External FFmpeg SpeedHQ: %s\n",
           has_ffmpeg ? "AVAILABLE (running interoperability checks)" : "NOT AVAILABLE (skipping reference tests)");

    if (has_ffmpeg) {
        size_t ffmpeg_size = 0;
        double ta0 = get_time_ms();
        ret = test_ffmpeg_encode_shq(width, height, src_uyvy, shq_buf, max_shq_size, &ffmpeg_size);
        double ta1 = get_time_ms();
        assert(ret == 0);
        printf("    [+] External FFmpeg Encoded 1080p frame: %zu bytes in %.2f ms\n", ffmpeg_size, ta1 - ta0);

        /* 4. Cross-Compatibility: Decode FFmpeg bitstream with Native OpenAVX decoder */
        printf("[*] 4. Testing Cross-Compatibility (FFmpeg Bitstream -> Native OpenAVX Decoder)...\n");
        memset(dst_uyvy, 0, raw_size);
        ret = avx_speedhq_decode(width, height, shq_buf, ffmpeg_size, dst_uyvy);
        assert(ret == 0);
        compute_uyvy_psnr(width, height, src_uyvy, dst_uyvy, &y_psnr, &uv_psnr, &max_diff);
        printf("    [+] OpenAVX parsed FFmpeg bitstream: Y-PSNR=%.2f dB, UV-PSNR=%.2f dB (max diff=%d)\n", y_psnr,
               uv_psnr, max_diff);
        assert(y_psnr >= 35.0);
        assert(uv_psnr >= 35.0);
        assert(max_diff <= 10);

        /* 5. Reverse Cross-Compatibility: Decode Native OpenAVX bitstream with FFmpeg decoder */
        printf("[*] 5. Testing Reverse Cross-Compatibility (Native OpenAVX Bitstream -> FFmpeg Decoder)...\n");
        memset(dst_uyvy, 0, raw_size);
        ret = test_ffmpeg_decode_shq(width, height, shq_buf, native_size, dst_uyvy);
        assert(ret == 0);
        compute_uyvy_psnr(width, height, src_uyvy, dst_uyvy, &y_psnr, &uv_psnr, &max_diff);
        printf("    [+] FFmpeg parsed OpenAVX bitstream: Y-PSNR=%.2f dB, UV-PSNR=%.2f dB (max diff=%d)\n", y_psnr,
               uv_psnr, max_diff);
        assert(y_psnr >= 35.0);
        assert(uv_psnr >= 35.0);
        assert(max_diff <= 10);
    }

    /* 6. High-Frequency Checkerboard Test (verifies AC escape codes and DC row reset) */
    printf("[*] 6. Testing High-Frequency Checkerboard Pattern (Escape & DC Reset Verification)...\n");
    generate_checkerboard_uyvy(width, height, src_uyvy);
    size_t check_size = 0;
    ret = avx_speedhq_encode(width, height, src_uyvy, shq_buf, max_shq_size, &check_size);
    assert(ret == 0);

    memset(dst_uyvy, 0, raw_size);
    ret = avx_speedhq_decode(width, height, shq_buf, check_size, dst_uyvy);
    assert(ret == 0);
    compute_uyvy_psnr(width, height, src_uyvy, dst_uyvy, &y_psnr, &uv_psnr, &max_diff);
    printf("    [+] Checkerboard Native OpenAVX: Y-PSNR=%.2f dB, UV-PSNR=%.2f dB (max diff=%d)\n", y_psnr, uv_psnr,
           max_diff);
    assert(y_psnr >= 30.0);
    assert(uv_psnr >= 35.0);

    if (has_ffmpeg) {
        memset(dst_uyvy, 0, raw_size);
        ret = test_ffmpeg_decode_shq(width, height, shq_buf, check_size, dst_uyvy);
        assert(ret == 0);
        compute_uyvy_psnr(width, height, src_uyvy, dst_uyvy, &y_psnr, &uv_psnr, &max_diff);
        printf(
            "    [+] Checkerboard OpenAVX Bitstream -> FFmpeg Decoder: Y-PSNR=%.2f dB, UV-PSNR=%.2f dB (max diff=%d)\n",
            y_psnr, uv_psnr, max_diff);
        assert(y_psnr >= 30.0);
        assert(uv_psnr >= 35.0);
    }

    /* 7. Single-thread vs Multi-slice Byte-Exact Determinism, Concurrent Stress & Re-Init */
    printf("[*] 7. Testing 4-Slice Determinism, Concurrent Stress & Re-Init...\n");
    generate_test_uyvy(width, height, src_uyvy);
    size_t st_size = 0;
    uint8_t *st_shq = malloc(max_shq_size);
    uint8_t *st_dec = malloc(raw_size);
    assert(st_shq && st_dec);

    setenv("OPENAVX_SPEEDHQ_THREADS", "1", 1);
    avx_speedhq_destroy();
    assert(avx_speedhq_encode(width, height, src_uyvy, st_shq, max_shq_size, &st_size) == 0);
    assert(avx_speedhq_decode(width, height, st_shq, st_size, st_dec) == 0);

    setenv("OPENAVX_SPEEDHQ_THREADS", "4", 1);
    avx_speedhq_destroy();
    size_t mt_size = 0;
    assert(avx_speedhq_encode(width, height, src_uyvy, shq_buf, max_shq_size, &mt_size) == 0);
    assert(mt_size == st_size);
    assert(memcmp(shq_buf, st_shq, mt_size) == 0);

    memset(dst_uyvy, 0, raw_size);
    assert(avx_speedhq_decode(width, height, shq_buf, mt_size, dst_uyvy) == 0);
    assert(memcmp(dst_uyvy, st_dec, raw_size) == 0);
    unsetenv("OPENAVX_SPEEDHQ_THREADS");
    printf("    [+] 1T vs 4T bitstreams & decoded UYVY are 100%% byte-identical (%zu bytes)\n", mt_size);

    /* Concurrent multi-thread stress (> scratch slot count to cover pool + fallback) */
    ShqMtStressArg mt_arg = {width, height, src_uyvy, st_shq, st_size};
    avx_thread_t workers[6];
    for (int w = 0; w < 6; ++w) {
        assert(avx_thread_create(&workers[w], shq_mt_stress_worker, &mt_arg) == 0);
    }
    for (int w = 0; w < 6; ++w) {
        avx_thread_join(workers[w]);
    }
    printf("    [+] 6 concurrent threads x 6 frames (1080p encode+decode) passed cleanly!\n");

    avx_speedhq_destroy();
    /* Verify codec works again seamlessly if re-used after destroy() */
    size_t reinit_size = 0;
    assert(avx_speedhq_encode(width, height, src_uyvy, shq_buf, max_shq_size, &reinit_size) == 0);
    assert(reinit_size == st_size && memcmp(shq_buf, st_shq, reinit_size) == 0);
    free(st_shq);
    free(st_dec);

    avx_speedhq_destroy();
    if (has_ffmpeg) {
        test_ffmpeg_destroy();
    }
    free(src_uyvy);
    free(dst_uyvy);
    free(shq_buf);

    printf("\n[PASS] openavx_speedhq_test\n\n");
    return 0;
}
