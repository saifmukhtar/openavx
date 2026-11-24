/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_rtsp_packetizer.c - libFuzzer Target for RTP Packetizer & Base64 Encoder
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "avx_rtsp.h"

static void dummy_rtp_cb(const uint8_t *rtp_pkt, size_t rtp_len, void *user_data) {
    (void)rtp_pkt;
    (void)rtp_len;
    (void)user_data;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size < 4) return 0;

    /* 1. Fuzz Base64 Encoder */
    size_t b64_cap = ((size + 2) / 3) * 4 + 16;
    char *b64_buf = (char*)malloc(b64_cap);
    if (b64_buf) {
        avx_base64_encode(data, size, b64_buf, b64_cap);
        free(b64_buf);
    }

    /* 2. Fuzz RTP Packetizer */
    int codec_sel = data[0] % 3;
    avx_rtsp_codec_e codec = (codec_sel == 0) ? AVX_RTSP_CODEC_H264 :
                           (codec_sel == 1) ? AVX_RTSP_CODEC_HEVC : AVX_RTSP_CODEC_AV1;

    uint16_t seq = (uint16_t)(data[1] | (data[2] << 8));
    uint32_t ts = (uint32_t)(data[3] * 90000);
    uint32_t ssrc = 0x12345678;
    bool is_last = (data[0] & 0x08) != 0;

    avx_rtsp_rtp_packetize_nalu(
        codec,
        data + 4, size - 4,
        ts, &seq, ssrc,
        is_last,
        dummy_rtp_cb,
        NULL
    );

    return 0;
}
