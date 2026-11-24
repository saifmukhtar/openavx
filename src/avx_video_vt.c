/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_video_vt.c - macOS VideoToolbox Hardware Video Decoder Backend
 */

#include "avx_video_codec.h"
#include "avx_internal.h"
#include "avx_rtsp.h"
#include "avx_color.h"

#if defined(AVX_OS_MACOS)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t                     src_fourcc;
    uint32_t                     width;
    uint32_t                     height;

    CMVideoFormatDescriptionRef  fmt_desc;
    VTDecompressionSessionRef    session;

    uint8_t                     *vps;
    size_t                       vps_len;
    uint8_t                     *sps;
    size_t                       sps_len;
    uint8_t                     *pps;
    size_t                       pps_len;
    uint8_t                     *av1_seq;
    size_t                       av1_seq_len;

    uint8_t                     *avcc_buf;
    size_t                       avcc_cap;

    bool                         has_frame;
    bool                         cb_error;
    uint32_t                     out_width;
    uint32_t                     out_height;
    uint8_t                     *frame_buf;
    size_t                       frame_buf_size;
} avx_vt_dec_ctx_t;

static void vt_replace_buf(uint8_t **dst, size_t *dst_len, const uint8_t *src, size_t src_len, bool *changed) {
    if (!src || src_len == 0) return;
    if (*dst && *dst_len == src_len && memcmp(*dst, src, src_len) == 0) return;
    uint8_t *nb = (uint8_t *)malloc(src_len);
    if (!nb) return;
    memcpy(nb, src, src_len);
    free(*dst);
    *dst = nb;
    *dst_len = src_len;
    if (changed) *changed = true;
}

static bool vt_probe_av1_available(void) {
    if (__builtin_available(macOS 13.0, *)) {
        if (!VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1)) return false;
        CMVideoFormatDescriptionRef fmt = NULL;
        OSStatus st = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_AV1, 128, 128, NULL, &fmt);
        if (st != noErr || !fmt) return false;
        VTDecompressionSessionRef sess = NULL;
        VTDecompressionOutputCallbackRecord cb = {0};
        st = VTDecompressionSessionCreate(kCFAllocatorDefault, fmt, NULL, NULL, &cb, &sess);
        CFRelease(fmt);
        if (st == noErr && sess) {
            VTDecompressionSessionInvalidate(sess);
            CFRelease(sess);
            return true;
        }
    }
    return false;
}

static bool vt_dec_supports_fourcc(uint32_t src_fourcc) {
    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 || src_fourcc == 0x34363268 ||
        src_fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || src_fourcc == 0x43564568) {
        return true;
    }
    if (src_fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
        return vt_probe_av1_available();
    }
    return false;
}

static void vt_release_session(avx_vt_dec_ctx_t *ctx) {
    if (ctx->session) {
        VTDecompressionSessionWaitForAsynchronousFrames(ctx->session);
        VTDecompressionSessionInvalidate(ctx->session);
        CFRelease(ctx->session);
        ctx->session = NULL;
    }
    if (ctx->fmt_desc) {
        CFRelease(ctx->fmt_desc);
        ctx->fmt_desc = NULL;
    }
}

static void vt_dec_destroy(void *backend_ctx) {
    avx_vt_dec_ctx_t *ctx = (avx_vt_dec_ctx_t *)backend_ctx;
    if (!ctx) return;

    vt_release_session(ctx);
    free(ctx->vps);
    free(ctx->sps);
    free(ctx->pps);
    free(ctx->av1_seq);
    free(ctx->avcc_buf);
    free(ctx->frame_buf);
    free(ctx);
}

static void vt_output_callback(
    void *decompressionOutputRefCon,
    void *sourceFrameRefCon,
    OSStatus status,
    VTDecodeInfoFlags infoFlags,
    CVImageBufferRef imageBuffer,
    CMTime presentationTimeStamp,
    CMTime presentationDuration)
{
    (void)sourceFrameRefCon;
    (void)infoFlags;
    (void)presentationTimeStamp;
    (void)presentationDuration;

    avx_vt_dec_ctx_t *ctx = (avx_vt_dec_ctx_t *)decompressionOutputRefCon;
    if (!ctx) return;

    if (status != noErr || !imageBuffer) {
        if (status != noErr) ctx->cb_error = true;
        return;
    }

    if (CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        ctx->cb_error = true;
        return;
    }

    uint32_t w = (uint32_t)CVPixelBufferGetWidth(imageBuffer);
    uint32_t h = (uint32_t)CVPixelBufferGetHeight(imageBuffer);
    uint64_t need_u64 = (uint64_t)w * (uint64_t)h * 2ULL;

    if (w > 0 && h > 0 && w <= AVX_MAX_VIDEO_WIDTH && h <= AVX_MAX_VIDEO_HEIGHT &&
        need_u64 <= AVX_MAX_DECOMPRESSED_FRAME_SIZE &&
        CVPixelBufferIsPlanar(imageBuffer) && CVPixelBufferGetPlaneCount(imageBuffer) >= 2) {
        uint32_t ch = (h + 1U) / 2U;
        size_t need = (size_t)w * (size_t)h + (size_t)w * (size_t)ch;
        if (ctx->frame_buf_size < need) {
            uint8_t *nb = (uint8_t *)realloc(ctx->frame_buf, need);
            if (nb) {
                ctx->frame_buf = nb;
                ctx->frame_buf_size = need;
            }
        }
        if (ctx->frame_buf && ctx->frame_buf_size >= need) {
            const uint8_t *y_src  = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 0);
            const uint8_t *uv_src = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(imageBuffer, 1);
            size_t y_stride  = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 0);
            size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, 1);

            if (y_src && uv_src && y_stride >= w && uv_stride >= w) {
                for (uint32_t y = 0; y < h; ++y) {
                    memcpy(ctx->frame_buf + (size_t)y * w, y_src + (size_t)y * y_stride, w);
                }
                uint8_t *dst_uv = ctx->frame_buf + (size_t)w * h;
                for (uint32_t y = 0; y < ch; ++y) {
                    memcpy(dst_uv + (size_t)y * w, uv_src + (size_t)y * uv_stride, w);
                }
                ctx->out_width = w;
                ctx->out_height = h;
                ctx->has_frame = true;
            }
        }
    }

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

