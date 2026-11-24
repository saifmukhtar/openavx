/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_dal_plugin.c - macOS CoreMediaIO DAL Virtual Camera Plugin Engine
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "openavx.h"
#include "avx_dal_sink.h"
#include "avx_virtcam_ipc.h"
#include "avx_platform.h"
#include "cmio/CMIOHardwarePlugIn.h"

#define kOpenAVXDeviceID 0x41565831 /* 'AVX1' */
#define kOpenAVXStreamID 0x41565853 /* 'AVXS' */

typedef struct OpenAVXDALPlugin {
    CMIOHardwarePlugInInterface *vtbl;
    ULONG refCount;

    /* Device & Stream State */
    bool isRunning;
    pthread_t thread;
    bool threadRunning;
    pthread_mutex_t lock;

    avx_virtcam_consumer_t *consumer;
} OpenAVXDALPlugin;

/* Forward declarations */
static OSStatus DAL_Initialize(CMIOHardwarePlugInRef self);
static OSStatus DAL_InitializeWithObjectID(CMIOHardwarePlugInRef self, CMIOObjectID objectID);
static OSStatus DAL_Teardown(CMIOHardwarePlugInRef self);
static void     DAL_ObjectShow(CMIOHardwarePlugInRef self, CMIOObjectID objectID);
static Boolean  DAL_ObjectHasProperty(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address);
static OSStatus DAL_ObjectIsPropertySettable(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, Boolean *isSettable);
static OSStatus DAL_ObjectGetPropertyDataSize(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 *dataSize);
static OSStatus DAL_ObjectGetPropertyData(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 dataSize, UInt32 *dataUsed, void *data);
static OSStatus DAL_ObjectSetPropertyData(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 dataSize, const void *data);
static OSStatus DAL_DeviceSuspend(CMIOHardwarePlugInRef self, CMIODeviceID deviceID);
static OSStatus DAL_DeviceResume(CMIOHardwarePlugInRef self, CMIODeviceID deviceID);
static OSStatus DAL_DeviceStartStream(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, CMIOStreamID streamID);
static OSStatus DAL_DeviceStopStream(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, CMIOStreamID streamID);
static OSStatus DAL_DeviceProcessScheduledOutput(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, const AudioTimeStamp *scheduledOutput);

static HRESULT DAL_QueryInterface(void *thisPointer, CFUUIDBytes iid, LPVOID *ppv);
static ULONG   DAL_AddRef(void *thisPointer);
static ULONG   DAL_Release(void *thisPointer);

static CMIOHardwarePlugInInterface g_DALInterface = {
    ._reserved = NULL,
    .QueryInterface = DAL_QueryInterface,
    .AddRef = DAL_AddRef,
    .Release = DAL_Release,
    .Initialize = DAL_Initialize,
    .InitializeWithObjectID = DAL_InitializeWithObjectID,
    .Teardown = DAL_Teardown,
    .ObjectShow = DAL_ObjectShow,
    .ObjectHasProperty = DAL_ObjectHasProperty,
    .ObjectIsPropertySettable = DAL_ObjectIsPropertySettable,
    .ObjectGetPropertyDataSize = DAL_ObjectGetPropertyDataSize,
    .ObjectGetPropertyData = DAL_ObjectGetPropertyData,
    .ObjectSetPropertyData = DAL_ObjectSetPropertyData,
    .DeviceSuspend = DAL_DeviceSuspend,
    .DeviceResume = DAL_DeviceResume,
    .DeviceStartStream = DAL_DeviceStartStream,
    .DeviceStopStream = DAL_DeviceStopStream
};

/* ========================================================================= */
/* WORKER THREAD (Frame reader from shared memory IPC)                      */
/* ========================================================================= */

/* Pumps video frames from shared memory IPC into the CoreMediaIO DAL stream pipeline. */
static void *DAL_WorkerThread(void *arg)
{
    OpenAVXDALPlugin *plug = (OpenAVXDALPlugin*)arg;
    uint8_t *frame_buf = (uint8_t*)malloc(1920 * 1080 * 2);
    if (!frame_buf) return NULL;

    /* Fill default placeholder pattern */
    for (size_t i = 0; i < 1920 * 1080 * 2; i += 4) {
        frame_buf[i + 0] = 0x80; /* U */
        frame_buf[i + 1] = 0x40; /* Y0 */
        frame_buf[i + 2] = 0x80; /* V */
        frame_buf[i + 3] = 0x40; /* Y1 */
    }

    uint64_t last_seq = 0;
    while (plug->threadRunning) {
        if (plug->consumer) {
            avx_virtcam_shm_header_t hdr;
            if (avx_virtcam_consumer_read_frame(plug->consumer, &hdr, frame_buf, 1920 * 1080 * 2, &last_seq)) {
                /* Frame received cleanly from OpenAVX IPC */
            }
        }
        usleep(33333); /* ~30 fps cadence */
    }

    free(frame_buf);
    return NULL;
}

