/*
 * Copyright (c) 2005-2024 Apple Inc. All rights reserved.
 *
 * CoreMediaIO DAL Plugin Interface Definitions
 *
 * This header contains interface declarations, UUIDs, FourCCs, and structures
 * derived from Apple's CoreMediaIO framework (macOS SDK).
 *
 * Notice: This copy is maintained in third-party/proprietary/ for offline platform
 * compatibility and cross-compilation testing on non-macOS environments.
 * When compiling natively on macOS, the system framework headers from Xcode SDK
 * are used.
 */

#ifndef AVX_COMPAT_CMIO_HARDWARE_PLUGIN_H
#define AVX_COMPAT_CMIO_HARDWARE_PLUGIN_H

#if defined(AVX_OS_MACOS) && !defined(AVX_FORCE_COMPAT_CMIO)

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreMediaIO/CMIOHardwarePlugIn.h>
#include <CoreMediaIO/CMIOHardwareStream.h>
#include <CoreMediaIO/CMIOHardwareDevice.h>

#else

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* CoreFoundation Base Types */
typedef const void *CFAllocatorRef;
typedef const void *CFUUIDRef;
typedef const void *CFStringRef;
typedef const void *CFTypeRef;
typedef const void *CFArrayRef;

typedef struct CFUUIDBytes {
    uint8_t byte0;  uint8_t byte1;  uint8_t byte2;  uint8_t byte3;
    uint8_t byte4;  uint8_t byte5;  uint8_t byte6;  uint8_t byte7;
    uint8_t byte8;  uint8_t byte9;  uint8_t byte10; uint8_t byte11;
    uint8_t byte12; uint8_t byte13; uint8_t byte14; uint8_t byte15;
} CFUUIDBytes;

/* Apple / COM Base Types */
typedef int32_t   OSStatus;
typedef uint32_t  UInt32;
typedef int32_t   SInt32;
typedef uint64_t  UInt64;
typedef uint8_t   Boolean;
typedef int32_t   HRESULT;
typedef uint32_t  ULONG;
typedef void     *LPVOID;

#ifndef S_OK
#define S_OK ((HRESULT)0L)
#endif
#ifndef E_POINTER
#define E_POINTER ((HRESULT)0x80004003L)
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#endif
#ifndef noErr
#define noErr 0
#endif
#ifndef kCMIOHardwareNoError
#define kCMIOHardwareNoError 0
#endif
#ifndef kCMIOHardwareUnspecifiedError
#define kCMIOHardwareUnspecifiedError 0x77686174 /* 'what' */
#endif
#ifndef kCMIOHardwareUnknownPropertyError
#define kCMIOHardwareUnknownPropertyError 0x77686f3f /* 'who?' */
#endif
#ifndef kCMIOHardwareBadObjectError
#define kCMIOHardwareBadObjectError 0x216f626a /* '!obj' */
#endif

/* CoreMediaIO Object Types */
typedef UInt32 CMIOObjectID;
typedef CMIOObjectID CMIODeviceID;
typedef CMIOObjectID CMIOStreamID;
typedef CMIOObjectID CMIOControlID;
typedef UInt32 CMIOClassID;

/* Special Object IDs */
#define kCMIOObjectSystemObject              1
#define kCMIOObjectUnknown                   0

/* Object Scopes & Elements */
#define kCMIOObjectPropertyScopeGlobal       0x676c6f62 /* 'glob' */
#define kCMIOObjectPropertyScopeInput        0x696e7074 /* 'inpt' */
#define kCMIOObjectPropertyScopeOutput       0x6f757470 /* 'outp' */
#define kCMIOObjectPropertyScopeWildcard     0x2a2a2a2a /* '****' */
#define kCMIOObjectPropertyElementMaster     0
#define kCMIOObjectPropertyElementWildcard   0xFFFFFFFF

/* CoreMediaIO Property Address */
typedef struct CMIOObjectPropertyAddress {
    UInt32 mSelector;
    UInt32 mScope;
    UInt32 mElement;
} CMIOObjectPropertyAddress;

/* Property Selectors */
#define kCMIOObjectPropertyClass             0x636c6173 /* 'clas' */
#define kCMIOObjectPropertyOwner             0x73746476 /* 'stdv' */
#define kCMIOObjectPropertyCreator           0x6f706c67 /* 'oplg' */
#define kCMIOObjectPropertyName              0x6c6e616d /* 'lnam' */
#define kCMIOObjectPropertyManufacturer      0x6c6d616b /* 'lmak' */
#define kCMIOObjectPropertyModelUID          0x6d756964 /* 'muid' */