/* Allocates or re-creates Apple VTDecompressionSession bound to CoreVideo pixel buffer attributes. */
static int vt_ensure_session(avx_vt_dec_ctx_t *ctx, CMVideoFormatDescriptionRef new_fmt) {
    if (!new_fmt) return -1;

    if (ctx->session && VTDecompressionSessionCanAcceptFormatDescription(ctx->session, new_fmt)) {
        if (ctx->fmt_desc) CFRelease(ctx->fmt_desc);
        ctx->fmt_desc = new_fmt;
        return 0;
    }

    vt_release_session(ctx);
    ctx->fmt_desc = new_fmt;

    CFMutableDictionaryRef decoder_spec = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (decoder_spec) {
        if (__builtin_available(macOS 10.9, *)) {
            CFDictionarySetValue(decoder_spec,
                                 kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
                                 kCFBooleanTrue);
        }
    }

    CFMutableDictionaryRef dst_attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (dst_attrs) {
        int32_t nv12_fmt = (int32_t)kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        CFNumberRef pix_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &nv12_fmt);
        if (pix_num) {
            CFDictionarySetValue(dst_attrs, kCVPixelBufferPixelFormatTypeKey, pix_num);
            CFRelease(pix_num);
        }
    }

    VTDecompressionOutputCallbackRecord cb_rec = {
        .decompressionOutputCallback = vt_output_callback,
        .decompressionOutputRefCon = ctx
    };

    OSStatus st = VTDecompressionSessionCreate(
        kCFAllocatorDefault,
        ctx->fmt_desc,
        decoder_spec,
        dst_attrs,
        &cb_rec,
        &ctx->session);
    if (st != noErr && decoder_spec) {
        /* Retry without hardware requirement if hardware decoder block is absent */
        st = VTDecompressionSessionCreate(
            kCFAllocatorDefault,
            ctx->fmt_desc,
            NULL,
            dst_attrs,
            &cb_rec,
            &ctx->session);
    }

    if (decoder_spec) CFRelease(decoder_spec);
    if (dst_attrs) CFRelease(dst_attrs);

    if (st != noErr || !ctx->session) {
        vt_release_session(ctx);
        return -1;
    }
    return 0;
}

/* Creates CMVideoFormatDescription from in-band parameter sets (VPS/SPS/PPS) for VideoToolbox. */
static int vt_rebuild_h264_hevc_format(avx_vt_dec_ctx_t *ctx, bool is_hevc) {
    if (!ctx->sps || !ctx->pps) return -1;

    CMVideoFormatDescriptionRef new_fmt = NULL;
    OSStatus st = -1;
    if (!is_hevc) {
        const uint8_t *ptrs[2] = { ctx->sps, ctx->pps };
        size_t sizes[2] = { ctx->sps_len, ctx->pps_len };
        st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, 2, ptrs, sizes, 4, &new_fmt);
    } else {
        if (!ctx->vps) return -1;
        const uint8_t *ptrs[3] = { ctx->vps, ctx->sps, ctx->pps };
        size_t sizes[3] = { ctx->vps_len, ctx->sps_len, ctx->pps_len };
        st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            kCFAllocatorDefault, 3, ptrs, sizes, 4, NULL, &new_fmt);
    }
    if (st != noErr || !new_fmt) {
        if (new_fmt) CFRelease(new_fmt);
        return -1;
    }
    return vt_ensure_session(ctx, new_fmt);
}

