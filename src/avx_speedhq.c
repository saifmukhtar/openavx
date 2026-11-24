/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (c) 2017 Steinar H. Gunderson
 * Copyright (c) 2017-2026 FFmpeg contributors
 * Copyright (c) 2026 Saif Mukhtar (integration and optimizations for OpenAVX)
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_speedhq.c - Standalone SpeedHQ intra-frame DCT encoder and decoder
 *
 * Derived from FFmpeg libavcodec (speedhq.c / speedhqenc.c).
 * Licensed under the GNU Lesser General Public License version 2.1 or later.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "avx_platform.h"
#include "avx_internal.h"
#include "avx_speedhq.h"
#include "avx_simd.h"

/* SpeedHQ unscaled base quantization matrix (MPEG-2 intra baseline) */
static const uint8_t s_unscaled_quant_matrix[64] = {
    16, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

/* Standard 8x8 Zig-Zag scan order */
static const uint8_t s_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* SpeedHQ AC Variable Length Coding Entry */
typedef struct {
    uint16_t code;   /* VLC bit pattern */
    uint8_t  len;    /* Code length in bits (2..16) */
    uint8_t  run;    /* Zero run count (0..31) */
    uint8_t  level;  /* Absolute coefficient magnitude (1..40) */
} avx_shq_vlc_entry_t;

#define SPEEDHQ_AC_VLC_COUNT   121
#define SPEEDHQ_AC_SHORT_COUNT 41

#define SPEEDHQ_ESCAPE_CODE    0x0020
#define SPEEDHQ_ESCAPE_BITS    6
#define SPEEDHQ_EOB_CODE       0x0006
#define SPEEDHQ_EOB_BITS       4

#define SHQ_DEC_EOB            254
#define SHQ_DEC_ESCAPE         253
#define SHQ_DEC_LONG           0

/* Re-packed AC VLC Table sorted strictly by (len, code) */
static const avx_shq_vlc_entry_t s_speedhq_ac_table[121] = {
    { 0x0001,  2,  0,  1 },    { 0x0002,  3,  1,  1 },    { 0x0003,  3,  0,  2 },    { 0x000E,  4,  0,  3 },
    { 0x0007,  5,  0,  4 },    { 0x000C,  5,  1,  2 },    { 0x0014,  5,  2,  1 },    { 0x0017,  5,  0,  5 },
    { 0x001C,  5,  3,  1 },    { 0x0008,  6,  0,  7 },    { 0x0018,  6,  4,  1 },    { 0x0028,  6,  0,  6 },
    { 0x0038,  6,  5,  1 },    { 0x000F,  7,  9,  1 },    { 0x0010,  7,  7,  1 },    { 0x001F,  7,  0,  9 },
    { 0x002F,  7, 10,  1 },    { 0x0030,  7,  6,  1 },    { 0x004F,  7,  1,  3 },    { 0x0050,  7,  8,  1 },
    { 0x006F,  7,  0,  8 },    { 0x0070,  7,  2,  2 },    { 0x0004,  8,  1,  5 },    { 0x0024,  8, 13,  1 },
    { 0x003F,  8,  2,  3 },    { 0x0044,  8,  0, 11 },    { 0x005F,  8,  0, 12 },    { 0x0064,  8,  3,  2 },
    { 0x007F,  8,  0, 14 },    { 0x0084,  8, 11,  1 },    { 0x00A4,  8, 12,  1 },    { 0x00BF,  8,  4,  2 },
    { 0x00C4,  8,  0, 10 },    { 0x00DF,  8,  0, 13 },    { 0x00E4,  8,  1,  4 },    { 0x00FF,  8,  0, 15 },
    { 0x0040,  9,  5,  2 },    { 0x0140,  9, 14,  1 },    { 0x01C0,  9, 15,  1 },    { 0x00C0, 10,  2,  4 },
    { 0x02C0, 10, 16,  1 },    { 0x0080, 12,  2,  9 },    { 0x0180, 12,  2,  6 },    { 0x0280, 12,  2,  7 },
    { 0x0380, 12,  3,  3 },    { 0x0480, 12,  4,  3 },    { 0x0580, 12, 18,  1 },    { 0x0680, 12, 21,  1 },
    { 0x0780, 12,  6,  2 },    { 0x0880, 12,  8,  2 },    { 0x0980, 12, 19,  1 },    { 0x0A80, 12,  7,  2 },
    { 0x0B80, 12,  4,  4 },    { 0x0C80, 12,  2,  8 },    { 0x0D80, 12,  3,  5 },    { 0x0E80, 12, 20,  1 },
    { 0x0F80, 12, 17,  1 },    { 0x0100, 13, 10,  2 },    { 0x0300, 13,  1, 19 },    { 0x0500, 13,  2,  5 },
    { 0x0700, 13, 25,  1 },    { 0x0900, 13,  5,  3 },    { 0x0B00, 13,  2, 10 },    { 0x0D00, 13,  1,  6 },
    { 0x0F00, 13, 23,  1 },    { 0x1100, 13,  9,  2 },    { 0x1300, 13,  2, 11 },    { 0x1500, 13,  1,  7 },
    { 0x1700, 13, 24,  1 },    { 0x1900, 13,  3,  4 },    { 0x1B00, 13, 26,  1 },    { 0x1D00, 13,  1, 20 },
    { 0x1F00, 13, 22,  1 },    { 0x0200, 14,  0, 31 },    { 0x0600, 14,  0, 23 },    { 0x0A00, 14,  0, 27 },
    { 0x0E00, 14,  0, 19 },    { 0x1200, 14,  0, 29 },    { 0x1600, 14,  0, 21 },    { 0x1A00, 14,  0, 25 },
    { 0x1E00, 14,  0, 17 },    { 0x2200, 14,  0, 30 },    { 0x2600, 14,  0, 22 },    { 0x2A00, 14,  0, 26 },
    { 0x2E00, 14,  0, 18 },    { 0x3200, 14,  0, 28 },    { 0x3600, 14,  0, 20 },    { 0x3A00, 14,  0, 24 },
    { 0x3E00, 14,  0, 16 },    { 0x0400, 15,  0, 40 },    { 0x0C00, 15,  0, 32 },    { 0x1400, 15,  0, 36 },
    { 0x1C00, 15,  1, 11 },    { 0x2400, 15,  0, 38 },    { 0x2C00, 15,  1, 13 },    { 0x3400, 15,  0, 34 },
    { 0x3C00, 15,  1,  9 },    { 0x4400, 15,  0, 39 },    { 0x4C00, 15,  1, 14 },    { 0x5400, 15,  0, 35 },
    { 0x5C00, 15,  1, 10 },    { 0x6400, 15,  0, 37 },    { 0x6C00, 15,  1, 12 },    { 0x7400, 15,  0, 33 },
    { 0x7C00, 15,  1,  8 },    { 0x0800, 16,  1, 18 },    { 0x1800, 16, 13,  2 },    { 0x2800, 16,  6,  3 },
    { 0x3800, 16, 30,  1 },    { 0x4800, 16,  1, 16 },    { 0x5800, 16, 11,  2 },    { 0x6800, 16, 15,  2 },
    { 0x7800, 16, 28,  1 },    { 0x8800, 16,  1, 17 },    { 0x9800, 16, 12,  2 },    { 0xA800, 16, 16,  2 },
    { 0xB800, 16, 29,  1 },    { 0xC800, 16,  1, 15 },    { 0xD800, 16, 31,  1 },    { 0xE800, 16, 14,  2 },
    { 0xF800, 16, 27,  1 },
};

/* Fast O(1) Prefix Lookups */
static uint8_t  s_shq_dec_lut[1024];
static int16_t  s_shq_enc_lut[32][41];
static bool     s_shq_tables_ready = false;

/*
 * Populates 10-bit direct lookup table (s_shq_dec_lut[1024]) for O(1) AC VLC decoding
 * of the 41 most common codes, plus EOB and Escape tokens.
 */
static void speedhq_init_vlc_tables(void)
{
    if (s_shq_tables_ready) return;
    memset(s_shq_dec_lut, 0, sizeof(s_shq_dec_lut));
    memset(s_shq_enc_lut, 0xFF, sizeof(s_shq_enc_lut));

    /* Mark EOB (0x0006, 4 bits) in 10-bit LUT */
    for (int v = 0; v < 1024; ++v) {
        if ((v & 0x0F) == SPEEDHQ_EOB_CODE) s_shq_dec_lut[v] = SHQ_DEC_EOB;
    }
    /* Mark Escape (0x0020, 6 bits) in 10-bit LUT */
    for (int v = 0; v < 1024; ++v) {
        if ((v & 0x3F) == SPEEDHQ_ESCAPE_CODE) s_shq_dec_lut[v] = SHQ_DEC_ESCAPE;
    }

    /* Populate 41 short codes (len <= 10) in 10-bit LUT */
    for (int i = 0; i < SPEEDHQ_AC_SHORT_COUNT; ++i) {
        const avx_shq_vlc_entry_t *e = &s_speedhq_ac_table[i];
        uint32_t mask = (1U << e->len) - 1;
        for (int v = 0; v < 1024; ++v) {
            if ((v & mask) == e->code) {
                s_shq_dec_lut[v] = (uint8_t)(i + 1);
            }
        }
    }

    /* Populate 2D encoder LUT */
    for (int i = 0; i < SPEEDHQ_AC_VLC_COUNT; ++i) {
        const avx_shq_vlc_entry_t *e = &s_speedhq_ac_table[i];
        if (e->run < 32 && e->level <= 40) {
            s_shq_enc_lut[e->run][e->level] = (int16_t)i;
        }
    }

    s_shq_tables_ready = true;
}



/* Fast reverse DC codes matching little-endian MPEG-2 / SpeedHQ */
static const uint16_t s_dc_lum_bits[12] = { 3, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9, 9 };
static const uint16_t s_dc_lum_code[12] = {
    0x1, 0x0, 0x2, 0x5, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF
};
static const uint16_t s_dc_chr_bits[12] = { 2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10 };
static const uint16_t s_dc_chr_code[12] = {
    0x0, 0x2, 0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF
};

/* Little-endian Bitwriter (64-bit accumulator with optional growable slice buffer) */
typedef struct {
    uint8_t *buf;
    size_t   size;
    size_t   capacity;
    size_t   max_capacity;
    bool     can_grow;
    bool     overflow;
    uint64_t bit_buf;
    int      bit_count;
} avx_bitwriter_t;

static inline void bitwriter_init_ex(avx_bitwriter_t *bw, uint8_t *buf, size_t capacity,
                                     bool can_grow, size_t max_capacity)
{
    bw->buf = buf;
    bw->size = 0;
    bw->capacity = capacity;
    bw->max_capacity = max_capacity;
    bw->can_grow = can_grow;
    bw->overflow = false;
    bw->bit_buf = 0;
    bw->bit_count = 0;
}

static inline void bitwriter_init(avx_bitwriter_t *bw, uint8_t *buf, size_t capacity)
{
    bitwriter_init_ex(bw, buf, capacity, false, capacity);
}

static inline bool bitwriter_ensure_room(avx_bitwriter_t *bw, size_t need_bytes)
{
    if (bw->size + need_bytes <= bw->capacity) return true;
    if (!bw->can_grow || bw->capacity >= bw->max_capacity) {
        bw->overflow = true;
        return false;
    }
    size_t new_cap = bw->capacity ? bw->capacity * 2 : 65536;
    while (new_cap < bw->size + need_bytes && new_cap < bw->max_capacity) {
        if (new_cap > bw->max_capacity / 2) {
            new_cap = bw->max_capacity;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap > bw->max_capacity) new_cap = bw->max_capacity;
    if (new_cap < bw->size + need_bytes) {
        bw->overflow = true;
        return false;
    }
    uint8_t *nb = (uint8_t *)realloc(bw->buf, new_cap);
    if (!nb) {
        bw->overflow = true;
        return false;
    }
    bw->buf = nb;
    bw->capacity = new_cap;
    return true;
}

/*
 * Appends bits in little-endian order (LSB first into accumulator), flushing
 * 32-bit words in little-endian byte order as required by SpeedHQ bitstreams.
 */
static inline void bitwriter_put_le(avx_bitwriter_t *bw, int n, uint32_t val)
{
    uint64_t mask = (n >= 32) ? 0xFFFFFFFFULL : ((1ULL << n) - 1ULL);
    bw->bit_buf |= ((uint64_t)val & mask) << bw->bit_count;
    bw->bit_count += n;
    if (bw->bit_count >= 32) {
        if (bw->size + 4 <= bw->capacity || bitwriter_ensure_room(bw, 4)) {
            uint32_t lo = (uint32_t)(bw->bit_buf & 0xFFFFFFFFULL);
            bw->buf[bw->size + 0] = (uint8_t)(lo);
            bw->buf[bw->size + 1] = (uint8_t)(lo >> 8);
            bw->buf[bw->size + 2] = (uint8_t)(lo >> 16);
            bw->buf[bw->size + 3] = (uint8_t)(lo >> 24);
            bw->size += 4;
            bw->bit_buf >>= 32;
            bw->bit_count -= 32;
        } else {
            while (bw->bit_count >= 8) {
                if (bw->size < bw->capacity) {
                    bw->buf[bw->size++] = (uint8_t)(bw->bit_buf & 0xFFU);
                } else {
                    bw->overflow = true;
                }
                bw->bit_buf >>= 8;
                bw->bit_count -= 8;
            }
        }
    }
}

static inline void bitwriter_reserve_3(avx_bitwriter_t *bw)
{
    /* Called at byte-aligned slice boundary (bit_count == 0) to hold 24-bit slice length */
    if (bitwriter_ensure_room(bw, 3)) {
        bw->buf[bw->size + 0] = 0;
        bw->buf[bw->size + 1] = 0;
        bw->buf[bw->size + 2] = 0;
        bw->size += 3;
    }
}

static inline void bitwriter_flush(avx_bitwriter_t *bw)
{
    while (bw->bit_count >= 8) {
        if (bitwriter_ensure_room(bw, 1)) {
            bw->buf[bw->size++] = (uint8_t)(bw->bit_buf & 0xFFU);
        }
        bw->bit_buf >>= 8;
        bw->bit_count -= 8;
    }
    if (bw->bit_count > 0) {
        if (bitwriter_ensure_room(bw, 1)) {
            bw->buf[bw->size++] = (uint8_t)(bw->bit_buf & 0xFFU);
        }
        bw->bit_buf = 0;
        bw->bit_count = 0;
    }
}

/* Precomputed per-frame quantization thresholds & fixed-point division reciprocals */
typedef struct {
    uint32_t quant_zz[64];
    uint32_t mul_zz[64];
    int32_t  dec_quant_zz[64];
} avx_shq_quant_ctx_t;

static inline void speedhq_prepare_quant_ctx(avx_shq_quant_ctx_t *qctx, int qscale)
{
    qctx->quant_zz[0] = 1;
    qctx->mul_zz[0] = 0x80000000U;
    qctx->dec_quant_zz[0] = 1;
    for (int i = 1; i < 64; ++i) {
        int mat = s_unscaled_quant_matrix[s_zigzag[i]];
        int dq = (mat * qscale) / 16;
        int eq = (dq < 1) ? 1 : dq;
        qctx->dec_quant_zz[i] = (int32_t)dq;
        qctx->quant_zz[i] = (uint32_t)eq;
        qctx->mul_zz[i] = (uint32_t)(((1ULL << 31) + (uint32_t)eq - 1ULL) / (uint32_t)eq);
    }
}

/* Little-endian Bitreader */
typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
    uint32_t bit_buf;
    int bit_count;
} avx_bitreader_t;

static inline void bitreader_init(avx_bitreader_t *br, const uint8_t *buf, size_t size)
{
    br->buf = buf;
    br->size = size;
    br->pos = 0;
    br->bit_buf = 0;
    br->bit_count = 0;
}

static inline void bitreader_refill(avx_bitreader_t *br)
{
    if (br->bit_count < 0) br->bit_count = 0;
    while (br->bit_count <= 24 && br->pos < br->size) {
        br->bit_buf |= ((uint32_t)br->buf[br->pos++]) << br->bit_count;
        br->bit_count += 8;
    }
}

static inline uint32_t bitreader_get_le(avx_bitreader_t *br, int n)
{
    bitreader_refill(br);
    uint32_t val = (n >= 32) ? br->bit_buf : (br->bit_buf & ((1U << n) - 1));
    br->bit_buf = (n >= 32) ? 0 : (br->bit_buf >> n);
    br->bit_count -= n;
    if (br->bit_count < 0) br->bit_count = 0;
    return val;
}

static inline uint32_t bitreader_peek_le(avx_bitreader_t *br, int n)
{
    bitreader_refill(br);
    return (n >= 32) ? br->bit_buf : (br->bit_buf & ((1U << n) - 1));
}

static inline void bitreader_skip(avx_bitreader_t *br, int n)
{
    br->bit_buf = (n >= 32) ? 0 : (br->bit_buf >> n);
    br->bit_count -= n;
    if (br->bit_count < 0) br->bit_count = 0;
}

/* Fast 2D Integer Forward DCT (AAN integer approximation - SIMD Accelerated) */
static void fdct_8x8(const int16_t in[64], int16_t out[64])
{
    avx_simd_fdct_8x8(in, out);
}

/* Fast 2D Integer Inverse DCT (IDCT) with saturation to [0, 255] - SIMD Accelerated */
static void idct_8x8_put(const int16_t in[64], uint8_t *dest, int stride)
{
    avx_simd_idct_8x8_put(in, dest, stride);
}

/* Encode a DC difference */
static inline void encode_dc_diff(avx_bitwriter_t *bw, int diff, int component)
{
    if (diff == 0) {
        if (component == 0) bitwriter_put_le(bw, s_dc_lum_bits[0], s_dc_lum_code[0]);
        else bitwriter_put_le(bw, s_dc_chr_bits[0], s_dc_chr_code[0]);
        return;
    }

    int abs_d = diff < 0 ? -diff : diff;
#if defined(__GNUC__) || defined(__clang__)
    int index = (abs_d >= 2048) ? 11 : (32 - __builtin_clz((unsigned)abs_d));
#else
    int index = 0;
    while ((1 << index) <= abs_d && index < 11) index++;
#endif

    int extra = diff > 0 ? diff : (diff - 1) & ((1 << index) - 1);

    if (component == 0) {
        bitwriter_put_le(bw, s_dc_lum_bits[index], s_dc_lum_code[index]);
    } else {
        bitwriter_put_le(bw, s_dc_chr_bits[index], s_dc_chr_code[index]);
    }
    bitwriter_put_le(bw, index, (uint32_t)extra);
}

/* Encode 8x8 block into bitwriter using precomputed division-free quantization */
static void encode_block_c99(avx_bitwriter_t *bw, const int16_t block[64],
                             int *last_dc, int component, const avx_shq_quant_ctx_t *qctx)
{
    /* 1. DC Coefficient: diff is last_dc - block[0] (opposite in SpeedHQ) */
    int dc = (block[0] + 1) >> 1;
    int diff = *last_dc - dc;
    encode_dc_diff(bw, diff, component);
    *last_dc = dc;

    /* 2. AC Coefficients (division-free via exact 31-bit fixed-point reciprocals) */
    int run = 0;
    for (int i = 1; i < 64; ++i) {
        int32_t val = (int32_t)block[s_zigzag[i]];
        int32_t mask = val >> 31;
        uint32_t aval = (uint32_t)((val ^ mask) - mask);
        if (aval < qctx->quant_zz[i]) {
            run++;
            continue;
        }

        uint32_t alevel = (uint32_t)(((uint64_t)aval * (uint64_t)qctx->mul_zz[i]) >> 31);
        int sign = mask & 1;

        /* Check if (run, alevel) matches precomputed VLC table via O(1) LUT */
        int code_idx = -1;
        if (run < 32 && alevel <= 40U) {
            code_idx = s_shq_enc_lut[run][alevel];
        }

        if (code_idx >= 0) {
            /* Emit VLC code + sign bit */
            const avx_shq_vlc_entry_t *entry = &s_speedhq_ac_table[code_idx];
            uint32_t vlc = (uint32_t)entry->code | ((uint32_t)sign << entry->len);
            bitwriter_put_le(bw, entry->len + 1, vlc);
        } else {
            /* Emit Escape (0x20, 6 bits) + run (6 bits) + (level + 2048) (12 bits) */
            int level = sign ? -(int)alevel : (int)alevel;
            uint32_t esc = SPEEDHQ_ESCAPE_CODE | ((uint32_t)run << 6) | ((uint32_t)(level + 2048) << 12);
            bitwriter_put_le(bw, 24, esc);
        }
        run = 0;
    }

    /* 3. End-Of-Block (EOB): 4 bits (0x0006) */
    bitwriter_put_le(bw, SPEEDHQ_EOB_BITS, SPEEDHQ_EOB_CODE);
}

/* Helper to decode a single 8x8 DCT block */
static void decode_single_block_c99(avx_bitreader_t *br, int *last_dc, int comp,
                                    const avx_shq_quant_ctx_t *qctx, uint8_t *dest)
{
    int16_t block[64] = {0};

    /* Decode DC */
    int index = 0;
    if (comp == 0) {
        while (index < 12) {
            if (bitreader_peek_le(br, s_dc_lum_bits[index]) == s_dc_lum_code[index]) {
                bitreader_skip(br, s_dc_lum_bits[index]);
                break;
            }
            index++;
        }
    } else {
        while (index < 12) {
            if (bitreader_peek_le(br, s_dc_chr_bits[index]) == s_dc_chr_code[index]) {
                bitreader_skip(br, s_dc_chr_bits[index]);
                break;
            }
            index++;
        }
    }

    int diff = 0;
    if (index > 0 && index < 12) {
        uint32_t extra = bitreader_get_le(br, index);
        if (extra & (1 << (index - 1))) {
            diff = (int)extra;
        } else {
            diff = (int)extra - ((1 << index) - 1);
        }
    }

    *last_dc -= diff;
    block[s_zigzag[0]] = (int16_t)(*last_dc << 1);

    /* Decode AC coefficients */
    int ac_idx = 1;
    while (ac_idx < 64) {
        if (br->pos >= br->size && br->bit_count <= 0) break;

        uint32_t peek10 = bitreader_peek_le(br, 10);
        uint8_t sym = s_shq_dec_lut[peek10];

        if (sym == SHQ_DEC_EOB) {
            bitreader_skip(br, SPEEDHQ_EOB_BITS);
            break;
        }

        if (sym == SHQ_DEC_ESCAPE) {
            bitreader_skip(br, SPEEDHQ_ESCAPE_BITS);
            int run = (int)bitreader_get_le(br, 6);
            int level = (int)bitreader_get_le(br, 12) - 2048;
            ac_idx += run;
            if (ac_idx < 64) {
                block[s_zigzag[ac_idx]] = (int16_t)(level * qctx->dec_quant_zz[ac_idx]);
            }
            ac_idx++;
            continue;
        }

        if (sym != SHQ_DEC_LONG) {
            const avx_shq_vlc_entry_t *entry = &s_speedhq_ac_table[sym - 1];
            int sign;
            if (entry->len <= 9) {
                sign = (int)((peek10 >> entry->len) & 1U);
                bitreader_skip(br, entry->len + 1);
            } else {
                bitreader_skip(br, entry->len);
                sign = (int)bitreader_get_le(br, 1);
            }
            int level = sign ? -(int)entry->level : (int)entry->level;

            ac_idx += entry->run;
            if (ac_idx < 64) {
                block[s_zigzag[ac_idx]] = (int16_t)(level * qctx->dec_quant_zz[ac_idx]);
            }
            ac_idx++;
            continue;
        }

        /* Fallback for long codes (length >= 12, entries 41..120) */
        bool matched = false;
        for (int c = SPEEDHQ_AC_SHORT_COUNT; c < SPEEDHQ_AC_VLC_COUNT; ++c) {
            const avx_shq_vlc_entry_t *entry = &s_speedhq_ac_table[c];
            if (bitreader_peek_le(br, entry->len) == entry->code) {
                bitreader_skip(br, entry->len);
                int sign = (int)bitreader_get_le(br, 1);
                int level = (int)entry->level;
                if (sign) level = -level;

                ac_idx += entry->run;
                if (ac_idx < 64) {
                    block[s_zigzag[ac_idx]] = (int16_t)(level * qctx->dec_quant_zz[ac_idx]);
                }
                ac_idx++;
                matched = true;
                break;
            }
        }
        if (!matched) {
            if (br->pos >= br->size && br->bit_count <= 0) break;
            bitreader_skip(br, 1);
        }
    }

    idct_8x8_put(block, dest, 8);
}

/* Encode a 16x8 block of Alpha values using SpeedHQ RLE */
static void encode_alpha_block(avx_bitwriter_t *bw, const uint8_t *src, int stride, int max_rows, uint8_t last_alpha[16])
{
    uint8_t diff_block[128];
    if (src && max_rows > 0) {
        for (int y = 0; y < 8; ++y) {
            int y_clamped = (y < max_rows) ? y : (max_rows - 1);
            for (int x = 0; x < 16; ++x) {
                uint8_t cur = src[(size_t)y_clamped * stride + x];
                diff_block[y * 16 + x] = (uint8_t)(last_alpha[x] - cur);
                last_alpha[x] = cur;
            }
        }
    } else {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 16; ++x) {
                diff_block[y * 16 + x] = (uint8_t)(last_alpha[x] - 255);
                last_alpha[x] = 255;
            }
        }
    }

    int i = 0;
    while (i < 128) {
        int run = 0;
        while (i + run < 128 && diff_block[i + run] == 0) {
            run++;
        }
        if (i + run == 128) {
            bitwriter_put_le(bw, 3, 3); /* EOB */
            break;
        }

        if (run == 0) {
            bitwriter_put_le(bw, 1, 0);
        } else if (run >= 1 && run <= 4) {
            bitwriter_put_le(bw, 4, ((uint32_t)(run - 1) << 2) | 1);
        } else {
            bitwriter_put_le(bw, 10, ((uint32_t)run << 3) | 7);
        }
        i += run;

        int8_t sdiff = (int8_t)diff_block[i];
        if (sdiff == 1) {
            bitwriter_put_le(bw, 2, 1);
        } else if (sdiff == -1) {
            bitwriter_put_le(bw, 2, 3);
        } else if (sdiff >= 2 && sdiff <= 5) {
            bitwriter_put_le(bw, 5, ((uint32_t)(sdiff - 2) << 3) | 2);
        } else if (sdiff >= -5 && sdiff <= -2) {
            bitwriter_put_le(bw, 5, (((uint32_t)(-sdiff - 2)) << 3) | (1 << 2) | 2);
        } else {
            bitwriter_put_le(bw, 10, ((uint32_t)(uint8_t)diff_block[i]) << 2);
        }
        i++;
    }
}

