/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_dshow_sink.c - Windows DirectShow Virtual Camera COM Filter
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "openavx.h"
#include "avx_dshow_sink.h"
#include "avx_virtcam_ipc.h"
#include "avx_platform.h"

#if defined(AVX_OS_WINDOWS)
#include <windows.h>
#include <guiddef.h>

/* OpenAVX Virtual Camera GUIDs */
/* CLSID_OpenAVXCam: {9C86F6B0-80E2-4AE8-B937-2E8B9915D2C1} */
static const GUID CLSID_OpenAVXCam = 
    { 0x9c86f6b0, 0x80e2, 0x4ae8, { 0xb9, 0x37, 0x2e, 0x8b, 0x99, 0x15, 0xd2, 0xc1 } };

/* CLSID_VideoInputDeviceCategory: {860BB310-5D01-11d0-BD3B-00A0C911CE86} */
static const char *k_video_category_guid = "{860BB310-5D01-11d0-BD3B-00A0C911CE86}";
static const char *k_openavx_clsid_str   = "{9C86F6B0-80E2-4AE8-B937-2E8B9915D2C1}";

/* Registers CLSID and InprocServer32 in HKCU under Video Input Device category without elevation. */
bool avx_dshow_register_filter(const char *dll_path)
{
    char full_dll[MAX_PATH] = "";
    if (dll_path && dll_path[0] != '\0') {
        GetFullPathNameA(dll_path, MAX_PATH, full_dll, NULL);
    } else {
        HMODULE hMod = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&avx_dshow_register_filter, &hMod);
        GetModuleFileNameA(hMod, full_dll, MAX_PATH);
    }

    char subkey[512];
    HKEY hKey = NULL;

    /* 1. Register CLSID in HKCU\Software\Classes\CLSID\{GUID} */
    snprintf(subkey, sizeof(subkey), "Software\\Classes\\CLSID\\%s", k_openavx_clsid_str);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, subkey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) {
        return false;
    }
    const char *friendly_name = "OpenAVX Virtual Camera";
    RegSetValueExA(hKey, NULL, 0, REG_SZ, (const BYTE*)friendly_name, (DWORD)strlen(friendly_name) + 1);
    RegCloseKey(hKey);

    /* 2. Register InprocServer32 */
    snprintf(subkey, sizeof(subkey), "Software\\Classes\\CLSID\\%s\\InprocServer32", k_openavx_clsid_str);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, subkey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, NULL, 0, REG_SZ, (const BYTE*)full_dll, (DWORD)strlen(full_dll) + 1);
        const char *model = "Both";
        RegSetValueExA(hKey, "ThreadingModel", 0, REG_SZ, (const BYTE*)model, (DWORD)strlen(model) + 1);
        RegCloseKey(hKey);
    }

    /* 3. Register under Video Input Device Category (System Camera Enumeration) */
    snprintf(subkey, sizeof(subkey), "Software\\Classes\\CLSID\\%s\\Instance\\%s", k_video_category_guid, k_openavx_clsid_str);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, subkey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "FriendlyName", 0, REG_SZ, (const BYTE*)friendly_name, (DWORD)strlen(friendly_name) + 1);
        RegSetValueExA(hKey, "CLSID", 0, REG_SZ, (const BYTE*)k_openavx_clsid_str, (DWORD)strlen(k_openavx_clsid_str) + 1);
        RegCloseKey(hKey);
    }

    oavx_log_write(OAVX_LOG_INFO, "dshow", "Registered OpenAVX Virtual Camera in HKCU (Zero Admin): %s", full_dll);
    return true;
}

/* Removes OpenAVX virtual camera CLSID entries from user registry hive. */
bool avx_dshow_unregister_filter(void)
{
    char subkey[512];

    snprintf(subkey, sizeof(subkey), "Software\\Classes\\CLSID\\%s\\Instance\\%s", k_video_category_guid, k_openavx_clsid_str);
    RegDeleteKeyA(HKEY_CURRENT_USER, subkey);

    snprintf(subkey, sizeof(subkey), "Software\\Classes\\CLSID\\%s\\InprocServer32", k_openavx_clsid_str);
    RegDeleteKeyA(HKEY_CURRENT_USER, subkey);

    snprintf(subkey, sizeof(subkey), "Software\\Classes\\CLSID\\%s", k_openavx_clsid_str);
    RegDeleteKeyA(HKEY_CURRENT_USER, subkey);

    oavx_log_write(OAVX_LOG_INFO, "dshow", "Unregistered OpenAVX Virtual Camera from HKCU");
    return true;
}

bool avx_dshow_is_registered(void)
{
    char subkey[512];
    HKEY hKey = NULL;
    snprintf(subkey, sizeof(subkey), "Software\\Classes\\CLSID\\%s\\Instance\\%s", k_video_category_guid, k_openavx_clsid_str);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

#else /* Non-Windows Stubs */

bool avx_dshow_register_filter(const char *dll_path) { (void)dll_path; return false; }
bool avx_dshow_unregister_filter(void) { return false; }
bool avx_dshow_is_registered(void) { return false; }

#endif
