/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/sys/socket.h - Win32 emulation of sys/socket.h
 */

#ifndef OPENAVX_COMPAT_WIN32_SYS_SOCKET_H
#define OPENAVX_COMPAT_WIN32_SYS_SOCKET_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string.h>

#ifndef AF_UNIX
#define AF_UNIX 1
#endif

#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif

static inline int socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)domain; (void)type; (void)protocol;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return -1;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    if (bind(listener, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        closesocket(listener);
        return -1;
    }
    int len = sizeof(sin);
    if (getsockname(listener, (struct sockaddr*)&sin, &len) != 0 || listen(listener, 1) != 0) {
        closesocket(listener);
        return -1;
    }
    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    if (c == INVALID_SOCKET) {
        closesocket(listener);
        return -1;
    }
    if (connect(c, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        closesocket(c);
        closesocket(listener);
        return -1;
    }
    SOCKET a = accept(listener, NULL, NULL);
    closesocket(listener);
    if (a == INVALID_SOCKET) {
        closesocket(c);
        return -1;
    }
    sv[0] = (int)c;
    sv[1] = (int)a;
    return 0;
}
#endif

#endif /* OPENAVX_COMPAT_WIN32_SYS_SOCKET_H */
