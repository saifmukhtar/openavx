/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_fec.h - SMPTE ST 2022-1 2D XOR Forward Error Correction
 */

#ifndef AVX_FEC_H
#define AVX_FEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "avx_platform.h"
#include "openavx.h"

#define OAVX_FOURCC_FEC_COLUMN  0x31434546 /* 'FEC1' - SMPTE 2022-1 Column Parity */
#define OAVX_FOURCC_FEC_ROW     0x32434546 /* 'FEC2' - SMPTE 2022-1 Row Parity */
#define AVX_FOURCC_FEC_COLUMN   OAVX_FOURCC_FEC_COLUMN
#define AVX_FOURCC_FEC_ROW      OAVX_FOURCC_FEC_ROW

#define AVX_FEC_MAX_COLS 32
#define AVX_FEC_MAX_ROWS 32
#define AVX_FEC_MAX_BLOCK_FRAGS (AVX_FEC_MAX_COLS * AVX_FEC_MAX_ROWS)

#ifdef __cplusplus
extern "C" {
#endif

AVX_PACKED_START
typedef struct {
    uint32_t media_fourcc;    /* FourCC of the underlying media frame */
    uint16_t matrix_d;        /* Matrix columns D (e.g. 10) */
    uint16_t matrix_l;        /* Matrix rows L (e.g. 5) */
    uint16_t block_idx;       /* Matrix block index within the frame */
    uint16_t parity_idx;      /* Column index (0..D-1) or Row index (0..L-1) */
    uint16_t base_frag_idx;   /* First media fragment index in this block */
    uint16_t block_frags;     /* Number of media fragments in this block */
    uint16_t parity_len;      /* Length in bytes of XOR parity payload */
    uint16_t last_frag_len;   /* Exact length of the last media fragment in this block */
} AVX_PACKED avx_fec_wire_header_t;
AVX_PACKED_END

typedef struct {
    bool     enabled;         /* Whether FEC is enabled */
    uint16_t matrix_d;        /* Matrix columns D (default: 10) */
    uint16_t matrix_l;        /* Matrix rows L (default: 5) */
    bool     enable_row_fec;  /* Whether 2D row parity is enabled */
} avx_fec_config_t;

typedef struct {
    uint8_t *data;
    size_t   len;
} avx_fec_slice_t;

/**
 * High-speed vectorized bitwise XOR kernel (AVX2 / ARM NEON / scalar 64-bit).
 * Computes: dst[i] ^= src[i] for i = 0 .. len - 1.
 */
OPENAVX_API void avx_fec_xor_block(uint8_t *dst, const uint8_t *src, size_t len);

/**
 * Generates SMPTE ST 2022-1 2D XOR column and row parity slices for a block of media fragments.
 *
 * @param media_slices Array of media fragment slices (length block_frags).
 * @param block_frags Number of fragments in this block (<= matrix_d * matrix_l).
 * @param matrix_d Columns count D (1 .. AVX_FEC_MAX_COLS).
 * @param matrix_l Rows count L (1 .. AVX_FEC_MAX_ROWS).
 * @param enable_row_fec If true, computes row parity slices in addition to column parity.
 * @param out_col_parity Output array of matrix_d column parity slices (preallocated by caller).
 * @param out_row_parity Output array of matrix_l row parity slices (or NULL if enable_row_fec is false).
 * @return 0 on success, -1 on invalid parameter.
 */
OPENAVX_API int avx_fec_generate_block(
    const avx_fec_slice_t *media_slices,
    uint16_t block_frags,
    uint16_t matrix_d,
    uint16_t matrix_l,
    bool enable_row_fec,
    avx_fec_slice_t *out_col_parity,
    avx_fec_slice_t *out_row_parity
);

/**
 * Performs iterative 2D SMPTE ST 2022-1 reconstruction on a block with missing fragments.
 *
 * @param media_slices Array of block_frags media slices. Reconstructed slices will be written into
 *                     caller-provided buffers in media_slices[i].data.
 * @param is_present Array of block_frags booleans indicating which fragments have arrived (true) or are missing (false).
 *                   Will be updated to true for any reconstructed fragment.
 * @param block_frags Number of fragments in this block.
 * @param matrix_d Columns count D.
 * @param matrix_l Rows count L.
 * @param col_parity Array of matrix_d column parity slices.
 * @param col_present Array of matrix_d booleans indicating which column parity packets arrived.
 * @param row_parity Array of matrix_l row parity slices (or NULL if row parity was disabled).
 * @param row_present Array of matrix_l booleans indicating which row parity packets arrived (or NULL).
 * @return Number of fragments successfully recovered (>= 0), or -1 on error.
 */
OPENAVX_API int avx_fec_reconstruct_block(
    avx_fec_slice_t *media_slices,
    bool *is_present,
    uint16_t block_frags,
    uint16_t matrix_d,
    uint16_t matrix_l,
    const avx_fec_slice_t *col_parity,
    const bool *col_present,
    const avx_fec_slice_t *row_parity,
    const bool *row_present
);

#ifdef __cplusplus
}
#endif

#endif /* AVX_FEC_H */
