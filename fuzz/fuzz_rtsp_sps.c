/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_rtsp_sps.c - libFuzzer Target for H.264, HEVC & AV1 Header Parsing
 */

#include <stdint.h>
#include <stddef.h>
#include "avx_rtsp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size == 0) return 0;

    int width = 0, height = 0, profile = 0, level = 0;

    /* Fuzz H.264 SPS Exp-Golomb Bitstream Parser */
    avx_rtsp_parse_sps_h264(data, size, &width, &height, &profile, &level);

    /* Fuzz HEVC SPS Bitstream Parser */
    avx_rtsp_parse_sps_hevc(data, size, &width, &height, &profile, &level);

    /* Fuzz AV1 Sequence Header OBU Parser */
    avx_rtsp_parse_sequence_header_av1(data, size, &width, &height, &profile, &level);

    /* Fuzz Keyframe Inspection on all three codecs */
    avx_rtsp_is_keyframe(AVX_RTSP_CODEC_H264, data, size);
    avx_rtsp_is_keyframe(AVX_RTSP_CODEC_HEVC, data, size);
    avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, data, size);

    return 0;
}
