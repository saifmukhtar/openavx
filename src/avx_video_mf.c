/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_video_mf.c - Windows Media Foundation Hardware / Inbox Video Decoder Backend
 */

#include "avx_video_codec.h"
#include "avx_internal.h"

#if defined(AVX_OS_WINDOWS)

#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *pfn_CoInitializeEx)(LPVOID, DWORD);
typedef void    (WINAPI *pfn_CoTaskMemFree)(LPVOID);
typedef HRESULT (WINAPI *pfn_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *pfn_MFShutdown)(void);
typedef HRESULT (WINAPI *pfn_MFTEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
                                        const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *);
typedef HRESULT (WINAPI *pfn_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT (WINAPI *pfn_MFCreateSample)(IMFSample **);
typedef HRESULT (WINAPI *pfn_MFCreateMemoryBuffer)(DWORD, IMFMediaBuffer **);

static avx_mutex_t s_mf_lock = AVX_MUTEX_INITIALIZER;
static bool        s_mf_loaded = false;
static bool        s_mf_failed = false;
static HMODULE     s_ole32_dll = NULL;
static HMODULE     s_mfplat_dll = NULL;

static pfn_CoInitializeEx     p_CoInitializeEx = NULL;
static pfn_CoTaskMemFree      p_CoTaskMemFree = NULL;
static pfn_MFStartup          p_MFStartup = NULL;
static pfn_MFShutdown         p_MFShutdown = NULL;
static pfn_MFTEnumEx          p_MFTEnumEx = NULL;
static pfn_MFCreateMediaType  p_MFCreateMediaType = NULL;
static pfn_MFCreateSample     p_MFCreateSample = NULL;
static pfn_MFCreateMemoryBuffer p_MFCreateMemoryBuffer = NULL;

/* Dynamically binds Windows COM and Media Foundation runtime DLLs (ole32.dll, mfplat.dll). */
static bool mf_load_runtime(void) {
    avx_mutex_lock(&s_mf_lock);
    if (s_mf_loaded) {
        avx_mutex_unlock(&s_mf_lock);
        return true;
    }
    if (s_mf_failed) {
        avx_mutex_unlock(&s_mf_lock);
        return false;
    }

    s_ole32_dll = LoadLibraryW(L"ole32.dll");
    s_mfplat_dll = LoadLibraryW(L"mfplat.dll");
    if (!s_ole32_dll || !s_mfplat_dll) {
        s_mf_failed = true;
        avx_mutex_unlock(&s_mf_lock);
        return false;
    }

    p_CoInitializeEx       = (pfn_CoInitializeEx)(void *)GetProcAddress(s_ole32_dll, "CoInitializeEx");
    p_CoTaskMemFree        = (pfn_CoTaskMemFree)(void *)GetProcAddress(s_ole32_dll, "CoTaskMemFree");
    p_MFStartup            = (pfn_MFStartup)(void *)GetProcAddress(s_mfplat_dll, "MFStartup");
    p_MFShutdown           = (pfn_MFShutdown)(void *)GetProcAddress(s_mfplat_dll, "MFShutdown");
    p_MFTEnumEx            = (pfn_MFTEnumEx)(void *)GetProcAddress(s_mfplat_dll, "MFTEnumEx");
    p_MFCreateMediaType    = (pfn_MFCreateMediaType)(void *)GetProcAddress(s_mfplat_dll, "MFCreateMediaType");
    p_MFCreateSample       = (pfn_MFCreateSample)(void *)GetProcAddress(s_mfplat_dll, "MFCreateSample");
    p_MFCreateMemoryBuffer = (pfn_MFCreateMemoryBuffer)(void *)GetProcAddress(s_mfplat_dll, "MFCreateMemoryBuffer");

    if (!p_CoInitializeEx || !p_CoTaskMemFree || !p_MFStartup || !p_MFTEnumEx ||
        !p_MFCreateMediaType || !p_MFCreateSample || !p_MFCreateMemoryBuffer) {
        s_mf_failed = true;
        avx_mutex_unlock(&s_mf_lock);
        return false;
    }

    HRESULT hr = p_CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        s_mf_failed = true;
        avx_mutex_unlock(&s_mf_lock);
        return false;
    }

    if (FAILED(p_MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
        s_mf_failed = true;
        avx_mutex_unlock(&s_mf_lock);
        return false;
    }

    s_mf_loaded = true;
    avx_mutex_unlock(&s_mf_lock);
    return true;
}

typedef struct {
    IMFTransform         *mft;
    IMFMediaEventGenerator *evgen;
    bool                  is_async;
    bool                  provider_allocs;
    uint32_t              src_fourcc;
    uint32_t              width;
    uint32_t              height;
    uint32_t              stride;
    GUID                  out_subtype;
    DWORD                 out_buf_size;
    int                   pending_inputs;
    uint8_t              *frame_buf;
    size_t                frame_buf_size;
} avx_mf_dec_ctx_t;

static bool mf_fourcc_to_guid(uint32_t fourcc, GUID *out_guid) {
    if (fourcc == OAVX_FOURCC_VIDEO_TYPE_H264 || fourcc == 0x34363268) {
        *out_guid = MFVideoFormat_H264;
        return true;
    } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_HEVC || fourcc == 0x43564568) {
        *out_guid = MFVideoFormat_HEVC;
        return true;
    } else if (fourcc == OAVX_FOURCC_VIDEO_TYPE_AV01) {
        *out_guid = MFVideoFormat_AV1;
        return true;
    }
    return false;
}

