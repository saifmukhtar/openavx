/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_platform.h - Cross-platform socket, threading, and timer abstractions
 */

#ifndef AVX_PLATFORM_H
#define AVX_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* 1. OS & PLATFORM IDENTIFICATION                                           */
/* ========================================================================= */

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(__CYGWIN__)
#define AVX_OS_WINDOWS 1
#define AVX_PLATFORM_NAME "Windows"
#elif defined(__APPLE__) && defined(__MACH__)
#define AVX_OS_MACOS 1
#define AVX_OS_POSIX 1
#define AVX_PLATFORM_NAME "macOS"
#elif defined(__linux__)
#define AVX_OS_LINUX 1
#define AVX_OS_POSIX 1
#define AVX_PLATFORM_NAME "Linux"
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#define AVX_OS_BSD 1
#define AVX_OS_POSIX 1
#define AVX_PLATFORM_NAME "BSD"
#else
#define AVX_OS_POSIX 1
#define AVX_PLATFORM_NAME "POSIX"
#endif

/* Architecture Identification */
#if defined(__x86_64__) || defined(_M_X64)
#define AVX_ARCH_X86_64 1
#define AVX_ARCH_NAME "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define AVX_ARCH_ARM64 1
#define AVX_ARCH_NAME "ARM64"
#elif defined(__arm__) || defined(_M_ARM)
#define AVX_ARCH_ARM32 1
#define AVX_ARCH_NAME "ARM32"
#elif defined(__i386__) || defined(_M_IX86)
#define AVX_ARCH_X86 1
#define AVX_ARCH_NAME "x86"
#else
#define AVX_ARCH_NAME "Generic"
#endif

/* SIMD Function Target Attributes (Clang/GCC function multi-versioning, no-op on MSVC) */
#if defined(__GNUC__) || defined(__clang__)
#define AVX_TARGET_AVX2 __attribute__((target("avx2,fma")))
#else
#define AVX_TARGET_AVX2
#endif

/* Path separators */
#if defined(AVX_OS_WINDOWS)
#define AVX_PATH_SEP "\\"
#define AVX_PATH_SEP_CHAR '\\'
#else
#define AVX_PATH_SEP "/"
#define AVX_PATH_SEP_CHAR '/'
#endif

/* ========================================================================= */
/* 2. SOCKET & NETWORKING ABSTRACTION                                        */
/* ========================================================================= */

#if defined(AVX_OS_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <io.h>

typedef SOCKET avx_socket_t;
typedef int avx_socklen_t;
typedef WSAPOLLFD avx_pollfd_t;
typedef ULONG avx_nfds_t;

#define AVX_INVALID_SOCKET INVALID_SOCKET
#define AVX_SOCKET_ERROR SOCKET_ERROR
#define avx_socket_is_valid(s) ((s) != INVALID_SOCKET)

#define avx_socket_close(s) closesocket(s)
#define avx_socket_errno() WSAGetLastError()

#define AVX_EWOULDBLOCK WSAEWOULDBLOCK
#define AVX_EAGAIN WSAEWOULDBLOCK
#define AVX_EINPROGRESS WSAEINPROGRESS
#define AVX_EINTR WSAEINTR
#define AVX_ECONNRESET WSAECONNRESET

#define AVX_SHUT_RDWR SD_BOTH
#define AVX_SHUT_RD SD_RECEIVE
#define AVX_SHUT_WR SD_SEND

#define avx_poll WSAPoll

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#if defined(_MSC_VER) && !defined(strtok_r)
#define strtok_r(s, d, p) strtok_s(s, d, p)
#endif

static inline int avx_gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return -1;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    uint64_t t = u.QuadPart - 116444736000000000ULL;
    tv->tv_sec = (long)(t / 10000000ULL);
    tv->tv_usec = (long)((t % 10000000ULL) / 10);
    return 0;
}
#ifndef gettimeofday
#define gettimeofday(tv, tz) avx_gettimeofday((tv), (tz))
#endif

#else /* POSIX (Linux, macOS, BSD) */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

static inline int avx_gettimeofday(struct timeval *tv, void *tz) {
    return gettimeofday(tv, (struct timezone *)tz);
}

typedef int avx_socket_t;
typedef socklen_t avx_socklen_t;
typedef struct pollfd avx_pollfd_t;
typedef nfds_t avx_nfds_t;

#define AVX_INVALID_SOCKET (-1)
#define AVX_SOCKET_ERROR (-1)
#define avx_socket_is_valid(s) ((s) >= 0)

#define avx_socket_close(s) close(s)
#define avx_socket_errno() (errno)

#define AVX_EWOULDBLOCK EWOULDBLOCK
#define AVX_EAGAIN EAGAIN
#define AVX_EINPROGRESS EINPROGRESS
#define AVX_EINTR EINTR
#define AVX_ECONNRESET ECONNRESET