static int vt_rebuild_av1_format(avx_vt_dec_ctx_t *ctx, const uint8_t *seq_obu, size_t seq_len) {
    if (!seq_obu || seq_len == 0) return -1;

    int w = (int)ctx->width;
    int h = (int)ctx->height;
    int prof = 0, lvl = 0;
    avx_rtsp_parse_sequence_header_av1(seq_obu, seq_len, &w, &h, &prof, &lvl);
    if (w <= 0 || h <= 0) return -1;
    ctx->width = (uint32_t)w;
    ctx->height = (uint32_t)h;

    size_t av1c_len = 4 + seq_len;
    uint8_t *av1c = (uint8_t *)malloc(av1c_len);
    if (!av1c) return -1;
    av1c[0] = 0x81;
    av1c[1] = (uint8_t)(((prof & 0x07) << 5) | (lvl & 0x1F));
    av1c[2] = 0x00;
    av1c[3] = 0x00;
    memcpy(av1c + 4, seq_obu, seq_len);

    CFDataRef av1c_data = CFDataCreate(kCFAllocatorDefault, av1c, (CFIndex)av1c_len);
    free(av1c);
    if (!av1c_data) return -1;

    CFMutableDictionaryRef atoms = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef exts = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CMVideoFormatDescriptionRef new_fmt = NULL;
    if (atoms && exts) {
        CFStringRef key_av1c = CFSTR("av1C");
        CFDictionarySetValue(atoms, key_av1c, av1c_data);
        CFDictionarySetValue(exts, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
        CMVideoFormatDescriptionCreate(
            kCFAllocatorDefault, kCMVideoCodecType_AV1, w, h, exts, &new_fmt);
    }

    if (atoms) CFRelease(atoms);
    if (exts) CFRelease(exts);
    CFRelease(av1c_data);

    if (!new_fmt) return -1;
    return vt_ensure_session(ctx, new_fmt);
}

static int vt_dec_create(const avx_video_dec_cfg_t *cfg, void **backend_ctx) {
    if (!cfg || !backend_ctx || !vt_dec_supports_fourcc(cfg->fourcc)) return -1;

    avx_vt_dec_ctx_t *ctx = (avx_vt_dec_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    ctx->src_fourcc = cfg->fourcc;
    ctx->width = cfg->width ? cfg->width : 1920;
    ctx->height = cfg->height ? cfg->height : 1080;
    *backend_ctx = ctx;
    return 0;
}

static int vt_decode_sample_buffer(avx_vt_dec_ctx_t *ctx, const uint8_t *sample_bytes, size_t sample_len,
                                   oavx_video_frame_t *out_frame)
{
    if (!ctx->session || !ctx->fmt_desc || sample_len == 0) {
        return AVX_VIDEO_CODEC_NEED_MORE_DATA;
    }

    CMBlockBufferRef block_buf = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault,
        (void *)sample_bytes,
        sample_len,
        kCFAllocatorNull,
        NULL,
        0,
        sample_len,
        0,
        &block_buf);
    if (st != noErr || !block_buf) {
        if (block_buf) CFRelease(block_buf);
        return AVX_VIDEO_CODEC_ERROR;
    }

    CMSampleBufferRef sample_buf = NULL;
    st = CMSampleBufferCreateReady(
        kCFAllocatorDefault,
        block_buf,
        ctx->fmt_desc,
        1,
        0,
        NULL,
        1,
        &sample_len,
        &sample_buf);
    CFRelease(block_buf);
    if (st != noErr || !sample_buf) {
        if (sample_buf) CFRelease(sample_buf);
        return AVX_VIDEO_CODEC_ERROR;
    }

    ctx->has_frame = false;
    ctx->cb_error = false;
    st = VTDecompressionSessionDecodeFrame(ctx->session, sample_buf, 0, NULL, NULL);
    CFRelease(sample_buf);

    if (st != noErr || ctx->cb_error) {
        return AVX_VIDEO_CODEC_ERROR;
    }
    if (!ctx->has_frame) {
        return AVX_VIDEO_CODEC_NEED_MORE_DATA;
    }

    uint32_t w = ctx->out_width;
    uint32_t h = ctx->out_height;
    out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_NV12;
    out_frame->width = w;
    out_frame->height = h;
    out_frame->planes[0] = ctx->frame_buf;
    out_frame->strides[0] = w;
    out_frame->planes[1] = ctx->frame_buf + (size_t)w * h;
    out_frame->strides[1] = w;
    out_frame->planes[2] = NULL;
    out_frame->strides[2] = 0;
    out_frame->planes[3] = NULL;
    out_frame->strides[3] = 0;
    return AVX_VIDEO_CODEC_OK;
}

/* Packages Annex-B NAL stream into CMSampleBuffer and feeds VideoToolbox hardware decompression. */
static int vt_dec_decode(void *backend_ctx,
                         const uint8_t *pkt_data, size_t pkt_size,
                         oavx_video_frame_t *out_frame)
{
    avx_vt_dec_ctx_t *ctx = (avx_vt_dec_ctx_t *)backend_ctx;
    if (!ctx || !pkt_data || pkt_size == 0 || !out_frame) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    if (ctx->src_fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
        size_t pos = 0;
        if (pkt_size >= 4 && pkt_data[0] == 0 && pkt_data[1] == 0) {
            if (pkt_data[2] == 1) pos = 3;
            else if (pkt_data[2] == 0 && pkt_data[3] == 1) pos = 4;
        }
        while (pos < pkt_size) {
            size_t obu_start = pos;
            uint8_t header = pkt_data[pos++];
            int obu_type = (header >> 3) & 0x0F;
            if (header & 0x04) {
                if (pos >= pkt_size) break;
                pos++;
            }
            size_t obu_len = pkt_size - pos;
            if (header & 0x02) {
                size_t val = 0, shift = 0;
                while (pos < pkt_size) {
                    uint8_t b = pkt_data[pos++];
                    val |= (size_t)(b & 0x7F) << shift;
                    if (!(b & 0x80)) break;
                    shift += 7;
                }
                obu_len = val;
            }
            if (pos + obu_len > pkt_size) break;
            if (obu_type == 1) {
                size_t seq_sz = (pos + obu_len) - obu_start;
                bool seq_changed = false;
                vt_replace_buf(&ctx->av1_seq, &ctx->av1_seq_len, pkt_data + obu_start, seq_sz, &seq_changed);
                if (!ctx->session || seq_changed) {
                    if (vt_rebuild_av1_format(ctx, ctx->av1_seq, ctx->av1_seq_len) != 0) {
                        return AVX_VIDEO_CODEC_ERROR;
                    }
                }
                break;
            }
            if (!(header & 0x02)) break; /* Stop if OBU does not have an explicit length field */
            pos += obu_len;
        }
        if (!ctx->session) {
            return AVX_VIDEO_CODEC_NEED_MORE_DATA;
        }
        return vt_decode_sample_buffer(ctx, pkt_data, pkt_size, out_frame);
    }

    bool is_hevc = (ctx->src_fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || ctx->src_fourcc == 0x43564568);

    size_t max_avcc = pkt_size + 256;
    if (ctx->avcc_cap < max_avcc) {
        uint8_t *nb = (uint8_t *)realloc(ctx->avcc_buf, max_avcc);
        if (!nb) return AVX_VIDEO_CODEC_ERROR;
        ctx->avcc_buf = nb;
        ctx->avcc_cap = max_avcc;
    }

    bool params_changed = false;
    bool saw_start_code = false;
    size_t avcc_len = 0;
    size_t pos = 0;

    while (pos < pkt_size) {
        size_t sc_len = 0;
        if (pos + 3 <= pkt_size && pkt_data[pos] == 0 && pkt_data[pos + 1] == 0) {
            if (pkt_data[pos + 2] == 1) sc_len = 3;
            else if (pos + 4 <= pkt_size && pkt_data[pos + 2] == 0 && pkt_data[pos + 3] == 1) sc_len = 4;
        }

        size_t nalu_start;
        size_t next_pos;
        if (sc_len > 0) {
            saw_start_code = true;
            nalu_start = pos + sc_len;
            next_pos = nalu_start;
            while (next_pos + 3 <= pkt_size) {
                if (pkt_data[next_pos] == 0 && pkt_data[next_pos + 1] == 0 &&
                    (pkt_data[next_pos + 2] == 1 ||
                     (next_pos + 4 <= pkt_size && pkt_data[next_pos + 2] == 0 && pkt_data[next_pos + 3] == 1))) {
                    break;
                }
                next_pos++;
            }
            if (next_pos + 3 > pkt_size) next_pos = pkt_size;
        } else if (!saw_start_code && pos == 0) {
            nalu_start = 0;
            next_pos = pkt_size;
        } else {
            pos++;
            continue;
        }

        size_t nlen = next_pos - nalu_start;
        if (nlen > 0) {
            const uint8_t *np = pkt_data + nalu_start;
            bool is_param = false;
            if (!is_hevc) {
                uint8_t ntype = np[0] & 0x1F;
                if (ntype == 7)      { vt_replace_buf(&ctx->sps, &ctx->sps_len, np, nlen, &params_changed); is_param = true; }
                else if (ntype == 8) { vt_replace_buf(&ctx->pps, &ctx->pps_len, np, nlen, &params_changed); is_param = true; }
            } else {
                uint8_t ntype = (np[0] >> 1) & 0x3F;
                if (ntype == 32)      { vt_replace_buf(&ctx->vps, &ctx->vps_len, np, nlen, &params_changed); is_param = true; }
                else if (ntype == 33) { vt_replace_buf(&ctx->sps, &ctx->sps_len, np, nlen, &params_changed); is_param = true; }
                else if (ntype == 34) { vt_replace_buf(&ctx->pps, &ctx->pps_len, np, nlen, &params_changed); is_param = true; }
            }

            if (!is_param && avcc_len + 4 + nlen <= ctx->avcc_cap) {
                ctx->avcc_buf[avcc_len + 0] = (uint8_t)((nlen >> 24) & 0xFF);
                ctx->avcc_buf[avcc_len + 1] = (uint8_t)((nlen >> 16) & 0xFF);
                ctx->avcc_buf[avcc_len + 2] = (uint8_t)((nlen >> 8) & 0xFF);
                ctx->avcc_buf[avcc_len + 3] = (uint8_t)(nlen & 0xFF);
                memcpy(ctx->avcc_buf + avcc_len + 4, np, nlen);
                avcc_len += 4 + nlen;
            }
        }
        pos = next_pos;
    }

    if ((params_changed || !ctx->session) && ctx->sps && ctx->pps && (!is_hevc || ctx->vps)) {
        if (vt_rebuild_h264_hevc_format(ctx, is_hevc) != 0) {
            return AVX_VIDEO_CODEC_ERROR;
        }
    }

    if (!ctx->session || avcc_len == 0) {
        return AVX_VIDEO_CODEC_NEED_MORE_DATA;
    }

    return vt_decode_sample_buffer(ctx, ctx->avcc_buf, avcc_len, out_frame);
}

static void vt_dec_reset(void *backend_ctx) {
    avx_vt_dec_ctx_t *ctx = (avx_vt_dec_ctx_t *)backend_ctx;
    if (!ctx || !ctx->session) return;
    VTDecompressionSessionWaitForAsynchronousFrames(ctx->session);
}

static const avx_video_decoder_ops_t s_vt_decoder_ops = {
    .name = "videotoolbox",
    .priority = 90,
    .is_hardware = true,
    .supports_fourcc = vt_dec_supports_fourcc,
    .create = vt_dec_create,
    .decode = vt_dec_decode,
    .reset = vt_dec_reset,
    .destroy = vt_dec_destroy
};

const avx_video_decoder_ops_t *avx_video_vt_get_ops(void) {
    return &s_vt_decoder_ops;
}

/* ========================================================================= */
/* VideoToolbox Hardware Video Encoder Backend                               */
/* ========================================================================= */

typedef struct {
    uint32_t                target_fourcc;
    uint32_t                width;
    uint32_t                height;
    int32_t                 fps_num;
    int32_t                 fps_den;
    uint32_t                bitrate_kbps;
    uint8_t                 quality;
    int64_t                 frame_count;

    VTCompressionSessionRef session;
    bool                    force_keyframe;

    uint8_t                *out_buf;
    size_t                  out_cap;
    size_t                  out_size;
    bool                    out_is_keyframe;
    bool                    encode_error;

    uint8_t                *tmp_buf;
    size_t                  tmp_cap;
} avx_vt_enc_ctx_t;

static void vt_compression_output_callback(
    void *outputCallbackRefCon,
    void *sourceFrameRefCon,
    OSStatus status,
    VTEncodeInfoFlags infoFlags,
    CMSampleBufferRef sampleBuffer)
{
    (void)sourceFrameRefCon;
    (void)infoFlags;
    avx_vt_enc_ctx_t *ctx = (avx_vt_enc_ctx_t *)outputCallbackRefCon;
    if (!ctx) return;
    if (status != noErr || !sampleBuffer) {
        ctx->encode_error = true;
        return;
    }

    bool is_key = false;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        is_key = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
    }

    size_t write_pos = 0;

    /* On keyframe, prepend parameter sets (VPS/SPS/PPS) in Annex-B format */
    if (is_key) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (fmt) {
            if (ctx->target_fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) {
                size_t sps_count = 0;
                const uint8_t *sps = NULL;
                size_t sps_size = 0;
                const uint8_t *pps = NULL;
                size_t pps_size = 0;
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, &sps, &sps_size, &sps_count, NULL) == noErr && sps && sps_size > 0) {
                    if (write_pos + 4 + sps_size <= ctx->out_cap) {
                        ctx->out_buf[write_pos++] = 0;
                        ctx->out_buf[write_pos++] = 0;
                        ctx->out_buf[write_pos++] = 0;
                        ctx->out_buf[write_pos++] = 1;
                        memcpy(ctx->out_buf + write_pos, sps, sps_size);
                        write_pos += sps_size;
                    }
                }
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 1, &pps, &pps_size, &sps_count, NULL) == noErr && pps && pps_size > 0) {
                    if (write_pos + 4 + pps_size <= ctx->out_cap) {
                        ctx->out_buf[write_pos++] = 0;
                        ctx->out_buf[write_pos++] = 0;
                        ctx->out_buf[write_pos++] = 0;
                        ctx->out_buf[write_pos++] = 1;
                        memcpy(ctx->out_buf + write_pos, pps, pps_size);
                        write_pos += pps_size;
                    }
                }
            } else if (ctx->target_fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC) {
                size_t param_count = 0;
                if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, 0, NULL, NULL, &param_count, NULL) == noErr) {
                    for (size_t i = 0; i < param_count; ++i) {
                        const uint8_t *param_ptr = NULL;
                        size_t param_size = 0;
                        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fmt, i, &param_ptr, &param_size, NULL, NULL) == noErr && param_ptr && param_size > 0) {
                            if (write_pos + 4 + param_size <= ctx->out_cap) {
                                ctx->out_buf[write_pos++] = 0;
                                ctx->out_buf[write_pos++] = 0;
                                ctx->out_buf[write_pos++] = 0;
                                ctx->out_buf[write_pos++] = 1;
                                memcpy(ctx->out_buf + write_pos, param_ptr, param_size);
                                write_pos += param_size;
                            }
                        }
                    }
                }
            }
        }
    }

    /* Extract slice NALs from CMBlockBuffer and convert AVCC to Annex-B */
    CMBlockBufferRef block_buf = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (block_buf) {
        size_t total_len = CMBlockBufferGetDataLength(block_buf);
        char *data_ptr = NULL;
        CMBlockBufferRef contig_buf = NULL;
        if (CMBlockBufferGetDataPointer(block_buf, 0, NULL, NULL, &data_ptr) != noErr || !data_ptr) {
            if (CMBlockBufferCreateContiguous(kCFAllocatorDefault, block_buf, kCFAllocatorDefault, NULL, 0, 0, 0, &contig_buf) == noErr) {
                CMBlockBufferGetDataPointer(contig_buf, 0, NULL, NULL, &data_ptr);
            }
        }

        if (data_ptr) {
            size_t offset = 0;
            while (offset + 4 <= total_len) {
                uint32_t nal_len = ((uint32_t)(uint8_t)data_ptr[offset + 0] << 24) |
                                   ((uint32_t)(uint8_t)data_ptr[offset + 1] << 16) |
                                   ((uint32_t)(uint8_t)data_ptr[offset + 2] << 8)  |
                                   ((uint32_t)(uint8_t)data_ptr[offset + 3]);
                offset += 4;
                if (offset + nal_len > total_len) break;

                if (write_pos + 4 + nal_len <= ctx->out_cap) {
                    ctx->out_buf[write_pos++] = 0;
                    ctx->out_buf[write_pos++] = 0;
                    ctx->out_buf[write_pos++] = 0;
                    ctx->out_buf[write_pos++] = 1;
                    memcpy(ctx->out_buf + write_pos, data_ptr + offset, nal_len);
                    write_pos += nal_len;
                }
                offset += nal_len;
            }
        }
        if (contig_buf) CFRelease(contig_buf);
    }

    ctx->out_size = write_pos;
    ctx->out_is_keyframe = is_key;
}