static int mf_select_output_type(avx_mf_dec_ctx_t *ctx) {
    for (DWORD idx = 0; ; ++idx) {
        IMFMediaType *mt = NULL;
        HRESULT hr = IMFTransform_GetOutputAvailableType(ctx->mft, 0, idx, &mt);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr) || !mt) break;

        GUID sub = {0};
        if (SUCCEEDED(IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, &sub))) {
            if (IsEqualGUID(&sub, &MFVideoFormat_NV12) || IsEqualGUID(&sub, &MFVideoFormat_I420)) {
                if (SUCCEEDED(IMFTransform_SetOutputType(ctx->mft, 0, mt, 0))) {
                    ctx->out_subtype = sub;
                    UINT64 fsize = 0;
                    if (SUCCEEDED(IMFMediaType_GetUINT64(mt, &MF_MT_FRAME_SIZE, &fsize))) {
                        UINT32 w = (UINT32)(fsize >> 32);
                        UINT32 h = (UINT32)(fsize & 0xFFFFFFFFU);
                        if (w > 0 && h > 0) {
                            ctx->width = w;
                            ctx->height = h;
                        }
                    }
                    UINT32 def_stride_u32 = 0;
                    if (SUCCEEDED(IMFMediaType_GetUINT32(mt, &MF_MT_DEFAULT_STRIDE, &def_stride_u32))) {
                        INT32 s_signed = (INT32)def_stride_u32;
                        UINT32 abs_s = (s_signed < 0) ? (UINT32)(-s_signed) : (UINT32)s_signed;
                        ctx->stride = (abs_s >= ctx->width) ? abs_s : ctx->width;
                    } else {
                        ctx->stride = ctx->width;
                    }

                    MFT_OUTPUT_STREAM_INFO osi;
                    memset(&osi, 0, sizeof(osi));
                    if (SUCCEEDED(IMFTransform_GetOutputStreamInfo(ctx->mft, 0, &osi))) {
                        ctx->out_buf_size = osi.cbSize;
                        ctx->provider_allocs = ((osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0);
                    }
                    IMFMediaType_Release(mt);
                    return 0;
                }
            }
        }
        IMFMediaType_Release(mt);
    }
    return -1;
}

static bool mf_dec_supports_fourcc(uint32_t src_fourcc) {
    GUID in_sub = {0};
    if (!mf_fourcc_to_guid(src_fourcc, &in_sub) || !mf_load_runtime()) {
        return false;
    }

    MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video, in_sub };
    IMFActivate **acts = NULL;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT |
                   MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    HRESULT hr = p_MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &in_info, NULL, &acts, &count);
    if (SUCCEEDED(hr) && acts) {
        for (UINT32 i = 0; i < count; ++i) {
            if (acts[i]) IMFActivate_Release(acts[i]);
        }
        p_CoTaskMemFree(acts);
        return count > 0;
    }
    return false;
}