/* Decode a 16x8 block of Alpha values using SpeedHQ RLE */
static void decode_alpha_block(avx_bitreader_t *br, uint8_t *dest, int stride, int max_rows, uint8_t last_alpha[16])
{
    uint8_t block[128];
    memset(block, 0, sizeof(block));
    int i = 0;

    while (i < 128) {
        if (br->pos >= br->size && br->bit_count <= 0) break;

        /* EOB: 110 (3 bits, code 3) */
        if (bitreader_peek_le(br, 3) == 3) {
            bitreader_skip(br, 3);
            break;
        }

        /* Decode run */
        int run = 0;
        if (bitreader_peek_le(br, 1) == 0) {
            bitreader_skip(br, 1);
            run = 0;
        } else if ((bitreader_peek_le(br, 2) & 3) == 1) {
            uint32_t code = bitreader_get_le(br, 4);
            run = (int)(code >> 2) + 1;
        } else if ((bitreader_peek_le(br, 3) & 7) == 7) {
            uint32_t code = bitreader_get_le(br, 10);
            run = (int)(code >> 3);
        } else {
            bitreader_skip(br, 1);
            continue;
        }

        i += run;
        if (i >= 128) break;

        /* Decode level */
        int level = 0;
        if ((bitreader_peek_le(br, 1) & 1) == 1) {
            uint32_t code = bitreader_get_le(br, 2);
            level = (code >> 1) ? -1 : 1;
        } else if ((bitreader_peek_le(br, 2) & 3) == 2) {
            uint32_t code = bitreader_get_le(br, 5);
            int sign = (int)((code >> 2) & 1);
            int val = (int)(code >> 3) + 2;
            level = sign ? -val : val;
        } else if ((bitreader_peek_le(br, 2) & 3) == 0) {
            uint32_t code = bitreader_get_le(br, 10);
            level = (int)(code >> 2);
        } else {
            bitreader_skip(br, 1);
            continue;
        }

        block[i++] = (uint8_t)level;
    }

    if (dest && max_rows > 0) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 16; ++x) {
                last_alpha[x] -= block[y * 16 + x];
            }
            if (y < max_rows) {
                memcpy(dest + (size_t)y * stride, last_alpha, 16);
            }
        }
    } else {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 16; ++x) {
                last_alpha[x] -= block[y * 16 + x];
            }
        }
    }
}

