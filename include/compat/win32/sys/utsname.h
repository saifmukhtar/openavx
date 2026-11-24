/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/sys/utsname.h - MSVC emulation of sys/utsname.h
 */

#ifndef OPENAVX_COMPAT_WIN32_SYS_UTSNAME_H
#define OPENAVX_COMPAT_WIN32_SYS_UTSNAME_H

#ifdef _WIN32
#include <string.h>

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

static inline int uname(struct utsname *name) {
    if (!name) return -1;
    strncpy(name->sysname, "Windows", sizeof(name->sysname) - 1);
    strncpy(name->nodename, "localhost", sizeof(name->nodename) - 1);
    strncpy(name->release, "10.0", sizeof(name->release) - 1);
    strncpy(name->version, "Windows", sizeof(name->version) - 1);
    strncpy(name->machine, "x86_64", sizeof(name->machine) - 1);
    return 0;
}
#endif

#endif /* OPENAVX_COMPAT_WIN32_SYS_UTSNAME_H */