/* Device Properties */
#define kCMIODevicePropertyDeviceUID         0x75696465 /* 'uide' */
#define kCMIODevicePropertyModelUID          0x6d756964 /* 'muid' */
#define kCMIODevicePropertyTransportType     0x7472616e /* 'tran' */
#define kCMIODevicePropertyDeviceIsAlive     0x6c69766e /* 'livn' */
#define kCMIODevicePropertyDeviceIsRunning   0x676f696e /* 'goin' */
#define kCMIODevicePropertyStreams           0x73746d23 /* 'stm#' */
#define kCMIODevicePropertyExcludeNonDALAccess 0x7864616c /* 'xdal' */

/* Stream Properties */
#define kCMIOStreamPropertyDirection         0x73646972 /* 'sdir' */
#define kCMIOStreamPropertyTerminalType      0x7465726d /* 'term' */
#define kCMIOStreamPropertyStartingChannel   0x7363686e /* 'schn' */
#define kCMIOStreamPropertyLatency           0x6c746e63 /* 'ltnc' */
#define kCMIOStreamPropertyFormatDescription 0x70667420 /* 'pft ' */
#define kCMIOStreamPropertyFormatDescriptions 0x70667473 /* 'pfts' */
#define kCMIOStreamPropertyFrameRate         0x6e667274 /* 'nfrt' */
#define kCMIOStreamPropertyFrameRates        0x6e667223 /* 'nfr#' */

/* AudioTimeStamp representation */
typedef struct AudioTimeStamp {
    double    mSampleTime;
    UInt64    mHostTime;
    double    mRateScalar;
    UInt64    mWordClockTime;
    UInt32    mSMPTETime[8];
    UInt32    mFlags;
    UInt32    mReserved;
} AudioTimeStamp;

/* CoreMedia Video Dimensions */
typedef struct CMVideoDimensions {
    int32_t width;
    int32_t height;
} CMVideoDimensions;

/* CMIOHardwarePlugIn Forward References */
typedef struct CMIOHardwarePlugInInterface CMIOHardwarePlugInInterface;
typedef CMIOHardwarePlugInInterface **CMIOHardwarePlugInRef;

struct CMIOHardwarePlugInInterface {
    /* IUnknown */
    void*    _reserved;
    HRESULT  (*QueryInterface)(void *thisPointer, CFUUIDBytes iid, LPVOID *ppv);
    ULONG    (*AddRef)(void *thisPointer);
    ULONG    (*Release)(void *thisPointer);

    /* CMIOHardwarePlugIn Interface */
    OSStatus (*Initialize)(CMIOHardwarePlugInRef self);
    OSStatus (*InitializeWithObjectID)(CMIOHardwarePlugInRef self, CMIOObjectID objectID);
    OSStatus (*Teardown)(CMIOHardwarePlugInRef self);
    void     (*ObjectShow)(CMIOHardwarePlugInRef self, CMIOObjectID objectID);
    Boolean  (*ObjectHasProperty)(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address);
    OSStatus (*ObjectIsPropertySettable)(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, Boolean *isSettable);
    OSStatus (*ObjectGetPropertyDataSize)(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 *dataSize);
    OSStatus (*ObjectGetPropertyData)(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 dataSize, UInt32 *dataUsed, void *data);
    OSStatus (*ObjectSetPropertyData)(CMIOHardwarePlugInRef self, CMIOObjectID objectID, const CMIOObjectPropertyAddress *address, UInt32 qualifierDataSize, const void *qualifierData, UInt32 dataSize, const void *data);
    OSStatus (*DeviceSuspend)(CMIOHardwarePlugInRef self, CMIODeviceID deviceID);
    OSStatus (*DeviceResume)(CMIOHardwarePlugInRef self, CMIODeviceID deviceID);
    OSStatus (*DeviceStartStream)(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, CMIOStreamID streamID);
    OSStatus (*DeviceStopStream)(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, CMIOStreamID streamID);
    OSStatus (*DeviceProcessScheduledOutput)(CMIOHardwarePlugInRef self, CMIODeviceID deviceID, const AudioTimeStamp *scheduledOutput);
};

/* CMIO Hardware Plugin Type UUID: 30010C1C-93BF-11D8-8B5B-000A95881667 */
static const CFUUIDBytes kCMIOHardwarePlugInTypeUUID = {
    0x30, 0x01, 0x0c, 0x1c, 0x93, 0xbf, 0x11, 0xd8,
    0x8b, 0x5b, 0x00, 0x0a, 0x95, 0x88, 0x16, 0x67
};

/* CMIO Hardware Plugin Interface UUID: 6D343053-93BF-11D8-8534-000A95881667 */
static const CFUUIDBytes kCMIOHardwarePlugInInterfaceUUID = {
    0x6d, 0x34, 0x30, 0x53, 0x93, 0xbf, 0x11, 0xd8,
    0x85, 0x34, 0x00, 0x0a, 0x95, 0x88, 0x16, 0x67
};

#endif /* AVX_OS_MACOS */

#endif /* AVX_COMPAT_CMIO_HARDWARE_PLUGIN_H */