/*
 * C99 Standalone Encoder: 1080p UYVY + Alpha -> SpeedHQ
 */
static volatile uint8_t s_speedhq_quality = 96;

void avx_speedhq_set_quality(uint8_t quality) {
    if (quality < 20) quality = 20;
    if (quality > 98) quality = 98;
    s_speedhq_quality = quality;
}

uint8_t avx_speedhq_get_quality(void) {
    return s_speedhq_quality;
}

#define SHQ_POOL_MAX_WORKERS 3
#define SHQ_TASK_QUEUE_CAP   32
#define SHQ_SCRATCH_SLOTS    4

typedef void (*shq_slice_fn_t)(void *ctx, int slice_idx);

typedef struct {
    avx_mutex_t lock;
    avx_cond_t  cond;
    int         remaining;
} avx_shq_job_t;

typedef struct {
    shq_slice_fn_t fn;
    void          *ctx;
    int            slice_idx;
    avx_shq_job_t *job;
} avx_shq_task_t;

typedef struct {
    bool     in_use;
    uint8_t *buf[3];
    size_t   cap[3];
} avx_shq_scratch_slot_t;

typedef struct {
    avx_mutex_t            lock;
    avx_cond_t             work_cond;
    bool                   started;
    bool                   shutdown;
#if defined(AVX_OS_POSIX)
    pid_t                  owner_pid;
#endif
    int                    num_workers;
    avx_thread_t           workers[SHQ_POOL_MAX_WORKERS];
    avx_shq_task_t         queue[SHQ_TASK_QUEUE_CAP];
    int                    q_head;
    int                    q_count;
    avx_shq_scratch_slot_t scratch[SHQ_SCRATCH_SLOTS];
} avx_shq_pool_t;

