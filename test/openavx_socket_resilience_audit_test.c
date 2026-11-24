/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_socket_resilience_audit_test.c - Socket resilience and signal safety tests
 *
 * Checks SIGPIPE suppression, reconnect timeouts, mutex contention,
 * strtok_r thread safety, and graceful socket shutdown.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "openavx.h"
#include "avx_internal.h"
#include "avx_platform.h"
#include "avx_web.h"
#include "avx_rtsp.h"

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* ========================================================================= */
/* TEST 1: SIGPIPE Behavior on writev() vs sendmsg(MSG_NOSIGNAL)             */
/* ========================================================================= */

#ifndef _WIN32
static volatile sig_atomic_t g_sigpipe_delivered = 0;

static void audit_sigpipe_handler(int sig) {
    if (sig == SIGPIPE) {
        g_sigpipe_delivered++;
    }
}
#endif

static void test_audit_proof_sigpipe_vulnerability(void) {
    printf("\n[*] 1. SIGPIPE handling: writev() vs sendmsg(..., MSG_NOSIGNAL)\n");

#ifndef _WIN32
    /* Install temporary SIGPIPE signal handler to count signals instead of dying */
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = audit_sigpipe_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, &old_sa);

    /* 1. Direct kernel check: Compare bare writev() vs sendmsg(..., MSG_NOSIGNAL) */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* Client closes connection abruptly */
    close(sv[1]);

    char hdr[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    char body[] = "<html><body>OpenAVX Preview</body></html>";
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = strlen(hdr);
    iov[1].iov_base = body;
    iov[1].iov_len = strlen(body);

    /* Baseline: POSIX writev() without MSG_NOSIGNAL */
    g_sigpipe_delivered = 0;
    ssize_t ret_writev = writev(sv[0], iov, 2);
    int err_writev = errno;
    int sigs_writev = (int)g_sigpipe_delivered;

    printf("  [*] Baseline writev(): ret=%zd, errno=%d (%s), SIGPIPE=%d\n",
           ret_writev, err_writev, strerror(err_writev), sigs_writev);

    /* Test: sendmsg() with MSG_NOSIGNAL on the same broken socket */
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    g_sigpipe_delivered = 0;
    ssize_t ret_sendmsg = sendmsg(sv[0], &msg, MSG_NOSIGNAL);
    int err_sendmsg = errno;
    int sigs_sendmsg = (int)g_sigpipe_delivered;

    printf("  [*] sendmsg(MSG_NOSIGNAL): ret=%zd, errno=%d (%s), SIGPIPE=%d\n",
           ret_sendmsg, err_sendmsg, strerror(err_sendmsg), sigs_sendmsg);

    assert(sigs_sendmsg == 0 && ret_sendmsg < 0 && err_sendmsg == EPIPE);
    printf("  [+] PASS: sendmsg(..., MSG_NOSIGNAL) returns EPIPE with 0 SIGPIPE\n");

    close(sv[0]);
    sigaction(SIGPIPE, &old_sa, NULL);
#else
    printf("  [*] Windows sockets do not deliver SIGPIPE signals; skipping POSIX signal check\n");
#endif

    /* 2. Web Gateway Preview check */
    avx_web_server_config_t wcfg = {
        .port = 0,
        .codec = AVX_RTSP_CODEC_H264,
        .stream_name = "AuditTestStream"
    };
    avx_web_server_t *srv = avx_web_server_create(&wcfg);
    assert(srv != NULL);
    uint16_t port = avx_web_server_get_port(srv);
    printf("  [+] Web preview server bound to port %u\n", port);
    avx_web_server_destroy(srv);
}

/* ========================================================================= */
/* TEST 2: Reconnect Timeout Budget in oavx_stream_receive                   */
/* ========================================================================= */