static bool vt_enc_supports_fourcc(uint32_t target_fourcc) {
    return (target_fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 ||
            target_fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC);
}

/* Configures Apple VTCompressionSession for real-time low-latency hardware H.264/HEVC encoding. */
static int vt_enc_create(const avx_video_enc_cfg_t *cfg, void **backend_ctx) {
    if (!cfg || !backend_ctx || !vt_enc_supports_fourcc(cfg->fourcc)) return -1;
    if (cfg->width == 0 || cfg->height == 0 ||
        cfg->width > AVX_MAX_VIDEO_WIDTH || cfg->height > AVX_MAX_VIDEO_HEIGHT) {
        return -1;
    }

    avx_vt_enc_ctx_t *ctx = (avx_vt_enc_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;

    ctx->target_fourcc = cfg->fourcc;
    ctx->width = cfg->width;
    ctx->height = cfg->height;
    ctx->fps_num = cfg->fps_num > 0 ? cfg->fps_num : 60;
    ctx->fps_den = cfg->fps_den > 0 ? cfg->fps_den : 1;
    ctx->bitrate_kbps = cfg->bitrate_kbps;
    ctx->quality = cfg->quality;

    CMVideoCodecType codec_type = (cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC)
                                  ? kCMVideoCodecType_HEVC
                                  : kCMVideoCodecType_H264;

    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        (int32_t)cfg->width,
        (int32_t)cfg->height,
        codec_type,
        NULL,
        NULL,
        kCFAllocatorDefault,
        vt_compression_output_callback,
        ctx,
        &ctx->session
    );
    if (status != noErr || !ctx->session) {
        free(ctx);
        return -1;
    }

    /* Low-latency broadcast properties */
    VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    if (cfg->fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) {
        VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    } else {
        VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel);
    }

    /* Framerate */
    float fps = (float)ctx->fps_num / (float)ctx->fps_den;
    CFNumberRef fps_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &fps);
    if (fps_num) {
        VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_ExpectedFrameRate, fps_num);
        CFRelease(fps_num);
    }

    /* Bitrate */
    int32_t br_bps = 0;
    if (cfg->bitrate_kbps > 0) {
        br_bps = (int32_t)(cfg->bitrate_kbps * 1000U);
    } else {
        uint64_t pixels = (uint64_t)cfg->width * cfg->height;
        if (pixels >= 3840ULL * 2160ULL) br_bps = 35000000;
        else if (pixels >= 1920ULL * 1080ULL) br_bps = 12000000;
        else br_bps = 6000000;
    }
    CFNumberRef br_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &br_bps);
    if (br_num) {
        VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_AverageBitRate, br_num);
        CFRelease(br_num);
    }

    /* Keyframe interval */
    int32_t kf_int = cfg->keyframe_interval > 0 ? (int32_t)cfg->keyframe_interval : (int32_t)(fps * 2.0f);
    if (kf_int <= 0) kf_int = 60;
    CFNumberRef kf_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &kf_int);
    if (kf_num) {
        VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, kf_num);
        CFRelease(kf_num);
    }

    VTCompressionSessionPrepareToEncodeFrames(ctx->session);
    *backend_ctx = ctx;
    return 0;
}

