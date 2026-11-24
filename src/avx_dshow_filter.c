/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_dshow_filter.c - Windows DirectShow Virtual Camera COM In-Process Server DLL
 */

#if defined(AVX_OS_WINDOWS) || defined(__MINGW32__) || defined(__MINGW64__)

#define _WIN32_WINNT 0x0601
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <initguid.h>
#include <strmif.h>
#include <amvideo.h>
#include <uuids.h>
#include <vfwmsgs.h>

#include "openavx.h"
#include "avx_dshow_sink.h"
#include "avx_virtcam_ipc.h"
#include "avx_platform.h"

#ifndef FOURCC_YUY2
#define FOURCC_YUY2 0x32595559 /* "YUY2" */
#endif
#ifndef FOURCC_UYVY
#define FOURCC_UYVY 0x59565955 /* "UYVY" */
#endif

/* ========================================================================= */
/* GUID DEFINITIONS                                                          */
/* ========================================================================= */

/* CLSID_OpenAVXCam: {9C86F6B0-80E2-4AE8-B937-2E8B9915D2C1} */
DEFINE_GUID(CLSID_OpenAVXCam,
    0x9c86f6b0, 0x80e2, 0x4ae8, 0xb9, 0x37, 0x2e, 0x8b, 0x99, 0x15, 0xd2, 0xc1);

static volatile LONG g_cComponents = 0;
static volatile LONG g_cServerLocks = 0;
static HINSTANCE g_hInst = NULL;

/* Forward declarations */
typedef struct OpenAVXFilter OpenAVXFilter;
typedef struct OpenAVXPin OpenAVXPin;

/* ========================================================================= */
/* PIN IMPLEMENTATION (Output Capture Pin)                                  */
/* ========================================================================= */

struct OpenAVXPin {
    IPin                 lpVtbl_Pin;
    IAMStreamConfig      lpVtbl_StreamConfig;
    IKsPropertySet       lpVtbl_KsPropertySet;

    LONG                 refCount;
    OpenAVXFilter       *pFilter;
    IPin                *pConnectedPin;
    IMemInputPin        *pMemInputPin;
    IMemAllocator       *pAllocator;
    AM_MEDIA_TYPE        currentMediaType;
    bool                 bConnected;
};

/* ========================================================================= */
/* FILTER IMPLEMENTATION (IBaseFilter Source Filter)                         */
/* ========================================================================= */

struct OpenAVXFilter {
    IBaseFilter          lpVtbl_Filter;
    LONG                 refCount;

    FILTER_STATE         state;
    IFilterGraph        *pGraph;
    IReferenceClock     *pClock;
    OpenAVXPin           pin;

    /* Worker Streaming Thread */
    HANDLE               hThread;
    HANDLE               hStopEvent;
    avx_virtcam_consumer_t *consumer;
    CRITICAL_SECTION     lock;
};

/* ========================================================================= */
/* MEDIA TYPE HELPERS                                                        */
/* ========================================================================= */

static void init_video_media_type(AM_MEDIA_TYPE *pmt, DWORD width, DWORD height, DWORD fourcc_code)
{
    memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
    pmt->majortype = MEDIATYPE_Video;
    pmt->subtype   = (fourcc_code == FOURCC_YUY2) ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_UYVY;
    pmt->bFixedSizeSamples    = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->lSampleSize          = width * height * 2;
    pmt->formattype           = FORMAT_VideoInfo;

    VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!pvi) return;
    memset(pvi, 0, sizeof(VIDEOINFOHEADER));
    pvi->rcSource.right  = width;
    pvi->rcSource.bottom = height;
    pvi->rcTarget.right  = width;
    pvi->rcTarget.bottom = height;
    pvi->AvgTimePerFrame = 333333; /* 30 fps (100ns units) */

    BITMAPINFOHEADER *bmi = &pvi->bmiHeader;
    bmi->biSize        = sizeof(BITMAPINFOHEADER);
    bmi->biWidth       = width;
    bmi->biHeight      = height;
    bmi->biPlanes      = 1;
    bmi->biBitCount    = 16;
    bmi->biCompression = (fourcc_code == FOURCC_YUY2) ? FOURCC_YUY2 : FOURCC_UYVY;
    bmi->biSizeImage   = width * height * 2;

    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pbFormat = (BYTE*)pvi;
}

static void free_media_type(AM_MEDIA_TYPE *pmt)
{
    if (pmt->cbFormat && pmt->pbFormat) {
        CoTaskMemFree(pmt->pbFormat);
        pmt->pbFormat = NULL;
        pmt->cbFormat = 0;
    }
}

