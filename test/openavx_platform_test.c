/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_platform_test.c - Cross-Platform Portability Layer Verification Suite
 */

#include "avx_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAILED] %s (line %d): %s\n", __FILE__, __LINE__, msg); \
        exit(1); \
    } \
} while(0)

/* Worker thread context for mutex & condvar test */
typedef struct {
    avx_mutex_t lock;
    avx_cond_t cond;
    bool ready;
    int counter;
} thread_test_ctx_t;

static void* thread_worker(void* arg) {
    thread_test_ctx_t* ctx = (thread_test_ctx_t*)arg;

    /* Sleep briefly to verify asynchronous scheduling */
    avx_sleep_ms(15);

    avx_mutex_lock(&ctx->lock);
    ctx->counter += 42;
    ctx->ready = true;
    avx_cond_signal(&ctx->cond);
    avx_mutex_unlock(&ctx->lock);

    return NULL;
}

int main(void) {
    printf("=== OpenAVX Platform Abstraction Tests ===\n\n");

    /* 1. Platform & Architecture Identification */
    printf("[*] 1. Verifying Platform & Architecture Detection...\n");
    printf("    [+] Target OS:           %s\n", AVX_PLATFORM_NAME);
    printf("    [+] Target Architecture: %s\n", AVX_ARCH_NAME);
    printf("    [+] Path Separator:      '%c'\n", AVX_PATH_SEP_CHAR);

#if defined(AVX_OS_LINUX)
    ASSERT_TRUE(strcmp(AVX_PLATFORM_NAME, "Linux") == 0, "AVX_OS_LINUX defined but name mismatch");
#elif defined(AVX_OS_MACOS)
    ASSERT_TRUE(strcmp(AVX_PLATFORM_NAME, "macOS") == 0, "AVX_OS_MACOS defined but name mismatch");
#elif defined(AVX_OS_WINDOWS)
    ASSERT_TRUE(strcmp(AVX_PLATFORM_NAME, "Windows") == 0, "AVX_OS_WINDOWS defined but name mismatch");
#elif defined(AVX_OS_BSD)
    ASSERT_TRUE(strcmp(AVX_PLATFORM_NAME, "BSD") == 0, "AVX_OS_BSD defined but name mismatch");
#endif

    /* 2. Sockets Subsystem */
    printf("[*] 2. Testing Cross-Platform Sockets Subsystem...\n");
    bool sock_init = avx_platform_sockets_init();
    ASSERT_TRUE(sock_init, "avx_platform_sockets_init failed");

    avx_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(srv != AVX_INVALID_SOCKET, "Server socket creation failed");

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    avx_socket_set_nosigpipe(srv);

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    srv_addr.sin_port = 0; /* ephemeral port */

    int b_res = bind(srv, (struct sockaddr*)&srv_addr, sizeof(srv_addr));
    ASSERT_TRUE(b_res == 0, "bind() failed");

    avx_socklen_t srv_len = sizeof(srv_addr);
    getsockname(srv, (struct sockaddr*)&srv_addr, &srv_len);
    uint16_t port = ntohs(srv_addr.sin_port);

    listen(srv, 1);
    printf("    [+] Server bound to 127.0.0.1:%u\n", port);

    /* Connect client socket */
    avx_socket_t cli = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(cli != AVX_INVALID_SOCKET, "Client socket creation failed");
    avx_socket_set_nosigpipe(cli);

    struct sockaddr_in cli_addr;
    memset(&cli_addr, 0, sizeof(cli_addr));
    cli_addr.sin_family = AF_INET;
    cli_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cli_addr.sin_port = htons(port);

    int c_res = connect(cli, (struct sockaddr*)&cli_addr, sizeof(cli_addr));
    ASSERT_TRUE(c_res == 0, "connect() failed");

    /* Accept connection */
    avx_socket_t conn = accept(srv, NULL, NULL);
    ASSERT_TRUE(conn != AVX_INVALID_SOCKET, "accept() failed");
    avx_socket_set_nosigpipe(conn);

    /* Test TCP Keepalive and TCP_NODELAY */
    int ka_res = avx_socket_set_keepalive(conn, 2, 1, 3);
    ASSERT_TRUE(ka_res == 0, "avx_socket_set_keepalive() failed");
    int nd_res = avx_socket_set_tcp_nodelay(conn, true);
    ASSERT_TRUE(nd_res == 0, "avx_socket_set_tcp_nodelay() failed");

    int opt_val = 0;
    avx_socklen_t opt_len = sizeof(opt_val);
    ASSERT_TRUE(getsockopt(conn, SOL_SOCKET, SO_KEEPALIVE, (char*)&opt_val, &opt_len) == 0 && opt_val != 0,
                "SO_KEEPALIVE not enabled on socket");

    opt_val = 0;
    opt_len = sizeof(opt_val);
    ASSERT_TRUE(getsockopt(conn, IPPROTO_TCP, TCP_NODELAY, (char*)&opt_val, &opt_len) == 0 && opt_val != 0,
                "TCP_NODELAY not enabled on socket");
    printf("    [+] Socket TCP Keepalive and TCP_NODELAY verified.\n");

    /* Test non-blocking mode */
    int nb_res = avx_socket_set_nonblocking(conn, true);
    ASSERT_TRUE(nb_res == 0, "avx_socket_set_nonblocking() failed");

    /* Test poll abstraction */
    avx_pollfd_t pfd;
    pfd.fd = conn;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = avx_poll(&pfd, 1, 10);
    ASSERT_TRUE(pr == 0, "avx_poll() should have timed out with 0 events");

    /* Send data with MSG_NOSIGNAL */
    const char msg[] = "OPENAVX_PLATFORM_PING";
    ssize_t sent = send(cli, msg, sizeof(msg), MSG_NOSIGNAL);
    ASSERT_TRUE(sent == (ssize_t)sizeof(msg), "send() with MSG_NOSIGNAL failed");

    /* Poll again: should now be readable */
    pr = avx_poll(&pfd, 1, 100);
    ASSERT_TRUE(pr > 0 && (pfd.revents & POLLIN), "avx_poll() should signal POLLIN");

    char buf[64] = {0};
    ssize_t recvd = recv(conn, buf, sizeof(buf), 0);
    ASSERT_TRUE(recvd == (ssize_t)sizeof(msg), "recv() failed");
    ASSERT_TRUE(strcmp(buf, msg) == 0, "Payload data mismatch");
    printf("    [+] Socket loopback exchange succeeded with MSG_NOSIGNAL!\n");

    avx_socket_close(conn);
    avx_socket_close(cli);
    avx_socket_close(srv);
    avx_platform_sockets_cleanup();

    /* 3. Threading & Synchronization */
    printf("[*] 3. Testing Threading & Synchronization Primitives...\n");
    thread_test_ctx_t tctx;
    memset(&tctx, 0, sizeof(tctx));
    avx_mutex_init(&tctx.lock);
    avx_cond_init(&tctx.cond);

    /* Test timedwait timeout */
    avx_mutex_lock(&tctx.lock);
    int tw_res = avx_cond_timedwait(&tctx.cond, &tctx.lock, 10);
    ASSERT_TRUE(tw_res != 0, "timedwait should have timed out");
    avx_mutex_unlock(&tctx.lock);

    /* Launch worker thread */
    avx_thread_t th;
    int cr_res = avx_thread_create(&th, thread_worker, &tctx);
    ASSERT_TRUE(cr_res == 0, "avx_thread_create() failed");

    avx_mutex_lock(&tctx.lock);
    while (!tctx.ready) {
        avx_cond_wait(&tctx.cond, &tctx.lock);
    }
    ASSERT_TRUE(tctx.counter == 42, "Worker thread computation mismatch");
    avx_mutex_unlock(&tctx.lock);

    avx_thread_join(th);
    avx_mutex_destroy(&tctx.lock);
    avx_cond_destroy(&tctx.cond);
    printf("    [+] Mutex, Condvar, and Thread Create/Join verified!\n");

    /* 4. High-Resolution Clocks & Timers */
    printf("[*] 4. Testing High-Resolution Clocks & Timers...\n");
    int64_t t0 = avx_time_ms();
    int64_t t0_100ns = avx_time_100ns();
    ASSERT_TRUE(t0 > 0, "avx_time_ms returned <= 0");
    ASSERT_TRUE(t0_100ns > 0, "avx_time_100ns returned <= 0");

    avx_sleep_ms(30);

    int64_t t1 = avx_time_ms();
    int64_t elapsed = t1 - t0;
    ASSERT_TRUE(elapsed >= 25 && elapsed <= 500, "avx_sleep_ms(30) elapsed outside tolerance");
    printf("    [+] avx_sleep_ms(30) measured %lld ms (within tolerance)\n", (long long)elapsed);

    /* 5. Dynamic Library Loading */
    printf("[*] 5. Testing Dynamic Library Loading...\n");
#if defined(AVX_OS_WINDOWS)
    void* handle = avx_dlopen("kernel32.dll");
    ASSERT_TRUE(handle != NULL, "Failed to open kernel32.dll");
    void* sym = avx_dlsym(handle, "GetTickCount64");
    ASSERT_TRUE(sym != NULL, "Failed to resolve GetTickCount64");
    avx_dlclose(handle);
#elif defined(AVX_OS_MACOS)
    void* handle = avx_dlopen("libSystem.B.dylib");
    if (!handle) handle = avx_dlopen("/usr/lib/libSystem.B.dylib");
    ASSERT_TRUE(handle != NULL, "Failed to open libSystem.B.dylib");
    void* sym = avx_dlsym(handle, "sin");
    ASSERT_TRUE(sym != NULL, "Failed to resolve sin");
    avx_dlclose(handle);
#else
    void* handle = avx_dlopen("libm.so.6");
    if (!handle) handle = avx_dlopen("libm.so");
    ASSERT_TRUE(handle != NULL, "Failed to open libm.so");
    void* sym = avx_dlsym(handle, "sin");
    ASSERT_TRUE(sym != NULL, "Failed to resolve sin");
    double (*my_sin)(double) = (double (*)(double))sym;
    ASSERT_TRUE(fabs(my_sin(0.0)) < 0.0001, "Resolved sin(0) != 0");
    avx_dlclose(handle);
#endif
    printf("    [+] Dynamic library loading and symbol resolution verified!\n");

    /* 6. Cache-line / DMA-safe 64-Byte Aligned Allocation */
    printf("[*] 6. Testing 64-Byte Cache-Line Aligned Memory Allocation...\n");
    void* aligned_ptr = avx_aligned_alloc(64, 4096);
    ASSERT_TRUE(aligned_ptr != NULL, "avx_aligned_alloc(64, 4096) returned NULL");
    ASSERT_TRUE(((uintptr_t)aligned_ptr % 64) == 0, "Allocated pointer is not 64-byte aligned");
    memset(aligned_ptr, 0x5A, 4096);
    ASSERT_TRUE(((uint8_t*)aligned_ptr)[0] == 0x5A && ((uint8_t*)aligned_ptr)[4095] == 0x5A, "Aligned memory read/write check failed");
    avx_aligned_free(aligned_ptr);
    printf("    [+] 64-byte aligned allocation and deallocation verified!\n");

    printf("\n[PASS] openavx_platform_test\n");

    return 0;
}