static CVPixelBufferRef vt_enc_wrap_pixel_buffer(avx_vt_enc_ctx_t *ctx, const oavx_video_frame_t *in, uint32_t w, uint32_t h) {
    if (!in || !in->planes[0]) return NULL;

    CVPixelBufferRef px_buf = NULL;
    if (in->fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVY) {
        size_t stride = in->strides[0] ? in->strides[0] : (size_t)w * 2ULL;
        CVPixelBufferCreateWithBytes(
            kCFAllocatorDefault, w, h,
            kCVPixelFormatType_422YpCbCr8,
            in->planes[0], stride,
            NULL, NULL, NULL, &px_buf
        );
    } else if (in->fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
        void *ptrs[2] = { in->planes[0], in->planes[1] ? in->planes[1] : (in->planes[0] + (size_t)w * h) };
        size_t widths[2] = { w, (w + 1U) / 2U };
        size_t heights[2] = { h, (h + 1U) / 2U };
        size_t strides[2] = {
            in->strides[0] ? in->strides[0] : (size_t)w,
            in->strides[1] ? in->strides[1] : (size_t)w
        };
        CVPixelBufferCreateWithPlanarBytes(
            kCFAllocatorDefault, w, h,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            NULL, 0,
            2, ptrs, widths, heights, strides,
            NULL, NULL, NULL, &px_buf
        );
    } else if (in->fourcc == OAVX_FOURCC_VIDEO_TYPE_P216) {
        if (ctx && ctx->target_fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) {
            size_t needed = (size_t)w * (size_t)h * 2ULL;
            if (ctx->tmp_cap < needed) {
                uint8_t *nb = (uint8_t *)realloc(ctx->tmp_buf, needed);
                if (!nb) return NULL;
                ctx->tmp_buf = nb;
                ctx->tmp_cap = needed;
            }
            avx_color_p216_to_uyvy(in->planes[0], ctx->tmp_buf, (int)w, (int)h);
            CVPixelBufferCreateWithBytes(
                kCFAllocatorDefault, w, h,
                kCVPixelFormatType_422YpCbCr8,
                ctx->tmp_buf, (size_t)w * 2ULL,
                NULL, NULL, NULL, &px_buf
            );
        } else {
            void *ptrs[2] = { in->planes[0], in->planes[1] ? in->planes[1] : (in->planes[0] + (size_t)w * h * 2ULL) };
            size_t widths[2] = { w, (w + 1U) / 2U };
            size_t heights[2] = { h, h };
            size_t strides[2] = {
                in->strides[0] ? in->strides[0] : (size_t)w * 2ULL,
                in->strides[1] ? in->strides[1] : (size_t)w * 2ULL
            };
            CVPixelBufferCreateWithPlanarBytes(
                kCFAllocatorDefault, w, h,
                kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange,
                NULL, 0,
                2, ptrs, widths, heights, strides,
                NULL, NULL, NULL, &px_buf
            );
        }
    } else if (in->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRA || in->fourcc == OAVX_FOURCC_VIDEO_TYPE_BGRX) {
        size_t stride = in->strides[0] ? in->strides[0] : (size_t)w * 4ULL;
        CVPixelBufferCreateWithBytes(
            kCFAllocatorDefault, w, h,
            kCVPixelFormatType_32BGRA,
            in->planes[0], stride,
            NULL, NULL, NULL, &px_buf
        );
    } else if (in->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBA || in->fourcc == OAVX_FOURCC_VIDEO_TYPE_RGBX) {
        if (ctx) {
            size_t needed = (size_t)w * (size_t)h * 4ULL;
            if (ctx->tmp_cap < needed) {
                uint8_t *nb = (uint8_t *)realloc(ctx->tmp_buf, needed);
                if (!nb) return NULL;
                ctx->tmp_buf = nb;
                ctx->tmp_cap = needed;
            }
            avx_color_rgba_to_bgra(in->planes[0], ctx->tmp_buf, (int)w, (int)h);
            CVPixelBufferCreateWithBytes(
                kCFAllocatorDefault, w, h,
                kCVPixelFormatType_32BGRA,
                ctx->tmp_buf, (size_t)w * 4ULL,
                NULL, NULL, NULL, &px_buf
            );
        }
    } else if (in->fourcc == OAVX_FOURCC_VIDEO_TYPE_I420 || in->fourcc == OAVX_FOURCC_VIDEO_TYPE_YV12) {
        uint32_t cw = (w + 1U) / 2U;
        uint32_t ch = (h + 1U) / 2U;
        void *ptrs[3] = {
            in->planes[0],
            in->planes[1] ? in->planes[1] : (in->planes[0] + (size_t)w * h),
            in->planes[2] ? in->planes[2] : (in->planes[0] + (size_t)w * h + (size_t)cw * ch)
        };
        size_t widths[3] = { w, cw, cw };
        size_t heights[3] = { h, ch, ch };
        size_t strides[3] = {
            in->strides[0] ? in->strides[0] : (size_t)w,
            in->strides[1] ? in->strides[1] : (size_t)cw,
            in->strides[2] ? in->strides[2] : (size_t)cw
        };
        CVPixelBufferCreateWithPlanarBytes(
            kCFAllocatorDefault, w, h,
            kCVPixelFormatType_420YpCbCr8Planar,
            NULL, 0,
            3, ptrs, widths, heights, strides,
            NULL, NULL, NULL, &px_buf
        );
    }
    return px_buf;
}

