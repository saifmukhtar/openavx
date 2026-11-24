/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/unistd.h - POSIX unistd.h emulation for Windows MSVC
 */

#ifndef OPENAVX_COMPAT_WIN32_UNISTD_H
#define OPENAVX_COMPAT_WIN32_UNISTD_H

#if defined(_WIN32) || defined(_MSC_VER)

#include "avx_platform.h"
#include <io.h>
#include <process.h>
#include <direct.h>
#include <stdio.h>

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 0
#endif
#ifndef F_OK
#define F_OK 0
#endif

#ifndef usleep
#define usleep(us) avx_sleep_ms((DWORD)((us) / 1000))
#endif
#ifndef sleep
#define sleep(sec) avx_sleep_ms((DWORD)((sec) * 1000))
#endif

#ifndef isatty
#define isatty _isatty
#endif

#ifndef popen
#define popen _popen
#endif
#ifndef pclose
#define pclose _pclose
#endif

#ifndef WIFEXITED
#define WIFEXITED(s) 1
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(s) (s)
#endif

#endif /* _WIN32 */

#endif /* OPENAVX_COMPAT_WIN32_UNISTD_H */
