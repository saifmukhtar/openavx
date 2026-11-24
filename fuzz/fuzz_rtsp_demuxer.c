/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_rtsp_demuxer.c - libFuzzer Target for RTP & NAL Demuxer Engine
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "avx_rtsp.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size < 4) return 0;

    /* Select codec mode based on first byte */
    int codec_sel = data[0] % 3;
    avx_rtsp_codec_e codec = (codec_sel == 0) ? AVX_RTSP_CODEC_H264 :
                           (codec_sel == 1) ? AVX_RTSP_CODEC_HEVC : AVX_RTSP_CODEC_AV1;
    avx_rtsp_demuxer_t *demux = avx_rtsp_demuxer_create(codec);
    if (!demux) return 0;

    /* 1. Feed packet as raw RTP datagram (RFC 3550 / 6184 / 7798) */
    avx_rtsp_demuxer_feed_rtp(demux, data + 1, size - 1);

    avx_rtsp_frame_t frame;
    avx_rtsp_demuxer_get_frame(demux, &frame);

    /* 2. Feed packet as raw NAL unit */
    bool is_last = (data[0] & 2) != 0;
    uint32_t ts = (uint32_t)size * 90;
    avx_rtsp_demuxer_feed_nalu(demux, data + 1, size - 1, is_last, ts);

    avx_rtsp_demuxer_get_frame(demux, &frame);

    /* 3. Test parameter set injection with slice of remaining bytes */
    if (size > 16) {
        size_t part = (size - 1) / 3;
        avx_rtsp_demuxer_set_parameter_sets(
            demux, codec,
            data + 1, part,
            data + 1 + part, part,
            data + 1 + 2 * part, size - 1 - 2 * part
        );
    }

    avx_rtsp_demuxer_destroy(demux);
    return 0;
}
