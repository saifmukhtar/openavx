/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_video_decoder_test.c - Compressed Video Decode, Priming, Color Conversion & Native Bitstream Tests
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openavx.h"
#include "avx_platform.h"
#include "avx_video_codec.h"

#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        abort(); \
    } \
} while (0)

/* Valid 128x128 H.264 Baseline Annex-B bitstream:
 * - Frame 0: SPS (0..26) + PPS (26..35) + IDR slices (35..132)
 * - Frame 1: P-frame slices (132..154)
 */
static const uint8_t k_h264_f0[132] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x0B, 0xD9, 0x02, 0x04, 0x6C,
    0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0F, 0x23, 0xC5,
    0x0A, 0x92, 0x00, 0x00, 0x00, 0x01, 0x68, 0xCB, 0x83, 0xCB, 0x20, 0x00,
    0x00, 0x01, 0x65, 0x88, 0x84, 0x04, 0xAF, 0x11, 0x8A, 0x00, 0x02, 0x71,
    0x31, 0xC0, 0x00, 0x43, 0x5E, 0x38, 0x00, 0x09, 0x46, 0xC9, 0xC9, 0xC9,
    0xC9, 0xC9, 0xC9, 0xC9, 0xD7, 0x5D, 0x75, 0xD7, 0x5D, 0x75, 0xD7, 0x5D,
    0x75, 0xD7, 0x5D, 0x75, 0xD7, 0x5D, 0x75, 0xD7, 0x5D, 0x75, 0xE0, 0x00,
    0x00, 0x01, 0x65, 0x04, 0x22, 0x21, 0x01, 0x2B, 0xC4, 0x62, 0x80, 0x00,
    0x9C, 0x4C, 0x70, 0x00, 0x10, 0xD7, 0x8E, 0x00, 0x02, 0x51, 0xB2, 0x72,
    0x72, 0x72, 0x72, 0x72, 0x72, 0x75, 0xD7, 0x5D, 0x75, 0xD7, 0x5D, 0x75,
    0xD7, 0x5D, 0x75, 0xD7, 0x5D, 0x75, 0xD7, 0x5D, 0x75, 0xD7, 0x5D, 0x78
};
static const size_t k_h264_sps_pps_len = 35;

static const uint8_t k_h264_f1[22] = {
    0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x38, 0x09, 0xDE, 0x08, 0x60, 0x00,
    0x00, 0x01, 0x41, 0x04, 0x26, 0x8E, 0x02, 0x77, 0x82, 0x18
};

/* Valid 128x128 HEVC Main Annex-B bitstream:
 * - Frame 0: VPS + SPS + PPS + IDR (0..111)
 * - Frame 1: TRAIL_R (111..127)
 */
static const uint8_t k_hevc_f0[111] = {
    0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60,
    0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x1E, 0x92, 0x80, 0x90, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01,
    0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x1E, 0xA0, 0x10, 0x20, 0x20, 0x59, 0x64, 0xA9, 0x24, 0xCA, 0xF0,
    0x16, 0x80, 0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x0F, 0x04,
    0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xC1, 0x72, 0xB4, 0x22, 0x40, 0x00,
    0x00, 0x01, 0x28, 0x01, 0xAF, 0x08, 0x60, 0xF9, 0xEE, 0xAD, 0x5F, 0xFD,
    0x24, 0x5F, 0x89, 0x27, 0xED, 0x10, 0xBF, 0x9F, 0xCA, 0xEA, 0x4D, 0x9B,
    0x60, 0x25, 0x20
};

static const uint8_t k_hevc_f1[16] = {
    0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xD0, 0x09, 0x7E, 0x10, 0xC6, 0x10,
    0xC0, 0xFA, 0xA1, 0x28
};

/* Valid 128x128 AV1 OBU bitstream:
 * - Frame 0: Sequence Header OBU + Frame Header OBU + Tile Group (0..40)
 * - Frame 1: Frame Header OBU + Tile Group (40..62)
 */
static const uint8_t k_av1_f0[40] = {
    0x12, 0x00, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x03, 0x37, 0xFF, 0xE3, 0x57,
    0xCC, 0x02, 0x32, 0x18, 0x10, 0x00, 0x8C, 0x20, 0x81, 0x45, 0x08, 0x20,
    0x40, 0x00, 0x02, 0x00, 0x95, 0xCE, 0x9B, 0x15, 0xDB, 0x1A, 0x45, 0x90,
    0x10, 0x72, 0x83, 0x8F
};

static const uint8_t k_av1_f1[22] = {
    0x12, 0x00, 0x32, 0x12, 0x30, 0x02, 0x04, 0x09, 0x24, 0x92, 0x3A, 0x46,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x95, 0x9F, 0x80
};

/* Valid 128x128 10-Bit AV1 OBU bitstream (yuv420p10le):
 * - Frame 0: Sequence Header OBU (high 10-bit profile) + Frame Header + Tile Group (43 bytes)
 * - Frame 1: Frame Header OBU + Tile Group (22 bytes)
 */
static const uint8_t k_av1_10bit_f0[43] = {
    0x12, 0x00, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x03, 0x37, 0xFF, 0xE3, 0x57,
    0xCE, 0x02, 0x32, 0x1B, 0x10, 0x00, 0x8C, 0x20, 0x82, 0x08, 0x10, 0x40,
    0x40, 0x3C, 0x02, 0x00, 0x95, 0xCE, 0x9E, 0x61, 0xFB, 0x1B, 0x5B, 0x9B,
    0x63, 0x7D, 0xD1, 0x74, 0xC0, 0x31, 0xC0
};

