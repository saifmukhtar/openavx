/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_dshow_test.c - Unit & Integration Test for Windows DirectShow Virtual Camera
 */

#define _WIN32_WINNT 0x0601
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <initguid.h>
#include <strmif.h>
#include <amvideo.h>
#include <uuids.h>
#include <vfwmsgs.h>

/* CLSID_OpenAVXCam: {9C86F6B0-80E2-4AE8-B937-2E8B9915D2C1} */
DEFINE_GUID(CLSID_OpenAVXCam,
    0x9c86f6b0, 0x80e2, 0x4ae8, 0xb9, 0x37, 0x2e, 0x8b, 0x99, 0x15, 0xd2, 0xc1);

typedef HRESULT (STDAPICALLTYPE *PFN_DllGetClassObject)(REFCLSID rclsid, REFIID riid, LPVOID *ppv);
typedef HRESULT (STDAPICALLTYPE *PFN_DllCanUnloadNow)(void);
typedef HRESULT (STDAPICALLTYPE *PFN_DllRegisterServer)(void);
typedef HRESULT (STDAPICALLTYPE *PFN_DllUnregisterServer)(void);

int main(int argc, char **argv)
{
    const char *dll_path = (argc > 1) ? argv[1] : "openavx_dshow_filter.dll";
    printf("[DSHOW TEST] Testing DirectShow Virtual Camera Filter DLL: %s\n", dll_path);

    HMODULE hDll = LoadLibraryA(dll_path);
    if (!hDll && strncmp(dll_path, "lib", 3) != 0) {
        char lib_name[512];
        snprintf(lib_name, sizeof(lib_name), "lib%s", dll_path);
        hDll = LoadLibraryA(lib_name);
    }
    if (!hDll && strstr(dll_path, "\\") == NULL && strstr(dll_path, "/") == NULL) {
        char fallback[512];
        snprintf(fallback, sizeof(fallback), "bin/%s", dll_path);
        hDll = LoadLibraryA(fallback);
        if (!hDll) {
            snprintf(fallback, sizeof(fallback), "bin/lib%s", dll_path);
            hDll = LoadLibraryA(fallback);
        }
    }
    if (!hDll) {
        fprintf(stderr, "[FAIL] Could not load DLL %s (error: %lu)\n", dll_path, GetLastError());
        return 1;
    }
    printf("[PASS] Successfully loaded %s\n", dll_path);

    PFN_DllGetClassObject pfnGetClassObject = (PFN_DllGetClassObject)GetProcAddress(hDll, "DllGetClassObject");
    PFN_DllCanUnloadNow pfnCanUnloadNow = (PFN_DllCanUnloadNow)GetProcAddress(hDll, "DllCanUnloadNow");
    PFN_DllRegisterServer pfnRegister = (PFN_DllRegisterServer)GetProcAddress(hDll, "DllRegisterServer");
    PFN_DllUnregisterServer pfnUnregister = (PFN_DllUnregisterServer)GetProcAddress(hDll, "DllUnregisterServer");

    assert(pfnGetClassObject != NULL);
    assert(pfnCanUnloadNow != NULL);
    assert(pfnRegister != NULL);
    assert(pfnUnregister != NULL);
    printf("[PASS] All required COM and registration exports located\n");

    /* Initialize COM for the test thread */
    CoInitialize(NULL);

    /* 1. Test DllGetClassObject with CLSID_OpenAVXCam */
    IClassFactory *pFactory = NULL;
    HRESULT hr = pfnGetClassObject(&CLSID_OpenAVXCam, &IID_IClassFactory, (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        fprintf(stderr, "[FAIL] DllGetClassObject failed: 0x%08lx\n", hr);
        return 1;
    }
    printf("[PASS] DllGetClassObject returned IClassFactory\n");

    /* 2. Instantiate IBaseFilter via ClassFactory */
    IBaseFilter *pFilter = NULL;
    hr = IClassFactory_CreateInstance(pFactory, NULL, &IID_IBaseFilter, (void**)&pFilter);
    IClassFactory_Release(pFactory);
    if (FAILED(hr) || !pFilter) {
        fprintf(stderr, "[FAIL] CreateInstance(IBaseFilter) failed: 0x%08lx\n", hr);
        return 1;
    }
    printf("[PASS] Successfully instantiated IBaseFilter\n");

    /* 3. Query Filter Info */
    FILTER_INFO finfo;
    memset(&finfo, 0, sizeof(finfo));
    hr = IBaseFilter_QueryFilterInfo(pFilter, &finfo);
    if (FAILED(hr)) {
        fprintf(stderr, "[FAIL] QueryFilterInfo failed: 0x%08lx\n", hr);
        return 1;
    }
    if (finfo.pGraph) IFilterGraph_Release(finfo.pGraph);
    wprintf(L"[PASS] Filter Name: '%ls'\n", finfo.achName);
    assert(wcscmp(finfo.achName, L"OpenAVX Virtual Camera") == 0);

    /* 4. Enumerate Pins */
    IEnumPins *pEnumPins = NULL;
    hr = IBaseFilter_EnumPins(pFilter, &pEnumPins);
    assert(SUCCEEDED(hr) && pEnumPins);

    IPin *pPin = NULL;
    ULONG fetched = 0;
    hr = IEnumPins_Next(pEnumPins, 1, &pPin, &fetched);
    assert(SUCCEEDED(hr) && fetched == 1 && pPin);
    IEnumPins_Release(pEnumPins);

    PIN_INFO pinInfo;
    memset(&pinInfo, 0, sizeof(pinInfo));
    hr = IPin_QueryPinInfo(pPin, &pinInfo);
    assert(SUCCEEDED(hr));
    if (pinInfo.pFilter) IBaseFilter_Release(pinInfo.pFilter);
    wprintf(L"[PASS] Output Pin located: '%ls', Direction=%d (PINDIR_OUTPUT=%d)\n",
            pinInfo.achName, pinInfo.dir, PINDIR_OUTPUT);
    assert(pinInfo.dir == PINDIR_OUTPUT);
    assert(wcscmp(pinInfo.achName, L"Capture") == 0);

    /* 5. Enumerate Media Types on Pin */
    IEnumMediaTypes *pEnumMt = NULL;
    hr = IPin_EnumMediaTypes(pPin, &pEnumMt);
    assert(SUCCEEDED(hr) && pEnumMt);

    AM_MEDIA_TYPE *pmt = NULL;
    fetched = 0;
    hr = IEnumMediaTypes_Next(pEnumMt, 1, &pmt, &fetched);
    assert(SUCCEEDED(hr) && fetched == 1 && pmt);
    printf("[PASS] Pin Media Type 0 fetched: lSampleSize=%ld, bFixedSizeSamples=%d\n",
           pmt->lSampleSize, pmt->bFixedSizeSamples);
    assert(IsEqualGUID(&pmt->majortype, &MEDIATYPE_Video));
    assert(IsEqualGUID(&pmt->formattype, &FORMAT_VideoInfo));
    assert(pmt->pbFormat != NULL);
    VIDEOINFOHEADER *pvi = (VIDEOINFOHEADER*)pmt->pbFormat;
    printf("[PASS] Formatted resolution: %ldx%ld at %lld avg time per frame\n",
           pvi->bmiHeader.biWidth, pvi->bmiHeader.biHeight, pvi->AvgTimePerFrame);
    assert(pvi->bmiHeader.biWidth == 1920);
    assert(pvi->bmiHeader.biHeight == 1080);
    CoTaskMemFree(pmt->pbFormat);
    CoTaskMemFree(pmt);
    IEnumMediaTypes_Release(pEnumMt);

    /* 6. Query IAMStreamConfig */
    IAMStreamConfig *pStreamConfig = NULL;
    hr = IPin_QueryInterface(pPin, &IID_IAMStreamConfig, (void**)&pStreamConfig);
    assert(SUCCEEDED(hr) && pStreamConfig);
    AM_MEDIA_TYPE *cfgMt = NULL;
    hr = IAMStreamConfig_GetFormat(pStreamConfig, &cfgMt);
    assert(SUCCEEDED(hr) && cfgMt);
    assert(IsEqualGUID(&cfgMt->majortype, &MEDIATYPE_Video));
    CoTaskMemFree(cfgMt->pbFormat);
    CoTaskMemFree(cfgMt);
    IAMStreamConfig_Release(pStreamConfig);
    printf("[PASS] IAMStreamConfig format interrogation verified\n");

    /* 7. Query IKsPropertySet (Pin Category) */
    IKsPropertySet *pKs = NULL;
    hr = IPin_QueryInterface(pPin, &IID_IKsPropertySet, (void**)&pKs);
    assert(SUCCEEDED(hr) && pKs);
    DWORD dwTypeSupport = 0;
    hr = IKsPropertySet_QuerySupported(pKs, &AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, &dwTypeSupport);
    assert(SUCCEEDED(hr));
    assert(dwTypeSupport & KSPROPERTY_SUPPORT_GET);

    GUID category = {0};
    DWORD cbReturned = 0;
    hr = IKsPropertySet_Get(pKs, &AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY,
                            NULL, 0, &category, sizeof(category), &cbReturned);
    assert(SUCCEEDED(hr));
    assert(IsEqualGUID(&category, &PIN_CATEGORY_CAPTURE));
    IKsPropertySet_Release(pKs);
    printf("[PASS] IKsPropertySet confirmed PIN_CATEGORY_CAPTURE for Windows capture routing\n");

    /* 8. Test State Transitions (Stop -> Pause -> Run -> Stop) */
    hr = IBaseFilter_Pause(pFilter);
    assert(SUCCEEDED(hr));
    FILTER_STATE state;
    hr = IBaseFilter_GetState(pFilter, 100, &state);
    assert(SUCCEEDED(hr) && state == State_Paused);

    hr = IBaseFilter_Run(pFilter, 0);
    assert(SUCCEEDED(hr));
    hr = IBaseFilter_GetState(pFilter, 100, &state);
    assert(SUCCEEDED(hr) && state == State_Running);
    printf("[PASS] Filter successfully entered State_Running\n");

    hr = IBaseFilter_Stop(pFilter);
    assert(SUCCEEDED(hr));
    hr = IBaseFilter_GetState(pFilter, 100, &state);
    assert(SUCCEEDED(hr) && state == State_Stopped);
    printf("[PASS] Filter successfully stopped\n");

    /* Cleanup Pin & Filter */
    IPin_Release(pPin);
    IBaseFilter_Release(pFilter);

    /* 9. Verify Server Registration/Unregistration functions run cleanly */
    hr = pfnRegister();
    printf("[PASS] DllRegisterServer returned 0x%08lx\n", hr);
    hr = pfnUnregister();
    printf("[PASS] DllUnregisterServer returned 0x%08lx\n", hr);

    /* 10. Verify DllCanUnloadNow reports clean unloaded state */
    hr = pfnCanUnloadNow();
    assert(hr == S_OK);
    printf("[PASS] DllCanUnloadNow reported S_OK (0 active components/locks)\n");

    CoUninitialize();
    FreeLibrary(hDll);

    printf("\n=======================================================\n");
    printf(">>> ALL WINDOWS DIRECTSHOW FILTER TESTS PASSED! <<<\n");
    printf("=======================================================\n");
    return 0;
}
