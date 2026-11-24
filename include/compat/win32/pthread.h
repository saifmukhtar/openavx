/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/pthread.h - Win32 emulation of POSIX threads using avx_platform.h
 */

#ifndef OPENAVX_COMPAT_WIN32_PTHREAD_H
#define OPENAVX_COMPAT_WIN32_PTHREAD_H

#ifdef _WIN32
#include "avx_platform.h"

typedef avx_thread_t pthread_t;
typedef void* pthread_attr_t;

static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
    (void)attr;
    return avx_thread_create(thread, start_routine, arg);
}

static inline int pthread_join(pthread_t thread, void **retval) {
    (void)retval;
    return avx_thread_join(thread);
}

static inline int pthread_detach(pthread_t thread) {
    if (thread) {
        CloseHandle((HANDLE)thread);
    }
    return 0;
}

typedef avx_mutex_t pthread_mutex_t;
typedef void* pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER {0}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    avx_mutex_init(mutex);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    avx_mutex_lock(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    avx_mutex_unlock(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    avx_mutex_destroy(mutex);
    return 0;
}

#endif /* _WIN32 */

#endif /* OPENAVX_COMPAT_WIN32_PTHREAD_H */