static HRESULT copy_media_type(AM_MEDIA_TYPE *pDest, const AM_MEDIA_TYPE *pSrc)
{
    *pDest = *pSrc;
    if (pSrc->cbFormat > 0 && pSrc->pbFormat) {
        pDest->pbFormat = (BYTE*)CoTaskMemAlloc(pSrc->cbFormat);
        if (!pDest->pbFormat) return E_OUTOFMEMORY;
        memcpy(pDest->pbFormat, pSrc->pbFormat, pSrc->cbFormat);
    }
    return S_OK;
}

/* ========================================================================= */
/* STREAMING THREAD WORKER                                                   */
/* ========================================================================= */

/* Worker thread reading shared memory frames and pushing IMediaSample downstream to filter graph. */
static DWORD WINAPI StreamingThreadProc(LPVOID lpParam)
{
    OpenAVXFilter *filter = (OpenAVXFilter*)lpParam;
    OpenAVXPin *pin = &filter->pin;

    if (!pin->pMemInputPin || !pin->pAllocator) return 0;

    IMemAllocator_Commit(pin->pAllocator);

    if (!filter->consumer) {
        filter->consumer = avx_virtcam_consumer_create();
    }

    uint8_t *frame_buf = (uint8_t*)malloc(1920 * 1080 * 2);
    if (!frame_buf) return 0;

    /* Fill placeholder green bars if no phone is connected yet */
    for (size_t i = 0; i < 1920 * 1080 * 2; i += 4) {
        frame_buf[i + 0] = 0x80; /* U */
        frame_buf[i + 1] = 0x40; /* Y0 */
        frame_buf[i + 2] = 0x80; /* V */
        frame_buf[i + 3] = 0x40; /* Y1 */
    }

    uint64_t last_seq = 0;
    REFERENCE_TIME rtFrame = 0;
    REFERENCE_TIME rtDuration = 333333; /* 30 fps */

    while (WaitForSingleObject(filter->hStopEvent, 33) == WAIT_TIMEOUT) {
        if (filter->state != State_Running) continue;

        if (filter->consumer) {
            avx_virtcam_shm_header_t hdr;
            if (avx_virtcam_consumer_read_frame(filter->consumer, &hdr, frame_buf, 1920 * 1080 * 2, &last_seq)) {
                /* Frame received cleanly from OpenAVX shared memory */
            }
        }

        IMediaSample *pSample = NULL;
        HRESULT hr = IMemAllocator_GetBuffer(pin->pAllocator, &pSample, NULL, NULL, 0);
        if (SUCCEEDED(hr) && pSample) {
            BYTE *pDest = NULL;
            if (SUCCEEDED(IMediaSample_GetPointer(pSample, &pDest))) {
                long copy_size = (long)(1920 * 1080 * 2);
                long dst_size = IMediaSample_GetSize(pSample);
                if (copy_size > dst_size) copy_size = dst_size;
                memcpy(pDest, frame_buf, copy_size);
                IMediaSample_SetActualDataLength(pSample, copy_size);
            }

            REFERENCE_TIME rtStart = rtFrame;
            REFERENCE_TIME rtEnd   = rtStart + rtDuration;
            rtFrame += rtDuration;

            IMediaSample_SetTime(pSample, &rtStart, &rtEnd);
            IMediaSample_SetSyncPoint(pSample, TRUE);
            IMediaSample_SetPreroll(pSample, FALSE);

            IMemInputPin_Receive(pin->pMemInputPin, pSample);
            IMediaSample_Release(pSample);
        }
    }

    free(frame_buf);
    return 0;
}

/* ========================================================================= */
/* PIN VTABLE METHODS                                                        */
/* ========================================================================= */

static OpenAVXPin* PinFromIPin(IPin *p) {
    return (OpenAVXPin*)((BYTE*)p - offsetof(OpenAVXPin, lpVtbl_Pin));
}