static avx_shq_pool_t s_shq_pool = {
    .lock = AVX_MUTEX_INITIALIZER,
    .work_cond = AVX_COND_INITIALIZER,
    .started = false,
    .shutdown = false,
    .num_workers = 0,
    .q_head = 0,
    .q_count = 0
};

static int speedhq_cpu_count(void)
{
    const char *env = getenv("OPENAVX_SPEEDHQ_THREADS");
    if (env && env[0] != '\0') {
        int forced = atoi(env);
        if (forced >= 1) return (forced > 4) ? 4 : forced;
    }
#if defined(AVX_OS_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (si.dwNumberOfProcessors > 0) ? (int)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
#endif
}

static void *shq_worker_main(void *arg)
{
    (void)arg;
    while (1) {
        avx_mutex_lock(&s_shq_pool.lock);
        while (!s_shq_pool.shutdown && s_shq_pool.q_count == 0) {
            avx_cond_wait(&s_shq_pool.work_cond, &s_shq_pool.lock);
        }
        if (s_shq_pool.shutdown && s_shq_pool.q_count == 0) {
            avx_mutex_unlock(&s_shq_pool.lock);
            break;
        }
        avx_shq_task_t task = s_shq_pool.queue[s_shq_pool.q_head];
        s_shq_pool.q_head = (s_shq_pool.q_head + 1) % SHQ_TASK_QUEUE_CAP;
        s_shq_pool.q_count--;
        avx_mutex_unlock(&s_shq_pool.lock);

        if (task.fn) {
            task.fn(task.ctx, task.slice_idx);
        }

        avx_mutex_lock(&task.job->lock);
        task.job->remaining--;
        if (task.job->remaining == 0) {
            avx_cond_signal(&task.job->cond);
        }
        avx_mutex_unlock(&task.job->lock);
    }
    return NULL;
}

static void shq_pool_reset_if_forked_locked(void)
{
#if defined(AVX_OS_POSIX)
    if (s_shq_pool.started && s_shq_pool.owner_pid != getpid()) {
        s_shq_pool.started = false;
        s_shq_pool.shutdown = false;
        s_shq_pool.num_workers = 0;
        s_shq_pool.q_head = 0;
        s_shq_pool.q_count = 0;
        memset(s_shq_pool.scratch, 0, sizeof(s_shq_pool.scratch));
        avx_cond_init(&s_shq_pool.work_cond);
    }
#endif
}

static bool shq_pool_ensure_started_locked(void)
{
    shq_pool_reset_if_forked_locked();
    if (s_shq_pool.started) return (s_shq_pool.num_workers > 0);
    if (s_shq_pool.shutdown) return false;

    int want = speedhq_cpu_count() - 1;
    if (want > SHQ_POOL_MAX_WORKERS) want = SHQ_POOL_MAX_WORKERS;
    if (want < 0) want = 0;

    s_shq_pool.started = true;
#if defined(AVX_OS_POSIX)
    s_shq_pool.owner_pid = getpid();
#endif
    s_shq_pool.num_workers = 0;

    for (int i = 0; i < want; ++i) {
        if (avx_thread_create(&s_shq_pool.workers[s_shq_pool.num_workers], shq_worker_main, NULL) == 0) {
            s_shq_pool.num_workers++;
        } else {
            break;
        }
    }
    return (s_shq_pool.num_workers > 0);
}

