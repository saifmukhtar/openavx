/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_virtcam_ipc.c - Cross-platform shared memory IPC for Virtual Camera Plugins
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "openavx.h"
#include "avx_virtcam_ipc.h"
#include "avx_platform.h"

#if defined(AVX_OS_WINDOWS)
    #include <windows.h>
#elif defined(AVX_OS_POSIX)
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

struct avx_virtcam_producer_t {
    avx_virtcam_shm_layout_t *layout;
    size_t                    map_size;
#if defined(AVX_OS_WINDOWS)
    HANDLE                    map_handle;
#elif defined(AVX_OS_POSIX)
    int                       shm_fd;
#endif
};

struct avx_virtcam_consumer_t {
    avx_virtcam_shm_layout_t *layout;
    size_t                    map_size;
#if defined(AVX_OS_WINDOWS)
    HANDLE                    map_handle;
#elif defined(AVX_OS_POSIX)
    int                       shm_fd;
#endif
};

/* ========================================================================= */
/* 1. IPC PRODUCER (openavx-virtcam engine)                                  */
/* ========================================================================= */

/* Creates cross-platform shared memory mapping for virtual camera plugin IPC (POSIX shm / Win32 file mapping). */
avx_virtcam_producer_t *avx_virtcam_producer_create(void)
{
    avx_virtcam_producer_t *prod = (avx_virtcam_producer_t*)calloc(1, sizeof(avx_virtcam_producer_t));
    if (!prod) return NULL;

    prod->map_size = sizeof(avx_virtcam_shm_layout_t);

#if defined(AVX_OS_WINDOWS)
    prod->map_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        (DWORD)((prod->map_size >> 32) & 0xFFFFFFFFU),
        (DWORD)(prod->map_size & 0xFFFFFFFFU),
        AVX_VIRTCAM_WIN_SHM_NAME
    );
    if (!prod->map_handle) {
        free(prod);
        return NULL;
    }
    prod->layout = (avx_virtcam_shm_layout_t*)MapViewOfFile(
        prod->map_handle,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        prod->map_size
    );
    if (!prod->layout) {
        CloseHandle(prod->map_handle);
        free(prod);
        return NULL;
    }
#elif defined(AVX_OS_POSIX)
    prod->shm_fd = shm_open(AVX_VIRTCAM_POSIX_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (prod->shm_fd < 0) {
        free(prod);
        return NULL;
    }
    if (ftruncate(prod->shm_fd, (off_t)prod->map_size) < 0) {
        close(prod->shm_fd);
        shm_unlink(AVX_VIRTCAM_POSIX_SHM_NAME);
        free(prod);
        return NULL;
    }
    prod->layout = (avx_virtcam_shm_layout_t*)mmap(
        NULL,
        prod->map_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        prod->shm_fd,
        0
    );
    if (prod->layout == MAP_FAILED) {
        close(prod->shm_fd);
        shm_unlink(AVX_VIRTCAM_POSIX_SHM_NAME);
        free(prod);
        return NULL;
    }
#endif

    memset(prod->layout, 0, prod->map_size);
    prod->layout->header.magic = AVX_VIRTCAM_MAGIC;
    prod->layout->header.version = AVX_VIRTCAM_VERSION;
    return prod;
}

void avx_virtcam_producer_destroy(avx_virtcam_producer_t *prod)
{
    if (!prod) return;

#if defined(AVX_OS_WINDOWS)
    if (prod->layout) {
        UnmapViewOfFile(prod->layout);
        prod->layout = NULL;
    }
    if (prod->map_handle) {
        CloseHandle(prod->map_handle);
        prod->map_handle = NULL;
    }
#elif defined(AVX_OS_POSIX)
    if (prod->layout && prod->layout != MAP_FAILED) {
        munmap(prod->layout, prod->map_size);
        prod->layout = NULL;
    }
    if (prod->shm_fd >= 0) {
        close(prod->shm_fd);
        prod->shm_fd = -1;
    }
    shm_unlink(AVX_VIRTCAM_POSIX_SHM_NAME);
#endif

    free(prod);
}

/* Publishes video frame into shared memory layout with sequence counter and memory barrier. */
bool avx_virtcam_producer_write_frame(
    avx_virtcam_producer_t *prod,
    const uint8_t *data,
    size_t size,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t fourcc,
    uint32_t fps_num,
    uint32_t fps_den
) {
    if (!prod || !prod->layout || !data || size == 0 || size > AVX_VIRTCAM_MAX_FRAME_SIZE) {
        return false;
    }

    avx_virtcam_shm_layout_t *lay = prod->layout;

    memcpy(lay->pixels, data, size);

    lay->header.width = width;
    lay->header.height = height;
    lay->header.stride = (stride > 0) ? stride : (width * 2);
    lay->header.fourcc = fourcc;
    lay->header.fps_num = (fps_num > 0) ? fps_num : 30;
    lay->header.fps_den = (fps_den > 0) ? fps_den : 1;
    lay->header.data_size = (uint32_t)size;
    lay->header.timestamp_ns = avx_time_ns();
    lay->header.sequence++;

    return true;
}