#define AVX_SHUT_RDWR SHUT_RDWR
#define AVX_SHUT_RD SHUT_RD
#define AVX_SHUT_WR SHUT_WR

#define avx_poll poll

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#if defined(_MSC_VER) && !defined(strtok_r)
#define strtok_r(s, d, p) strtok_s(s, d, p)
#endif

#endif

/* Set SO_NOSIGPIPE on socket (macOS & BSD prevent SIGPIPE on broken connections) */
static inline void avx_socket_set_nosigpipe(avx_socket_t s) {
#if (defined(AVX_OS_MACOS) || defined(AVX_OS_BSD)) && defined(SO_NOSIGPIPE)
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&opt, sizeof(opt));
#else
    (void)s;
#endif
}

/* Set socket non-blocking mode */
static inline int avx_socket_set_nonblocking(avx_socket_t s, bool nonblock) {
#if defined(AVX_OS_WINDOWS)
    u_long mode = nonblock ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags);
#endif
}

/* Enable and configure TCP keepalive probes on socket */
static inline int avx_socket_set_keepalive(avx_socket_t s, int idle_sec, int interval_sec, int count) {
    if (!avx_socket_is_valid(s)) return -1;
    int opt = 1;
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&opt, sizeof(opt)) < 0) {
        return -1;
    }

#if defined(AVX_OS_WINDOWS)
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, (const char *)&idle_sec, sizeof(idle_sec));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, (const char *)&interval_sec, sizeof(interval_sec));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, (const char *)&count, sizeof(count));
#elif defined(SIO_KEEPALIVE_VALS)
    struct tcp_keepalive vals;
    vals.onoff = 1;
    vals.keepalivetime = (u_long)(idle_sec * 1000);
    vals.keepaliveinterval = (u_long)(interval_sec * 1000);
    DWORD bytes_ret = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &vals, sizeof(vals), NULL, 0, &bytes_ret, NULL, NULL);
#endif
#else
#if defined(TCP_KEEPIDLE)
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, (const char *)&idle_sec, sizeof(idle_sec));
#elif defined(TCP_KEEPALIVE)
    setsockopt(s, IPPROTO_TCP, TCP_KEEPALIVE, (const char *)&idle_sec, sizeof(idle_sec));
#endif

#if defined(TCP_KEEPINTVL)
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, (const char *)&interval_sec, sizeof(interval_sec));
#endif

#if defined(TCP_KEEPCNT)
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, (const char *)&count, sizeof(count));
#endif
#endif
    return 0;
}

/* Configure TCP_NODELAY (disable Nagle algorithm for low-latency streaming) */
static inline int avx_socket_set_tcp_nodelay(avx_socket_t s, bool nodelay) {
    if (!avx_socket_is_valid(s)) return -1;
    int opt = nodelay ? 1 : 0;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));
}

/* Initialize platform sockets (Winsock startup on Windows, SIGPIPE ignore on POSIX) */
static inline bool avx_platform_sockets_init(void) {
#if defined(AVX_OS_WINDOWS)
    WSADATA wsa;
    return (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#elif defined(AVX_OS_POSIX)
#if defined(SIGPIPE)
    signal(SIGPIPE, SIG_IGN);
#endif
    return true;
#else
    return true;
#endif
}

/* Cleanup platform sockets (WSACleanup on Windows, no-op on POSIX) */
static inline void avx_platform_sockets_cleanup(void) {
#if defined(AVX_OS_WINDOWS)
    WSACleanup();
#endif
}

/* ========================================================================= */
/* 3. THREADING & SYNCHRONIZATION ABSTRACTION                                */
/* ========================================================================= */

#if !defined(AVX_OS_WINDOWS) || defined(__MINGW32__) || defined(__MINGW64__)

#include <pthread.h>

typedef pthread_t avx_thread_t;
typedef pthread_mutex_t avx_mutex_t;
typedef pthread_cond_t avx_cond_t;

#define AVX_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define AVX_COND_INITIALIZER PTHREAD_COND_INITIALIZER

#define avx_mutex_init(m) pthread_mutex_init((m), NULL)
#define avx_mutex_destroy(m) pthread_mutex_destroy(m)
#define avx_mutex_lock(m) pthread_mutex_lock(m)
#define avx_mutex_unlock(m) pthread_mutex_unlock(m)

#define avx_cond_init(c) pthread_cond_init((c), NULL)
#define avx_cond_destroy(c) pthread_cond_destroy(c)
#define avx_cond_wait(c, m) pthread_cond_wait((c), (m))
#define avx_cond_signal(c) pthread_cond_signal(c)
#define avx_cond_broadcast(c) pthread_cond_broadcast(c)

static inline int avx_cond_timedwait(avx_cond_t *c, avx_mutex_t *m, uint32_t timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_ms / 1000);
    ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(c, m, &ts);
    return (rc == 0) ? 0 : -1;
}