static void shq_dispatch_slices(shq_slice_fn_t fn, void *ctx, int num_slices)
{
    if (num_slices <= 1) {
        if (num_slices == 1) fn(ctx, 0);
        return;
    }

    avx_mutex_lock(&s_shq_pool.lock);
    bool pool_ok = shq_pool_ensure_started_locked();
    if (!pool_ok || s_shq_pool.shutdown) {
        avx_mutex_unlock(&s_shq_pool.lock);
        for (int s = 0; s < num_slices; ++s) {
            fn(ctx, s);
        }
        return;
    }

    avx_shq_job_t job;
    avx_mutex_init(&job.lock);
    avx_cond_init(&job.cond);
    job.remaining = 0;

    bool local_run[4] = { true, false, false, false };
    for (int s = 1; s < num_slices && s < 4; ++s) {
        if (s_shq_pool.q_count < SHQ_TASK_QUEUE_CAP) {
            int tail = (s_shq_pool.q_head + s_shq_pool.q_count) % SHQ_TASK_QUEUE_CAP;
            s_shq_pool.queue[tail].fn = fn;
            s_shq_pool.queue[tail].ctx = ctx;
            s_shq_pool.queue[tail].slice_idx = s;
            s_shq_pool.queue[tail].job = &job;
            s_shq_pool.q_count++;
            job.remaining++;
        } else {
            local_run[s] = true;
        }
    }

    if (job.remaining > 0) {
        avx_cond_broadcast(&s_shq_pool.work_cond);
    }
    avx_mutex_unlock(&s_shq_pool.lock);

    /* Execute slice 0 (and any overflow slices) directly on caller thread */
    for (int s = 0; s < num_slices && s < 4; ++s) {
        if (local_run[s]) {
            fn(ctx, s);
        }
    }

    /* Help complete any still-queued tasks belonging to this job to guarantee zero starvation */
    while (1) {
        avx_shq_task_t stolen;
        bool found_own = false;

        avx_mutex_lock(&s_shq_pool.lock);
        for (int k = 0; k < s_shq_pool.q_count; ++k) {
            int idx = (s_shq_pool.q_head + k) % SHQ_TASK_QUEUE_CAP;
            if (s_shq_pool.queue[idx].job == &job) {
                stolen = s_shq_pool.queue[idx];
                for (int m = k; m + 1 < s_shq_pool.q_count; ++m) {
                    int dst = (s_shq_pool.q_head + m) % SHQ_TASK_QUEUE_CAP;
                    int src = (s_shq_pool.q_head + m + 1) % SHQ_TASK_QUEUE_CAP;
                    s_shq_pool.queue[dst] = s_shq_pool.queue[src];
                }
                s_shq_pool.q_count--;
                found_own = true;
                break;
            }
        }
        avx_mutex_unlock(&s_shq_pool.lock);

        if (found_own) {
            if (stolen.fn) stolen.fn(stolen.ctx, stolen.slice_idx);
            avx_mutex_lock(&job.lock);
            job.remaining--;
            avx_mutex_unlock(&job.lock);
            continue;
        }

        avx_mutex_lock(&job.lock);
        while (job.remaining > 0) {
            avx_cond_wait(&job.cond, &job.lock);
        }
        avx_mutex_unlock(&job.lock);
        break;
    }

    avx_cond_destroy(&job.cond);
    avx_mutex_destroy(&job.lock);
}

static int shq_scratch_checkout(uint8_t *out_bufs[3], size_t out_caps[3], size_t min_cap, size_t max_cap)
{
    int slot_idx = -1;
    avx_mutex_lock(&s_shq_pool.lock);
    shq_pool_reset_if_forked_locked();
    for (int i = 0; i < SHQ_SCRATCH_SLOTS; ++i) {
        if (!s_shq_pool.scratch[i].in_use) {
            s_shq_pool.scratch[i].in_use = true;
            slot_idx = i;
            for (int s = 0; s < 3; ++s) {
                out_bufs[s] = s_shq_pool.scratch[i].buf[s];
                out_caps[s] = s_shq_pool.scratch[i].cap[s];
            }
            break;
        }
    }
    avx_mutex_unlock(&s_shq_pool.lock);

    for (int s = 0; s < 3; ++s) {
        if (out_caps[s] < min_cap) {
            size_t req = (min_cap <= max_cap) ? min_cap : max_cap;
            uint8_t *nb = (uint8_t *)realloc(out_bufs[s], req);
            if (!nb) {
                for (int k = 0; k < 3; ++k) {
                    if (slot_idx >= 0) {
                        /* Persist already-grown buffers even if a later allocation fails */
                        if (out_bufs[k]) {
                            avx_mutex_lock(&s_shq_pool.lock);
                            s_shq_pool.scratch[slot_idx].buf[k] = out_bufs[k];
                            s_shq_pool.scratch[slot_idx].cap[k] = out_caps[k];
                            avx_mutex_unlock(&s_shq_pool.lock);
                        }
                    } else {
                        free(out_bufs[k]);
                        out_bufs[k] = NULL;
                    }
                }
                if (slot_idx >= 0) {
                    avx_mutex_lock(&s_shq_pool.lock);
                    s_shq_pool.scratch[slot_idx].in_use = false;
                    avx_mutex_unlock(&s_shq_pool.lock);
                }
                return -2;
            }
            out_bufs[s] = nb;
            out_caps[s] = req;
        }
    }
    return slot_idx; /* >= 0 if pooled slot checked out, -1 if transient malloc buffers */
}

static void shq_scratch_checkin(int slot_idx, uint8_t *bufs[3], const size_t caps[3])
{
    if (slot_idx >= 0) {
        avx_mutex_lock(&s_shq_pool.lock);
        for (int s = 0; s < 3; ++s) {
            s_shq_pool.scratch[slot_idx].buf[s] = bufs[s];
            s_shq_pool.scratch[slot_idx].cap[s] = caps[s];
        }
        s_shq_pool.scratch[slot_idx].in_use = false;
        avx_mutex_unlock(&s_shq_pool.lock);
    } else {
        for (int s = 0; s < 3; ++s) {
            free(bufs[s]);
        }
    }
}

static void shq_pool_shutdown_internal(void)
{
    avx_thread_t join_threads[SHQ_POOL_MAX_WORKERS];
    int join_count = 0;

    avx_mutex_lock(&s_shq_pool.lock);
    shq_pool_reset_if_forked_locked();
    if (s_shq_pool.started) {
        s_shq_pool.shutdown = true;
        avx_cond_broadcast(&s_shq_pool.work_cond);
        join_count = s_shq_pool.num_workers;
        for (int i = 0; i < join_count; ++i) {
            join_threads[i] = s_shq_pool.workers[i];
        }
        s_shq_pool.num_workers = 0;
        s_shq_pool.started = false;
    }
    avx_mutex_unlock(&s_shq_pool.lock);

    for (int i = 0; i < join_count; ++i) {
        avx_thread_join(join_threads[i]);
    }

    avx_mutex_lock(&s_shq_pool.lock);
    for (int i = 0; i < SHQ_SCRATCH_SLOTS; ++i) {
        if (!s_shq_pool.scratch[i].in_use) {
            for (int s = 0; s < 3; ++s) {
                free(s_shq_pool.scratch[i].buf[s]);
                s_shq_pool.scratch[i].buf[s] = NULL;
                s_shq_pool.scratch[i].cap[s] = 0;
            }
        }
    }
    s_shq_pool.shutdown = false;
    avx_mutex_unlock(&s_shq_pool.lock);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor))
static void speedhq_module_destructor(void)
{
    shq_pool_shutdown_internal();
}
#endif

typedef struct {
    int                        width;
    int                        height;
    int                        mb_w;
    int                        mb_h;
    int                        num_slices;
    bool                       has_alpha;
    bool                       is_dct_alpha;
    const uint8_t             *uyvy_data;
    size_t                     uyvy_stride;
    const uint8_t             *alpha_data;
    size_t                     alpha_stride;
    const avx_shq_quant_ctx_t *qctx;
    avx_bitwriter_t            bws[4];
} shq_enc_frame_ctx_t;

static inline void shq_extract_uyvy_row_pair(const uint8_t *row_top, const uint8_t *row_bot, int r,
                                             int16_t blk_y0[64], int16_t blk_y1[64],
                                             int16_t blk_y2[64], int16_t blk_y3[64],
                                             int16_t blk_cb0[64], int16_t blk_cb1[64],
                                             int16_t blk_cr0[64], int16_t blk_cr1[64])
{
    int base = r * 8;
    for (int c = 0; c < 8; ++c) {
        blk_y0[base + c]  = row_top[c * 2 + 1];
        blk_y1[base + c]  = row_top[(c + 8) * 2 + 1];
        blk_y2[base + c]  = row_bot[c * 2 + 1];
        blk_y3[base + c]  = row_bot[(c + 8) * 2 + 1];

        blk_cb0[base + c] = row_top[c * 4 + 0];
        blk_cr0[base + c] = row_top[c * 4 + 2];
        blk_cb1[base + c] = row_bot[c * 4 + 0];
        blk_cr1[base + c] = row_bot[c * 4 + 2];
    }
}