/* Wraps input frame in CVPixelBufferRef, submits to VTCompressionSession, and drains NAL packets. */
static int vt_enc_encode(void *backend_ctx,
                         const oavx_video_frame_t *in_frame,
                         uint8_t *out_buf, size_t out_cap,
                         size_t *out_size, bool *out_is_keyframe)
{
    avx_vt_enc_ctx_t *ctx = (avx_vt_enc_ctx_t *)backend_ctx;
    if (!ctx || !ctx->session || !in_frame || !out_buf || !out_size || out_cap < 16) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    uint32_t w = in_frame->width ? in_frame->width : ctx->width;
    uint32_t h = in_frame->height ? in_frame->height : ctx->height;
    if (w != ctx->width || h != ctx->height) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    CVPixelBufferRef px_buf = vt_enc_wrap_pixel_buffer(ctx, in_frame, w, h);
    if (!px_buf) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    ctx->out_buf = out_buf;
    ctx->out_cap = out_cap;
    ctx->out_size = 0;
    ctx->out_is_keyframe = false;
    ctx->encode_error = false;

    CMTime pts = CMTimeMake(ctx->frame_count++, ctx->fps_den > 0 ? (ctx->fps_num / ctx->fps_den) : 30);

    CFDictionaryRef frame_props = NULL;
    if (ctx->force_keyframe) {
        const void *keys[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
        const void *vals[] = { kCFBooleanTrue };
        frame_props = CFDictionaryCreate(
            kCFAllocatorDefault, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
        );
        ctx->force_keyframe = false;
    }

    VTEncodeInfoFlags flags = 0;
    OSStatus st = VTCompressionSessionEncodeFrame(
        ctx->session,
        px_buf,
        pts,
        kCMTimeInvalid,
        frame_props,
        NULL,
        &flags
    );
    if (frame_props) CFRelease(frame_props);
    CVPixelBufferRelease(px_buf);

    if (st != noErr) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    /* Synchronously complete pending frames for immediate zero-delay output */
    VTCompressionSessionCompleteFrames(ctx->session, kCMTimeInvalid);

    if (ctx->encode_error || ctx->out_size == 0) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    *out_size = ctx->out_size;
    if (out_is_keyframe) *out_is_keyframe = ctx->out_is_keyframe;
    return AVX_VIDEO_CODEC_OK;
}

static int vt_enc_set_quality(void *backend_ctx, uint8_t quality) {
    avx_vt_enc_ctx_t *ctx = (avx_vt_enc_ctx_t *)backend_ctx;
    if (!ctx || !ctx->session) return -1;
    ctx->quality = quality;
    float q = (float)quality / 100.0f;
    CFNumberRef q_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &q);
    if (q_num) {
        VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_Quality, q_num);
        CFRelease(q_num);
    }
    return 0;
}