static inline int avx_thread_create(avx_thread_t *t, void *(*fn)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* Ensure at least 2 MB stack to prevent stack overflow on musl libc (Alpine) */
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    int rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}
#define avx_thread_join(t) pthread_join((t), NULL)

#else /* Windows Native Threads & Synchronization */

#include <windows.h>
#include <process.h>

typedef HANDLE avx_thread_t;
typedef SRWLOCK avx_mutex_t;
typedef CONDITION_VARIABLE avx_cond_t;

#define AVX_MUTEX_INITIALIZER SRWLOCK_INIT
#define AVX_COND_INITIALIZER CONDITION_VARIABLE_INIT

#define avx_mutex_init(m) InitializeSRWLock(m)
#define avx_mutex_destroy(m) ((void)(m))
#define avx_mutex_lock(m) AcquireSRWLockExclusive(m)
#define avx_mutex_unlock(m) ReleaseSRWLockExclusive(m)

#define avx_cond_init(c) InitializeConditionVariable(c)
#define avx_cond_destroy(c) ((void)(c))
#define avx_cond_wait(c, m) SleepConditionVariableSRW((c), (m), INFINITE, 0)
#define avx_cond_signal(c) WakeConditionVariable(c)
#define avx_cond_broadcast(c) WakeAllConditionVariable(c)

static inline int avx_cond_timedwait(avx_cond_t *c, avx_mutex_t *m, uint32_t timeout_ms) {
    return SleepConditionVariableSRW(c, m, (DWORD)timeout_ms, 0) ? 0 : -1;
}

typedef void *(*avx_thread_fn_t)(void *);
struct avx_win_thread_arg_s {
    avx_thread_fn_t fn;
    void *arg;
};

static inline unsigned __stdcall avx_win_thread_trampoline(void *p) {
    struct avx_win_thread_arg_s *a = (struct avx_win_thread_arg_s *)p;
    avx_thread_fn_t fn = a->fn;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return 0;
}

static inline int avx_thread_create(avx_thread_t *t, void *(*fn)(void *), void *arg) {
    struct avx_win_thread_arg_s *a = (struct avx_win_thread_arg_s *)malloc(sizeof(*a));
    if (!a) return -1;
    a->fn = fn;
    a->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, avx_win_thread_trampoline, a, 0, NULL);
    if (!h) {
        free(a);
        return -1;
    }
    *t = (HANDLE)h;
    return 0;
}

static inline int avx_thread_join(avx_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

#endif

/* ========================================================================= */
/* 4. DYNAMIC LIBRARY LOADING ABSTRACTION                                    */
/* ========================================================================= */

#if defined(AVX_OS_WINDOWS)
#define avx_dlopen(path) ((void *)LoadLibraryA(path))
#define avx_dlsym(handle, sym) ((void *)GetProcAddress((HMODULE)(handle), (sym)))
#define avx_dlclose(handle) FreeLibrary((HMODULE)(handle))
#else
#include <dlfcn.h>
#define avx_dlopen(path) dlopen(path, RTLD_LAZY | RTLD_GLOBAL)
#define avx_dlsym(handle, sym) dlsym(handle, sym)
#define avx_dlclose(handle) dlclose(handle)
#endif

/* ========================================================================= */
/* 5. HIGH-RESOLUTION TIMERS & SLEEP ABSTRACTION                             */
/* ========================================================================= */

#if defined(AVX_OS_WINDOWS)

static inline void avx_sleep_ms(uint32_t ms) {
    Sleep((DWORD)ms);
}

static inline void avx_sleep_ns(uint64_t ns) {
    avx_sleep_ms((uint32_t)((ns + 999999ULL) / 1000000ULL));
}

static inline int64_t avx_time_ms(void) {
    return (int64_t)GetTickCount64();
}

static inline int64_t avx_time_ns(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (int64_t)((count.QuadPart * 1000000000LL) / freq.QuadPart);
}

static inline int64_t avx_time_100ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)u.QuadPart;
}

#else /* POSIX (Linux, macOS, BSD) */

static inline void avx_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}

static inline void avx_sleep_ns(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    nanosleep(&ts, NULL);
}

static inline int64_t avx_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}

static inline int64_t avx_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000000000LL) + (int64_t)ts.tv_nsec;
}

static inline int64_t avx_time_100ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((int64_t)ts.tv_sec * 10000000LL) + (ts.tv_nsec / 100LL);
}

#endif

/* ========================================================================= */
/* 6. COMPATIBILITY, TYPES & PACKING HELPERS                                 */
/* ========================================================================= */

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;

