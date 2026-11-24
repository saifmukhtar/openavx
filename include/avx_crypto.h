/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_crypto.h - Native End-to-End Encryption (ChaCha20-Poly1305 / XChaCha20)
 */

#ifndef AVX_CRYPTO_H
#define AVX_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "avx_platform.h"
#include "openavx.h"

#define AVX_CRYPTO_KEY_SIZE    32  /* 256-bit symmetric master key */
#define AVX_CRYPTO_MAC_SIZE    16  /* 128-bit Poly1305 authentication tag */
#define AVX_CRYPTO_NONCE_SIZE  24  /* 192-bit XChaCha20 nonce */
#define AVX_CRYPTO_SALT_SIZE   8   /* 64-bit session salt */
#define AVX_CRYPTO_OVERHEAD    (AVX_CRYPTO_SALT_SIZE + AVX_CRYPTO_MAC_SIZE) /* 24 bytes */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t key[AVX_CRYPTO_KEY_SIZE];
    uint8_t session_salt[AVX_CRYPTO_SALT_SIZE];
    bool    enabled;
} avx_crypto_ctx_t;

/**
 * Initializes a crypto context from a user-supplied passphrase and stream name.
 * Uses BLAKE2b to derive a cryptographically strong 256-bit master key.
 * If passphrase is NULL or empty string, context is disabled (enabled = false).
 */
OPENAVX_API int avx_crypto_init(avx_crypto_ctx_t *ctx, const char *passphrase, const char *stream_name);

/**
 * Generates a fresh random 8-byte session salt for sender session.
 */
OPENAVX_API void avx_crypto_generate_salt(avx_crypto_ctx_t *ctx);

/**
 * Wipes sensitive key material from memory.
 */
OPENAVX_API void avx_crypto_wipe(avx_crypto_ctx_t *ctx);

/**
 * Encrypts and authenticates a fragment payload using XChaCha20-Poly1305.
 * The 32-byte UDP header is authenticated as Additional Data (AD).
 *
 * out_buf layout: [session_salt: 8 bytes] [cipher_payload: plain_len bytes] [mac: 16 bytes]
 * out_len will be plain_len + AVX_CRYPTO_OVERHEAD (plain_len + 24).
 */
OPENAVX_API int avx_crypto_encrypt_payload(const avx_crypto_ctx_t *ctx,
                                           uint32_t frame_seq, uint16_t frag_idx, uint16_t frag_count,
                                           const void *header_ad, size_t ad_len,
                                           const void *plain_payload, size_t plain_len,
                                           void *out_buf, size_t *out_len);

/**
 * Decrypts and verifies a fragment payload using XChaCha20-Poly1305.
 * in_buf layout: [session_salt: 8 bytes] [cipher_payload: (in_len - 24) bytes] [mac: 16 bytes]
 *
 * Returns 0 on success, -1 on MAC failure / tampering.
 */
OPENAVX_API int avx_crypto_decrypt_payload(const avx_crypto_ctx_t *ctx,
                                           uint32_t frame_seq, uint16_t frag_idx, uint16_t frag_count,
                                           const void *header_ad, size_t ad_len,
                                           const void *in_buf, size_t in_len,
                                           void *plain_out, size_t *out_plain_len);

#ifdef __cplusplus
}
#endif

#endif /* AVX_CRYPTO_H */
