/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_fec.c - Pure C99 & SIMD SMPTE ST 2022-1 2D XOR Forward Error Correction
 */

#include "avx_fec.h"
#include <string.h>
#include <stdlib.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

/*
 * SIMD XOR accumulation kernel for FEC block parity. Computes dst ^= src using
 * AVX2 (256-bit) or NEON (128-bit) with 64-bit word and byte-level tail processing.
 */
void avx_fec_xor_block(uint8_t *dst, const uint8_t *src, size_t len) {
    if (!dst || !src || len == 0) return;

    size_t i = 0;

#if defined(__AVX2__)
    for (; i + 32 <= len; i += 32) {
        __m256i vd = _mm256_loadu_si256((const __m256i*)(dst + i));
        __m256i vs = _mm256_loadu_si256((const __m256i*)(src + i));
        _mm256_storeu_si256((__m256i*)(dst + i), _mm256_xor_si256(vd, vs));
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; i + 16 <= len; i += 16) {
        uint8x16_t vd = vld1q_u8(dst + i);
        uint8x16_t vs = vld1q_u8(src + i);
        vst1q_u8(dst + i, veorq_u8(vd, vs));
    }
#endif

    /* 64-bit scalar tail */
    for (; i + 8 <= len; i += 8) {
        uint64_t vd, vs;
        memcpy(&vd, dst + i, 8);
        memcpy(&vs, src + i, 8);
        vd ^= vs;
        memcpy(dst + i, &vd, 8);
    }

    /* Byte tail */
    for (; i < len; ++i) {
        dst[i] ^= src[i];
    }
}

/*
 * Generates SMPTE ST 2022-1 2D XOR FEC parity matrices. Column parity protects against
 * long burst packet losses; optional row parity protects against dispersed losses.
 */
int avx_fec_generate_block(
    const avx_fec_slice_t *media_slices,
    uint16_t block_frags,
    uint16_t matrix_d,
    uint16_t matrix_l,
    bool enable_row_fec,
    avx_fec_slice_t *out_col_parity,
    avx_fec_slice_t *out_row_parity
) {
    if (!media_slices || block_frags == 0 || matrix_d == 0 || matrix_l == 0 || !out_col_parity) {
        return -1;
    }
    if (matrix_d > AVX_FEC_MAX_COLS || matrix_l > AVX_FEC_MAX_ROWS) {
        return -1;
    }

    /* 1. Generate Column Parity (1D FEC): C[j] = XOR over row i of P[i * D + j] */
    for (uint16_t j = 0; j < matrix_d; ++j) {
        /* Find maximum fragment length in this column */
        size_t max_col_len = 0;
        for (uint16_t i = 0; i < matrix_l; ++i) {
            uint16_t idx = (uint16_t)(i * matrix_d + j);
            if (idx < block_frags && media_slices[idx].data && media_slices[idx].len > max_col_len) {
                max_col_len = media_slices[idx].len;
            }
        }

        if (max_col_len > 0 && out_col_parity[j].data) {
            memset(out_col_parity[j].data, 0, max_col_len);
            for (uint16_t i = 0; i < matrix_l; ++i) {
                uint16_t idx = (uint16_t)(i * matrix_d + j);
                if (idx < block_frags && media_slices[idx].data && media_slices[idx].len > 0) {
                    avx_fec_xor_block(out_col_parity[j].data, media_slices[idx].data, media_slices[idx].len);
                }
            }
            out_col_parity[j].len = max_col_len;
        } else {
            out_col_parity[j].len = 0;
        }
    }

    /* 2. Generate Row Parity (2D FEC): R[i] = XOR over col j of P[i * D + j] */
    if (enable_row_fec && out_row_parity) {
        for (uint16_t i = 0; i < matrix_l; ++i) {
            size_t max_row_len = 0;
            for (uint16_t j = 0; j < matrix_d; ++j) {
                uint16_t idx = (uint16_t)(i * matrix_d + j);
                if (idx < block_frags && media_slices[idx].data && media_slices[idx].len > max_row_len) {
                    max_row_len = media_slices[idx].len;
                }
            }

            if (max_row_len > 0 && out_row_parity[i].data) {
                memset(out_row_parity[i].data, 0, max_row_len);
                for (uint16_t j = 0; j < matrix_d; ++j) {
                    uint16_t idx = (uint16_t)(i * matrix_d + j);
                    if (idx < block_frags && media_slices[idx].data && media_slices[idx].len > 0) {
                        avx_fec_xor_block(out_row_parity[i].data, media_slices[idx].data, media_slices[idx].len);
                    }
                }
                out_row_parity[i].len = max_row_len;
            } else {
                out_row_parity[i].len = 0;
            }
        }
    }

    return 0;
}

