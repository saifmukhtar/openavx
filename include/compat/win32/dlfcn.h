/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/dlfcn.h - Win32 emulation of dlfcn.h
 */

#ifndef OPENAVX_COMPAT_WIN32_DLFCN_H
#define OPENAVX_COMPAT_WIN32_DLFCN_H

#if defined(_WIN32) || defined(_MSC_VER)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef RTLD_LAZY
#define RTLD_LAZY 0x0001
#endif
#ifndef RTLD_NOW
#define RTLD_NOW 0x0002
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0x0100
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0x0000
#endif

static inline void* dlopen(const char* filename, int flags) {
    (void)flags;
    if (!filename) return NULL;
    return (void*)LoadLibraryA(filename);
}

static inline void* dlsym(void* handle, const char* symbol) {
    if (!handle || !symbol) return NULL;
    return (void*)GetProcAddress((HMODULE)handle, symbol);
}

static inline int dlclose(void* handle) {
    if (!handle) return -1;
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

static inline const char* dlerror(void) {
    return "Dynamic load error";
}

#endif /* __MINGW32__ vs _MSC_VER */

#endif /* OPENAVX_COMPAT_WIN32_DLFCN_H */