static void shq_encode_slice_fn(void *vctx, int slice)
{
    shq_enc_frame_ctx_t *fctx = (shq_enc_frame_ctx_t *)vctx;
    avx_bitwriter_t *bw = &fctx->bws[slice];
    int mb_w = fctx->mb_w;
    int mb_h = fctx->mb_h;
    int num_slices = fctx->num_slices;
    int width = fctx->width;
    int height = fctx->height;
    size_t uyvy_stride = fctx->uyvy_stride ? fctx->uyvy_stride : (size_t)width * 2;
    const uint8_t *uyvy_data = fctx->uyvy_data;
    const uint8_t *alpha_data = fctx->alpha_data;
    size_t alpha_stride = fctx->alpha_stride ? fctx->alpha_stride : (size_t)width;
    const avx_shq_quant_ctx_t *qctx = fctx->qctx;

    size_t slice_start_pos = bw->size;
    bitwriter_reserve_3(bw);

    for (int mb_y = slice; mb_y < mb_h; mb_y += num_slices) {
        int last_dc[4] = { 1024, 1024, 1024, 1024 };
        uint8_t last_alpha[16];
        if (fctx->has_alpha && !fctx->is_dct_alpha) {
            memset(last_alpha, 255, sizeof(last_alpha));
        }
        int y_base = mb_y * 16;
        bool interior_y = (y_base + 15 < height);

        for (int mb_x = 0; mb_x < mb_w; ++mb_x) {
            int x_base = mb_x * 16;

            int16_t blk_y0[64], blk_y1[64], blk_y2[64], blk_y3[64];
            int16_t blk_cb0[64], blk_cb1[64], blk_cr0[64], blk_cr1[64];

            if (interior_y) {
                const uint8_t *row_top = uyvy_data + (size_t)y_base * uyvy_stride + (size_t)x_base * 2;
                const uint8_t *row_bot = row_top + 8 * uyvy_stride;
                for (int r = 0; r < 8; ++r) {
                    shq_extract_uyvy_row_pair(row_top, row_bot, r,
                                              blk_y0, blk_y1, blk_y2, blk_y3,
                                              blk_cb0, blk_cb1, blk_cr0, blk_cr1);
                    row_top += uyvy_stride;
                    row_bot += uyvy_stride;
                }
            } else {
                for (int r = 0; r < 8; ++r) {
                    int y_top = y_base + r;
                    if (y_top >= height) y_top = height - 1;
                    int y_bot = y_base + r + 8;
                    if (y_bot >= height) y_bot = height - 1;
                    const uint8_t *row_top = uyvy_data + (size_t)y_top * uyvy_stride + (size_t)x_base * 2;
                    const uint8_t *row_bot = uyvy_data + (size_t)y_bot * uyvy_stride + (size_t)x_base * 2;
                    shq_extract_uyvy_row_pair(row_top, row_bot, r,
                                              blk_y0, blk_y1, blk_y2, blk_y3,
                                              blk_cb0, blk_cb1, blk_cr0, blk_cr1);
                }
            }

            /* FDCT & Encode */
            int16_t dct[64];
            fdct_8x8(blk_y0, dct);  encode_block_c99(bw, dct, &last_dc[0], 0, qctx);
            fdct_8x8(blk_y1, dct);  encode_block_c99(bw, dct, &last_dc[0], 0, qctx);
            fdct_8x8(blk_y2, dct);  encode_block_c99(bw, dct, &last_dc[0], 0, qctx);
            fdct_8x8(blk_y3, dct);  encode_block_c99(bw, dct, &last_dc[0], 0, qctx);

            fdct_8x8(blk_cb0, dct); encode_block_c99(bw, dct, &last_dc[1], 1, qctx);
            fdct_8x8(blk_cr0, dct); encode_block_c99(bw, dct, &last_dc[2], 2, qctx);
            fdct_8x8(blk_cb1, dct); encode_block_c99(bw, dct, &last_dc[1], 1, qctx);
            fdct_8x8(blk_cr1, dct); encode_block_c99(bw, dct, &last_dc[2], 2, qctx);

            /* Alpha encoding */
            if (fctx->has_alpha) {
                if (fctx->is_dct_alpha) {
                    int16_t blk_a0[64], blk_a1[64], blk_a2[64], blk_a3[64];
                    for (int r = 0; r < 8; ++r) {
                        int y_top = y_base + r;
                        if (y_top >= height) y_top = height - 1;
                        int y_bot = y_base + r + 8;
                        if (y_bot >= height) y_bot = height - 1;
                        const uint8_t *a_top = alpha_data ? (alpha_data + (size_t)y_top * alpha_stride + x_base) : NULL;
                        const uint8_t *a_bot = alpha_data ? (alpha_data + (size_t)y_bot * alpha_stride + x_base) : NULL;
                        for (int c = 0; c < 8; ++c) {
                            blk_a0[r * 8 + c] = a_top ? a_top[c] : 255;
                            blk_a1[r * 8 + c] = a_top ? a_top[c + 8] : 255;
                            blk_a2[r * 8 + c] = a_bot ? a_bot[c] : 255;
                            blk_a3[r * 8 + c] = a_bot ? a_bot[c + 8] : 255;
                        }
                    }
                    fdct_8x8(blk_a0, dct); encode_block_c99(bw, dct, &last_dc[3], 0, qctx);
                    fdct_8x8(blk_a1, dct); encode_block_c99(bw, dct, &last_dc[3], 0, qctx);
                    fdct_8x8(blk_a2, dct); encode_block_c99(bw, dct, &last_dc[3], 0, qctx);
                    fdct_8x8(blk_a3, dct); encode_block_c99(bw, dct, &last_dc[3], 0, qctx);
                } else {
                    int rows_top = (height > y_base) ? (height - y_base) : 0;
                    if (rows_top > 8) rows_top = 8;
                    const uint8_t *a_top = (alpha_data && rows_top > 0) ?
                        (alpha_data + (size_t)y_base * alpha_stride + x_base) : NULL;
                    encode_alpha_block(bw, a_top, (int)alpha_stride, rows_top, last_alpha);

                    int rows_bot = (height > y_base + 8) ? (height - (y_base + 8)) : 0;
                    if (rows_bot > 8) rows_bot = 8;
                    const uint8_t *a_bot = (alpha_data && rows_bot > 0) ?
                        (alpha_data + (size_t)(y_base + 8) * alpha_stride + x_base) : NULL;
                    encode_alpha_block(bw, a_bot, (int)alpha_stride, rows_bot, last_alpha);
                }
            }
        }
    }

    bitwriter_flush(bw);

    if (!bw->overflow && slice_start_pos + 3 <= bw->size) {
        uint32_t slice_len = (uint32_t)(bw->size - slice_start_pos);
        bw->buf[slice_start_pos + 0] = (uint8_t)(slice_len & 0xFF);
        bw->buf[slice_start_pos + 1] = (uint8_t)((slice_len >> 8) & 0xFF);
        bw->buf[slice_start_pos + 2] = (uint8_t)((slice_len >> 16) & 0xFF);
    }
}

/*
 * Core SpeedHQ encoder with support for custom strides and alpha channels.
 * Uses 4-slice vertical multithreading for frames >= 128k pixels; writes
 * slice 0 in-place and slices 1..3 via thread-pooled scratch buffers.
 */