/*
 * Iterative 2D erasure solver. Alternates row and column passes to reconstruct
 * any vector containing exactly one missing packet until convergence or exhaustion.
 */
int avx_fec_reconstruct_block(
    avx_fec_slice_t *media_slices,
    bool *is_present,
    uint16_t block_frags,
    uint16_t matrix_d,
    uint16_t matrix_l,
    const avx_fec_slice_t *col_parity,
    const bool *col_present,
    const avx_fec_slice_t *row_parity,
    const bool *row_present
) {
    if (!media_slices || !is_present || block_frags == 0 || matrix_d == 0 || matrix_l == 0) {
        return -1;
    }
    if (matrix_d > AVX_FEC_MAX_COLS || matrix_l > AVX_FEC_MAX_ROWS) {
        return -1;
    }

    int total_recovered = 0;
    bool progress = true;

    /* Iterative 2D Solver Loop: continues while rows or columns with single missing packets can be resolved */
    while (progress) {
        progress = false;

        /* Step 1: Scan Columns */
        if (col_parity && col_present) {
            for (uint16_t j = 0; j < matrix_d; ++j) {
                if (!col_present[j] || !col_parity[j].data || col_parity[j].len == 0) {
                    continue;
                }

                int missing_count = 0;
                uint16_t missing_row = 0;
                for (uint16_t i = 0; i < matrix_l; ++i) {
                    uint16_t idx = (uint16_t)(i * matrix_d + j);
                    if (idx < block_frags && !is_present[idx]) {
                        missing_count++;
                        missing_row = i;
                    }
                }

                /* Exactly 1 missing fragment in column j -> Reconstruct it! */
                if (missing_count == 1) {
                    uint16_t target_idx = (uint16_t)(missing_row * matrix_d + j);
                    if (media_slices[target_idx].data) {
                        size_t parity_len = col_parity[j].len;
                        memcpy(media_slices[target_idx].data, col_parity[j].data, parity_len);

                        for (uint16_t i = 0; i < matrix_l; ++i) {
                            if (i == missing_row) continue;
                            uint16_t other_idx = (uint16_t)(i * matrix_d + j);
                            if (other_idx < block_frags && is_present[other_idx] &&
                                media_slices[other_idx].data && media_slices[other_idx].len > 0) {
                                avx_fec_xor_block(media_slices[target_idx].data,
                                                  media_slices[other_idx].data,
                                                  media_slices[other_idx].len);
                            }
                        }

                        media_slices[target_idx].len = parity_len;
                        is_present[target_idx] = true;
                        total_recovered++;
                        progress = true;
                    }
                }
            }
        }

        /* Step 2: Scan Rows */
        if (row_parity && row_present) {
            for (uint16_t i = 0; i < matrix_l; ++i) {
                if (!row_present[i] || !row_parity[i].data || row_parity[i].len == 0) {
                    continue;
                }

                int missing_count = 0;
                uint16_t missing_col = 0;
                for (uint16_t j = 0; j < matrix_d; ++j) {
                    uint16_t idx = (uint16_t)(i * matrix_d + j);
                    if (idx < block_frags && !is_present[idx]) {
                        missing_count++;
                        missing_col = j;
                    }
                }

                /* Exactly 1 missing fragment in row i -> Reconstruct it! */
                if (missing_count == 1) {
                    uint16_t target_idx = (uint16_t)(i * matrix_d + missing_col);
                    if (media_slices[target_idx].data) {
                        size_t parity_len = row_parity[i].len;
                        memcpy(media_slices[target_idx].data, row_parity[i].data, parity_len);

                        for (uint16_t j = 0; j < matrix_d; ++j) {
                            if (j == missing_col) continue;
                            uint16_t other_idx = (uint16_t)(i * matrix_d + j);
                            if (other_idx < block_frags && is_present[other_idx] &&
                                media_slices[other_idx].data && media_slices[other_idx].len > 0) {
                                avx_fec_xor_block(media_slices[target_idx].data,
                                                  media_slices[other_idx].data,
                                                  media_slices[other_idx].len);
                            }
                        }

                        media_slices[target_idx].len = parity_len;
                        is_present[target_idx] = true;
                        total_recovered++;
                        progress = true;
                    }
                }
            }
        }
    }

    return total_recovered;
}
