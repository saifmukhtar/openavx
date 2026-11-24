/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_log.c - Unified thread-safe library & CLI logger
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "openavx.h"
#include "avx_platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(AVX_OS_POSIX)
    #include <unistd.h>
#endif

static bool ascii_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static avx_mutex_t          g_log_lock = AVX_MUTEX_INITIALIZER;
static oavx_log_level_e     g_log_level = OAVX_LOG_WARN;
static bool                 g_log_env_checked = false;
static oavx_log_callback_t  g_log_cb = NULL;
static void                *g_log_user_ctx = NULL;

/* Inspects OPENAVX_LOG_LEVEL environment variable on first invocation with case-insensitive parsing. */
static void apply_env_log_level_locked(void) {
    if (g_log_env_checked) return;
    g_log_env_checked = true;

    const char *env = getenv("OPENAVX_LOG_LEVEL");
    if (!env || env[0] == '\0') return;

    if (ascii_eq_nocase(env, "quiet") || strcmp(env, "0") == 0) {
        g_log_level = OAVX_LOG_QUIET;
    } else if (ascii_eq_nocase(env, "error") || strcmp(env, "1") == 0) {
        g_log_level = OAVX_LOG_ERROR;
    } else if (ascii_eq_nocase(env, "warn") || ascii_eq_nocase(env, "warning") || strcmp(env, "2") == 0) {
        g_log_level = OAVX_LOG_WARN;
    } else if (ascii_eq_nocase(env, "info") || strcmp(env, "3") == 0) {
        g_log_level = OAVX_LOG_INFO;
    } else if (ascii_eq_nocase(env, "debug") || ascii_eq_nocase(env, "trace") || strcmp(env, "4") == 0) {
        g_log_level = OAVX_LOG_DEBUG;
    }
}

static const char* level_label(oavx_log_level_e level) {
    switch (level) {
        case OAVX_LOG_ERROR: return "error";
        case OAVX_LOG_WARN:  return "warn";
        case OAVX_LOG_INFO:  return "info";
        case OAVX_LOG_DEBUG: return "debug";
        default:             return "log";
    }
}

static const char* level_ansi_color(oavx_log_level_e level) {
    switch (level) {
        case OAVX_LOG_ERROR: return "\033[1;31m";
        case OAVX_LOG_WARN:  return "\033[1;33m";
        case OAVX_LOG_INFO:  return "\033[36m";
        case OAVX_LOG_DEBUG: return "\033[90m";
        default:             return "";
    }
}

OPENAVX_API void oavx_set_log_level(oavx_log_level_e level) {
    avx_mutex_lock(&g_log_lock);
    g_log_env_checked = true;
    g_log_level = level;
    avx_mutex_unlock(&g_log_lock);
}

OPENAVX_API oavx_log_level_e oavx_get_log_level(void) {
    avx_mutex_lock(&g_log_lock);
    apply_env_log_level_locked();
    oavx_log_level_e cur = g_log_level;
    avx_mutex_unlock(&g_log_lock);
    return cur;
}

OPENAVX_API void oavx_set_log_callback(oavx_log_callback_t cb, void *user_ctx) {
    avx_mutex_lock(&g_log_lock);
    g_log_cb = cb;
    g_log_user_ctx = user_ctx;
    avx_mutex_unlock(&g_log_lock);
}

/* Thread-safe formatted logging with TTY ANSI colorization and custom user callback dispatch. */
OPENAVX_API void oavx_log_write(oavx_log_level_e level, const char *module, const char *fmt, ...) {
    if (!fmt || level <= OAVX_LOG_QUIET) return;

    avx_mutex_lock(&g_log_lock);
    apply_env_log_level_locked();
    oavx_log_level_e cur_level = g_log_level;
    oavx_log_callback_t cb = g_log_cb;
    void *user_ctx = g_log_user_ctx;
    avx_mutex_unlock(&g_log_lock);

    if (level > cur_level) return;

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Strip trailing newline if caller included one so output stays single-line per record */
    size_t mlen = strlen(msg);
    while (mlen > 0 && (msg[mlen - 1] == '\n' || msg[mlen - 1] == '\r')) {
        msg[--mlen] = '\0';
    }

    if (cb) {
        cb(level, (module && module[0] != '\0') ? module : "core", msg, user_ctx);
        return;
    }

    const char *lbl = level_label(level);
    bool use_color = false;
#if defined(AVX_OS_POSIX)
    use_color = (isatty(fileno(stderr)) != 0);
#endif

    avx_mutex_lock(&g_log_lock);
    if (module && module[0] != '\0') {
        if (use_color) {
            fprintf(stderr, "%s[openavx:%s] [%s]\033[0m %s\n",
                    level_ansi_color(level), module, lbl, msg);
        } else {
            fprintf(stderr, "[openavx:%s] [%s] %s\n", module, lbl, msg);
        }
    } else {
        if (use_color) {
            fprintf(stderr, "%s[openavx] [%s]\033[0m %s\n",
                    level_ansi_color(level), lbl, msg);
        } else {
            fprintf(stderr, "[openavx] [%s] %s\n", lbl, msg);
        }
    }
    fflush(stderr);
    avx_mutex_unlock(&g_log_lock);
}
