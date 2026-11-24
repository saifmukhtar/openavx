/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_fmp4_generator.c - libFuzzer Target for C99 fMP4 ISOBMFF Generator
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "openavx.h"
#include "avx_web.h"
#include "avx_rtsp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size < 8) return 0;

    int codec_sel = data[0] % 3;
    avx_rtsp_codec_e codec = (codec_sel == 0) ? AVX_RTSP_CODEC_H264 :
                           (codec_sel == 1) ? AVX_RTSP_CODEC_HEVC : AVX_RTSP_CODEC_AV1;

    uint32_t width = (uint32_t)(data[1] | (data[2] << 8));
    uint32_t height = (uint32_t)(data[3] | (data[4] << 8));

    size_t payload_len = size - 8;
    const uint8_t *payload = data + 8;

    /* Split payload into synthetic sps, pps, vps */
    size_t part1 = payload_len / 3;
    size_t part2 = payload_len / 3;
    size_t part3 = payload_len - part1 - part2;

    const uint8_t *sps = payload;
    const uint8_t *pps = payload + part1;
    const uint8_t *vps = payload + part1 + part2;

    /* 1. Fuzz Init Segment Generator */
    uint8_t init_buf[4096];
    size_t out_cap = (data[5] % 2 == 0) ? sizeof(init_buf) : (data[6] % 128); /* Test both full & undersized */
    avx_fmp4_generate_init_segment(
        codec, width, height,
        sps, part1,
        pps, part2,
        vps, part3,
        init_buf, out_cap
    );

    /* 2. Fuzz Media Fragment Generator */
    uint32_t seq = ((uint32_t)data[1]) | (((uint32_t)data[2]) << 8) | (((uint32_t)data[3]) << 16) | (((uint32_t)data[4]) << 24);
    uint64_t pts = (uint64_t)seq * 1500;
    bool is_keyframe = (data[7] & 1) != 0;

    uint8_t frag_buf[8192];
    size_t frag_cap = (data[7] & 2) ? sizeof(frag_buf) : (data[5] % 256);
    avx_fmp4_generate_media_fragment(
        seq, pts, 1500, is_keyframe,
        payload, payload_len,
        frag_buf, frag_cap
    );

    return 0;
}
