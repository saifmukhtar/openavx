/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/sys/uio.h - Win32 emulation of sys/uio.h
 */

#ifndef OPENAVX_COMPAT_WIN32_SYS_UIO_H
#define OPENAVX_COMPAT_WIN32_SYS_UIO_H

#if defined(_WIN32) || defined(_MSC_VER)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stddef.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};

static inline int writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) return -1;
    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        int n = send((SOCKET)fd, (const char*)iov[i].iov_base, (int)iov[i].iov_len, 0);
        if (n < 0) {
            return (total > 0) ? total : -1;
        }
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return total;
}

#endif /* __MINGW32__ vs _MSC_VER */

#endif /* OPENAVX_COMPAT_WIN32_SYS_UIO_H */