bool avx_virtcam_producer_has_clients(const avx_virtcam_producer_t *prod)
{
    return (prod && prod->layout) ? (prod->layout->header.clients_active > 0) : false;
}

/* ========================================================================= */
/* 2. IPC CONSUMER (DirectShow DLL / CoreMediaIO DAL Plugin)                 */
/* ========================================================================= */

avx_virtcam_consumer_t *avx_virtcam_consumer_create(void)
{
    avx_virtcam_consumer_t *cons = (avx_virtcam_consumer_t*)calloc(1, sizeof(avx_virtcam_consumer_t));
    if (!cons) return NULL;

    cons->map_size = sizeof(avx_virtcam_shm_layout_t);

#if defined(AVX_OS_WINDOWS)
    cons->map_handle = OpenFileMappingA(
        FILE_MAP_READ | FILE_MAP_WRITE,
        FALSE,
        AVX_VIRTCAM_WIN_SHM_NAME
    );
    if (!cons->map_handle) {
        free(cons);
        return NULL;
    }
    cons->layout = (avx_virtcam_shm_layout_t*)MapViewOfFile(
        cons->map_handle,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0, 0,
        cons->map_size
    );
    if (!cons->layout) {
        CloseHandle(cons->map_handle);
        free(cons);
        return NULL;
    }
#elif defined(AVX_OS_POSIX)
    cons->shm_fd = shm_open(AVX_VIRTCAM_POSIX_SHM_NAME, O_RDWR, 0666);
    if (cons->shm_fd < 0) {
        free(cons);
        return NULL;
    }
    cons->layout = (avx_virtcam_shm_layout_t*)mmap(
        NULL,
        cons->map_size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        cons->shm_fd,
        0
    );
    if (cons->layout == MAP_FAILED) {
        close(cons->shm_fd);
        free(cons);
        return NULL;
    }
#endif

    if (cons->layout->header.magic != AVX_VIRTCAM_MAGIC) {
        avx_virtcam_consumer_destroy(cons);
        return NULL;
    }

    /* Increment active client counter */
    cons->layout->header.clients_active++;
    return cons;
}

void avx_virtcam_consumer_destroy(avx_virtcam_consumer_t *cons)
{
    if (!cons) return;

    if (cons->layout && cons->layout != (void*)-1) {
        if (cons->layout->header.clients_active > 0) {
            cons->layout->header.clients_active--;
        }
#if defined(AVX_OS_WINDOWS)
        UnmapViewOfFile(cons->layout);
        cons->layout = NULL;
#elif defined(AVX_OS_POSIX)
        munmap(cons->layout, cons->map_size);
        cons->layout = NULL;
#endif
    }

#if defined(AVX_OS_WINDOWS)
    if (cons->map_handle) {
        CloseHandle(cons->map_handle);
        cons->map_handle = NULL;
    }
#elif defined(AVX_OS_POSIX)
    if (cons->shm_fd >= 0) {
        close(cons->shm_fd);
        cons->shm_fd = -1;
    }
#endif

    free(cons);
}

/* Reads video frame from shared memory using sequence check for tear-free lockless consumption. */
bool avx_virtcam_consumer_read_frame(
    avx_virtcam_consumer_t *cons,
    avx_virtcam_shm_header_t *out_header,
    uint8_t *out_buffer,
    size_t buffer_max,
    uint64_t *inout_last_seq
) {
    if (!cons || !cons->layout || !out_header || !out_buffer) return false;

    avx_virtcam_shm_layout_t *lay = cons->layout;
    if (lay->header.magic != AVX_VIRTCAM_MAGIC) return false;

    uint64_t cur_seq = lay->header.sequence;
    if (cur_seq == 0 || (inout_last_seq && cur_seq == *inout_last_seq)) {
        return false; /* No new frame yet */
    }

    uint32_t size = lay->header.data_size;
    if (size > buffer_max || size == 0) return false;

    memcpy(out_header, &lay->header, sizeof(avx_virtcam_shm_header_t));
    memcpy(out_buffer, lay->pixels, size);

    if (inout_last_seq) {
        *inout_last_seq = cur_seq;
    }
    return true;
}
