/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/compat/win32/pthread.h - Win32 emulation of POSIX threads using avx_platform.h
 */

#ifndef OPENAVX_COMPAT_WIN32_PTHREAD_H
#define OPENAVX_COMPAT_WIN32_PTHREAD_H

#if defined(__MINGW32__) || defined(__MINGW64__)
#include_next <pthread.h>
#elif defined(_MSC_VER) || defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <stdlib.h>

typedef HANDLE pthread_t;
typedef void* pthread_attr_t;

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} avx_pthread_wrapper_t;

static inline unsigned __stdcall avx_pthread_trampoline(void *p) {
    avx_pthread_wrapper_t *w = (avx_pthread_wrapper_t*)p;
    void *(*fn)(void *) = w->start_routine;
    void *arg = w->arg;
    free(w);
    fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
    (void)attr;
    avx_pthread_wrapper_t *w = (avx_pthread_wrapper_t*)malloc(sizeof(avx_pthread_wrapper_t));
    if (!w) return -1;
    w->start_routine = start_routine;
    w->arg = arg;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, avx_pthread_trampoline, w, 0, NULL);
    if (!h) {
        free(w);
        return -1;
    }
    if (thread) *thread = h;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval) {
    (void)retval;
    if (!thread) return 0;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

static inline int pthread_detach(pthread_t thread) {
    if (thread) {
        CloseHandle(thread);
    }
    return 0;
}

typedef SRWLOCK pthread_mutex_t;
typedef void* pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    InitializeSRWLock(mutex);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    (void)mutex;
    return 0;
}

#endif /* __MINGW32__ vs _MSC_VER */

#endif /* OPENAVX_COMPAT_WIN32_PTHREAD_H */