#define AVX_PACKED_START __pragma(pack(push, 1))
#define AVX_PACKED_END __pragma(pack(pop))
#define AVX_PACKED
#else
#define AVX_PACKED_START
#define AVX_PACKED_END
#define AVX_PACKED __attribute__((packed))
#endif

/* Cache-line & DMA aligned memory allocation (64-byte alignment for ARM/AVX2/DMA) */
static inline void *avx_aligned_alloc(size_t alignment, size_t size) {
#if defined(AVX_OS_WINDOWS)
    return _aligned_malloc(size, alignment);
#elif defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 200112L) || defined(_XOPEN_SOURCE) && (_XOPEN_SOURCE >= 600) ||  \
    defined(__APPLE__) || defined(__FreeBSD__)
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
    return ptr;
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
    return aligned_alloc(alignment, size);
#else
    return malloc(size);
#endif
}

static inline void avx_aligned_free(void *ptr) {
#if defined(AVX_OS_WINDOWS)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

#if defined(AVX_OS_WINDOWS)
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef popen
#define popen _popen
#endif
#ifndef pclose
#define pclose _pclose
#endif
#ifndef usleep
#define usleep(us) avx_sleep_ms(((us) + 999) / 1000)
#endif
#ifndef sleep
#define sleep(s) avx_sleep_ms((s) * 1000)
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef access
#define access _access
#endif
static inline int avx_setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value) return -1;
    if (!overwrite && getenv(name) != NULL) return 0;
    return (_putenv_s(name, value) == 0) ? 0 : -1;
}
static inline int avx_unsetenv(const char *name) {
    if (!name) return -1;
    return (_putenv_s(name, "") == 0) ? 0 : -1;
}
#ifndef setenv
#define setenv(name, value, overwrite) avx_setenv((name), (value), (overwrite))
#endif
#ifndef unsetenv
#define unsetenv(name) avx_unsetenv((name))
#endif
#endif

/* Minimal getopt_long implementation for native MSVC */
#if defined(_MSC_VER)
struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};
#define no_argument 0
#define required_argument 1
#define optional_argument 2

static char *avx_optarg = NULL;
static int avx_optind = 1;
#ifndef optarg
#define optarg avx_optarg
#endif
#ifndef optind
#define optind avx_optind
#endif

static inline int getopt_long(int argc, char *const argv[], const char *optstring, const struct option *longopts,
                              int *longindex) {
    if (avx_optind >= argc || !argv[avx_optind]) return -1;
    char *arg = argv[avx_optind];
    if (arg[0] != '-' || arg[1] == '\0') return -1;
    if (arg[1] == '-' && arg[2] == '\0') {
        avx_optind++;
        return -1;
    }

    if (arg[1] == '-') {
        const char *opt_name = arg + 2;
        const char *eq = strchr(opt_name, '=');
        size_t name_len = eq ? (size_t)(eq - opt_name) : strlen(opt_name);

        for (int i = 0; longopts && longopts[i].name; ++i) {
            if (strncmp(longopts[i].name, opt_name, name_len) == 0 && longopts[i].name[name_len] == '\0') {
                if (longindex) *longindex = i;
                avx_optind++;
                if (longopts[i].has_arg == required_argument) {
                    if (eq) {
                        avx_optarg = (char *)(eq + 1);
                    } else if (avx_optind < argc) {
                        avx_optarg = argv[avx_optind++];
                    } else {
                        return '?';
                    }
                } else if (longopts[i].has_arg == optional_argument && eq) {
                    avx_optarg = (char *)(eq + 1);
                } else {
                    avx_optarg = NULL;
                }
                if (longopts[i].flag) {
                    *longopts[i].flag = longopts[i].val;
                    return 0;
                }
                return longopts[i].val;
            }
        }
        return '?';
    }

    char opt = arg[1];
    avx_optind++;
    const char *match = strchr(optstring, opt);
    if (!match) return '?';

    if (match[1] == ':') {
        if (match[2] == ':') {
            avx_optarg = (arg[2] != '\0') ? (arg + 2) : NULL;
        } else if (arg[2] != '\0') {
            avx_optarg = arg + 2;
        } else if (avx_optind < argc) {
            avx_optarg = argv[avx_optind++];
        } else {
            return '?';
        }
    } else {
        avx_optarg = NULL;
    }
    return (int)opt;
}
#else
#include <getopt.h>
#endif

/* Thread-local storage abstraction */
#if defined(_MSC_VER)
#define AVX_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define AVX_THREAD_LOCAL __thread
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define AVX_THREAD_LOCAL _Thread_local
#else
#define AVX_THREAD_LOCAL
#endif

#ifdef __cplusplus
}
#endif

#endif /* AVX_PLATFORM_H */