int avx_speedhq_encode_strided_ex(uint32_t fourcc, int width, int height,
                                  const uint8_t *uyvy_data, size_t uyvy_stride,
                                  const uint8_t *alpha_data, size_t alpha_stride,
                                  uint8_t quality,
                                  uint8_t *out_buf, size_t out_max,
                                  size_t *out_size)
{
    avx_speedhq_init();

    if (!uyvy_data || !out_buf || !out_size || width <= 0 || height <= 0 ||
        width > AVX_MAX_VIDEO_WIDTH || height > AVX_MAX_VIDEO_HEIGHT || width % 16 != 0) {
        return -1;
    }

    size_t min_uyvy_stride = (size_t)width * 2;
    size_t min_alpha_stride = (size_t)width;
    if (uyvy_stride == 0) uyvy_stride = min_uyvy_stride;
    if (alpha_stride == 0) alpha_stride = min_alpha_stride;
    if (uyvy_stride < min_uyvy_stride) return -1;
    if (alpha_data && alpha_stride < min_alpha_stride) return -1;

    uint32_t tag = fourcc ? fourcc : (alpha_data ? 0x33514853 : 0x32514853);
    if (!avx_is_speedhq_fourcc(tag)) return -1;
    bool has_alpha = (tag == 0x31514853 || tag == 0x33514853 || tag == 0x35514853 ||
                      tag == 0x37514853 || tag == 0x39514853 || alpha_data != NULL);

    /* SpeedHQ Header: Quality byte + 24-bit little endian second_field_offset */
    uint8_t effective_q = quality ? quality : s_speedhq_quality;
    if (effective_q < 20) effective_q = 20;
    if (effective_q > 98) effective_q = 98;
    int qscale = 100 - effective_q;
    if (qscale < 1) qscale = 1;

    if (out_max < 64) return -2;

    out_buf[0] = effective_q;
    out_buf[1] = 4; /* second_field_offset = 4 (progressive / single-field) */
    out_buf[2] = 0;
    out_buf[3] = 0;

    avx_shq_quant_ctx_t qctx;
    speedhq_prepare_quant_ctx(&qctx, qscale);

    int mb_w = width / 16;
    int mb_h = (height + 15) / 16;
    int num_slices = (mb_h >= 4) ? 4 : 1;

    shq_enc_frame_ctx_t fctx;
    fctx.width = width;
    fctx.height = height;
    fctx.mb_w = mb_w;
    fctx.mb_h = mb_h;
    fctx.num_slices = num_slices;
    fctx.has_alpha = has_alpha;
    fctx.is_dct_alpha = (tag == 0x37514853 || tag == 0x39514853);
    fctx.uyvy_data = uyvy_data;
    fctx.uyvy_stride = uyvy_stride;
    fctx.alpha_data = alpha_data;
    fctx.alpha_stride = alpha_stride;
    fctx.qctx = &qctx;

    bool use_mt = (num_slices == 4) && ((size_t)width * (size_t)height >= 131072U);
    if (!use_mt) {
        avx_bitwriter_t bw;
        bitwriter_init(&bw, out_buf + 4, out_max - 4);
        fctx.bws[0] = bw;
        for (int slice = 0; slice < num_slices; ++slice) {
            shq_encode_slice_fn(&fctx, slice);
            if (slice + 1 < num_slices) {
                fctx.bws[slice + 1] = fctx.bws[slice];
            }
        }
        if (fctx.bws[num_slices - 1].overflow) return -2;
        *out_size = 4 + fctx.bws[num_slices - 1].size;
        return 0;
    }

    size_t max_slice_cap = out_max - 4;
    size_t slice_mb_rows = (size_t)((mb_h + 3) / 4);
    size_t init_slice_cap = (size_t)mb_w * slice_mb_rows * (has_alpha ? 1024U : 600U) + 8192U;
    if (init_slice_cap > max_slice_cap) init_slice_cap = max_slice_cap;
    if (init_slice_cap < 4096U && max_slice_cap >= 4096U) init_slice_cap = 4096U;

    uint8_t *scratch_bufs[3] = { NULL, NULL, NULL };
    size_t   scratch_caps[3] = { 0, 0, 0 };
    int slot_idx = shq_scratch_checkout(scratch_bufs, scratch_caps, init_slice_cap, max_slice_cap);
    if (slot_idx == -2) {
        return -2;
    }

    /* Slice 0 writes directly into out_buf + 4; slices 1..3 use pooled scratch buffers */
    bitwriter_init(&fctx.bws[0], out_buf + 4, max_slice_cap);
    for (int s = 1; s < 4; ++s) {
        bitwriter_init_ex(&fctx.bws[s], scratch_bufs[s - 1], scratch_caps[s - 1], true, max_slice_cap);
    }

    shq_dispatch_slices(shq_encode_slice_fn, &fctx, 4);

    for (int s = 1; s < 4; ++s) {
        scratch_bufs[s - 1] = fctx.bws[s].buf;
        scratch_caps[s - 1] = fctx.bws[s].capacity;
    }

    bool overflowed = fctx.bws[0].overflow;
    size_t write_pos = 4 + fctx.bws[0].size;
    for (int s = 1; s < 4 && !overflowed; ++s) {
        if (fctx.bws[s].overflow || write_pos + fctx.bws[s].size > out_max) {
            overflowed = true;
            break;
        }
        memcpy(out_buf + write_pos, fctx.bws[s].buf, fctx.bws[s].size);
        write_pos += fctx.bws[s].size;
    }

    shq_scratch_checkin(slot_idx, scratch_bufs, scratch_caps);

    if (overflowed) return -2;
    *out_size = write_pos;
    return 0;
}

int avx_speedhq_encode_c99_ex(uint32_t fourcc, int width, int height,
                              const uint8_t *uyvy_data, const uint8_t *alpha_data,
                              uint8_t *out_buf, size_t out_max,
                              size_t *out_size)
{
    return avx_speedhq_encode_strided_ex(fourcc, width, height,
                                         uyvy_data, 0,
                                         alpha_data, 0,
                                         0,
                                         out_buf, out_max, out_size);
}

int avx_speedhq_encode_c99(int width, int height,
                           const uint8_t *uyvy_data,
                           uint8_t *out_buf, size_t out_max,
                           size_t *out_size)
{
    return avx_speedhq_encode_c99_ex(0x32514853, width, height, uyvy_data, NULL, out_buf, out_max, out_size);
}

/*
 * C99 Standalone Decoder: SpeedHQ -> UYVY + Alpha
 */
typedef struct {
    int                        width;
    int                        height;
    int                        mb_w;
    int                        mb_h;
    int                        num_slices;
    bool                       has_alpha;
    bool                       is_dct_alpha;
    uint8_t                   *uyvy_out;
    uint8_t                   *alpha_out;
    const avx_shq_quant_ctx_t *qctx;
    const uint8_t             *slice_ptrs[4];
    size_t                     slice_bytes[4];
} shq_dec_frame_ctx_t;

static inline void shq_store_uyvy_row(uint8_t *row, const uint8_t *cb, const uint8_t *cr,
                                      const uint8_t *y_left, const uint8_t *y_right, int r)
{
    int base = r * 8;
    for (int c = 0; c < 4; ++c) {
        row[c * 4 + 0] = cb[base + c];
        row[c * 4 + 1] = y_left[base + c * 2 + 0];
        row[c * 4 + 2] = cr[base + c];
        row[c * 4 + 3] = y_left[base + c * 2 + 1];
    }
    for (int c = 0; c < 4; ++c) {
        row[(c + 4) * 4 + 0] = cb[base + c + 4];
        row[(c + 4) * 4 + 1] = y_right[base + c * 2 + 0];
        row[(c + 4) * 4 + 2] = cr[base + c + 4];
        row[(c + 4) * 4 + 3] = y_right[base + c * 2 + 1];
    }
}

static void shq_decode_slice_fn(void *vctx, int slice)
{
    shq_dec_frame_ctx_t *fctx = (shq_dec_frame_ctx_t *)vctx;
    if (!fctx->slice_ptrs[slice]) return;

    avx_bitreader_t br;
    bitreader_init(&br, fctx->slice_ptrs[slice], fctx->slice_bytes[slice]);

    int mb_w = fctx->mb_w;
    int mb_h = fctx->mb_h;
    int num_slices = fctx->num_slices;
    int width = fctx->width;
    int height = fctx->height;
    size_t uyvy_stride = (size_t)width * 2;
    uint8_t *uyvy_out = fctx->uyvy_out;
    uint8_t *alpha_out = fctx->alpha_out;
    const avx_shq_quant_ctx_t *qctx = fctx->qctx;

    for (int mb_y = slice; mb_y < mb_h; mb_y += num_slices) {
        int last_dc[4] = { 1024, 1024, 1024, 1024 };
        uint8_t last_alpha[16];
        if (fctx->has_alpha && !fctx->is_dct_alpha) {
            memset(last_alpha, 255, sizeof(last_alpha));
        }
        int y_base = mb_y * 16;
        bool interior_y = (y_base + 15 < height);

        for (int mb_x = 0; mb_x < mb_w; ++mb_x) {
            int x_base = mb_x * 16;

            /* Decode 8 blocks: 4 Y, 2 Cb, 2 Cr */
            uint8_t dec_y0[64], dec_y1[64], dec_y2[64], dec_y3[64];
            uint8_t dec_cb0[64], dec_cb1[64], dec_cr0[64], dec_cr1[64];

            decode_single_block_c99(&br, &last_dc[0], 0, qctx, dec_y0);
            decode_single_block_c99(&br, &last_dc[0], 0, qctx, dec_y1);
            decode_single_block_c99(&br, &last_dc[0], 0, qctx, dec_y2);
            decode_single_block_c99(&br, &last_dc[0], 0, qctx, dec_y3);

            decode_single_block_c99(&br, &last_dc[1], 1, qctx, dec_cb0);
            decode_single_block_c99(&br, &last_dc[2], 2, qctx, dec_cr0);
            decode_single_block_c99(&br, &last_dc[1], 1, qctx, dec_cb1);
            decode_single_block_c99(&br, &last_dc[2], 2, qctx, dec_cr1);

            /* Reconstruct into packed UYVY */
            if (interior_y) {
                uint8_t *row_top = uyvy_out + (size_t)y_base * uyvy_stride + (size_t)x_base * 2;
                uint8_t *row_bot = row_top + 8 * uyvy_stride;
                for (int r = 0; r < 8; ++r) {
                    shq_store_uyvy_row(row_top, dec_cb0, dec_cr0, dec_y0, dec_y1, r);
                    shq_store_uyvy_row(row_bot, dec_cb1, dec_cr1, dec_y2, dec_y3, r);
                    row_top += uyvy_stride;
                    row_bot += uyvy_stride;
                }
            } else {
                for (int r = 0; r < 8; ++r) {
                    int y_top = y_base + r;
                    int y_bot = y_base + r + 8;
                    if (y_top < height) {
                        uint8_t *row_top = uyvy_out + (size_t)y_top * uyvy_stride + (size_t)x_base * 2;
                        shq_store_uyvy_row(row_top, dec_cb0, dec_cr0, dec_y0, dec_y1, r);
                    }
                    if (y_bot < height) {
                        uint8_t *row_bot = uyvy_out + (size_t)y_bot * uyvy_stride + (size_t)x_base * 2;
                        shq_store_uyvy_row(row_bot, dec_cb1, dec_cr1, dec_y2, dec_y3, r);
                    }
                }
            }

            /* Decode Alpha if present */
            if (fctx->has_alpha) {
                if (fctx->is_dct_alpha) {
                    uint8_t dec_a0[64], dec_a1[64], dec_a2[64], dec_a3[64];
                    decode_single_block_c99(&br, &last_dc[3], 0, qctx, dec_a0);
                    decode_single_block_c99(&br, &last_dc[3], 0, qctx, dec_a1);
                    decode_single_block_c99(&br, &last_dc[3], 0, qctx, dec_a2);
                    decode_single_block_c99(&br, &last_dc[3], 0, qctx, dec_a3);

                    if (alpha_out) {
                        for (int r = 0; r < 8; ++r) {
                            int y_top = y_base + r;
                            int y_bot = y_base + r + 8;
                            if (y_top < height) {
                                uint8_t *row_top = alpha_out + (size_t)y_top * width + x_base;
                                memcpy(row_top, dec_a0 + r * 8, 8);
                                memcpy(row_top + 8, dec_a1 + r * 8, 8);
                            }
                            if (y_bot < height) {
                                uint8_t *row_bot = alpha_out + (size_t)y_bot * width + x_base;
                                memcpy(row_bot, dec_a2 + r * 8, 8);
                                memcpy(row_bot + 8, dec_a3 + r * 8, 8);
                            }
                        }
                    }
                } else {
                    int rows_top = (height > y_base) ? (height - y_base) : 0;
                    if (rows_top > 8) rows_top = 8;
                    uint8_t *a_top = (alpha_out && rows_top > 0) ?
                        (alpha_out + (size_t)y_base * width + x_base) : NULL;
                    decode_alpha_block(&br, a_top, width, rows_top, last_alpha);

                    int rows_bot = (height > y_base + 8) ? (height - (y_base + 8)) : 0;
                    if (rows_bot > 8) rows_bot = 8;
                    uint8_t *a_bot = (alpha_out && rows_bot > 0) ?
                        (alpha_out + (size_t)(y_base + 8) * width + x_base) : NULL;
                    decode_alpha_block(&br, a_bot, width, rows_bot, last_alpha);
                }
            }
        }
    }
}