/* ========================================================================= */
/* IUNKNOWN IMPLEMENTATION                                                   */
/* ========================================================================= */

/* CMIO Hardware Plugin Interface UUID: 6D343053-93BF-11D8-8534-000A95881667 */
static const CFUUIDBytes s_CMIOHardwarePlugInInterfaceUUID = {
    0x6d, 0x34, 0x30, 0x53, 0x93, 0xbf, 0x11, 0xd8,
    0x85, 0x34, 0x00, 0x0a, 0x95, 0x88, 0x16, 0x67
};

static HRESULT DAL_QueryInterface(void *thisPointer, CFUUIDBytes iid, LPVOID *ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;

    if (memcmp(&iid, &s_CMIOHardwarePlugInInterfaceUUID, sizeof(CFUUIDBytes)) == 0) {
        *ppv = thisPointer;
        DAL_AddRef(thisPointer);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG DAL_AddRef(void *thisPointer)
{
    OpenAVXDALPlugin *p = (OpenAVXDALPlugin*)thisPointer;
    return __sync_add_and_fetch(&p->refCount, 1);
}

static ULONG DAL_Release(void *thisPointer)
{
    OpenAVXDALPlugin *p = (OpenAVXDALPlugin*)thisPointer;
    ULONG c = __sync_sub_and_fetch(&p->refCount, 1);
    if (c == 0) {
        DAL_Teardown((CMIOHardwarePlugInRef)thisPointer);
        pthread_mutex_destroy(&p->lock);
        free(p);
    }
    return c;
}

/* ========================================================================= */
/* CMIO HARDWARE PLUGIN METHODS                                              */
/* ========================================================================= */

static OSStatus DAL_Initialize(CMIOHardwarePlugInRef self)
{
    (void)self;
    return kCMIOHardwareNoError;
}

static OSStatus DAL_InitializeWithObjectID(CMIOHardwarePlugInRef self, CMIOObjectID objectID)
{
    (void)self;
    (void)objectID;
    return kCMIOHardwareNoError;
}

static OSStatus DAL_Teardown(CMIOHardwarePlugInRef self)
{
    OpenAVXDALPlugin *plug = (OpenAVXDALPlugin*)self;
    if (plug->threadRunning) {
        plug->threadRunning = false;
        pthread_join(plug->thread, NULL);
    }
    if (plug->consumer) {
        avx_virtcam_consumer_destroy(plug->consumer);
        plug->consumer = NULL;
    }
    return kCMIOHardwareNoError;
}

static void DAL_ObjectShow(CMIOHardwarePlugInRef self, CMIOObjectID objectID)
{
    (void)self;
    (void)objectID;
}

static Boolean DAL_ObjectHasProperty(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address)
{
    (void)self;
    if (!address) return false;

    if (objectID == kCMIOObjectSystemObject) {
        switch (address->mSelector) {
            case kCMIOObjectPropertyName:
            case kCMIOObjectPropertyManufacturer:
            case kCMIODevicePropertyStreams:
                return true;
            default:
                return false;
        }
    } else if (objectID == kOpenAVXDeviceID) {
        switch (address->mSelector) {
            case kCMIOObjectPropertyName:
            case kCMIOObjectPropertyManufacturer:
            case kCMIODevicePropertyDeviceUID:
            case kCMIODevicePropertyModelUID:
            case kCMIODevicePropertyTransportType:
            case kCMIODevicePropertyDeviceIsAlive:
            case kCMIODevicePropertyDeviceIsRunning:
            case kCMIODevicePropertyStreams:
                return true;
            default:
                return false;
        }
    } else if (objectID == kOpenAVXStreamID) {
        switch (address->mSelector) {
            case kCMIOStreamPropertyDirection:
            case kCMIOStreamPropertyStartingChannel:
            case kCMIOStreamPropertyFormatDescription:
            case kCMIOStreamPropertyFormatDescriptions:
            case kCMIOStreamPropertyFrameRate:
            case kCMIOStreamPropertyFrameRates:
                return true;
            default:
                return false;
        }
    }
    return false;
}

static OSStatus DAL_ObjectIsPropertySettable(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, Boolean *isSettable)
{
    (void)self;
    (void)objectID;
    (void)address;
    if (isSettable) *isSettable = false;
    return kCMIOHardwareNoError;
}

static OSStatus DAL_ObjectGetPropertyDataSize(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 *dataSize)
{
    (void)self;
    (void)qualifierDataSize;
    (void)qualifierData;
    if (!address || !dataSize) return kCMIOHardwareBadObjectError;

    if (objectID == kCMIOObjectSystemObject) {
        if (address->mSelector == kCMIODevicePropertyStreams) {
            *dataSize = sizeof(CMIODeviceID); /* Returns 1 device: kOpenAVXDeviceID */
            return kCMIOHardwareNoError;
        }
    } else if (objectID == kOpenAVXDeviceID) {
        switch (address->mSelector) {
            case kCMIOObjectPropertyName:
            case kCMIOObjectPropertyManufacturer:
            case kCMIODevicePropertyDeviceUID:
            case kCMIODevicePropertyModelUID:
                *dataSize = sizeof(CFStringRef);
                return kCMIOHardwareNoError;
            case kCMIODevicePropertyTransportType:
                *dataSize = sizeof(UInt32);
                return kCMIOHardwareNoError;
            case kCMIODevicePropertyDeviceIsAlive:
            case kCMIODevicePropertyDeviceIsRunning:
                *dataSize = sizeof(UInt32);
                return kCMIOHardwareNoError;
            case kCMIODevicePropertyStreams:
                *dataSize = sizeof(CMIOStreamID);
                return kCMIOHardwareNoError;
            default:
                break;
        }
    } else if (objectID == kOpenAVXStreamID) {
        switch (address->mSelector) {
            case kCMIOStreamPropertyDirection:
            case kCMIOStreamPropertyStartingChannel:
                *dataSize = sizeof(UInt32);
                return kCMIOHardwareNoError;
            case kCMIOStreamPropertyFormatDescription:
            case kCMIOStreamPropertyFormatDescriptions:
                *dataSize = sizeof(void*);
                return kCMIOHardwareNoError;
            case kCMIOStreamPropertyFrameRate:
                *dataSize = sizeof(double);
                return kCMIOHardwareNoError;
            case kCMIOStreamPropertyFrameRates:
                *dataSize = sizeof(double) * 2;
                return kCMIOHardwareNoError;
            default:
                break;
        }
    }

    return kCMIOHardwareUnknownPropertyError;
}

static OSStatus DAL_ObjectGetPropertyData(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 dataSize, UInt32 *dataUsed, void *data)
{
    OpenAVXDALPlugin *plug = (OpenAVXDALPlugin*)self;
    (void)qualifierDataSize;
    (void)qualifierData;
    if (!address || !data || !dataUsed) return kCMIOHardwareBadObjectError;

    if (objectID == kCMIOObjectSystemObject) {
        if (address->mSelector == kCMIODevicePropertyStreams) {
            if (dataSize < sizeof(CMIODeviceID)) return kCMIOHardwareBadObjectError;
            CMIODeviceID *devs = (CMIODeviceID*)data;
            devs[0] = kOpenAVXDeviceID;
            *dataUsed = sizeof(CMIODeviceID);
            return kCMIOHardwareNoError;
        }
    } else if (objectID == kOpenAVXDeviceID) {
        switch (address->mSelector) {
            case kCMIODevicePropertyDeviceIsAlive: {
                if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadObjectError;
                *(UInt32*)data = 1;
                *dataUsed = sizeof(UInt32);
                return kCMIOHardwareNoError;
            }
            case kCMIODevicePropertyDeviceIsRunning: {
                if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadObjectError;
                *(UInt32*)data = plug->isRunning ? 1 : 0;
                *dataUsed = sizeof(UInt32);
                return kCMIOHardwareNoError;
            }
            case kCMIODevicePropertyTransportType: {
                if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadObjectError;
                *(UInt32*)data = 0x76697274; /* 'virt' */
                *dataUsed = sizeof(UInt32);
                return kCMIOHardwareNoError;
            }
            case kCMIODevicePropertyStreams: {
                if (dataSize < sizeof(CMIOStreamID)) return kCMIOHardwareBadObjectError;
                *(CMIOStreamID*)data = kOpenAVXStreamID;
                *dataUsed = sizeof(CMIOStreamID);
                return kCMIOHardwareNoError;
            }
            default:
                break;
        }
    } else if (objectID == kOpenAVXStreamID) {
        switch (address->mSelector) {
            case kCMIOStreamPropertyDirection: {
                if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadObjectError;
                *(UInt32*)data = 1; /* Output to host application */
                *dataUsed = sizeof(UInt32);
                return kCMIOHardwareNoError;
            }
            case kCMIOStreamPropertyStartingChannel: {
                if (dataSize < sizeof(UInt32)) return kCMIOHardwareBadObjectError;
                *(UInt32*)data = 1;
                *dataUsed = sizeof(UInt32);
                return kCMIOHardwareNoError;
            }
            case kCMIOStreamPropertyFrameRate: {
                if (dataSize < sizeof(double)) return kCMIOHardwareBadObjectError;
                *(double*)data = 30.0;
                *dataUsed = sizeof(double);
                return kCMIOHardwareNoError;
            }
            case kCMIOStreamPropertyFrameRates: {
                if (dataSize < sizeof(double) * 2) return kCMIOHardwareBadObjectError;
                double *rates = (double*)data;
                rates[0] = 30.0;
                rates[1] = 60.0;
                *dataUsed = sizeof(double) * 2;
                return kCMIOHardwareNoError;
            }
            default:
                break;
        }
    }

    return kCMIOHardwareUnknownPropertyError;
}

static OSStatus DAL_ObjectSetPropertyData(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 dataSize, const void *data)
{
    (void)self;
    (void)objectID;
    (void)address;
    (void)qualifierDataSize;
    (void)qualifierData;
    (void)dataSize;
    (void)data;
    return kCMIOHardwareUnknownPropertyError;
}

static OSStatus DAL_DeviceSuspend(CMIOHardwarePlugInRef self, CMIODeviceID deviceID)
{
    (void)self;
    (void)deviceID;
    return kCMIOHardwareNoError;
}

static OSStatus DAL_DeviceResume(CMIOHardwarePlugInRef self, CMIODeviceID deviceID)
{
    (void)self;
    (void)deviceID;
    return kCMIOHardwareNoError;
}

/* Attaches shared memory consumer and starts background frame pump for CoreMediaIO. */
static OSStatus DAL_DeviceStartStream(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, CMIOStreamID streamID)
{
    (void)deviceID;
    (void)streamID;
    OpenAVXDALPlugin *plug = (OpenAVXDALPlugin*)self;
    pthread_mutex_lock(&plug->lock);

    if (!plug->consumer) {
        plug->consumer = avx_virtcam_consumer_create();
    }

    if (!plug->threadRunning) {
        plug->threadRunning = true;
        pthread_create(&plug->thread, NULL, DAL_WorkerThread, plug);
    }
    plug->isRunning = true;

    pthread_mutex_unlock(&plug->lock);
    return kCMIOHardwareNoError;
}

/* Halts worker thread and detaches virtual camera shared memory consumer. */
static OSStatus DAL_DeviceStopStream(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, CMIOStreamID streamID)
{
    (void)deviceID;
    (void)streamID;
    OpenAVXDALPlugin *plug = (OpenAVXDALPlugin*)self;
    pthread_mutex_lock(&plug->lock);

    plug->isRunning = false;
    if (plug->threadRunning) {
        plug->threadRunning = false;
        pthread_join(plug->thread, NULL);
    }
    if (plug->consumer) {
        avx_virtcam_consumer_destroy(plug->consumer);
        plug->consumer = NULL;
    }

    pthread_mutex_unlock(&plug->lock);
    return kCMIOHardwareNoError;
}

static OSStatus DAL_DeviceProcessScheduledOutput(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, const AudioTimeStamp *scheduledOutput)
{
    (void)self;
    (void)deviceID;
    (void)scheduledOutput;
    return kCMIOHardwareNoError;
}

/* ========================================================================= */
/* PLUGIN FACTORY ENTRY POINT                                                */
/* ========================================================================= */

/* Factory entry point creating CMIOHardwarePlugInInterface COM instance for macOS DAL. */
OPENAVX_API void* OpenAVXDALPluginFactory(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID)
{
    (void)allocator;
    (void)requestedTypeUUID;

    OpenAVXDALPlugin *plug = (OpenAVXDALPlugin*)calloc(1, sizeof(OpenAVXDALPlugin));
    if (!plug) return NULL;

    plug->vtbl = &g_DALInterface;
    plug->refCount = 1;
    pthread_mutex_init(&plug->lock, NULL);

    return (void*)plug;
}