static HRESULT STDMETHODCALLTYPE Pin_QueryInterface(IPin *This, REFIID riid, void **ppvObject)
{
    if (!ppvObject) return E_POINTER;
    OpenAVXPin *pin = PinFromIPin(This);

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPin)) {
        *ppvObject = &pin->lpVtbl_Pin;
    } else if (IsEqualIID(riid, &IID_IAMStreamConfig)) {
        *ppvObject = &pin->lpVtbl_StreamConfig;
    } else if (IsEqualIID(riid, &IID_IKsPropertySet)) {
        *ppvObject = &pin->lpVtbl_KsPropertySet;
    } else {
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    This->lpVtbl->AddRef(This);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE Pin_AddRef(IPin *This) {
    OpenAVXPin *pin = PinFromIPin(This);
    return InterlockedIncrement(&pin->refCount);
}

static ULONG STDMETHODCALLTYPE Pin_Release(IPin *This) {
    OpenAVXPin *pin = PinFromIPin(This);
    LONG c = InterlockedDecrement(&pin->refCount);
    return c;
}

static HRESULT STDMETHODCALLTYPE Pin_Connect(IPin *This, IPin *pReceivePin, const AM_MEDIA_TYPE *pmt)
{
    if (!pReceivePin) return E_POINTER;
    OpenAVXPin *pin = PinFromIPin(This);
    if (pin->bConnected) return VFW_E_ALREADY_CONNECTED;

    AM_MEDIA_TYPE mt;
    if (pmt) {
        copy_media_type(&mt, pmt);
    } else {
        init_video_media_type(&mt, 1920, 1080, OAVX_FOURCC_VIDEO_TYPE_UYVY);
    }

    HRESULT hr = pReceivePin->lpVtbl->ReceiveConnection(pReceivePin, (IPin*)&pin->lpVtbl_Pin, &mt);
    if (FAILED(hr)) {
        free_media_type(&mt);
        return hr;
    }

    pin->pConnectedPin = pReceivePin;
    pReceivePin->lpVtbl->AddRef(pReceivePin);
    free_media_type(&pin->currentMediaType);
    pin->currentMediaType = mt;
    pin->bConnected = true;

    pReceivePin->lpVtbl->QueryInterface(pReceivePin, &IID_IMemInputPin, (void**)&pin->pMemInputPin);
    if (pin->pMemInputPin) {
        IMemInputPin_GetAllocator(pin->pMemInputPin, &pin->pAllocator);
        if (pin->pAllocator) {
            ALLOCATOR_PROPERTIES prop = { 1, 1920 * 1080 * 2, 0, 0 };
            ALLOCATOR_PROPERTIES actual;
            IMemAllocator_SetProperties(pin->pAllocator, &prop, &actual);
        }
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_ReceiveConnection(IPin *This, IPin *pConnector, const AM_MEDIA_TYPE *pmt) {
    (void)This; (void)pConnector; (void)pmt;
    return E_UNEXPECTED; /* Output pin does not accept input connections */
}

static HRESULT STDMETHODCALLTYPE Pin_Disconnect(IPin *This)
{
    OpenAVXPin *pin = PinFromIPin(This);
    if (!pin->bConnected) return S_FALSE;

    if (pin->pAllocator) {
        IMemAllocator_Decommit(pin->pAllocator);
        IMemAllocator_Release(pin->pAllocator);
        pin->pAllocator = NULL;
    }
    if (pin->pMemInputPin) {
        IMemInputPin_Release(pin->pMemInputPin);
        pin->pMemInputPin = NULL;
    }
    if (pin->pConnectedPin) {
        pin->pConnectedPin->lpVtbl->Release(pin->pConnectedPin);
        pin->pConnectedPin = NULL;
    }
    free_media_type(&pin->currentMediaType);
    pin->bConnected = false;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_ConnectedTo(IPin *This, IPin **pPin) {
    if (!pPin) return E_POINTER;
    OpenAVXPin *pin = PinFromIPin(This);
    if (!pin->bConnected) return VFW_E_NOT_CONNECTED;
    *pPin = pin->pConnectedPin;
    (*pPin)->lpVtbl->AddRef(*pPin);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_ConnectionMediaType(IPin *This, AM_MEDIA_TYPE *pmt) {
    if (!pmt) return E_POINTER;
    OpenAVXPin *pin = PinFromIPin(This);
    if (!pin->bConnected) return VFW_E_NOT_CONNECTED;
    return copy_media_type(pmt, &pin->currentMediaType);
}

static HRESULT STDMETHODCALLTYPE Pin_QueryPinInfo(IPin *This, PIN_INFO *pInfo) {
    if (!pInfo) return E_POINTER;
    OpenAVXPin *pin = PinFromIPin(This);
    pInfo->pFilter = (IBaseFilter*)&pin->pFilter->lpVtbl_Filter;
    if (pInfo->pFilter) pInfo->pFilter->lpVtbl->AddRef(pInfo->pFilter);
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy(pInfo->achName, L"Capture");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryDirection(IPin *This, PIN_DIRECTION *pPinDir) {
    (void)This;
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryId(IPin *This, LPWSTR *Id) {
    (void)This;
    if (!Id) return E_POINTER;
    *Id = (LPWSTR)CoTaskMemAlloc(16 * sizeof(WCHAR));
    if (!*Id) return E_OUTOFMEMORY;
    wcscpy(*Id, L"Capture");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryAccept(IPin *This, const AM_MEDIA_TYPE *pmt) {
    (void)This;
    if (!pmt) return E_POINTER;
    if (IsEqualIID(&pmt->majortype, &MEDIATYPE_Video)) {
        if (IsEqualIID(&pmt->subtype, &MEDIASUBTYPE_UYVY) || IsEqualIID(&pmt->subtype, &MEDIASUBTYPE_YUY2)) {
            return S_OK;
        }
    }
    return S_FALSE;
}

/* Media Types Enumerator */
typedef struct {
    IEnumMediaTypes lpVtbl;
    LONG refCount;
    ULONG index;
} OpenAVXEnumMediaTypes;

static HRESULT STDMETHODCALLTYPE EnumMediaTypes_QueryInterface(IEnumMediaTypes *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumMediaTypes)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE EnumMediaTypes_AddRef(IEnumMediaTypes *This) {
    OpenAVXEnumMediaTypes *e = (OpenAVXEnumMediaTypes*)This;
    return InterlockedIncrement(&e->refCount);
}
static ULONG STDMETHODCALLTYPE EnumMediaTypes_Release(IEnumMediaTypes *This) {
    OpenAVXEnumMediaTypes *e = (OpenAVXEnumMediaTypes*)This;
    LONG c = InterlockedDecrement(&e->refCount);
    if (c == 0) free(e);
    return c;
}
static HRESULT STDMETHODCALLTYPE EnumMediaTypes_Next(IEnumMediaTypes *This, ULONG cMediaTypes, AM_MEDIA_TYPE **ppMediaTypes, ULONG *pcFetched) {
    OpenAVXEnumMediaTypes *e = (OpenAVXEnumMediaTypes*)This;
    ULONG count = 0;
    while (count < cMediaTypes && e->index < 2) {
        ppMediaTypes[count] = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
        if (e->index == 0) {
            init_video_media_type(ppMediaTypes[count], 1920, 1080, FOURCC_UYVY);
        } else {
            init_video_media_type(ppMediaTypes[count], 1920, 1080, FOURCC_YUY2);
        }
        e->index++;
        count++;
    }
    if (pcFetched) *pcFetched = count;
    return (count == cMediaTypes) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE EnumMediaTypes_Skip(IEnumMediaTypes *This, ULONG cMediaTypes) {
    OpenAVXEnumMediaTypes *e = (OpenAVXEnumMediaTypes*)This;
    e->index += cMediaTypes;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE EnumMediaTypes_Reset(IEnumMediaTypes *This) {
    OpenAVXEnumMediaTypes *e = (OpenAVXEnumMediaTypes*)This;
    e->index = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE EnumMediaTypes_Clone(IEnumMediaTypes *This, IEnumMediaTypes **ppEnum) {
    (void)This; (void)ppEnum; return E_NOTIMPL;
}

static const IEnumMediaTypesVtbl g_EnumMediaTypesVtbl = {
    EnumMediaTypes_QueryInterface,
    EnumMediaTypes_AddRef,
    EnumMediaTypes_Release,
    EnumMediaTypes_Next,
    EnumMediaTypes_Skip,
    EnumMediaTypes_Reset,
    EnumMediaTypes_Clone
};

static HRESULT STDMETHODCALLTYPE Pin_EnumMediaTypes(IPin *This, IEnumMediaTypes **ppEnum) {
    (void)This;
    if (!ppEnum) return E_POINTER;
    OpenAVXEnumMediaTypes *e = (OpenAVXEnumMediaTypes*)calloc(1, sizeof(OpenAVXEnumMediaTypes));
    if (!e) return E_OUTOFMEMORY;
    e->lpVtbl.lpVtbl = (IEnumMediaTypesVtbl*)&g_EnumMediaTypesVtbl;
    e->refCount = 1;
    *ppEnum = (IEnumMediaTypes*)e;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Pin_QueryInternalConnections(IPin *This, IPin **apPin, ULONG *nPin) {
    (void)This; (void)apPin;
    if (nPin) *nPin = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE Pin_EndOfStream(IPin *This) { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE Pin_BeginFlush(IPin *This) { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE Pin_EndFlush(IPin *This) { (void)This; return S_OK; }
static HRESULT STDMETHODCALLTYPE Pin_NewSegment(IPin *This, REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) {
    (void)This; (void)tStart; (void)tStop; (void)dRate; return S_OK;
}

static const IPinVtbl g_PinVtbl = {
    Pin_QueryInterface,
    Pin_AddRef,
    Pin_Release,
    Pin_Connect,
    Pin_ReceiveConnection,
    Pin_Disconnect,
    Pin_ConnectedTo,
    Pin_ConnectionMediaType,
    Pin_QueryPinInfo,
    Pin_QueryDirection,
    Pin_QueryId,
    Pin_QueryAccept,
    Pin_EnumMediaTypes,
    Pin_QueryInternalConnections,
    Pin_EndOfStream,
    Pin_BeginFlush,
    Pin_EndFlush,
    Pin_NewSegment
};

/* ========================================================================= */
/* IAMStreamConfig & IKsPropertySet VTABLE METHODS                           */
/* ========================================================================= */

static OpenAVXPin* PinFromIStreamConfig(IAMStreamConfig *p) {
    return (OpenAVXPin*)((BYTE*)p - offsetof(OpenAVXPin, lpVtbl_StreamConfig));
}
static HRESULT STDMETHODCALLTYPE StreamConfig_QueryInterface(IAMStreamConfig *This, REFIID riid, void **ppv) {
    return Pin_QueryInterface((IPin*)&PinFromIStreamConfig(This)->lpVtbl_Pin, riid, ppv);
}
static ULONG STDMETHODCALLTYPE StreamConfig_AddRef(IAMStreamConfig *This) {
    return Pin_AddRef((IPin*)&PinFromIStreamConfig(This)->lpVtbl_Pin);
}
static ULONG STDMETHODCALLTYPE StreamConfig_Release(IAMStreamConfig *This) {
    return Pin_Release((IPin*)&PinFromIStreamConfig(This)->lpVtbl_Pin);
}
static HRESULT STDMETHODCALLTYPE StreamConfig_SetFormat(IAMStreamConfig *This, AM_MEDIA_TYPE *pmt) {
    OpenAVXPin *pin = PinFromIStreamConfig(This);
    if (!pmt) return E_POINTER;
    free_media_type(&pin->currentMediaType);
    return copy_media_type(&pin->currentMediaType, pmt);
}
static HRESULT STDMETHODCALLTYPE StreamConfig_GetFormat(IAMStreamConfig *This, AM_MEDIA_TYPE **pmt) {
    OpenAVXPin *pin = PinFromIStreamConfig(This);
    if (!pmt) return E_POINTER;
    *pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!*pmt) return E_OUTOFMEMORY;
    if (pin->currentMediaType.cbFormat > 0) {
        return copy_media_type(*pmt, &pin->currentMediaType);
    }
    init_video_media_type(*pmt, 1920, 1080, OAVX_FOURCC_VIDEO_TYPE_UYVY);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE StreamConfig_GetNumberOfCapabilities(IAMStreamConfig *This, int *piCount, int *piSize) {
    (void)This;
    if (piCount) *piCount = 1;
    if (piSize)  *piSize  = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE StreamConfig_GetStreamCaps(IAMStreamConfig *This, int iIndex, AM_MEDIA_TYPE **ppmt, BYTE *pSCC) {
    (void)This;
    if (iIndex != 0) return S_FALSE;
    if (ppmt) {
        *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
        init_video_media_type(*ppmt, 1920, 1080, OAVX_FOURCC_VIDEO_TYPE_UYVY);
    }
    if (pSCC) {
        VIDEO_STREAM_CONFIG_CAPS *caps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
        memset(caps, 0, sizeof(VIDEO_STREAM_CONFIG_CAPS));
        caps->guid = FORMAT_VideoInfo;
        caps->InputSize.cx = 1920; caps->InputSize.cy = 1080;
        caps->MinOutputSize.cx = 640; caps->MinOutputSize.cy = 360;
        caps->MaxOutputSize.cx = 3840; caps->MaxOutputSize.cy = 2160;
        caps->MinFrameInterval = 166666; /* 60 fps */
        caps->MaxFrameInterval = 10000000; /* 1 fps */
    }
    return S_OK;
}

static const IAMStreamConfigVtbl g_StreamConfigVtbl = {
    StreamConfig_QueryInterface,
    StreamConfig_AddRef,
    StreamConfig_Release,
    StreamConfig_SetFormat,
    StreamConfig_GetFormat,
    StreamConfig_GetNumberOfCapabilities,
    StreamConfig_GetStreamCaps
};

/* IKsPropertySet */
static OpenAVXPin* PinFromIKsPropertySet(IKsPropertySet *p) {
    return (OpenAVXPin*)((BYTE*)p - offsetof(OpenAVXPin, lpVtbl_KsPropertySet));
}
static HRESULT STDMETHODCALLTYPE Ks_QueryInterface(IKsPropertySet *This, REFIID riid, void **ppv) {
    return Pin_QueryInterface((IPin*)&PinFromIKsPropertySet(This)->lpVtbl_Pin, riid, ppv);
}
static ULONG STDMETHODCALLTYPE Ks_AddRef(IKsPropertySet *This) {
    return Pin_AddRef((IPin*)&PinFromIKsPropertySet(This)->lpVtbl_Pin);
}
static ULONG STDMETHODCALLTYPE Ks_Release(IKsPropertySet *This) {
    return Pin_Release((IPin*)&PinFromIKsPropertySet(This)->lpVtbl_Pin);
}
static HRESULT STDMETHODCALLTYPE Ks_Set(IKsPropertySet *This, REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) {
    (void)This; (void)guidPropSet; (void)dwPropID; (void)pInstanceData; (void)cbInstanceData; (void)pPropData; (void)cbPropData;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE Ks_Get(IKsPropertySet *This, REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD *pcbReturned) {
    (void)This; (void)pInstanceData; (void)cbInstanceData;
    if (IsEqualGUID(guidPropSet, &AMPROPSETID_Pin) && dwPropID == 0 /* AMPROPERTY_PIN_CATEGORY */) {
        if (!pPropData) {
            if (pcbReturned) *pcbReturned = sizeof(GUID);
            return S_OK;
        }
        if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
        memcpy(pPropData, &PIN_CATEGORY_CAPTURE, sizeof(GUID));
        if (pcbReturned) *pcbReturned = sizeof(GUID);
        return S_OK;
    }
    return E_PROP_SET_UNSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE Ks_QuerySupported(IKsPropertySet *This, REFGUID guidPropSet, DWORD dwPropID, DWORD *pTypeSupport) {
    (void)This;
    if (IsEqualGUID(guidPropSet, &AMPROPSETID_Pin) && dwPropID == 0) {
        if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
    return E_PROP_SET_UNSUPPORTED;
}

static const IKsPropertySetVtbl g_KsPropertySetVtbl = {
    Ks_QueryInterface,
    Ks_AddRef,
    Ks_Release,
    Ks_Set,
    Ks_Get,
    Ks_QuerySupported
};

/* ========================================================================= */
/* IBASEFILTER VTABLE METHODS                                                */
/* ========================================================================= */

static OpenAVXFilter* FilterFromIBaseFilter(IBaseFilter *p) {
    return (OpenAVXFilter*)((BYTE*)p - offsetof(OpenAVXFilter, lpVtbl_Filter));
}

static HRESULT STDMETHODCALLTYPE Filter_QueryInterface(IBaseFilter *This, REFIID riid, void **ppvObject)
{
    if (!ppvObject) return E_POINTER;
    OpenAVXFilter *f = FilterFromIBaseFilter(This);

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IPersist) ||
        IsEqualIID(riid, &IID_IMediaFilter) ||
        IsEqualIID(riid, &IID_IBaseFilter)) {
        *ppvObject = &f->lpVtbl_Filter;
    } else {
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    This->lpVtbl->AddRef(This);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE Filter_AddRef(IBaseFilter *This) {
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    return InterlockedIncrement(&f->refCount);
}

static ULONG STDMETHODCALLTYPE Filter_Release(IBaseFilter *This) {
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    LONG c = InterlockedDecrement(&f->refCount);
    if (c == 0) {
        This->lpVtbl->Stop(This);
        CloseHandle(f->hStopEvent);
        DeleteCriticalSection(&f->lock);
        free_media_type(&f->pin.currentMediaType);
        free(f);
        InterlockedDecrement(&g_cComponents);
    }
    return c;
}

static HRESULT STDMETHODCALLTYPE Filter_GetClassID(IBaseFilter *This, CLSID *pClassID) {
    (void)This;
    if (!pClassID) return E_POINTER;
    *pClassID = CLSID_OpenAVXCam;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_Stop(IBaseFilter *This)
{
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    EnterCriticalSection(&f->lock);

    if (f->state != State_Stopped) {
        f->state = State_Stopped;
        SetEvent(f->hStopEvent);
        if (f->hThread) {
            WaitForSingleObject(f->hThread, 1000);
            CloseHandle(f->hThread);
            f->hThread = NULL;
        }
        if (f->consumer) {
            avx_virtcam_consumer_destroy(f->consumer);
            f->consumer = NULL;
        }
        if (f->pin.pAllocator) {
            IMemAllocator_Decommit(f->pin.pAllocator);
        }
    }

    LeaveCriticalSection(&f->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_Pause(IBaseFilter *This)
{
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    EnterCriticalSection(&f->lock);
    f->state = State_Paused;
    LeaveCriticalSection(&f->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_Run(IBaseFilter *This, REFERENCE_TIME tStart)
{
    (void)tStart;
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    EnterCriticalSection(&f->lock);

    if (f->state != State_Running) {
        f->state = State_Running;
        ResetEvent(f->hStopEvent);
        if (!f->hThread) {
            f->hThread = CreateThread(NULL, 0, StreamingThreadProc, f, 0, NULL);
        }
    }

    LeaveCriticalSection(&f->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_GetState(IBaseFilter *This, DWORD dwMilliSecsTimeout, FILTER_STATE *State) {
    (void)dwMilliSecsTimeout;
    if (!State) return E_POINTER;
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    *State = f->state;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_SetSyncSource(IBaseFilter *This, IReferenceClock *pClock) {
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    if (f->pClock) f->pClock->lpVtbl->Release(f->pClock);
    f->pClock = pClock;
    if (f->pClock) f->pClock->lpVtbl->AddRef(f->pClock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_GetSyncSource(IBaseFilter *This, IReferenceClock **pClock) {
    if (!pClock) return E_POINTER;
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    *pClock = f->pClock;
    if (*pClock) (*pClock)->lpVtbl->AddRef(*pClock);
    return S_OK;
}

/* Pins Enumerator */
typedef struct {
    IEnumPins lpVtbl;
    LONG refCount;
    OpenAVXFilter *pFilter;
    ULONG index;
} OpenAVXEnumPins;

static HRESULT STDMETHODCALLTYPE EnumPins_QueryInterface(IEnumPins *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumPins)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE EnumPins_AddRef(IEnumPins *This) {
    OpenAVXEnumPins *e = (OpenAVXEnumPins*)This;
    return InterlockedIncrement(&e->refCount);
}
static ULONG STDMETHODCALLTYPE EnumPins_Release(IEnumPins *This) {
    OpenAVXEnumPins *e = (OpenAVXEnumPins*)This;
    LONG c = InterlockedDecrement(&e->refCount);
    if (c == 0) free(e);
    return c;
}
static HRESULT STDMETHODCALLTYPE EnumPins_Next(IEnumPins *This, ULONG cPins, IPin **ppPins, ULONG *pcFetched) {
    OpenAVXEnumPins *e = (OpenAVXEnumPins*)This;
    if (!ppPins) return E_POINTER;
    ULONG count = 0;
    if (cPins > 0 && e->index == 0) {
        ppPins[0] = (IPin*)&e->pFilter->pin.lpVtbl_Pin;
        ppPins[0]->lpVtbl->AddRef(ppPins[0]);
        e->index++;
        count = 1;
    }
    if (pcFetched) *pcFetched = count;
    return (count == cPins) ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE EnumPins_Skip(IEnumPins *This, ULONG cPins) {
    OpenAVXEnumPins *e = (OpenAVXEnumPins*)This;
    e->index += cPins;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE EnumPins_Reset(IEnumPins *This) {
    OpenAVXEnumPins *e = (OpenAVXEnumPins*)This;
    e->index = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE EnumPins_Clone(IEnumPins *This, IEnumPins **ppEnum) {
    (void)This; (void)ppEnum; return E_NOTIMPL;
}

static const IEnumPinsVtbl g_EnumPinsVtbl = {
    EnumPins_QueryInterface,
    EnumPins_AddRef,
    EnumPins_Release,
    EnumPins_Next,
    EnumPins_Skip,
    EnumPins_Reset,
    EnumPins_Clone
};

static HRESULT STDMETHODCALLTYPE Filter_EnumPins(IBaseFilter *This, IEnumPins **ppEnum)
{
    if (!ppEnum) return E_POINTER;
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    OpenAVXEnumPins *e = (OpenAVXEnumPins*)calloc(1, sizeof(OpenAVXEnumPins));
    if (!e) return E_OUTOFMEMORY;
    e->lpVtbl.lpVtbl = (IEnumPinsVtbl*)&g_EnumPinsVtbl;
    e->refCount = 1;
    e->pFilter = f;
    *ppEnum = (IEnumPins*)e;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_FindPin(IBaseFilter *This, LPCWSTR Id, IPin **ppPin)
{
    if (!ppPin) return E_POINTER;
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    if (wcscmp(Id, L"Capture") == 0) {
        *ppPin = (IPin*)&f->pin.lpVtbl_Pin;
        (*ppPin)->lpVtbl->AddRef(*ppPin);
        return S_OK;
    }
    *ppPin = NULL;
    return VFW_E_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE Filter_QueryFilterInfo(IBaseFilter *This, FILTER_INFO *pInfo)
{
    if (!pInfo) return E_POINTER;
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    wcscpy(pInfo->achName, L"OpenAVX Virtual Camera");
    pInfo->pGraph = f->pGraph;
    if (pInfo->pGraph) pInfo->pGraph->lpVtbl->AddRef(pInfo->pGraph);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_JoinFilterGraph(IBaseFilter *This, IFilterGraph *pGraph, LPCWSTR pName)
{
    (void)pName;
    OpenAVXFilter *f = FilterFromIBaseFilter(This);
    f->pGraph = pGraph;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE Filter_QueryVendorInfo(IBaseFilter *This, LPWSTR *pVendorInfo) {
    (void)This;
    if (!pVendorInfo) return E_POINTER;
    *pVendorInfo = (LPWSTR)CoTaskMemAlloc(32 * sizeof(WCHAR));
    if (!*pVendorInfo) return E_OUTOFMEMORY;
    wcscpy(*pVendorInfo, L"OpenAVX Open Source Project");
    return S_OK;
}

static const IBaseFilterVtbl g_FilterVtbl = {
    Filter_QueryInterface,
    Filter_AddRef,
    Filter_Release,
    Filter_GetClassID,
    Filter_Stop,
    Filter_Pause,
    Filter_Run,
    Filter_GetState,
    Filter_SetSyncSource,
    Filter_GetSyncSource,
    Filter_EnumPins,
    Filter_FindPin,
    Filter_QueryFilterInfo,
    Filter_JoinFilterGraph,
    Filter_QueryVendorInfo
};

static HRESULT CreateFilterInstance(REFIID riid, void **ppv)
{
    OpenAVXFilter *f = (OpenAVXFilter*)calloc(1, sizeof(OpenAVXFilter));
    if (!f) return E_OUTOFMEMORY;

    f->lpVtbl_Filter.lpVtbl = (IBaseFilterVtbl*)&g_FilterVtbl;
    f->refCount = 1;
    f->state = State_Stopped;
    InitializeCriticalSection(&f->lock);
    f->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    /* Setup output pin */
    f->pin.lpVtbl_Pin.lpVtbl = (IPinVtbl*)&g_PinVtbl;
    f->pin.lpVtbl_StreamConfig.lpVtbl = (IAMStreamConfigVtbl*)&g_StreamConfigVtbl;
    f->pin.lpVtbl_KsPropertySet.lpVtbl = (IKsPropertySetVtbl*)&g_KsPropertySetVtbl;
    f->pin.refCount = 1;
    f->pin.pFilter = f;
    init_video_media_type(&f->pin.currentMediaType, 1920, 1080, OAVX_FOURCC_VIDEO_TYPE_UYVY);

    InterlockedIncrement(&g_cComponents);

    HRESULT hr = f->lpVtbl_Filter.lpVtbl->QueryInterface((IBaseFilter*)&f->lpVtbl_Filter, riid, ppv);
    f->lpVtbl_Filter.lpVtbl->Release((IBaseFilter*)&f->lpVtbl_Filter);
    return hr;
}

/* ========================================================================= */
/* ICLASSFACTORY IMPLEMENTATION                                              */
/* ========================================================================= */

typedef struct {
    IClassFactory lpVtbl;
    LONG refCount;
} OpenAVXClassFactory;

static HRESULT STDMETHODCALLTYPE Factory_QueryInterface(IClassFactory *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE Factory_AddRef(IClassFactory *This) {
    OpenAVXClassFactory *f = (OpenAVXClassFactory*)This;
    return InterlockedIncrement(&f->refCount);
}
static ULONG STDMETHODCALLTYPE Factory_Release(IClassFactory *This) {
    OpenAVXClassFactory *f = (OpenAVXClassFactory*)This;
    LONG c = InterlockedDecrement(&f->refCount);
    if (c == 0) free(f);
    return c;
}
static HRESULT STDMETHODCALLTYPE Factory_CreateInstance(IClassFactory *This, IUnknown *pUnkOuter, REFIID riid, void **ppv) {
    (void)This;
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;
    return CreateFilterInstance(riid, ppv);
}
static HRESULT STDMETHODCALLTYPE Factory_LockServer(IClassFactory *This, BOOL fLock) {
    (void)This;
    if (fLock) InterlockedIncrement(&g_cServerLocks);
    else InterlockedDecrement(&g_cServerLocks);
    return S_OK;
}

static const IClassFactoryVtbl g_ClassFactoryVtbl = {
    Factory_QueryInterface,
    Factory_AddRef,
    Factory_Release,
    Factory_CreateInstance,
    Factory_LockServer
};

/* ========================================================================= */
/* DLL ENTRY POINTS & EXPORTS                                                */
/* ========================================================================= */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hInst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}

/* COM class factory entry point instantiating OpenAVX DirectShow source filter. */
__declspec(dllexport) STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;

    if (!IsEqualCLSID(rclsid, &CLSID_OpenAVXCam)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    OpenAVXClassFactory *factory = (OpenAVXClassFactory*)calloc(1, sizeof(OpenAVXClassFactory));
    if (!factory) return E_OUTOFMEMORY;
    factory->lpVtbl.lpVtbl = (IClassFactoryVtbl*)&g_ClassFactoryVtbl;
    factory->refCount = 1;

    HRESULT hr = factory->lpVtbl.lpVtbl->QueryInterface((IClassFactory*)factory, riid, ppv);
    factory->lpVtbl.lpVtbl->Release((IClassFactory*)factory);
    return hr;
}

__declspec(dllexport) STDAPI DllCanUnloadNow(void)
{
    return (g_cComponents == 0 && g_cServerLocks == 0) ? S_OK : S_FALSE;
}

/* Self-registration entry point registering filter under user DirectShow video category. */
__declspec(dllexport) STDAPI DllRegisterServer(void)
{
    char dll_path[MAX_PATH] = "";
    GetModuleFileNameA(g_hInst, dll_path, MAX_PATH);
    return avx_dshow_register_filter(dll_path) ? S_OK : E_FAIL;
}

__declspec(dllexport) STDAPI DllUnregisterServer(void)
{
    return avx_dshow_unregister_filter() ? S_OK : E_FAIL;
}

#endif /* AVX_OS_WINDOWS || MinGW */
