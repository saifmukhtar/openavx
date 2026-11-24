/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_dal_test.c - Unit & Integration Test for macOS CoreMediaIO DAL Virtual Camera Plugin
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "openavx.h"
#include "cmio/CMIOHardwarePlugIn.h"

#define kOpenAVXDeviceID 0x41565831 /* 'AVX1' */
#define kOpenAVXStreamID 0x41565853 /* 'AVXS' */

/* Exported factory declaration */
void* OpenAVXDALPluginFactory(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);

int main(void)
{
    printf("[DAL TEST] Testing macOS CoreMediaIO DAL Virtual Camera Plugin Factory...\n");

    /* 1. Call Plugin Factory */
    CMIOHardwarePlugInRef pluginRef = (CMIOHardwarePlugInRef)OpenAVXDALPluginFactory(NULL, NULL);
    assert(pluginRef != NULL);
    assert(*pluginRef != NULL);
    printf("[PASS] OpenAVXDALPluginFactory returned valid CMIOHardwarePlugInRef\n");

    CMIOHardwarePlugInInterface *vtbl = *pluginRef;

    /* 2. Test QueryInterface */
    void *queried = NULL;
    HRESULT hr = vtbl->QueryInterface(pluginRef, kCMIOHardwarePlugInInterfaceUUID, &queried);
    assert(hr == S_OK);
    assert(queried == (void*)pluginRef);
    printf("[PASS] QueryInterface returned valid interface matching kCMIOHardwarePlugInInterfaceUUID\n");
    vtbl->Release(pluginRef);

    /* 3. Test Initialize */
    OSStatus st = vtbl->Initialize(pluginRef);
    assert(st == kCMIOHardwareNoError);
    st = vtbl->InitializeWithObjectID(pluginRef, kCMIOObjectSystemObject);
    assert(st == kCMIOHardwareNoError);
    printf("[PASS] Initialize and InitializeWithObjectID succeeded\n");

    /* 4. Test System Object Properties (Device Enumeration) */
    CMIOObjectPropertyAddress addrStreams = { kCMIODevicePropertyStreams, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMaster };
    Boolean hasProp = vtbl->ObjectHasProperty(pluginRef, kCMIOObjectSystemObject, &addrStreams);
    assert(hasProp == true);

    UInt32 size = 0;
    st = vtbl->ObjectGetPropertyDataSize(pluginRef, kCMIOObjectSystemObject, &addrStreams, 0, NULL, &size);
    assert(st == kCMIOHardwareNoError && size == sizeof(CMIODeviceID));

    CMIODeviceID devId = 0;
    UInt32 dataUsed = 0;
    st = vtbl->ObjectGetPropertyData(pluginRef, kCMIOObjectSystemObject, &addrStreams, 0, NULL, sizeof(devId), &dataUsed, &devId);
    assert(st == kCMIOHardwareNoError && dataUsed == sizeof(CMIODeviceID));
    assert(devId == kOpenAVXDeviceID);
    printf("[PASS] System Object correctly reports Virtual Camera Device (ID=0x%08x)\n", devId);

    /* 5. Test Device Object Properties */
    CMIOObjectPropertyAddress addrAlive = { kCMIODevicePropertyDeviceIsAlive, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMaster };
    UInt32 isAlive = 0;
    st = vtbl->ObjectGetPropertyData(pluginRef, kOpenAVXDeviceID, &addrAlive, 0, NULL, sizeof(isAlive), &dataUsed, &isAlive);
    assert(st == kCMIOHardwareNoError && isAlive == 1);

    CMIOObjectPropertyAddress addrTransport = { kCMIODevicePropertyTransportType, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMaster };
    UInt32 transport = 0;
    st = vtbl->ObjectGetPropertyData(pluginRef, kOpenAVXDeviceID, &addrTransport, 0, NULL, sizeof(transport), &dataUsed, &transport);
    assert(st == kCMIOHardwareNoError && transport == 0x76697274); /* 'virt' */

    CMIOStreamID streamId = 0;
    st = vtbl->ObjectGetPropertyData(pluginRef, kOpenAVXDeviceID, &addrStreams, 0, NULL, sizeof(streamId), &dataUsed, &streamId);
    assert(st == kCMIOHardwareNoError && streamId == kOpenAVXStreamID);
    printf("[PASS] Device Object confirmed: IsAlive=1, Transport='virt', StreamID=0x%08x\n", streamId);

    /* 6. Test Stream Object Properties */
    CMIOObjectPropertyAddress addrDir = { kCMIOStreamPropertyDirection, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMaster };
    UInt32 dir = 0;
    st = vtbl->ObjectGetPropertyData(pluginRef, kOpenAVXStreamID, &addrDir, 0, NULL, sizeof(dir), &dataUsed, &dir);
    assert(st == kCMIOHardwareNoError && dir == 1); /* 1 = output capture stream */

    CMIOObjectPropertyAddress addrRates = { kCMIOStreamPropertyFrameRates, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMaster };
    double rates[2] = {0};
    st = vtbl->ObjectGetPropertyData(pluginRef, kOpenAVXStreamID, &addrRates, 0, NULL, sizeof(rates), &dataUsed, rates);
    assert(st == kCMIOHardwareNoError && rates[0] == 30.0 && rates[1] == 60.0);
    printf("[PASS] Stream Object confirmed: Direction=1 (Output), FrameRates=[%.1f, %.1f]\n", rates[0], rates[1]);

    /* 7. Test Streaming Engine Start & Stop */
    CMIOObjectPropertyAddress addrRunning = { kCMIODevicePropertyDeviceIsRunning, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMaster };
    UInt32 isRunning = 0;
    st = vtbl->ObjectGetPropertyData(pluginRef, kOpenAVXDeviceID, &addrRunning, 0, NULL, sizeof(isRunning), &dataUsed, &isRunning);
    assert(st == kCMIOHardwareNoError && isRunning == 0);

    st = vtbl->DeviceStartStream(pluginRef, kOpenAVXDeviceID, kOpenAVXStreamID);
    assert(st == kCMIOHardwareNoError);

    st = vtbl->ObjectGetPropertyData(pluginRef, kOpenAVXDeviceID, &addrRunning, 0, NULL, sizeof(isRunning), &dataUsed, &isRunning);
    assert(st == kCMIOHardwareNoError && isRunning == 1);
    printf("[PASS] DeviceStartStream activated stream: DeviceIsRunning=%u\n", isRunning);

    /* Allow background streaming thread to cycle */
    usleep(100000); /* 100ms */

    st = vtbl->DeviceStopStream(pluginRef, kOpenAVXDeviceID, kOpenAVXStreamID);
    assert(st == kCMIOHardwareNoError);

    st = vtbl->ObjectGetPropertyData(pluginRef, kOpenAVXDeviceID, &addrRunning, 0, NULL, sizeof(isRunning), &dataUsed, &isRunning);
    assert(st == kCMIOHardwareNoError && isRunning == 0);
    printf("[PASS] DeviceStopStream halted stream: DeviceIsRunning=%u\n", isRunning);

    /* 8. Teardown & Release */
    st = vtbl->Teardown(pluginRef);
    assert(st == kCMIOHardwareNoError);
    ULONG remaining = vtbl->Release(pluginRef);
    assert(remaining == 0);
    printf("[PASS] Teardown & Release completed with 0 remaining references\n");

    printf("\n=======================================================\n");
    printf(">>> ALL MACOS COREMEDIAIO DAL PLUGIN TESTS PASSED! <<<\n");
    printf("=======================================================\n");
    return 0;
}