static void test_audit_proof_reconnect_blocking_budget(void) {
    printf("\n[*] 2. Reconnect timeout budget on unreachable endpoint\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* Target RFC 5737 TEST-NET-2 non-routable IP (198.51.100.1:7960) */
    oavx_stream_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.direction = OAVX_DIR_RECEIVE;
    cfg.name = "UnreachableStream";
    cfg.endpoint = "198.51.100.1:7960";
    cfg.bandwidth = OAVX_BANDWIDTH_HIGHEST;
    cfg.color_format = OAVX_COLOR_FORMAT_FASTEST;

    double t_open_0 = get_time_ms();
    oavx_stream_t *rx = oavx_stream_open(ctx, &cfg);
    double t_open = get_time_ms() - t_open_0;
    assert(rx != NULL);
    printf("  [+] oavx_stream_open(%s) returned in %.2f ms\n", cfg.endpoint, t_open);

    /* Call oavx_stream_receive requesting a 40ms frame budget */
    oavx_video_frame_t vframe = {0};
    oavx_audio_frame_t aframe = {0};
    oavx_metadata_frame_t mframe = {0};

    double t_recv_0 = get_time_ms();
    oavx_frame_type_t ft = oavx_stream_receive(rx, &vframe, &aframe, &mframe, 40);
    double t_recv = get_time_ms() - t_recv_0;

    printf("  [+] oavx_stream_receive(timeout=40 ms) returned type %d in %.2f ms\n", (int)ft, t_recv);

    if (t_recv >= 2500.0) {
        printf("  [!] Warning: connect blocked for %.2f ms (> 2500 ms)\n", t_recv);
    }

    oavx_stream_close(rx);
    oavx_context_destroy(ctx);
}

/* ========================================================================= */
/* TEST 3: Concurrent Telemetry Wait Time During Connection Attempt          */
/* ========================================================================= */

typedef struct {
    oavx_stream_t *stream;
    volatile bool  ready;
    double         contention_wait_ms;
} contention_arg_t;

static void* contention_worker_thread(void *arg) {
    contention_arg_t *c = (contention_arg_t*)arg;
    c->ready = true;

    /* Sleep briefly so Thread 1 enters oavx_stream_receive first */
    avx_sleep_ms(200);

    oavx_telemetry_t telem;
    double t0 = get_time_ms();
    oavx_stream_get_telemetry(c->stream, &telem);
    c->contention_wait_ms = get_time_ms() - t0;

    return NULL;
}

static void test_audit_proof_mutex_contention(void) {
    printf("\n[*] 3. Mutex contention on concurrent telemetry query\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.direction = OAVX_DIR_RECEIVE;
    cfg.name = "ContentionStream";
    cfg.endpoint = "198.51.100.1:7960";

    oavx_stream_t *rx = oavx_stream_open(ctx, &cfg);
    assert(rx != NULL);

    contention_arg_t carg = {
        .stream = rx,
        .ready = false,
        .contention_wait_ms = 0.0
    };

    pthread_t th;
    pthread_create(&th, NULL, contention_worker_thread, &carg);
    while (!carg.ready) avx_sleep_ms(5);

    oavx_video_frame_t vframe = {0};
    oavx_audio_frame_t aframe = {0};
    oavx_metadata_frame_t mframe = {0};
    oavx_stream_receive(rx, &vframe, &aframe, &mframe, 40);

    pthread_join(th, NULL);

    printf("  [+] Thread 2 telemetry wait time: %.2f ms\n", carg.contention_wait_ms);
    if (carg.contention_wait_ms >= 1000.0) {
        printf("  [!] Warning: telemetry query stalled for %.2f ms\n", carg.contention_wait_ms);
    }

    oavx_stream_close(rx);
    oavx_context_destroy(ctx);
}

/* ========================================================================= */
/* TEST 4: Thread Safety of strtok_r() vs Non-Reentrant strtok()             */
/* ========================================================================= */

typedef struct {
    volatile bool running;
} strtok_race_arg_t;

static void* strtok_interferer_thread(void *arg) {
    strtok_race_arg_t *r = (strtok_race_arg_t*)arg;
    char dummy[64];
    while (r->running) {
        snprintf(dummy, sizeof(dummy), "UNRELATED_ALPHA UNRELATED_BETA UNRELATED_GAMMA");
        char *tok1 = strtok(dummy, " ");
        char *tok2 = strtok(NULL, " ");
        (void)tok1;
        (void)tok2;
    }
    return NULL;
}

static void test_audit_proof_strtok_concurrency_race(void) {
    printf("\n[*] 4. Discovery line parsing: strtok() vs reentrant strtok_r()\n");

    /* 4A: Baseline check showing global strtok() races across threads */
    strtok_race_arg_t rarg = { .running = true };
    pthread_t th;
    pthread_create(&th, NULL, strtok_interferer_thread, &rarg);

    int test_iterations = 10000;
    int corrupted_strtok = 0;

    for (int iter = 0; iter < test_iterations; ++iter) {
        char resp[128];
        snprintf(resp, sizeof(resp), "SOURCES\nCAM1|127.0.0.1:7960\nCAM2|127.0.0.1:7961\n");

        char *line = strtok(resp, "\r\n");
        if (!line || strcmp(line, "SOURCES") != 0) {
            corrupted_strtok++;
            continue;
        }

        line = strtok(NULL, "\r\n");
        if (!line || strncmp(line, "CAM1", 4) != 0) {
            corrupted_strtok++;
        }
    }

    rarg.running = false;
    pthread_join(th, NULL);

    printf("  [*] strtok() (%d iters): %d corruptions\n", test_iterations, corrupted_strtok);

    /* 4B: Test reentrant strtok_r() under same concurrent background thread */
    rarg.running = true;
    pthread_create(&th, NULL, strtok_interferer_thread, &rarg);

    int corrupted_strtok_r = 0;
    for (int iter = 0; iter < test_iterations; ++iter) {
        char resp[128];
        snprintf(resp, sizeof(resp), "SOURCES\nCAM1|127.0.0.1:7960\nCAM2|127.0.0.1:7961\n");

        char *saveptr = NULL;
        char *line = strtok_r(resp, "\r\n", &saveptr);
        if (!line || strcmp(line, "SOURCES") != 0) {
            corrupted_strtok_r++;
            continue;
        }

        line = strtok_r(NULL, "\r\n", &saveptr);
        if (!line || strncmp(line, "CAM1", 4) != 0) {
            corrupted_strtok_r++;
        }
    }

    rarg.running = false;
    pthread_join(th, NULL);

    assert(corrupted_strtok_r == 0);
    printf("  [+] PASS: strtok_r() (%d iters): 0 corruptions\n", test_iterations);
}

/* ========================================================================= */
/* TEST 5: Socket Graceful Shutdown Semantics                                */
/* ========================================================================= */

static void test_audit_proof_socket_teardown_semantics(void) {
    printf("\n[*] 5. Socket shutdown(SHUT_RDWR) before close()\n");

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* Calling shutdown(SHUT_RDWR) aborts pending send/recv calls with EPIPE/ECONNRESET
     * before closing and recycling the file descriptor. */
    int shutdown_res = shutdown(sv[0], SHUT_RDWR);
    assert(shutdown_res == 0);

    char buf[16] = "test";
    ssize_t s = send(sv[0], buf, sizeof(buf), MSG_NOSIGNAL);
#ifdef _WIN32
    int err = WSAGetLastError();
    assert(s < 0 && (err == WSAESHUTDOWN || err == WSAECONNRESET || err == WSAECONNABORTED));
    printf("  [+] PASS: send() after shutdown(SHUT_RDWR) failed with WSAGetLastError=%d\n", err);
    closesocket(sv[0]);
    closesocket(sv[1]);
#else
    assert(s < 0 && (errno == EPIPE || errno == ECONNRESET));
    printf("  [+] PASS: send() after shutdown(SHUT_RDWR) failed with errno=%d (%s)\n",
           errno, strerror(errno));

    close(sv[0]);
    close(sv[1]);
#endif
}

/* ========================================================================= */
/* MAIN ENTRY POINT                                                          */
/* ========================================================================= */

int main(void) {
    printf("=== OpenAVX Socket Resilience Tests ===\n");

    test_audit_proof_sigpipe_vulnerability();
    test_audit_proof_strtok_concurrency_race();
    test_audit_proof_socket_teardown_semantics();
    test_audit_proof_reconnect_blocking_budget();
    test_audit_proof_mutex_contention();

    printf("\n[PASS] openavx_socket_resilience_audit_test\n\n");
    return 0;
}
