/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_speedhq.c - libFuzzer Target for SpeedHQ Bitstream Decoding
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include "avx_internal.h"
#include "avx_speedhq.h"

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    avx_speedhq_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size < 6) return 0;

    /* Extract fuzzed FourCC across all 8 supported SpeedHQ types */
    static const uint32_t s_shq_fourccs[8] = {
        OAVX_FOURCC_VIDEO_TYPE_SHQ0,
        OAVX_FOURCC_VIDEO_TYPE_SHQ1,
        OAVX_FOURCC_VIDEO_TYPE_SHQ2,
        OAVX_FOURCC_VIDEO_TYPE_SHQ3,
        OAVX_FOURCC_VIDEO_TYPE_SHQ4,
        OAVX_FOURCC_VIDEO_TYPE_SHQ5,
        OAVX_FOURCC_VIDEO_TYPE_SHQ7,
        OAVX_FOURCC_VIDEO_TYPE_SHQ9
    };
    uint32_t fourcc = s_shq_fourccs[data[0] % 8];

    /* Extract fuzzed dimensions */
    uint16_t raw_w = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    uint16_t raw_h = (uint16_t)data[3] | ((uint16_t)data[4] << 8);

    /* Test both arbitrary and macroblock-aligned dimensions */
    int w = (int)(raw_w % 1921);
    int h = (int)(raw_h % 1081);

    if ((data[5] & 1) != 0) {
        w = (w / 16) * 16;
    }

    const uint8_t *payload = data + 6;
    size_t payload_size = size - 6;

    if (w > 0 && h > 0 && w <= AVX_MAX_VIDEO_WIDTH && h <= AVX_MAX_VIDEO_HEIGHT) {
        uint64_t out_u64 = (uint64_t)w * (uint64_t)h * 2ULL;
        uint64_t alpha_u64 = (uint64_t)w * (uint64_t)h;

        if (out_u64 <= 64ULL * 1024ULL * 1024ULL) {
            uint8_t *out_buf = (uint8_t *)malloc((size_t)out_u64);
            uint8_t *alpha_buf = (uint8_t *)malloc((size_t)alpha_u64);

            if (out_buf) {
                /* 1. Standalone pure C99 decoder */
                avx_speedhq_decode_c99(w, h, payload, payload_size, out_buf);

                /* 2. SIMD-dispatched decoder */
                avx_speedhq_decode(w, h, payload, payload_size, out_buf);

                /* 3. Extended decoders with alpha plane */
                if (alpha_buf) {
                    bool has_alpha = false;
                    avx_speedhq_decode_ex(fourcc, w, h, payload, payload_size, out_buf, alpha_buf, &has_alpha);
                    avx_speedhq_decode_c99_ex(fourcc, w, h, payload, payload_size, out_buf, alpha_buf, &has_alpha);
                }
                free(out_buf);
            }
            if (alpha_buf) {
                free(alpha_buf);
            }
        }
    } else {
        /* Edge cases: invalid or zero dimensions, NULL output buffers */
        avx_speedhq_decode(w, h, payload, payload_size, NULL);
        avx_speedhq_decode_c99(w, h, payload, payload_size, NULL);
    }

    return 0;
}