static const uint8_t k_av1_10bit_f1[22] = {
    0x12, 0x00, 0x32, 0x12, 0x30, 0x02, 0x04, 0x09, 0x24, 0x92, 0x3A, 0x46,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x9D, 0x9D, 0xC0
};

/* ========================================================================= */
/* Deterministic Test Decoder (Tests Transport -> Decode -> Color Pipeline)  */
/* ========================================================================= */

typedef struct {
    uint32_t w;
    uint32_t h;
    int      calls;
    uint8_t *nv12;
} mock_hx_ctx_t;

static volatile int g_mock_calls = 0;

static bool mock_hx_supports(uint32_t fourcc) {
    return fourcc == OAVX_FOURCC_VIDEO_TYPE_H264;
}

static int mock_hx_create(const avx_video_dec_cfg_t *cfg, void **out_ctx) {
    mock_hx_ctx_t *m = (mock_hx_ctx_t *)calloc(1, sizeof(*m));
    if (!m) return -1;
    m->w = (cfg && cfg->width) ? cfg->width : 128;
    m->h = (cfg && cfg->height) ? cfg->height : 128;
    size_t sz = (size_t)m->w * m->h + (size_t)m->w * ((m->h + 1) / 2);
    m->nv12 = (uint8_t *)malloc(sz);
    if (!m->nv12) {
        free(m);
        return -1;
    }
    *out_ctx = m;
    return 0;
}

static int mock_hx_decode(void *vctx, const uint8_t *pkt, size_t len, oavx_video_frame_t *out) {
    mock_hx_ctx_t *m = (mock_hx_ctx_t *)vctx;
    (void)pkt;
    if (!m || len == 0 || !out) return AVX_VIDEO_CODEC_ERROR;
    m->calls++;
    g_mock_calls++;
    if (m->calls == 1) {
        return AVX_VIDEO_CODEC_NEED_MORE_DATA;
    }

    size_t y_sz = (size_t)m->w * m->h;
    size_t uv_sz = (size_t)m->w * ((m->h + 1) / 2);
    memset(m->nv12, 145, y_sz);
    for (size_t i = 0; i + 1 < uv_sz; i += 2) {
        m->nv12[y_sz + i + 0] = 54;
        m->nv12[y_sz + i + 1] = 34;
    }
    out->fourcc = OAVX_FOURCC_VIDEO_TYPE_NV12;
    out->width = m->w;
    out->height = m->h;
    out->planes[0] = m->nv12;
    out->strides[0] = m->w;
    out->planes[1] = m->nv12 + y_sz;
    out->strides[1] = m->w;
    return AVX_VIDEO_CODEC_OK;
}

static void mock_hx_reset(void *vctx) {
    mock_hx_ctx_t *m = (mock_hx_ctx_t *)vctx;
    if (m) m->calls = 0;
}

static void mock_hx_destroy(void *vctx) {
    mock_hx_ctx_t *m = (mock_hx_ctx_t *)vctx;
    if (m) {
        free(m->nv12);
        free(m);
    }
}

static const avx_video_decoder_ops_t g_mock_hx_ops = {
    .name = "mock-compressed-pipeline",
    .priority = 250,
    .is_hardware = false,
    .supports_fourcc = mock_hx_supports,
    .create = mock_hx_create,
    .decode = mock_hx_decode,
    .reset = mock_hx_reset,
    .destroy = mock_hx_destroy
};

static oavx_frame_type_t send_and_recv_video(
    oavx_stream_t *tx,
    oavx_stream_t *rx,
    const uint8_t *pkt,
    size_t pkt_len,
    int64_t pts_ns,
    oavx_video_frame_t *out_vf)
{
    oavx_video_frame_t tx_v = {
        .width = 128,
        .height = 128,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_H264,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { (uint8_t *)(uintptr_t)pkt, NULL, NULL, NULL },
        .strides = { pkt_len, 0, 0, 0 },
        .framerate = { .num = 30000, .den = 1000 },
        .aspect_ratio = { .num = 1, .den = 1 },
        .pts_ns = pts_ns
    };
    TEST_CHECK(oavx_stream_send_video(tx, &tx_v) == OAVX_SUCCESS);

    for (int attempt = 0; attempt < 20; ++attempt) {
        oavx_frame_type_t ft = oavx_stream_receive(rx, out_vf, NULL, NULL, 30);
        if (ft != OAVX_FRAME_TYPE_NONE) {
            return ft;
        }
    }
    return OAVX_FRAME_TYPE_NONE;
}

