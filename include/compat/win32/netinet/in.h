/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/netinet/in.h - Win32 emulation of netinet/in.h
 */

#ifndef OPENAVX_COMPAT_WIN32_NETINET_IN_H
#define OPENAVX_COMPAT_WIN32_NETINET_IN_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#endif /* OPENAVX_COMPAT_WIN32_NETINET_IN_H */
