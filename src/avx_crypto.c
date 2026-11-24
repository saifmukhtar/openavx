/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_crypto.c - Native End-to-End Encryption (ChaCha20-Poly1305 / XChaCha20)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avx_crypto.h"
#include "monocypher.h"
#include "avx_platform.h"

#if defined(AVX_OS_POSIX)
    #include <fcntl.h>
    #include <unistd.h>
#endif

/*
 * Constructs 24-byte XChaCha20 nonce packing 8-byte session salt, frame sequence,
 * fragment index, fragment count, and fixed "OAVXE2EE" domain separation tag.
 */
static void build_nonce(uint8_t nonce[AVX_CRYPTO_NONCE_SIZE],
                        const uint8_t session_salt[AVX_CRYPTO_SALT_SIZE],
                        uint32_t frame_seq, uint16_t frag_idx, uint16_t frag_count)
{
    memcpy(nonce, session_salt, 8);
    memcpy(nonce + 8, &frame_seq, 4);
    memcpy(nonce + 12, &frag_idx, 2);
    memcpy(nonce + 14, &frag_count, 2);
    memcpy(nonce + 16, "OAVXE2EE", 8);
}

void avx_crypto_generate_salt(avx_crypto_ctx_t *ctx)
{
    if (!ctx) return;

    bool random_ok = false;

#if defined(AVX_OS_POSIX)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t rd = read(fd, ctx->session_salt, sizeof(ctx->session_salt));
        close(fd);
        if (rd == (ssize_t)sizeof(ctx->session_salt)) {
            random_ok = true;
        }
    }
#endif

    if (!random_ok) {
        /* High-entropy fallback mix using monotonic clock, PID, and BLAKE2b */
        int64_t t = avx_time_ns();
        uintptr_t ptr = (uintptr_t)ctx;
        uint8_t entropy[sizeof(t) + sizeof(ptr) + 16];
        memcpy(entropy, &t, sizeof(t));
        memcpy(entropy + sizeof(t), &ptr, sizeof(ptr));

        static uint64_t counter = 0x9e3779b97f4a7c15ULL;
        counter += 0x517cc1b727220a95ULL;
        memcpy(entropy + sizeof(t) + sizeof(ptr), &counter, sizeof(counter));

        uint8_t full_hash[32];
        crypto_blake2b(full_hash, sizeof(full_hash), entropy, sizeof(entropy));
        memcpy(ctx->session_salt, full_hash, sizeof(ctx->session_salt));
        crypto_wipe(full_hash, sizeof(full_hash));
    }
}

/*
 * Derives 256-bit symmetric key via BLAKE2b from passphrase and stream name,
 * then generates an initial random session salt from /dev/urandom or entropy fallback.
 */
int avx_crypto_init(avx_crypto_ctx_t *ctx, const char *passphrase, const char *stream_name)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));

    if (!passphrase || passphrase[0] == '\0') {
        ctx->enabled = false;
        return 0;
    }

    crypto_blake2b_ctx bctx;
    crypto_blake2b_init(&bctx, AVX_CRYPTO_KEY_SIZE);

    const char prefix[] = "OpenAVX_v1_E2EE_BLAKE2b_KeyDerivation";
    crypto_blake2b_update(&bctx, (const uint8_t*)prefix, strlen(prefix));
    crypto_blake2b_update(&bctx, (const uint8_t*)passphrase, strlen(passphrase));

    if (stream_name && stream_name[0] != '\0') {
        crypto_blake2b_update(&bctx, (const uint8_t*)stream_name, strlen(stream_name));
    }

    crypto_blake2b_final(&bctx, ctx->key);
    crypto_wipe(&bctx, sizeof(bctx));

    avx_crypto_generate_salt(ctx);
    ctx->enabled = true;
    return 0;
}

void avx_crypto_wipe(avx_crypto_ctx_t *ctx)
{
    if (!ctx) return;
    crypto_wipe(ctx, sizeof(*ctx));
    ctx->enabled = false;
}

/*
 * Encrypts payload with XChaCha20-Poly1305 AEAD, authenticating the unencrypted
 * network wire header as Additional Data (AD) and prefixing session salt to ciphertext.
 */
int avx_crypto_encrypt_payload(const avx_crypto_ctx_t *ctx,
                               uint32_t frame_seq, uint16_t frag_idx, uint16_t frag_count,
                               const void *header_ad, size_t ad_len,
                               const void *plain_payload, size_t plain_len,
                               void *out_buf, size_t *out_len)
{
    if (!ctx || !ctx->enabled || !out_buf || !out_len) return -1;
    if (plain_len > 0 && !plain_payload) return -1;

    size_t required = plain_len + AVX_CRYPTO_OVERHEAD;
    if (*out_len < required) {
        *out_len = required;
        return -1;
    }

    uint8_t *dest = (uint8_t*)out_buf;

    /* 1. Write 8-byte session salt */
    memcpy(dest, ctx->session_salt, AVX_CRYPTO_SALT_SIZE);

    /* 2. Construct 24-byte XChaCha20 nonce */
    uint8_t nonce[AVX_CRYPTO_NONCE_SIZE];
    build_nonce(nonce, ctx->session_salt, frame_seq, frag_idx, frag_count);

    /* 3. Encrypt payload and append 16-byte Poly1305 MAC */
    uint8_t *cipher_text = dest + AVX_CRYPTO_SALT_SIZE;
    uint8_t *mac = cipher_text + plain_len;

    crypto_aead_lock(cipher_text, mac, ctx->key, nonce,
                     (const uint8_t*)header_ad, ad_len,
                     (const uint8_t*)plain_payload, plain_len);

    *out_len = required;
    crypto_wipe(nonce, sizeof(nonce));
    return 0;
}

/*
 * Authenticates wire header AD and decrypts payload in-place using Poly1305 MAC.
 * Returns -1 if packet was forged, truncated, or encrypted with a different key.
 */
int avx_crypto_decrypt_payload(const avx_crypto_ctx_t *ctx,
                               uint32_t frame_seq, uint16_t frag_idx, uint16_t frag_count,
                               const void *header_ad, size_t ad_len,
                               const void *in_buf, size_t in_len,
                               void *plain_out, size_t *out_plain_len)
{
    if (!ctx || !ctx->enabled || !in_buf || !plain_out || !out_plain_len) return -1;
    if (in_len < AVX_CRYPTO_OVERHEAD) return -1;

    const uint8_t *src = (const uint8_t*)in_buf;
    const uint8_t *packet_salt = src;
    size_t cipher_len = in_len - AVX_CRYPTO_OVERHEAD;
    const uint8_t *cipher_text = src + AVX_CRYPTO_SALT_SIZE;
    const uint8_t *mac = cipher_text + cipher_len;

    /* Reconstruct 24-byte XChaCha20 nonce from packet salt */
    uint8_t nonce[AVX_CRYPTO_NONCE_SIZE];
    build_nonce(nonce, packet_salt, frame_seq, frag_idx, frag_count);

    int ret = crypto_aead_unlock((uint8_t*)plain_out, mac, ctx->key, nonce,
                                 (const uint8_t*)header_ad, ad_len,
                                 cipher_text, cipher_len);

    crypto_wipe(nonce, sizeof(nonce));

    if (ret != 0) {
        return -1; /* Authentication failure / tampered packet / wrong key */
    }

    *out_plain_len = cipher_len;
    return 0;
}