static void vt_enc_request_keyframe(void *backend_ctx) {
    avx_vt_enc_ctx_t *ctx = (avx_vt_enc_ctx_t *)backend_ctx;
    if (ctx) ctx->force_keyframe = true;
}

static void vt_enc_destroy(void *backend_ctx) {
    avx_vt_enc_ctx_t *ctx = (avx_vt_enc_ctx_t *)backend_ctx;
    if (!ctx) return;
    if (ctx->session) {
        VTCompressionSessionInvalidate(ctx->session);
        CFRelease(ctx->session);
        ctx->session = NULL;
    }
    if (ctx->tmp_buf) {
        free(ctx->tmp_buf);
        ctx->tmp_buf = NULL;
    }
    free(ctx);
}

static const avx_video_encoder_ops_t s_vt_encoder_ops = {
    .name = "videotoolbox",
    .priority = 90,
    .is_hardware = true,
    .supports_fourcc = vt_enc_supports_fourcc,
    .create = vt_enc_create,
    .encode = vt_enc_encode,
    .set_quality = vt_enc_set_quality,
    .request_keyframe = vt_enc_request_keyframe,
    .destroy = vt_enc_destroy
};

const avx_video_encoder_ops_t *avx_video_vt_get_encoder_ops(void) {
    return &s_vt_encoder_ops;
}

#else

const avx_video_decoder_ops_t *avx_video_vt_get_ops(void) {
    return NULL;
}

const avx_video_encoder_ops_t *avx_video_vt_get_encoder_ops(void) {
    return NULL;
}

#endif /* AVX_OS_MACOS */