static void mf_dec_destroy(void *backend_ctx) {
    avx_mf_dec_ctx_t *ctx = (avx_mf_dec_ctx_t *)backend_ctx;
    if (!ctx) return;

    if (ctx->mft) {
        IMFTransform_ProcessMessage(ctx->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(ctx->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (ctx->evgen) {
            IMFMediaEventGenerator_Release(ctx->evgen);
            ctx->evgen = NULL;
        }
        IMFTransform_Release(ctx->mft);
        ctx->mft = NULL;
    }
    free(ctx->frame_buf);
    free(ctx);
}

/* Enumerates and activates Media Foundation Transforms (MFTEnumEx), preferring hardware decoders. */
static int mf_try_activate_mft(avx_mf_dec_ctx_t *ctx, const GUID *in_sub, UINT32 flags) {
    MFT_REGISTER_TYPE_INFO in_info = { MFMediaType_Video, *in_sub };
    IMFActivate **acts = NULL;
    UINT32 count = 0;
    if (FAILED(p_MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &in_info, NULL, &acts, &count)) || !acts) {
        return -1;
    }

    int rc = -1;
    for (UINT32 i = 0; i < count; ++i) {
        IMFTransform *mft = NULL;
        if (!acts[i] || FAILED(IMFActivate_ActivateObject(acts[i], &IID_IMFTransform, (void **)&mft)) || !mft) {
            continue;
        }

        IMFAttributes *attrs = NULL;
        bool is_async = false;
        IMFMediaEventGenerator *evgen = NULL;
        if (SUCCEEDED(IMFTransform_GetAttributes(mft, &attrs)) && attrs) {
            UINT32 async_flag = 0;
            if (SUCCEEDED(IMFAttributes_GetUINT32(attrs, &MF_TRANSFORM_ASYNC, &async_flag)) && async_flag) {
                is_async = true;
                IMFAttributes_SetUINT32(attrs, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            }
            IMFAttributes_SetUINT32(attrs, &MF_LOW_LATENCY, TRUE);
            IMFAttributes_Release(attrs);
        }

        if (is_async) {
            if (FAILED(IMFTransform_QueryInterface(mft, &IID_IMFMediaEventGenerator, (void **)&evgen)) || !evgen) {
                IMFTransform_Release(mft);
                continue;
            }
        }

        IMFMediaType *in_mt = NULL;
        if (FAILED(p_MFCreateMediaType(&in_mt)) || !in_mt) {
            if (evgen) IMFMediaEventGenerator_Release(evgen);
            IMFTransform_Release(mft);
            continue;
        }
        IMFMediaType_SetGUID(in_mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(in_mt, &MF_MT_SUBTYPE, in_sub);
        IMFMediaType_SetUINT32(in_mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        UINT64 fsize = ((UINT64)ctx->width << 32) | (UINT64)ctx->height;
        IMFMediaType_SetUINT64(in_mt, &MF_MT_FRAME_SIZE, fsize);

        HRESULT hr = IMFTransform_SetInputType(mft, 0, in_mt, 0);
        IMFMediaType_Release(in_mt);
        if (FAILED(hr)) {
            if (evgen) IMFMediaEventGenerator_Release(evgen);
            IMFTransform_Release(mft);
            continue;
        }

        ctx->mft = mft;
        ctx->evgen = evgen;
        ctx->is_async = is_async;
        if (mf_select_output_type(ctx) != 0) {
            ctx->mft = NULL;
            ctx->evgen = NULL;
            ctx->is_async = false;
            if (evgen) IMFMediaEventGenerator_Release(evgen);
            IMFTransform_Release(mft);
            continue;
        }

        IMFTransform_ProcessMessage(mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        IMFTransform_ProcessMessage(mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        rc = 0;
        break;
    }

    for (UINT32 i = 0; i < count; ++i) {
        if (acts[i]) IMFActivate_Release(acts[i]);
    }
    p_CoTaskMemFree(acts);
    return rc;
}

static int mf_dec_create(const avx_video_dec_cfg_t *cfg, void **backend_ctx) {
    if (!cfg || !backend_ctx) return -1;

    GUID in_sub = {0};
    if (!mf_fourcc_to_guid(cfg->fourcc, &in_sub) || !mf_load_runtime()) {
        return -1;
    }

    p_CoInitializeEx(NULL, COINIT_MULTITHREADED);

    avx_mf_dec_ctx_t *ctx = (avx_mf_dec_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->src_fourcc = cfg->fourcc;
    ctx->width = cfg->width ? cfg->width : 1920;
    ctx->height = cfg->height ? cfg->height : 1080;

    if (mf_try_activate_mft(ctx, &in_sub,
                            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER) != 0 &&
        mf_try_activate_mft(ctx, &in_sub,
                            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER) != 0) {
        mf_dec_destroy(ctx);
        return -1;
    }

    *backend_ctx = ctx;
    return 0;
}

static int mf_drain_async_events(avx_mf_dec_ctx_t *ctx, bool *have_output) {
    if (!ctx->is_async || !ctx->evgen) return 0;

    for (int iter = 0; iter < 16; ++iter) {
        IMFMediaEvent *ev = NULL;
        HRESULT hr = IMFMediaEventGenerator_GetEvent(ctx->evgen, MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE || FAILED(hr) || !ev) break;

        MediaEventType met = MEUnknown;
        IMFMediaEvent_GetType(ev, &met);
        IMFMediaEvent_Release(ev);

        if (met == METransformNeedInput) {
            ctx->pending_inputs++;
        } else if (met == METransformHaveOutput) {
            *have_output = true;
        }
    }
    return 0;
}

/* Copies decoded IMFSample picture buffer to destination frame, adjusting for stride alignment. */
static int mf_copy_sample_to_frame(avx_mf_dec_ctx_t *ctx, IMFSample *sample, oavx_video_frame_t *out_frame) {
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) || !buf) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    BYTE *src_ptr = NULL;
    DWORD src_len = 0;
    LONG pitch = (LONG)(ctx->stride ? ctx->stride : ctx->width);
    IMF2DBuffer *buf2d = NULL;
    bool locked_2d = false;

    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&buf2d)) && buf2d) {
        if (SUCCEEDED(IMF2DBuffer_Lock2D(buf2d, &src_ptr, &pitch))) {
            locked_2d = true;
        } else {
            IMF2DBuffer_Release(buf2d);
            buf2d = NULL;
        }
    }
    if (!locked_2d && FAILED(IMFMediaBuffer_Lock(buf, &src_ptr, NULL, &src_len))) {
        IMFMediaBuffer_Release(buf);
        return AVX_VIDEO_CODEC_ERROR;
    }
    if (pitch < 0) pitch = -pitch;

    uint32_t w = ctx->width;
    uint32_t h = ctx->height;
    uint64_t need_u64 = (uint64_t)w * (uint64_t)h * 2ULL;
    size_t s = (size_t)pitch;
    if (w == 0 || h == 0 || w > AVX_MAX_VIDEO_WIDTH || h > AVX_MAX_VIDEO_HEIGHT ||
        need_u64 > AVX_MAX_DECOMPRESSED_FRAME_SIZE || s < (size_t)w) {
        if (locked_2d) { IMF2DBuffer_Unlock2D(buf2d); IMF2DBuffer_Release(buf2d); }
        else           { IMFMediaBuffer_Unlock(buf); }
        IMFMediaBuffer_Release(buf);
        return AVX_VIDEO_CODEC_ERROR;
    }

    bool is_nv12 = IsEqualGUID(&ctx->out_subtype, &MFVideoFormat_NV12);
    uint32_t cw = (w + 1U) / 2U;
    uint32_t ch = (h + 1U) / 2U;
    size_t need = is_nv12
        ? ((size_t)w * (size_t)h + (size_t)w * (size_t)ch)
        : ((size_t)w * (size_t)h + 2ULL * (size_t)cw * (size_t)ch);

    if (!locked_2d) {
        size_t min_src = is_nv12
            ? ((size_t)h * s + (size_t)ch * s)
            : ((size_t)h * s + 2ULL * (size_t)ch * (s / 2U));
        if ((size_t)src_len < min_src) {
            IMFMediaBuffer_Unlock(buf);
            IMFMediaBuffer_Release(buf);
            return AVX_VIDEO_CODEC_ERROR;
        }
    }

    if (ctx->frame_buf_size < need) {
        uint8_t *nb = (uint8_t *)realloc(ctx->frame_buf, need);
        if (!nb) {
            if (locked_2d) { IMF2DBuffer_Unlock2D(buf2d); IMF2DBuffer_Release(buf2d); }
            else           { IMFMediaBuffer_Unlock(buf); }
            IMFMediaBuffer_Release(buf);
            return AVX_VIDEO_CODEC_ERROR;
        }
        ctx->frame_buf = nb;
        ctx->frame_buf_size = need;
    }

    for (uint32_t y = 0; y < h; ++y) {
        memcpy(ctx->frame_buf + (size_t)y * w, src_ptr + (size_t)y * s, w);
    }

    if (is_nv12) {
        const uint8_t *src_uv = src_ptr + (size_t)h * s;
        uint8_t *dst_uv = ctx->frame_buf + (size_t)w * h;
        for (uint32_t y = 0; y < ch; ++y) {
            memcpy(dst_uv + (size_t)y * w, src_uv + (size_t)y * s, w);
        }
        out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_NV12;
        out_frame->planes[0] = ctx->frame_buf;
        out_frame->strides[0] = w;
        out_frame->planes[1] = dst_uv;
        out_frame->strides[1] = w;
        out_frame->planes[2] = NULL;
        out_frame->strides[2] = 0;
    } else {
        size_t cs = s / 2;
        const uint8_t *src_u = src_ptr + (size_t)h * s;
        const uint8_t *src_v = src_u + (size_t)ch * cs;
        uint8_t *dst_u = ctx->frame_buf + (size_t)w * h;
        uint8_t *dst_v = dst_u + (size_t)cw * ch;
        for (uint32_t y = 0; y < ch; ++y) memcpy(dst_u + (size_t)y * cw, src_u + (size_t)y * cs, cw);
        for (uint32_t y = 0; y < ch; ++y) memcpy(dst_v + (size_t)y * cw, src_v + (size_t)y * cs, cw);
        out_frame->fourcc = OAVX_FOURCC_VIDEO_TYPE_I420;
        out_frame->planes[0] = ctx->frame_buf;
        out_frame->strides[0] = w;
        out_frame->planes[1] = dst_u;
        out_frame->strides[1] = cw;
        out_frame->planes[2] = dst_v;
        out_frame->strides[2] = cw;
    }
    out_frame->width = w;
    out_frame->height = h;
    out_frame->planes[3] = NULL;
    out_frame->strides[3] = 0;

    if (locked_2d) { IMF2DBuffer_Unlock2D(buf2d); IMF2DBuffer_Release(buf2d); }
    else           { IMFMediaBuffer_Unlock(buf); }
    IMFMediaBuffer_Release(buf);
    return AVX_VIDEO_CODEC_OK;
}

/* Submits compressed packet to MFT via ProcessInput and drains decoded pictures via ProcessOutput. */
static int mf_dec_decode(void *backend_ctx,
                         const uint8_t *pkt_data, size_t pkt_size,
                         oavx_video_frame_t *out_frame)
{
    avx_mf_dec_ctx_t *ctx = (avx_mf_dec_ctx_t *)backend_ctx;
    if (!ctx || !ctx->mft || !pkt_data || pkt_size == 0 ||
        pkt_size > (size_t)0xFFFFFFFFU || !out_frame) {
        return AVX_VIDEO_CODEC_ERROR;
    }

    p_CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMFSample *in_sample = NULL;
    IMFMediaBuffer *in_buf = NULL;
    if (FAILED(p_MFCreateSample(&in_sample)) || !in_sample ||
        FAILED(p_MFCreateMemoryBuffer((DWORD)pkt_size, &in_buf)) || !in_buf) {
        if (in_sample) IMFSample_Release(in_sample);
        if (in_buf) IMFMediaBuffer_Release(in_buf);
        return AVX_VIDEO_CODEC_ERROR;
    }

    BYTE *dst = NULL;
    if (FAILED(IMFMediaBuffer_Lock(in_buf, &dst, NULL, NULL))) {
        IMFMediaBuffer_Release(in_buf);
        IMFSample_Release(in_sample);
        return AVX_VIDEO_CODEC_ERROR;
    }
    memcpy(dst, pkt_data, pkt_size);
    IMFMediaBuffer_Unlock(in_buf);
    IMFMediaBuffer_SetCurrentLength(in_buf, (DWORD)pkt_size);
    IMFSample_AddBuffer(in_sample, in_buf);
    IMFMediaBuffer_Release(in_buf);

    bool have_output = false;
    if (ctx->is_async) {
        mf_drain_async_events(ctx, &have_output);
    }

    HRESULT hr_in = IMFTransform_ProcessInput(ctx->mft, 0, in_sample, 0);
    bool retry_input = (hr_in == MF_E_NOTACCEPTING);
    if (FAILED(hr_in) && !retry_input) {
        IMFSample_Release(in_sample);
        return AVX_VIDEO_CODEC_ERROR;
    }
    if (!retry_input) {
        IMFSample_Release(in_sample);
        in_sample = NULL;
        if (ctx->pending_inputs > 0) ctx->pending_inputs--;
    }

    for (int attempt = 0; attempt < 12; ++attempt) {
        if (ctx->is_async) {
            mf_drain_async_events(ctx, &have_output);
            if (retry_input && ctx->pending_inputs > 0) {
                HRESULT hr_retry = IMFTransform_ProcessInput(ctx->mft, 0, in_sample, 0);
                if (SUCCEEDED(hr_retry)) {
                    IMFSample_Release(in_sample);
                    in_sample = NULL;
                    retry_input = false;
                    ctx->pending_inputs--;
                }
            }
        }

        MFT_OUTPUT_DATA_BUFFER out_data;
        memset(&out_data, 0, sizeof(out_data));
        IMFSample *caller_sample = NULL;
        if (!ctx->provider_allocs) {
            DWORD alloc_sz = ctx->out_buf_size ? ctx->out_buf_size : (ctx->width * ctx->height * 2U);
            IMFMediaBuffer *ob = NULL;
            if (SUCCEEDED(p_MFCreateSample(&caller_sample)) &&
                SUCCEEDED(p_MFCreateMemoryBuffer(alloc_sz, &ob))) {
                IMFSample_AddBuffer(caller_sample, ob);
                IMFMediaBuffer_Release(ob);
                out_data.pSample = caller_sample;
            } else {
                if (caller_sample) IMFSample_Release(caller_sample);
                if (ob) IMFMediaBuffer_Release(ob);
                if (in_sample) IMFSample_Release(in_sample);
                return AVX_VIDEO_CODEC_ERROR;
            }
        }

        DWORD status = 0;
        HRESULT hr_out = IMFTransform_ProcessOutput(ctx->mft, 0, 1, &out_data, &status);
        if (out_data.pEvents) IMFCollection_Release(out_data.pEvents);

        if (hr_out == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (caller_sample) IMFSample_Release(caller_sample);
            if (mf_select_output_type(ctx) != 0) {
                if (in_sample) IMFSample_Release(in_sample);
                return AVX_VIDEO_CODEC_ERROR;
            }
            continue;
        }
        if (hr_out == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (caller_sample) IMFSample_Release(caller_sample);
            if (retry_input) {
                HRESULT hr2 = IMFTransform_ProcessInput(ctx->mft, 0, in_sample, 0);
                if (SUCCEEDED(hr2)) {
                    IMFSample_Release(in_sample);
                    in_sample = NULL;
                    retry_input = false;
                    continue;
                }
                if (ctx->is_async && hr2 == MF_E_NOTACCEPTING) {
                    Sleep(1);
                    continue;
                }
                IMFSample_Release(in_sample);
                return AVX_VIDEO_CODEC_ERROR;
            }
            return AVX_VIDEO_CODEC_NEED_MORE_DATA;
        }
        if (FAILED(hr_out) || !out_data.pSample) {
            if (caller_sample) IMFSample_Release(caller_sample);
            if (ctx->is_async && retry_input) {
                Sleep(1);
                continue;
            }
            if (in_sample) IMFSample_Release(in_sample);
            return AVX_VIDEO_CODEC_NEED_MORE_DATA;
        }

        int rc = mf_copy_sample_to_frame(ctx, out_data.pSample, out_frame);
        IMFSample_Release(out_data.pSample);
        if (retry_input && in_sample) {
            IMFTransform_ProcessInput(ctx->mft, 0, in_sample, 0);
            IMFSample_Release(in_sample);
            in_sample = NULL;
        }
        return rc;
    }

    if (in_sample) IMFSample_Release(in_sample);
    return AVX_VIDEO_CODEC_NEED_MORE_DATA;
}

static void mf_dec_reset(void *backend_ctx) {
    avx_mf_dec_ctx_t *ctx = (avx_mf_dec_ctx_t *)backend_ctx;
    if (!ctx || !ctx->mft) return;
    IMFTransform_ProcessMessage(ctx->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    IMFTransform_ProcessMessage(ctx->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

static const avx_video_decoder_ops_t s_mf_decoder_ops = {
    .name = "mediafoundation",
    .priority = 90,
    .is_hardware = true,
    .supports_fourcc = mf_dec_supports_fourcc,
    .create = mf_dec_create,
    .decode = mf_dec_decode,
    .reset = mf_dec_reset,
    .destroy = mf_dec_destroy
};

const avx_video_decoder_ops_t *avx_video_mf_get_ops(void) {
    return &s_mf_decoder_ops;
}

#else

const avx_video_decoder_ops_t *avx_video_mf_get_ops(void) {
    return NULL;
}

#endif /* AVX_OS_WINDOWS */
