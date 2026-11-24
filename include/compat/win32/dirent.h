/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/dirent.h - Win32 emulation of dirent.h for MSVC
 */

#ifndef OPENAVX_COMPAT_WIN32_DIRENT_H
#define OPENAVX_COMPAT_WIN32_DIRENT_H

#if defined(__MINGW32__) || defined(__MINGW64__)
#include_next <dirent.h>
#elif defined(_MSC_VER) || defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

struct dirent {
    char d_name[MAX_PATH];
};

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA find_data;
    struct dirent ent;
    int first;
} DIR;

static inline DIR* opendir(const char *name) {
    if (!name) return NULL;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*.*", name);
    DIR *d = (DIR*)malloc(sizeof(DIR));
    if (!d) return NULL;
    d->handle = FindFirstFileA(search_path, &d->find_data);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static inline struct dirent* readdir(DIR *d) {
    if (!d || d->handle == INVALID_HANDLE_VALUE) return NULL;
    if (d->first) {
        d->first = 0;
        strncpy(d->ent.d_name, d->find_data.cFileName, sizeof(d->ent.d_name) - 1);
        d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
        return &d->ent;
    }
    if (FindNextFileA(d->handle, &d->find_data)) {
        strncpy(d->ent.d_name, d->find_data.cFileName, sizeof(d->ent.d_name) - 1);
        d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
        return &d->ent;
    }
    return NULL;
}

static inline int closedir(DIR *d) {
    if (!d) return -1;
    if (d->handle != INVALID_HANDLE_VALUE) {
        FindClose(d->handle);
    }
    free(d);
    return 0;
}

#ifndef S_ISREG
#if defined(_S_IFMT) && defined(_S_IFREG)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#elif defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#else
#define S_ISREG(m) (((m) & 0170000) == 0100000)
#endif
#endif

#ifndef S_ISDIR
#if defined(_S_IFMT) && defined(_S_IFDIR)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#elif defined(S_IFMT) && defined(S_IFDIR)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#else
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#endif
#endif

#endif /* __MINGW32__ vs _MSC_VER */

#endif /* OPENAVX_COMPAT_WIN32_DIRENT_H */