/*
 * Decodes SpeedHQ intra-frame bitstream into packed UYVY and optional alpha plane.
 * Reconstructs DC predictors per slice and uses O(1) LUT for high-frequency AC runs.
 */
int avx_speedhq_decode_c99_ex(uint32_t fourcc, int width, int height,
                              const uint8_t *shq_data, size_t shq_size,
                              uint8_t *uyvy_out, uint8_t *alpha_out,
                              bool *p_has_alpha)
{
    avx_speedhq_init();

    if (!shq_data || !uyvy_out || shq_size < 16 || width <= 0 || height <= 0 ||
        width > AVX_MAX_VIDEO_WIDTH || height > AVX_MAX_VIDEO_HEIGHT || width % 16 != 0) {
        return -1;
    }

    uint32_t tag = fourcc ? fourcc : 0x32514853;
    if (!avx_is_speedhq_fourcc(tag)) return -1;
    bool has_alpha = (tag == 0x31514853 || tag == 0x33514853 || tag == 0x35514853 ||
                      tag == 0x37514853 || tag == 0x39514853);
    if (p_has_alpha) *p_has_alpha = has_alpha;

    uint8_t quality = shq_data[0];
    int qscale = 100 - quality;
    if (qscale < 1) qscale = 1;

    uint32_t second_field_offset = (uint32_t)shq_data[1] |
                                   ((uint32_t)shq_data[2] << 8) |
                                   ((uint32_t)shq_data[3] << 16);
    if (second_field_offset >= shq_size && second_field_offset != 4) return -1;

    avx_shq_quant_ctx_t qctx;
    speedhq_prepare_quant_ctx(&qctx, qscale);

    size_t pos = 4;
    int mb_w = width / 16;
    int mb_h = (height + 15) / 16;
    int num_slices = (mb_h >= 4) ? 4 : 1;

    shq_dec_frame_ctx_t fctx;
    memset(&fctx, 0, sizeof(fctx));
    fctx.width = width;
    fctx.height = height;
    fctx.mb_w = mb_w;
    fctx.mb_h = mb_h;
    fctx.num_slices = num_slices;
    fctx.has_alpha = has_alpha;
    fctx.is_dct_alpha = (tag == 0x37514853 || tag == 0x39514853);
    fctx.uyvy_out = uyvy_out;
    fctx.alpha_out = alpha_out;
    fctx.qctx = &qctx;

    int valid_slices = 0;
    size_t total_slice_payload = 0;
    for (int slice = 0; slice < num_slices && pos < shq_size; ++slice) {
        if (pos + 3 > shq_size) break;
        uint32_t slice_len = (uint32_t)shq_data[pos] |
                             ((uint32_t)shq_data[pos + 1] << 8) |
                             ((uint32_t)shq_data[pos + 2] << 16);
        if (slice_len < 3 || pos + slice_len > shq_size) break;
        fctx.slice_ptrs[slice] = shq_data + pos + 3;
        fctx.slice_bytes[slice] = slice_len - 3;
        total_slice_payload += fctx.slice_bytes[slice];
        valid_slices++;
        pos += slice_len;
    }

    if (num_slices == 4 && valid_slices == 4 &&
        (size_t)width * (size_t)height >= 131072U && total_slice_payload >= 1024U) {
        shq_dispatch_slices(shq_decode_slice_fn, &fctx, 4);
    } else {
        for (int slice = 0; slice < valid_slices; ++slice) {
            shq_decode_slice_fn(&fctx, slice);
        }
    }

    if (!has_alpha && alpha_out) {
        memset(alpha_out, 255, (size_t)width * height);
    }

    return 0;
}

int avx_speedhq_decode_c99(int width, int height,
                           const uint8_t *shq_data, size_t shq_size,
                           uint8_t *uyvy_out)
{
    return avx_speedhq_decode_c99_ex(0x32514853, width, height, shq_data, shq_size, uyvy_out, NULL, NULL);
}

/*
 * Global Initialization and Dispatchers
 */
static bool s_initialized = false;
static avx_mutex_t s_init_lock = AVX_MUTEX_INITIALIZER;

OPENAVX_API bool avx_speedhq_init(void)
{
    avx_mutex_lock(&s_init_lock);
    if (!s_initialized) {
        speedhq_init_vlc_tables();
        s_initialized = true;
    }
    avx_mutex_unlock(&s_init_lock);
    return true;
}

bool avx_speedhq_has_acceleration(void)
{
    return avx_simd_get_caps() != AVX_SIMD_NONE;
}

int avx_speedhq_encode_ex(uint32_t fourcc, int width, int height,
                          const uint8_t *uyvy_data, const uint8_t *alpha_data,
                          uint8_t *out_buf, size_t out_max,
                          size_t *out_size)
{
    avx_speedhq_init();

    uint32_t tag = fourcc ? fourcc : (alpha_data ? 0x33514853 : 0x32514853);
    if (!avx_is_speedhq_fourcc(tag)) return -1;

    return avx_speedhq_encode_c99_ex(tag, width, height, uyvy_data, alpha_data, out_buf, out_max, out_size);
}

int avx_speedhq_decode_ex(uint32_t fourcc, int width, int height,
                          const uint8_t *shq_data, size_t shq_size,
                          uint8_t *uyvy_out, uint8_t *alpha_out,
                          bool *p_has_alpha)
{
    avx_speedhq_init();

    uint32_t tag = fourcc ? fourcc : 0x32514853;
    if (!avx_is_speedhq_fourcc(tag)) return -1;

    return avx_speedhq_decode_c99_ex(tag, width, height, shq_data, shq_size, uyvy_out, alpha_out, p_has_alpha);
}

int avx_speedhq_encode(int width, int height,
                       const uint8_t *uyvy_data,
                       uint8_t *out_buf, size_t out_max,
                       size_t *out_size)
{
    return avx_speedhq_encode_ex(0x32514853, width, height, uyvy_data, NULL, out_buf, out_max, out_size);
}

int avx_speedhq_decode(int width, int height,
                       const uint8_t *shq_data, size_t shq_size,
                       uint8_t *uyvy_out)
{
    return avx_speedhq_decode_ex(0x32514853, width, height, shq_data, shq_size, uyvy_out, NULL, NULL);
}

void avx_speedhq_destroy(void)
{
    shq_pool_shutdown_internal();
    avx_mutex_lock(&s_init_lock);
    s_initialized = false;
    avx_mutex_unlock(&s_init_lock);
}