static void test_transport_hx_decode_and_color_convert(oavx_context_t *ctx,
                                                       oavx_transport_e mode,
                                                       oavx_color_format_e color_fmt,
                                                       oavx_fourcc_video_type_e expect_fourcc)
{
    printf("[*] Testing compressed video decode pipeline (transport=%d, color_fmt=%d)...\n", (int)mode, (int)color_fmt);

    oavx_stream_config_t tx_cfg = {
        .name = "COMPRESSED_TX_TEST",
        .direction = OAVX_DIR_TRANSMIT,
        .transport = mode,
        .groups = "Public"
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    TEST_CHECK(tx != NULL);

    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
    TEST_CHECK(ep && ep->address);

    oavx_stream_config_t rx_cfg = {
        .name = "COMPRESSED_RX_TEST",
        .endpoint = ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .transport = mode,
        .color_format = color_fmt,
        .groups = "Public"
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    TEST_CHECK(rx != NULL);

    avx_sleep_ms(80);
    int base_calls = g_mock_calls;

    /* Packet 1: returns NEED_MORE_DATA unless caller asked for COMPRESSED_V1 */
    oavx_video_frame_t vf1 = {0};
    oavx_frame_type_t ft1 = send_and_recv_video(tx, rx, k_h264_f0, sizeof(k_h264_f0), 1000, &vf1);
    if (color_fmt == OAVX_COLOR_FORMAT_COMPRESSED_V1) {
        if (ft1 != OAVX_FRAME_TYPE_VIDEO) {
            fprintf(stderr, "FAIL: ft1=%d (expected VIDEO) calls=%d\n", (int)ft1, (int)g_mock_calls);
        }
        TEST_CHECK(ft1 == OAVX_FRAME_TYPE_VIDEO);
        TEST_CHECK(vf1.fourcc == OAVX_FOURCC_VIDEO_TYPE_H264);
        TEST_CHECK(vf1.strides[0] == sizeof(k_h264_f0));
        TEST_CHECK(memcmp(vf1.planes[0], k_h264_f0, sizeof(k_h264_f0)) == 0);
        TEST_CHECK(g_mock_calls == base_calls); /* Decoder never invoked */
    } else {
        /* Priming packet consumed silently — NOT leaked as compressed bytes */
        if (ft1 != OAVX_FRAME_TYPE_NONE || g_mock_calls != base_calls + 1) {
            fprintf(stderr, "FAIL: ft1=%d fourcc=0x%08X calls=%d base=%d\n",
                    (int)ft1, (unsigned)vf1.fourcc, (int)g_mock_calls, base_calls);
        }
        TEST_CHECK(ft1 == OAVX_FRAME_TYPE_NONE);
        TEST_CHECK(g_mock_calls == base_calls + 1);

        oavx_video_frame_t vf2 = {0};
        oavx_frame_type_t ft2 = send_and_recv_video(tx, rx, k_h264_f1, sizeof(k_h264_f1), 2000, &vf2);
        TEST_CHECK(ft2 == OAVX_FRAME_TYPE_VIDEO);
        TEST_CHECK(vf2.width == 128 && vf2.height == 128);
        TEST_CHECK(vf2.fourcc == expect_fourcc);
        TEST_CHECK(vf2.planes[0] != NULL);
        if (expect_fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA ||
            expect_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA) {
            TEST_CHECK(vf2.strides[0] == 128U * 4U);
            /* NV12 Y=145,U=54,V=34 -> green dominant, alpha 255 */
            TEST_CHECK(vf2.planes[0][1] > vf2.planes[0][0]);
            TEST_CHECK(vf2.planes[0][3] == 255);
        } else if (expect_fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
            TEST_CHECK(vf2.strides[0] == 128U * 2U);
            TEST_CHECK(vf2.planes[0][0] == 54 && vf2.planes[0][1] == 145 && vf2.planes[0][2] == 34);
        }
    }

    oavx_stream_close(rx);
    oavx_stream_close(tx);
}

static void exercise_native_decoder(uint32_t fourcc, const char *label,
                                    const uint8_t *priming_pkt, size_t priming_len,
                                    const uint8_t *pkt0, size_t len0,
                                    const uint8_t *pkt1, size_t len1)
{
    if (!avx_video_decoder_has_backend(fourcc)) {
        printf("  [SKIP] No native %s decoder backend on this host\n", label);
        return;
    }

    avx_video_dec_cfg_t cfg = { .fourcc = fourcc, .width = 128, .height = 128 };
    avx_video_decoder_t *dec = avx_video_decoder_create(&cfg);
    TEST_CHECK(dec != NULL);
    printf("[*] Testing real %s bitstream decode on backend '%s'...\n",
           label, avx_video_decoder_get_backend_name(dec));

    oavx_video_frame_t out = {0};
    if (priming_pkt && priming_len > 0) {
        int rcp = avx_video_decoder_decode(dec, priming_pkt, priming_len, &out);
        TEST_CHECK(rcp == AVX_VIDEO_CODEC_NEED_MORE_DATA || rcp == AVX_VIDEO_CODEC_OK);
    }

    int got_frame = 0;
    const uint8_t *pkts[4] = { pkt0, pkt1, pkt0, pkt1 };
    size_t lens[4] = { len0, len1, len0, len1 };
    for (int i = 0; i < 4; ++i) {
        int rc = avx_video_decoder_decode(dec, pkts[i], lens[i], &out);
        TEST_CHECK(rc == AVX_VIDEO_CODEC_OK || rc == AVX_VIDEO_CODEC_NEED_MORE_DATA);
        if (rc == AVX_VIDEO_CODEC_OK) {
            TEST_CHECK(out.width == 128 && out.height == 128);
            TEST_CHECK(out.fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12 || out.fourcc == OAVX_FOURCC_VIDEO_TYPE_I420);
            TEST_CHECK(out.planes[0] != NULL && out.strides[0] >= 128);
            TEST_CHECK(out.planes[1] != NULL && out.strides[1] >= 64);
            got_frame++;
        }
    }
    TEST_CHECK(got_frame >= 1);

    avx_video_decoder_reset(dec);
    int rcr = avx_video_decoder_decode(dec, pkt0, len0, &out);
    TEST_CHECK(rcr == AVX_VIDEO_CODEC_OK || rcr == AVX_VIDEO_CODEC_NEED_MORE_DATA);
    if (rcr == AVX_VIDEO_CODEC_NEED_MORE_DATA) {
        rcr = avx_video_decoder_decode(dec, pkt1, len1, &out);
        TEST_CHECK(rcr == AVX_VIDEO_CODEC_OK || rcr == AVX_VIDEO_CODEC_NEED_MORE_DATA);
    }

    avx_video_decoder_destroy(dec);
    printf("  [+] %s native bitstream decode passed!\n", label);
}

static size_t convert_annexb_to_avcc(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t pos = 0;
    size_t out_len = 0;
    while (pos + 3 <= src_len) {
        size_t sc_len = 0;
        if (src[pos] == 0 && src[pos + 1] == 0) {
            if (src[pos + 2] == 1) sc_len = 3;
            else if (pos + 4 <= src_len && src[pos + 2] == 0 && src[pos + 3] == 1) sc_len = 4;
        }
        if (sc_len > 0) {
            pos += sc_len;
            size_t nstart = pos;
            while (pos + 3 <= src_len) {
                if (src[pos] == 0 && src[pos + 1] == 0 &&
                    (src[pos + 2] == 1 || (pos + 4 <= src_len && src[pos + 2] == 0 && src[pos + 3] == 1))) {
                    break;
                }
                pos++;
            }
            if (pos + 3 > src_len) pos = src_len;
            size_t nlen = pos - nstart;
            if (nlen > 0 && out_len + 4 + nlen <= dst_cap) {
                dst[out_len + 0] = (uint8_t)((nlen >> 24) & 0xFF);
                dst[out_len + 1] = (uint8_t)((nlen >> 16) & 0xFF);
                dst[out_len + 2] = (uint8_t)((nlen >> 8) & 0xFF);
                dst[out_len + 3] = (uint8_t)(nlen & 0xFF);
                memcpy(dst + out_len + 4, src + nstart, nlen);
                out_len += 4 + nlen;
            }
        } else {
            pos++;
        }
    }
    return out_len;
}

static void exercise_avcc_decoder(uint32_t fourcc, const char *label,
                                  const uint8_t *annexb_f0, size_t len0,
                                  const uint8_t *annexb_f1, size_t len1)
{
    if (!avx_video_decoder_has_backend(fourcc)) {
        return;
    }

    uint8_t avcc_f0[512];
    uint8_t avcc_f1[512];
    size_t avcc_len0 = convert_annexb_to_avcc(annexb_f0, len0, avcc_f0, sizeof(avcc_f0));
    size_t avcc_len1 = convert_annexb_to_avcc(annexb_f1, len1, avcc_f1, sizeof(avcc_f1));
    TEST_CHECK(avcc_len0 > 4 && avcc_len1 > 4);

    avx_video_dec_cfg_t cfg = { .fourcc = fourcc, .width = 128, .height = 128 };
    avx_video_decoder_t *dec = avx_video_decoder_create(&cfg);
    TEST_CHECK(dec != NULL);
    printf("[*] Testing 4-byte length-prefixed AVCC/HVCC %s decode on backend '%s'...\n",
           label, avx_video_decoder_get_backend_name(dec));

    oavx_video_frame_t out = {0};
    int got_frame = 0;
    const uint8_t *pkts[4] = { avcc_f0, avcc_f1, avcc_f0, avcc_f1 };
    size_t lens[4] = { avcc_len0, avcc_len1, avcc_len0, avcc_len1 };
    for (int i = 0; i < 4; ++i) {
        int rc = avx_video_decoder_decode(dec, pkts[i], lens[i], &out);
        TEST_CHECK(rc == AVX_VIDEO_CODEC_OK || rc == AVX_VIDEO_CODEC_NEED_MORE_DATA);
        if (rc == AVX_VIDEO_CODEC_OK) {
            TEST_CHECK(out.width == 128 && out.height == 128);
            TEST_CHECK(out.planes[0] != NULL);
            got_frame++;
        }
    }
    TEST_CHECK(got_frame >= 1);
    avx_video_decoder_destroy(dec);
    printf("  [+] %s AVCC/HVCC bitstream decode passed!\n", label);
}

static void exercise_10bit_decoder(void) {
    if (!avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_AV01)) {
        printf("  [SKIP] No native AV1 decoder backend on this host\n");
        return;
    }

    avx_video_dec_cfg_t cfg = { .fourcc = OAVX_FOURCC_VIDEO_TYPE_AV01, .width = 128, .height = 128 };
    avx_video_decoder_t *dec = avx_video_decoder_create(&cfg);
    TEST_CHECK(dec != NULL);
    printf("[*] Testing 10-bit AV1 bitstream decode to native P216 on backend '%s'...\n",
           avx_video_decoder_get_backend_name(dec));

    oavx_video_frame_t out = {0};
    int rc0 = avx_video_decoder_decode(dec, k_av1_10bit_f0, sizeof(k_av1_10bit_f0), &out);
    TEST_CHECK(rc0 == AVX_VIDEO_CODEC_OK || rc0 == AVX_VIDEO_CODEC_NEED_MORE_DATA);

    if (rc0 == AVX_VIDEO_CODEC_OK) {
        TEST_CHECK(out.width == 128 && out.height == 128);
        TEST_CHECK(out.fourcc == OAVX_FOURCC_VIDEO_TYPE_P216);
        TEST_CHECK(out.planes[0] != NULL && out.strides[0] == 128U * 2U);
        TEST_CHECK(out.planes[1] != NULL && out.strides[1] == 128U * 2U);
        const uint16_t *py = (const uint16_t *)(const void *)out.planes[0];
        const uint16_t *puv = (const uint16_t *)(const void *)out.planes[1];
        TEST_CHECK(py[0] > 0 && (py[0] & 0x3F) == 0);
        TEST_CHECK(puv[0] > 0 && (puv[0] & 0x3F) == 0);
        TEST_CHECK(puv[1] > 0 && (puv[1] & 0x3F) == 0);
    }

    int rc1 = avx_video_decoder_decode(dec, k_av1_10bit_f1, sizeof(k_av1_10bit_f1), &out);
    TEST_CHECK(rc1 == AVX_VIDEO_CODEC_OK || rc1 == AVX_VIDEO_CODEC_NEED_MORE_DATA);
    if (rc1 == AVX_VIDEO_CODEC_OK) {
        TEST_CHECK(out.width == 128 && out.height == 128);
        TEST_CHECK(out.fourcc == OAVX_FOURCC_VIDEO_TYPE_P216);
        TEST_CHECK(out.planes[0] != NULL && out.strides[0] == 128U * 2U);
        TEST_CHECK(out.planes[1] != NULL && out.strides[1] == 128U * 2U);
        const uint16_t *py = (const uint16_t *)(const void *)out.planes[0];
        const uint16_t *puv = (const uint16_t *)(const void *)out.planes[1];
        TEST_CHECK(py[0] > 0 && (py[0] & 0x3F) == 0);
        TEST_CHECK(puv[0] > 0 && (puv[0] & 0x3F) == 0);
        TEST_CHECK(puv[1] > 0 && (puv[1] & 0x3F) == 0);
    }

    avx_video_decoder_destroy(dec);
    printf("  [+] AV1 10-bit -> native P216 decode passed!\n");
}

static void test_transport_10bit_pipeline(oavx_context_t *ctx,
                                          oavx_color_format_e color_fmt,
                                          oavx_fourcc_video_type_e expect_fourcc)
{
    if (!avx_video_decoder_has_backend(OAVX_FOURCC_VIDEO_TYPE_AV01)) {
        return;
    }
    printf("[*] Testing 10-bit transport pipeline (color_fmt=%d, expect=0x%08X)...\n",
           (int)color_fmt, (unsigned)expect_fourcc);

    oavx_stream_config_t tx_cfg = {
        .name = "10BIT_TX_TEST",
        .direction = OAVX_DIR_TRANSMIT,
        .transport = OAVX_TRANSPORT_TCP,
        .groups = "Public"
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    TEST_CHECK(tx != NULL);

    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
    TEST_CHECK(ep && ep->address);

    oavx_stream_config_t rx_cfg = {
        .name = "10BIT_RX_TEST",
        .endpoint = ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .transport = OAVX_TRANSPORT_TCP,
        .color_format = color_fmt,
        .groups = "Public"
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    TEST_CHECK(rx != NULL);

    avx_sleep_ms(80);

    oavx_video_frame_t tx_v = {
        .width = 128,
        .height = 128,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_AV01,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { (uint8_t *)(uintptr_t)k_av1_10bit_f0, NULL, NULL, NULL },
        .strides = { sizeof(k_av1_10bit_f0), 0, 0, 0 },
        .framerate = { .num = 30000, .den = 1000 },
        .aspect_ratio = { .num = 1, .den = 1 },
        .pts_ns = 1000
    };
    TEST_CHECK(oavx_stream_send_video(tx, &tx_v) == OAVX_SUCCESS);

    oavx_video_frame_t vf = {0};
    oavx_frame_type_t ft = OAVX_FRAME_TYPE_NONE;
    for (int attempt = 0; attempt < 25; ++attempt) {
        ft = oavx_stream_receive(rx, &vf, NULL, NULL, 30);
        if (ft != OAVX_FRAME_TYPE_NONE) break;
    }

    if (ft == OAVX_FRAME_TYPE_VIDEO) {
        TEST_CHECK(vf.width == 128 && vf.height == 128);
        TEST_CHECK(vf.fourcc == expect_fourcc);
        TEST_CHECK(vf.planes[0] != NULL);
        if (expect_fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
            TEST_CHECK(vf.planes[1] != NULL);
            TEST_CHECK(vf.strides[0] == 128U * 2U && vf.strides[1] == 128U * 2U);
            const uint16_t *py = (const uint16_t *)(const void *)vf.planes[0];
            TEST_CHECK((py[0] & 0x3F) == 0);
        } else if (expect_fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA) {
            TEST_CHECK(vf.strides[0] == 128U * 4U);
            TEST_CHECK(vf.planes[0][3] == 255);
        }
    }

    oavx_stream_close(rx);
    oavx_stream_close(tx);
}

static void exercise_encoder_roundtrip(uint32_t fourcc, const char *label) {
    if (!avx_video_encoder_has_backend(fourcc)) {
        printf("[*] Skipping %s encoder round-trip (no registered backend)\n", label);
        return;
    }

    printf("[*] Testing real %s video encode & decode round-trip...\n", label);

    avx_video_enc_cfg_t enc_cfg = {
        .fourcc = fourcc,
        .width = 128,
        .height = 128,
        .fps_num = 30,
        .fps_den = 1,
        .bitrate_kbps = 2000,
        .keyframe_interval = 30
    };
    avx_video_encoder_t *enc = avx_video_encoder_create(&enc_cfg);
    TEST_CHECK(enc != NULL);
    printf("  [+] Encoder initialized using backend: '%s'\n",
           avx_video_encoder_get_backend_name(enc));

    avx_video_dec_cfg_t dec_cfg = {
        .fourcc = fourcc,
        .width = 128,
        .height = 128,
        .preferred_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY
    };
    avx_video_decoder_t *dec = avx_video_decoder_create(&dec_cfg);
    TEST_CHECK(dec != NULL);

    /* 1. Test UYVY input */
    uint8_t uyvy[128 * 128 * 2];
    for (size_t i = 0; i < sizeof(uyvy); i += 4) {
        uyvy[i + 0] = 128;
        uyvy[i + 1] = (uint8_t)(16 + (i % 220));
        uyvy[i + 2] = 128;
        uyvy[i + 3] = (uint8_t)(16 + (i % 220));
    }
    oavx_video_frame_t in_frame = {
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .width = 128,
        .height = 128,
        .planes = { uyvy, NULL, NULL, NULL },
        .strides = { 128 * 2, 0, 0, 0 }
    };

    uint8_t pkt_buf[512 * 1024];
    size_t pkt_size = 0;
    bool is_key = false;

    /* Frame 0: Keyframe */
    int rc = avx_video_encoder_encode(enc, &in_frame, pkt_buf, sizeof(pkt_buf), &pkt_size, &is_key);
    TEST_CHECK(rc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(pkt_size > 4);
    TEST_CHECK(is_key == true);

    oavx_video_frame_t out_frame;
    memset(&out_frame, 0, sizeof(out_frame));
    int drc = avx_video_decoder_decode(dec, pkt_buf, pkt_size, &out_frame);
    TEST_CHECK(drc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(out_frame.width == 128 && out_frame.height == 128);

    /* Frame 1: P-frame */
    rc = avx_video_encoder_encode(enc, &in_frame, pkt_buf, sizeof(pkt_buf), &pkt_size, &is_key);
    TEST_CHECK(rc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(pkt_size > 0);
    drc = avx_video_decoder_decode(dec, pkt_buf, pkt_size, &out_frame);
    TEST_CHECK(drc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(out_frame.width == 128 && out_frame.height == 128);

    /* Frame 2: Force Keyframe */
    avx_video_encoder_request_keyframe(enc);
    rc = avx_video_encoder_encode(enc, &in_frame, pkt_buf, sizeof(pkt_buf), &pkt_size, &is_key);
    TEST_CHECK(rc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(pkt_size > 4);
    TEST_CHECK(is_key == true);
    drc = avx_video_decoder_decode(dec, pkt_buf, pkt_size, &out_frame);
    TEST_CHECK(drc == AVX_VIDEO_CODEC_OK);

    /* 2. Test RGBA input format */
    uint8_t rgba[128 * 128 * 4];
    memset(rgba, 0xAA, sizeof(rgba));
    oavx_video_frame_t rgba_frame = {
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_RGBA,
        .width = 128,
        .height = 128,
        .planes = { rgba, NULL, NULL, NULL },
        .strides = { 128 * 4, 0, 0, 0 }
    };
    rc = avx_video_encoder_encode(enc, &rgba_frame, pkt_buf, sizeof(pkt_buf), &pkt_size, &is_key);
    TEST_CHECK(rc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(pkt_size > 0);
    drc = avx_video_decoder_decode(dec, pkt_buf, pkt_size, &out_frame);
    TEST_CHECK(drc == AVX_VIDEO_CODEC_OK);

    /* 3. Test P216 input format */
    uint16_t p216_y[128 * 128];
    uint16_t p216_uv[128 * 128];
    for (size_t i = 0; i < 128 * 128; ++i) {
        p216_y[i] = (uint16_t)((16 + (i % 220)) << 6);
        p216_uv[i] = (uint16_t)(128 << 6);
    }
    oavx_video_frame_t p216_frame = {
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_P216,
        .width = 128,
        .height = 128,
        .planes = { (uint8_t *)p216_y, (uint8_t *)p216_uv, NULL, NULL },
        .strides = { 128 * 2, 128 * 2, 0, 0 }
    };
    rc = avx_video_encoder_encode(enc, &p216_frame, pkt_buf, sizeof(pkt_buf), &pkt_size, &is_key);
    TEST_CHECK(rc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(pkt_size > 0);
    drc = avx_video_decoder_decode(dec, pkt_buf, pkt_size, &out_frame);
    TEST_CHECK(drc == AVX_VIDEO_CODEC_OK);

    /* 4. Test dynamic quality adjustment */
    TEST_CHECK(avx_video_encoder_set_quality(enc, 80) == AVX_VIDEO_CODEC_OK);

    avx_video_decoder_destroy(dec);
    avx_video_encoder_destroy(enc);
    printf("  [+] %s encode & decode round-trip passed!\n", label);
}

static void exercise_encoder_hd_roundtrip(uint32_t fourcc, const char *label) {
    if (!avx_video_encoder_has_backend(fourcc) || !avx_video_decoder_has_backend(fourcc)) {
        printf("  [*] Skipping %s HD encode/decode roundtrip (missing backend)\n", label);
        return;
    }
    printf("[*] Testing real %s 720p HD video encode & decode round-trip...\n", label);

    const uint32_t w = 1280;
    const uint32_t h = 720;
    avx_video_enc_cfg_t enc_cfg = {
        .fourcc = fourcc,
        .width = w,
        .height = h,
        .bitrate_kbps = 4000,
        .fps_num = 30,
        .fps_den = 1,
        .quality = 80,
        .keyframe_interval = 30
    };
    avx_video_encoder_t *enc = avx_video_encoder_create(&enc_cfg);
    TEST_CHECK(enc != NULL);
    const char *bname = avx_video_encoder_get_backend_name(enc);
    printf("  [+] HD Encoder initialized using backend: '%s'\n", bname);

    avx_video_dec_cfg_t dec_cfg = {
        .fourcc = fourcc,
        .width = w,
        .height = h,
        .preferred_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY
    };
    avx_video_decoder_t *dec = avx_video_decoder_create(&dec_cfg);
    TEST_CHECK(dec != NULL);

    size_t uyvy_size = (size_t)w * h * 2;
    uint8_t *uyvy = (uint8_t *)malloc(uyvy_size);
    TEST_CHECK(uyvy != NULL);
    for (size_t i = 0; i < uyvy_size; i += 4) {
        uyvy[i + 0] = 128;
        uyvy[i + 1] = (uint8_t)(16 + (i % 220));
        uyvy[i + 2] = 128;
        uyvy[i + 3] = (uint8_t)(16 + (i % 220));
    }
    oavx_video_frame_t in_frame = {
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .width = w,
        .height = h,
        .planes = { uyvy, NULL, NULL, NULL },
        .strides = { w * 2, 0, 0, 0 }
    };

    size_t pkt_buf_size = 2 * 1024 * 1024;
    uint8_t *pkt_buf = (uint8_t *)malloc(pkt_buf_size);
    TEST_CHECK(pkt_buf != NULL);
    size_t pkt_size = 0;
    bool is_key = false;

    /* Frame 0: Keyframe */
    int rc = avx_video_encoder_encode(enc, &in_frame, pkt_buf, pkt_buf_size, &pkt_size, &is_key);
    TEST_CHECK(rc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(pkt_size > 4);
    TEST_CHECK(is_key == true);

    oavx_video_frame_t out_frame;
    memset(&out_frame, 0, sizeof(out_frame));
    int drc = avx_video_decoder_decode(dec, pkt_buf, pkt_size, &out_frame);
    TEST_CHECK(drc == AVX_VIDEO_CODEC_OK);
    TEST_CHECK(out_frame.width == w && out_frame.height == h);

    free(pkt_buf);
    free(uyvy);
    avx_video_decoder_destroy(dec);
    avx_video_encoder_destroy(enc);
    printf("  [+] %s HD 720p encode & decode round-trip passed!\n", label);
}

static void test_transport_encoding_pipeline(oavx_context_t *ctx,
                                             oavx_transport_e transport,
                                             uint32_t codec_fourcc,
                                             const char *label)
{
    if (!avx_video_encoder_has_backend(codec_fourcc) || !avx_video_decoder_has_backend(codec_fourcc)) {
        printf("  [*] Skipping %s transport encode/decode test (missing encoder or decoder backend)\n", label);
        return;
    }
    printf("[*] Testing live transport %s encoding pipeline (transport=%d)...\n", label, (int)transport);

    oavx_stream_config_t tx_cfg = {
        .name = "ENC_TX_TEST",
        .direction = OAVX_DIR_TRANSMIT,
        .transport = transport,
        .video_codec_fourcc = codec_fourcc,
        .video_bitrate_kbps = 4000,
        .keyframe_interval = 30,
        .groups = "Public"
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    TEST_CHECK(tx != NULL);
    TEST_CHECK(oavx_stream_get_video_compression(tx) == codec_fourcc);

    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
    TEST_CHECK(ep && ep->address);

    oavx_stream_config_t rx_cfg = {
        .name = "ENC_RX_TEST",
        .endpoint = ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .transport = transport,
        .color_format = OAVX_COLOR_FORMAT_BEST,
        .groups = "Public"
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    TEST_CHECK(rx != NULL);

    avx_sleep_ms(80);

    /* Generate raw UYVY video pattern */
    uint8_t uyvy[128 * 128 * 2];
    for (size_t i = 0; i < sizeof(uyvy); i += 4) {
        uyvy[i + 0] = 128;
        uyvy[i + 1] = (uint8_t)(16 + (i % 220));
        uyvy[i + 2] = 128;
        uyvy[i + 3] = (uint8_t)(16 + (i % 220));
    }

    oavx_video_frame_t tx_v = {
        .width = 128,
        .height = 128,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { uyvy, NULL, NULL, NULL },
        .strides = { 128 * 2, 0, 0, 0 },
        .framerate = { .num = 30000, .den = 1000 },
        .aspect_ratio = { .num = 1, .den = 1 },
        .pts_ns = 1000000
    };

    /* Send multiple frames so keyframe + inter frames are transmitted and decoded */
    bool received_frame = false;
    for (int f = 0; f < 5; ++f) {
        tx_v.pts_ns = (int64_t)(f + 1) * 33333333LL;
        TEST_CHECK(oavx_stream_send_video(tx, &tx_v) == OAVX_SUCCESS);

        for (int attempt = 0; attempt < 10; ++attempt) {
            oavx_video_frame_t vf = {0};
            oavx_frame_type_t ft = oavx_stream_receive(rx, &vf, NULL, NULL, 30);
            if (ft == OAVX_FRAME_TYPE_VIDEO) {
                TEST_CHECK(vf.width == 128 && vf.height == 128);
                TEST_CHECK(vf.planes[0] != NULL);
                received_frame = true;
                break;
            }
        }
    }
    TEST_CHECK(received_frame == true);

    /* Test dynamic reconfiguration API */
    TEST_CHECK(oavx_stream_set_video_compression(tx, OAVX_FOURCC_VIDEO_TYPE_HEVC, 2000) == OAVX_SUCCESS);
    TEST_CHECK(oavx_stream_get_video_compression(tx) == OAVX_FOURCC_VIDEO_TYPE_HEVC);

    oavx_stream_close(rx);
    oavx_stream_close(tx);
    printf("  [+] %s transport live encoding pipeline passed!\n", label);
}

int main(void) {
    printf("\n=== OpenAVX Video Decoder & Transport Pipeline Verification ===\n\n");

    oavx_context_t *ctx = oavx_context_create();
    TEST_CHECK(ctx != NULL);
    TEST_CHECK(avx_video_decoder_register(&g_mock_hx_ops));

    /* 1. RUDP path: H264 priming suppression + NV12 -> BGRA/UYVY conversion */
    test_transport_hx_decode_and_color_convert(
        ctx, OAVX_TRANSPORT_RUDP, OAVX_COLOR_FORMAT_BGRX_BGRA, OAVX_FOURCC_VIDEO_TYPE_BGRA);
    test_transport_hx_decode_and_color_convert(
        ctx, OAVX_TRANSPORT_RUDP, OAVX_COLOR_FORMAT_UYVY_BGRA, OAVX_FOURCC_VIDEO_TYPE_UYVY);

    /* 2. TCP path: NV12 -> RGBA conversion + explicit COMPRESSED_V1 passthrough */
    test_transport_hx_decode_and_color_convert(
        ctx, OAVX_TRANSPORT_TCP, OAVX_COLOR_FORMAT_RGBX_RGBA, OAVX_FOURCC_VIDEO_TYPE_RGBA);
    test_transport_hx_decode_and_color_convert(
        ctx, OAVX_TRANSPORT_TCP, OAVX_COLOR_FORMAT_COMPRESSED_V1, OAVX_FOURCC_VIDEO_TYPE_H264);

    /* 3. Destroy engine/context cleanly (clears mock backend), then re-init & probe real OS decoders */
    oavx_context_destroy(ctx);
    TEST_CHECK(avx_video_codec_init());

    exercise_native_decoder(OAVX_FOURCC_VIDEO_TYPE_H264, "H.264",
                            k_h264_f0, k_h264_sps_pps_len,
                            k_h264_f0, sizeof(k_h264_f0),
                            k_h264_f1, sizeof(k_h264_f1));

    exercise_native_decoder(OAVX_FOURCC_VIDEO_TYPE_HEVC, "HEVC",
                            NULL, 0,
                            k_hevc_f0, sizeof(k_hevc_f0),
                            k_hevc_f1, sizeof(k_hevc_f1));

    exercise_native_decoder(OAVX_FOURCC_VIDEO_TYPE_AV01, "AV1",
                            NULL, 0,
                            k_av1_f0, sizeof(k_av1_f0),
                            k_av1_f1, sizeof(k_av1_f1));

    /* 4. Verify 4-byte length-prefixed AVCC / HVCC container bitstream auto-conversion */
    exercise_avcc_decoder(OAVX_FOURCC_VIDEO_TYPE_H264, "H.264",
                          k_h264_f0, sizeof(k_h264_f0),
                          k_h264_f1, sizeof(k_h264_f1));

    exercise_avcc_decoder(OAVX_FOURCC_VIDEO_TYPE_HEVC, "HEVC",
                          k_hevc_f0, sizeof(k_hevc_f0),
                          k_hevc_f1, sizeof(k_hevc_f1));

    /* 5. Verify 10-bit AV1 bitstream decode to native P216 */
    exercise_10bit_decoder();

    /* 6. Verify end-to-end 10-bit transport pipeline */
    ctx = oavx_context_create();
    TEST_CHECK(ctx != NULL);
    test_transport_10bit_pipeline(ctx, OAVX_COLOR_FORMAT_BEST, OAVX_FOURCC_VIDEO_TYPE_P216);
    test_transport_10bit_pipeline(ctx, OAVX_COLOR_FORMAT_P216, OAVX_FOURCC_VIDEO_TYPE_P216);
    test_transport_10bit_pipeline(ctx, OAVX_COLOR_FORMAT_RGBX_RGBA, OAVX_FOURCC_VIDEO_TYPE_RGBA);
    oavx_context_destroy(ctx);

    /* 7. Verify real video encoder round-trip (H.264 & HEVC) */
    exercise_encoder_roundtrip(OAVX_FOURCC_VIDEO_TYPE_H264, "H.264");
    exercise_encoder_roundtrip(OAVX_FOURCC_VIDEO_TYPE_HEVC, "HEVC");
    exercise_encoder_hd_roundtrip(OAVX_FOURCC_VIDEO_TYPE_H264, "H.264");
    exercise_encoder_hd_roundtrip(OAVX_FOURCC_VIDEO_TYPE_HEVC, "HEVC");

    /* 8. Verify live transport encoding pipeline across TCP & RUDP */
    ctx = oavx_context_create();
    TEST_CHECK(ctx != NULL);
    test_transport_encoding_pipeline(ctx, OAVX_TRANSPORT_TCP, OAVX_FOURCC_VIDEO_TYPE_H264, "H.264 (TCP)");
    test_transport_encoding_pipeline(ctx, OAVX_TRANSPORT_RUDP, OAVX_FOURCC_VIDEO_TYPE_H264, "H.264 (RUDP)");
    test_transport_encoding_pipeline(ctx, OAVX_TRANSPORT_TCP, OAVX_FOURCC_VIDEO_TYPE_HEVC, "HEVC (TCP)");
    oavx_context_destroy(ctx);

    printf("\n[PASS] openavx_video_decoder_test\n");
    return 0;
}
