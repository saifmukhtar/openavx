/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/unistd.h - POSIX unistd.h emulation for Windows MSVC
 */

#ifndef OPENAVX_COMPAT_WIN32_UNISTD_H
#define OPENAVX_COMPAT_WIN32_UNISTD_H

#if defined(__MINGW32__) || defined(__MINGW64__)
#include_next <unistd.h>
#endif

#if defined(_WIN32) || defined(_MSC_VER)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "compat/win32/pthread.h"

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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#if defined(_MSC_VER)
typedef unsigned int useconds_t;
typedef int pid_t;
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif

#ifndef SIGKILL
#define SIGKILL 9
#endif

#ifndef WIFEXITED
#define WIFEXITED(s) 1
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(s) (s)
#endif
#ifndef WIFSIGNALED
#define WIFSIGNALED(s) 0
#endif
#ifndef WTERMSIG
#define WTERMSIG(s) 0
#endif

#if defined(_MSC_VER)
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

static inline int clock_gettime(int clk_id, struct timespec *tp) {
    (void)clk_id;
    if (!tp) return -1;
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    tp->tv_sec = (time_t)(count.QuadPart / freq.QuadPart);
    tp->tv_nsec = (long)(((count.QuadPart % freq.QuadPart) * 1000000000ULL) / freq.QuadPart);
    return 0;
}

static inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    if (!req) return -1;
    DWORD ms = (DWORD)(req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000);
    Sleep(ms);
    return 0;
}

static inline int usleep(useconds_t us) {
    Sleep((DWORD)((us + 999) / 1000));
    return 0;
}

static inline unsigned int sleep(unsigned int sec) {
    Sleep((DWORD)(sec * 1000));
    return 0;
}

static inline int kill(pid_t pid, int sig) {
    (void)pid;
    (void)sig;
    return 0;
}

static inline pid_t waitpid(pid_t pid, int *status, int options) {
    (void)status;
    (void)options;
    return pid;
}

static inline unsigned int alarm(unsigned int sec) {
    (void)sec;
    return 0;
}
#endif /* _MSC_VER */

static inline char* mkdtemp(char *tmpl) {
    if (!tmpl) return NULL;
    char *p = _mktemp(tmpl);
    if (!p) return NULL;
    if (_mkdir(p) != 0) return NULL;
    return p;
}

static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}

static inline int unsetenv(const char *name) {
    return _putenv_s(name, "");
}

#ifndef isatty
#define isatty _isatty
#endif

#ifndef popen
#define popen _popen
#endif
#ifndef pclose
#define pclose _pclose
#endif

#ifndef rmdir
#define rmdir _rmdir
#endif
#ifndef unlink
#define unlink _unlink
#endif

static inline int win32_close_fd_or_socket(int fd) {
    if (closesocket((SOCKET)fd) == 0) {
        return 0;
    }
    return _close(fd);
}
#undef close
#define close(fd) win32_close_fd_or_socket(fd)

#endif /* _WIN32 */

#endif /* OPENAVX_COMPAT_WIN32_UNISTD_H */
