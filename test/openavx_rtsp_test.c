/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_rtsp_test.c - Unit and Integration Test Suite for Native RFC 2326 RTP / RTSP Subsystem
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "avx_platform.h"

#include "openavx.h"
#include "avx_rtsp.h"

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s:%d: Assertion '%s' failed\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "[FAIL] %s:%d: Assertion '%s == %s' failed (%ld != %ld)\n", \
                __FILE__, __LINE__, #a, #b, (long)(a), (long)(b)); \
        exit(1); \
    } \
} while(0)

/* ========================================================================= */
/* TEST 1: H.264 SPS RESOLUTION PARSING                                      */
/* ========================================================================= */

static void test_h264_sps_parsing(void) {
    printf("[*] Testing H.264 SPS Bitstream Parsing (1080p, 720p)...\n");

    /* Genuine 1080p High Profile SPS: 1920x1080 */
    const uint8_t sps_1080p[] = {
        0x67, 0x64, 0x00, 0x29, 0xac, 0x2c, 0xa5, 0x01, 0xe0, 0x08,
        0x9f, 0x97, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00,
        0x00, 0x03, 0x03, 0x20, 0xf1, 0x83, 0x19, 0x60
    };

    int width = 0, height = 0, profile = 0, level = 0;
    bool ok = avx_rtsp_parse_sps_h264(sps_1080p, sizeof(sps_1080p), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 1920);
    ASSERT_EQ(height, 1080);
    ASSERT_EQ(profile, 100); /* High Profile */
    ASSERT_EQ(level, 41);    /* Level 4.1 */

    /* Genuine 720p SPS: 1280x720 */
    const uint8_t sps_720p[] = {
        103, 244, 0, 31, 145, 155, 40, 10, 0, 183, 96, 34,
        0, 0, 3, 0, 2, 0, 0, 3, 0, 120, 30, 48, 99, 44
    };
    ok = avx_rtsp_parse_sps_h264(sps_720p, sizeof(sps_720p), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 1280);
    ASSERT_EQ(height, 720);
    ASSERT_EQ(profile, 244);
    ASSERT_EQ(level, 31);

    printf("  [+] H.264 SPS Parsing: PASSED\n");
}

/* ========================================================================= */
/* TEST 2: HEVC SPS RESOLUTION PARSING                                       */
/* ========================================================================= */

static void test_hevc_sps_parsing(void) {
    printf("[*] Testing HEVC (H.265) SPS Bitstream Parsing (1080p)...\n");

    /* Genuine 1080p HEVC SPS (NAL type 33) */
    const uint8_t sps_hevc[] = {
        66, 1, 1, 4, 8, 0, 0, 3, 0, 158, 8, 0, 0, 3, 0, 0, 120, 144,
        0, 120, 16, 2, 28, 178, 202, 205, 36, 153, 94, 2, 220, 8, 8,
        0, 16, 0, 0, 3, 0, 16, 0, 0, 3, 1, 224, 128
    };

    int width = 0, height = 0, profile = 0, level = 0;
    bool ok = avx_rtsp_parse_sps_hevc(sps_hevc, sizeof(sps_hevc), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 1920);
    ASSERT_EQ(height, 1080);
    ASSERT_EQ(profile, 4); /* Range Extensions Profile */

    printf("  [+] HEVC SPS Parsing: PASSED\n");
}

/* ========================================================================= */
/* TEST 3: AV1 SEQUENCE HEADER OBU PARSING                                   */
/* ========================================================================= */

static void test_av1_sequence_header_parsing(void) {
    printf("[*] Testing AV1 OBU Sequence Header Bitstream Parsing (1080p, 720p, 4K)...\n");

    /* Genuine 1080p AV1 Sequence Header OBU (Profile 0 Main, Level 4.0 / idx 8) */
    const uint8_t obu_1080p[] = {
        10, 11, 0, 0, 0, 66, 171, 191, 195, 113, 171, 230, 1
    };

    int width = 0, height = 0, profile = 0, level = 0;
    bool ok = avx_rtsp_parse_sequence_header_av1(obu_1080p, sizeof(obu_1080p), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 1920);
    ASSERT_EQ(height, 1080);
    ASSERT_EQ(profile, 0); /* Main Profile */
    ASSERT_EQ(level, 8);   /* Level 4.0 (idx 8) */

    /* Genuine 720p AV1 Sequence Header OBU */
    const uint8_t obu_720p[] = {
        10, 11, 0, 0, 0, 45, 76, 255, 179, 198, 175, 152, 4
    };
    ok = avx_rtsp_parse_sequence_header_av1(obu_720p, sizeof(obu_720p), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 1280);
    ASSERT_EQ(height, 720);
    ASSERT_EQ(profile, 0);
    ASSERT_EQ(level, 5);

    /* Genuine 4K (3840x2160) AV1 Sequence Header OBU */
    const uint8_t obu_4k[] = {
        10, 12, 0, 0, 0, 98, 239, 191, 225, 188, 106, 249, 128, 64
    };
    ok = avx_rtsp_parse_sequence_header_av1(obu_4k, sizeof(obu_4k), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 3840);
    ASSERT_EQ(height, 2160);
    ASSERT_EQ(profile, 0);
    ASSERT_EQ(level, 12);

    /* Test with Temporal Delimiter prefix (OBU type 2, size 0: [18, 0, 10, ...]) */
    const uint8_t obu_with_td[] = {
        18, 0, 10, 11, 0, 0, 0, 66, 171, 191, 195, 113, 171, 230, 1
    };
    width = height = profile = level = 0;
    ok = avx_rtsp_parse_sequence_header_av1(obu_with_td, sizeof(obu_with_td), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 1920);
    ASSERT_EQ(height, 1080);

    /* Test with 4-byte Annex-B start code prefix (00 00 00 01) */
    const uint8_t obu_with_sc4[] = {
        0, 0, 0, 1, 10, 11, 0, 0, 0, 66, 171, 191, 195, 113, 171, 230, 1
    };
    width = height = profile = level = 0;
    ok = avx_rtsp_parse_sequence_header_av1(obu_with_sc4, sizeof(obu_with_sc4), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 1920);
    ASSERT_EQ(height, 1080);

    /* Test with 3-byte Annex-B start code prefix (00 00 01) */
    const uint8_t obu_with_sc3[] = {
        0, 0, 1, 10, 11, 0, 0, 0, 66, 171, 191, 195, 113, 171, 230, 1
    };
    width = height = profile = level = 0;
    ok = avx_rtsp_parse_sequence_header_av1(obu_with_sc3, sizeof(obu_with_sc3), &width, &height, &profile, &level);
    ASSERT_TRUE(ok);
    ASSERT_EQ(width, 1920);
    ASSERT_EQ(height, 1080);

    /* Test corrupted / truncated / invalid inputs (memory safety) */
    ASSERT_TRUE(!avx_rtsp_parse_sequence_header_av1(NULL, 10, &width, &height, NULL, NULL));
    ASSERT_TRUE(!avx_rtsp_parse_sequence_header_av1(obu_1080p, 3, &width, &height, NULL, NULL));
    uint8_t invalid_obu[13];
    memcpy(invalid_obu, obu_1080p, sizeof(obu_1080p));
    invalid_obu[0] = 0x80; /* forbidden bit set */
    ASSERT_TRUE(!avx_rtsp_parse_sequence_header_av1(invalid_obu, sizeof(invalid_obu), &width, &height, NULL, NULL));

    printf("  [+] AV1 Sequence Header Parsing: PASSED\n");
}

/* ========================================================================= */
/* TEST 4: AV1 KEYFRAME & INTER-FRAME DETECTION                              */
/* ========================================================================= */

static void test_av1_keyframe_detection(void) {
    printf("[*] Testing AV1 Keyframe & Delta Frame Inspection...\n");

    /* 1. Sequence Header OBU -> Must be keyframe/config */
    const uint8_t seq_obu[] = { 10, 11, 0, 0, 0, 66, 171, 191, 195, 113, 171, 230, 1 };
    ASSERT_TRUE(avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, seq_obu, sizeof(seq_obu)));

    /* 2. Frame OBU with KEY_FRAME (frame_type == 0, show_existing == 0) */
    const uint8_t frame_key[] = { 0x32, 0x04, 0x14, 0x00, 0x00, 0x00 };
    ASSERT_TRUE(avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, frame_key, sizeof(frame_key)));

    /* 3. Frame OBU with INTER_FRAME (frame_type == 1, show_existing == 0) */
    const uint8_t frame_inter[] = { 0x32, 0x04, 0x28, 0x00, 0x00, 0x00 };
    ASSERT_TRUE(!avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, frame_inter, sizeof(frame_inter)));

    /* 4. Frame Header OBU with INTRA_ONLY_FRAME (frame_type == 2) */
    const uint8_t frame_intra_only[] = { 0x1A, 0x04, 0x40, 0x00, 0x00, 0x00 };
    ASSERT_TRUE(avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, frame_intra_only, sizeof(frame_intra_only)));

    /* 5. Frame Header OBU with SWITCH_FRAME (frame_type == 3) */
    const uint8_t frame_switch[] = { 0x1A, 0x04, 0x60, 0x00, 0x00, 0x00 };
    ASSERT_TRUE(!avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, frame_switch, sizeof(frame_switch)));

    /* 6. Temporal Delimiter followed by Keyframe Frame OBU */
    const uint8_t td_and_key[] = { 18, 0, 0x32, 0x04, 0x14, 0x00, 0x00, 0x00 };
    ASSERT_TRUE(avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, td_and_key, sizeof(td_and_key)));

    /* 7. Temporal Delimiter followed by Inter Frame OBU */
    const uint8_t td_and_inter[] = { 18, 0, 0x32, 0x04, 0x28, 0x00, 0x00, 0x00 };
    ASSERT_TRUE(!avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, td_and_inter, sizeof(td_and_inter)));

    /* 8. Show existing frame (show_existing_frame == 1) */
    const uint8_t show_existing[] = { 0x1A, 0x04, 0x88, 0x00, 0x00, 0x00 };
    ASSERT_TRUE(!avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, show_existing, sizeof(show_existing)));

    /* 9. Truncated & invalid */
    ASSERT_TRUE(!avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, NULL, 10));
    ASSERT_TRUE(!avx_rtsp_is_keyframe(AVX_RTSP_CODEC_AV1, frame_key, 0));

    printf("  [+] AV1 Keyframe Inspection: PASSED\n");
}

/* ========================================================================= */
/* TEST 5: AV1 DEMUXER INTEGRATION & ORPHAN KEYFRAME REPAIR                  */
/* ========================================================================= */

static void test_av1_demuxer_integration(void) {
    printf("[*] Testing AV1 Demuxer Integration & Parameter Ingestion...\n");

    const uint8_t seq_1080p[] = { 10, 11, 0, 0, 0, 66, 171, 191, 195, 113, 171, 230, 1 };
    const uint8_t frame_key[]   = { 0x32, 0x04, 0x14, 0xAA, 0xBB, 0xCC };
    const uint8_t frame_inter[] = { 0x32, 0x04, 0x28, 0x11, 0x22, 0x33 };

    avx_rtsp_demuxer_t *demux = avx_rtsp_demuxer_create(AVX_RTSP_CODEC_AV1);
    ASSERT_TRUE(demux != NULL);

    /* 1. Feed Sequence Header OBU as standalone NALU */
    bool ready = avx_rtsp_demuxer_feed_nalu(demux, seq_1080p, sizeof(seq_1080p), false, 90000);
    ASSERT_TRUE(!ready);

    /* 2. Feed Keyframe OBU with is_last_slice = true */
    ready = avx_rtsp_demuxer_feed_nalu(demux, frame_key, sizeof(frame_key), true, 90000);
    ASSERT_TRUE(ready);

    avx_rtsp_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    bool got = avx_rtsp_demuxer_get_frame(demux, &frame);
    ASSERT_TRUE(got);
    ASSERT_EQ(frame.codec, AVX_RTSP_CODEC_AV1);
    ASSERT_EQ(frame.fourcc, OAVX_FOURCC_VIDEO_TYPE_AV01);
    ASSERT_EQ(frame.frame_type, AVX_RTSP_FRAME_TYPE_KEYFRAME);
    ASSERT_EQ(frame.width, 1920);
    ASSERT_EQ(frame.height, 1080);
    ASSERT_EQ(frame.rtp_timestamp, 90000);

    /* 3. Feed Inter Frame OBU at next timestamp */
    ready = avx_rtsp_demuxer_feed_nalu(demux, frame_inter, sizeof(frame_inter), true, 93000);
    ASSERT_TRUE(ready);

    memset(&frame, 0, sizeof(frame));
    got = avx_rtsp_demuxer_get_frame(demux, &frame);
    ASSERT_TRUE(got);
    ASSERT_EQ(frame.codec, AVX_RTSP_CODEC_AV1);
    ASSERT_EQ(frame.frame_type, AVX_RTSP_FRAME_TYPE_DELTA);
    ASSERT_EQ(frame.rtp_timestamp, 93000);

    /* 4. Test Orphan Keyframe Auto-Repair with Cached AV1 Sequence Header */
    avx_rtsp_demuxer_reset(demux);
    bool set_ok = avx_rtsp_demuxer_set_parameter_sets(demux, AVX_RTSP_CODEC_AV1, seq_1080p, sizeof(seq_1080p), NULL, 0, NULL, 0);
    ASSERT_TRUE(set_ok);

    /* Feed orphan keyframe that does NOT contain Sequence Header */
    ready = avx_rtsp_demuxer_feed_nalu(demux, frame_key, sizeof(frame_key), true, 96000);
    ASSERT_TRUE(ready);

    memset(&frame, 0, sizeof(frame));
    got = avx_rtsp_demuxer_get_frame(demux, &frame);
    ASSERT_TRUE(got);
    ASSERT_EQ(frame.frame_type, AVX_RTSP_FRAME_TYPE_KEYFRAME);
    ASSERT_EQ(frame.width, 1920);
    ASSERT_EQ(frame.height, 1080);
    /* Verify Sequence Header was automatically prepended */
    ASSERT_TRUE(frame.size > sizeof(seq_1080p) + sizeof(frame_key));
    /* Check for 00 00 00 01 start code followed by seq_1080p */
    ASSERT_EQ(frame.data[0], 0);
    ASSERT_EQ(frame.data[1], 0);
    ASSERT_EQ(frame.data[2], 0);
    ASSERT_EQ(frame.data[3], 1);
    ASSERT_TRUE(memcmp(frame.data + 4, seq_1080p, sizeof(seq_1080p)) == 0);

    avx_rtsp_demuxer_destroy(demux);
    printf("  [+] AV1 Demuxer Integration & Repair: PASSED\n");
}

/* ========================================================================= */
/* TEST 3: RTP DEPACKETIZER (SINGLE NALU, STAP-A, FU-A FRAGMENTATION)        */
/* ========================================================================= */

static void test_rtp_h264_depacketization(void) {
    printf("[*] Testing RTP H.264 Depacketization (RFC 6184)...\n");

    avx_rtsp_demuxer_t *demux = avx_rtsp_demuxer_create(AVX_RTSP_CODEC_H264);
    ASSERT_TRUE(demux != NULL);

    /* 3A: STAP-A Packet carrying SPS (type 7) and PPS (type 8) */
    const uint8_t sps_nal[] = {
        0x67, 0x64, 0x00, 0x29, 0xac, 0x2c, 0xa5, 0x01, 0xe0, 0x08,
        0x9f, 0x97, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00,
        0x00, 0x03, 0x03, 0x20, 0xf1, 0x83, 0x19, 0x60
    };
    const uint8_t pps_nal[] = { 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0 };

    /* Construct STAP-A packet with 12-byte RTP header */
    uint8_t stap_pkt[256];
    size_t pos = 0;
    /* RTP Header (V=2, P=0, X=0, CC=0, M=0, PT=96, Seq=100, TS=90000, SSRC=0x12345678) */
    stap_pkt[pos++] = 0x80;
    stap_pkt[pos++] = 96; /* PT 96, Marker=0 */
    stap_pkt[pos++] = 0x00; stap_pkt[pos++] = 100; /* Seq 100 */
    stap_pkt[pos++] = 0x00; stap_pkt[pos++] = 0x01; stap_pkt[pos++] = 0x5F; stap_pkt[pos++] = 0x90; /* TS 90000 */
    stap_pkt[pos++] = 0x12; stap_pkt[pos++] = 0x34; stap_pkt[pos++] = 0x56; stap_pkt[pos++] = 0x78; /* SSRC */

    /* STAP-A NAL header: NRI=3, Type=24 (0x78) */
    stap_pkt[pos++] = 0x78;
    /* NALU 1: SPS (2-byte length + payload) */
    stap_pkt[pos++] = (uint8_t)(sizeof(sps_nal) >> 8);
    stap_pkt[pos++] = (uint8_t)(sizeof(sps_nal) & 0xFF);
    memcpy(stap_pkt + pos, sps_nal, sizeof(sps_nal));
    pos += sizeof(sps_nal);
    /* NALU 2: PPS */
    stap_pkt[pos++] = (uint8_t)(sizeof(pps_nal) >> 8);
    stap_pkt[pos++] = (uint8_t)(sizeof(pps_nal) & 0xFF);
    memcpy(stap_pkt + pos, pps_nal, sizeof(pps_nal));
    pos += sizeof(pps_nal);

    bool frame_done = avx_rtsp_demuxer_feed_rtp(demux, stap_pkt, pos);
    ASSERT_TRUE(!frame_done); /* Parameter sets alone do not mark frame end */

    /* 3B: FU-A Fragmented IDR Keyframe (NAL Type 5, 3000 bytes payload) */
    size_t idr_len = 3000;
    uint8_t *orig_idr = (uint8_t*)malloc(idr_len);
    orig_idr[0] = 0x65; /* NRI=3, Type=5 (IDR) */
    for (size_t i = 1; i < idr_len; ++i) {
        orig_idr[i] = (uint8_t)(i & 0xFF);
    }

    /* Split across 3 FU-A packets: Slice 1 (Start), Slice 2 (Cont), Slice 3 (End with M=1) */
    size_t slice_payload = 1000;
    for (int frag = 0; frag < 3; ++frag) {
        uint8_t fu_pkt[1500];
        size_t p = 0;
        fu_pkt[p++] = 0x80;
        fu_pkt[p++] = (frag == 2) ? (96 | 0x80) : 96; /* Marker bit set on final frag */
        fu_pkt[p++] = 0x00; fu_pkt[p++] = (uint8_t)(101 + frag);
        fu_pkt[p++] = 0x00; fu_pkt[p++] = 0x01; fu_pkt[p++] = 0x5F; fu_pkt[p++] = 0x90; /* Same TS */
        fu_pkt[p++] = 0x12; fu_pkt[p++] = 0x34; fu_pkt[p++] = 0x56; fu_pkt[p++] = 0x78;

        /* FU indicator: NRI=3, Type=28 (0x7C) */
        fu_pkt[p++] = 0x7C;
        /* FU header */
        uint8_t fu_hdr = 5; /* original NAL type */
        if (frag == 0) fu_hdr |= 0x80; /* Start bit S=1 */
        if (frag == 2) fu_hdr |= 0x40; /* End bit E=1 */
        fu_pkt[p++] = fu_hdr;

        size_t offset_in_idr = 1 + frag * slice_payload;
        size_t copy_len = (frag < 2) ? slice_payload : (idr_len - 1 - 2 * slice_payload);
        memcpy(fu_pkt + p, orig_idr + offset_in_idr, copy_len);
        p += copy_len;

        bool ready = avx_rtsp_demuxer_feed_rtp(demux, fu_pkt, p);
        if (frag < 2) {
            ASSERT_TRUE(!ready);
        } else {
            ASSERT_TRUE(ready);
        }
    }

    /* Verify reassembled frame */
    avx_rtsp_frame_t frame;
    bool ok = avx_rtsp_demuxer_get_frame(demux, &frame);
    ASSERT_TRUE(ok);
    ASSERT_EQ(frame.codec, AVX_RTSP_CODEC_H264);
    ASSERT_EQ(frame.frame_type, AVX_RTSP_FRAME_TYPE_KEYFRAME);
    ASSERT_EQ(frame.width, 1920);
    ASSERT_EQ(frame.height, 1080);
    ASSERT_EQ(frame.fourcc, OAVX_FOURCC_VIDEO_TYPE_H264);
    ASSERT_TRUE(frame.size >= idr_len + 4);

    /* Verify Annex-B start codes and IDR content */
    const uint8_t *ptr = frame.data;
    /* Find IDR in frame: 00 00 00 01 65 */
    const uint8_t sc_idr[5] = { 0x00, 0x00, 0x00, 0x01, 0x65 };
    bool found_idr = false;
    for (size_t i = 0; i < frame.size - 5; ++i) {
        if (memcmp(ptr + i, sc_idr, 5) == 0) {
            found_idr = true;
            /* Verify payload */
            ASSERT_EQ(memcmp(ptr + i + 4, orig_idr, 100), 0);
            break;
        }
    }
    ASSERT_TRUE(found_idr);

    free(orig_idr);
    avx_rtsp_demuxer_destroy(demux);
    printf("  [+] RTP H.264 Depacketizer (STAP-A & FU-A): PASSED\n");
}

/* ========================================================================= */
/* TEST 3B: RTP HEVC DEPACKETIZER (RFC 7798 AP & FU)                         */
/* ========================================================================= */

static void test_rtp_hevc_depacketization(void) {
    printf("[*] Testing RTP HEVC Depacketization (RFC 7798 AP & FU)...\n");

    avx_rtsp_demuxer_t *demux = avx_rtsp_demuxer_create(AVX_RTSP_CODEC_HEVC);
    ASSERT_TRUE(demux != NULL);

    /* 1. AP Packet carrying VPS (type 32) and SPS (type 33) */
    uint8_t vps_nal[16] = { 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00 };
    uint8_t sps_nal[16] = { 0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9E, 0x08, 0x00, 0x00, 0x03, 0x00, 0x00 };

    uint8_t ap_pkt[256];
    size_t pos = 0;
    ap_pkt[pos++] = 0x80;
    ap_pkt[pos++] = 97; /* PT 97, M=0 */
    ap_pkt[pos++] = 0x00; ap_pkt[pos++] = 200;
    ap_pkt[pos++] = 0x00; ap_pkt[pos++] = 0x02; ap_pkt[pos++] = 0xBF; ap_pkt[pos++] = 0x20;
    ap_pkt[pos++] = 0x55; ap_pkt[pos++] = 0x66; ap_pkt[pos++] = 0x77; ap_pkt[pos++] = 0x88;

    /* HEVC AP NAL Header: Type 48 (48 << 1 = 96 = 0x60), TID=1 */
    ap_pkt[pos++] = 0x60;
    ap_pkt[pos++] = 0x01;

    /* NALU 1: VPS */
    ap_pkt[pos++] = (uint8_t)(sizeof(vps_nal) >> 8);
    ap_pkt[pos++] = (uint8_t)(sizeof(vps_nal) & 0xFF);
    memcpy(ap_pkt + pos, vps_nal, sizeof(vps_nal));
    pos += sizeof(vps_nal);

    /* NALU 2: SPS */
    ap_pkt[pos++] = (uint8_t)(sizeof(sps_nal) >> 8);
    ap_pkt[pos++] = (uint8_t)(sizeof(sps_nal) & 0xFF);
    memcpy(ap_pkt + pos, sps_nal, sizeof(sps_nal));
    pos += sizeof(sps_nal);

    bool ready = avx_rtsp_demuxer_feed_rtp(demux, ap_pkt, pos);
    ASSERT_TRUE(!ready);

    /* 2. HEVC FU Packet carrying fragmented IDR (type 19) */
    size_t hevc_idr_len = 2000;
    uint8_t *hevc_idr = (uint8_t*)malloc(hevc_idr_len);
    hevc_idr[0] = (19 << 1); /* Type 19 = IDR_W_RADL (0x26) */
    hevc_idr[1] = 0x01;      /* TID = 1 */
    for (size_t i = 2; i < hevc_idr_len; ++i) {
        hevc_idr[i] = (uint8_t)(i ^ 0x5A);
    }

    /* 2 slices: Start and End with M=1 */
    size_t half = 1000;
    for (int frag = 0; frag < 2; ++frag) {
        uint8_t fu_pkt[1200];
        size_t p = 0;
        fu_pkt[p++] = 0x80;
        fu_pkt[p++] = (frag == 1) ? (97 | 0x80) : 97; /* Marker bit set on frag 1 */
        fu_pkt[p++] = 0x00; fu_pkt[p++] = (uint8_t)(201 + frag);
        fu_pkt[p++] = 0x00; fu_pkt[p++] = 0x02; fu_pkt[p++] = 0xBF; fu_pkt[p++] = 0x20;
        fu_pkt[p++] = 0x55; fu_pkt[p++] = 0x66; fu_pkt[p++] = 0x77; fu_pkt[p++] = 0x88;

        /* HEVC FU Payload Header: Type 49 (49 << 1 = 98 = 0x62), TID=1 */
        fu_pkt[p++] = 0x62;
        fu_pkt[p++] = 0x01;
        /* FU Header: S(1) | E(1) | Type(6) */
        uint8_t fu_hdr = 19; /* Type 19 */
        if (frag == 0) fu_hdr |= 0x80; /* S=1 */
        if (frag == 1) fu_hdr |= 0x40; /* E=1 */
        fu_pkt[p++] = fu_hdr;

        size_t offset_in_nalu = (frag == 0) ? 2 : (2 + half);
        size_t copy_len = (frag == 0) ? half : (hevc_idr_len - 2 - half);
        memcpy(fu_pkt + p, hevc_idr + offset_in_nalu, copy_len);
        p += copy_len;

        bool r = avx_rtsp_demuxer_feed_rtp(demux, fu_pkt, p);
        if (frag == 0) {
            ASSERT_TRUE(!r);
        } else {
            ASSERT_TRUE(r);
        }
    }

    avx_rtsp_frame_t frame;
    bool ok = avx_rtsp_demuxer_get_frame(demux, &frame);
    ASSERT_TRUE(ok);
    ASSERT_EQ(frame.codec, AVX_RTSP_CODEC_HEVC);
    ASSERT_EQ(frame.frame_type, AVX_RTSP_FRAME_TYPE_KEYFRAME);
    ASSERT_EQ(frame.fourcc, OAVX_FOURCC_VIDEO_TYPE_HEVC);
    ASSERT_TRUE(frame.size >= hevc_idr_len);

    free(hevc_idr);
    avx_rtsp_demuxer_destroy(demux);
    printf("  [+] RTP HEVC Depacketizer (AP & FU): PASSED\n");
}

/* ========================================================================= */
/* TEST 4: ORPHAN IDR PARAMETER SET CACHE REPAIR                             */
/* ========================================================================= */

static void test_orphan_idr_repair(void) {
    printf("[*] Testing Orphan Keyframe Parameter Set Cache Auto-Repair...\n");

    avx_rtsp_demuxer_t *demux = avx_rtsp_demuxer_create(AVX_RTSP_CODEC_H264);
    ASSERT_TRUE(demux != NULL);

    /* Inject out-of-band SPS & PPS (e.g. from RTSP SDP) */
    const uint8_t dummy_sps[] = { 0x67, 0x64, 0x00, 0x29, 0xAC, 0x2C, 0xA5, 0x01, 0xE0, 0x08, 0x9F, 0x97 };
    const uint8_t dummy_pps[] = { 0x68, 0xEB, 0xE3, 0xCB };
    avx_rtsp_demuxer_set_parameter_sets(demux, AVX_RTSP_CODEC_H264, dummy_sps, sizeof(dummy_sps), dummy_pps, sizeof(dummy_pps), NULL, 0);

    /* Feed an IDR NALU that arrives completely alone without SPS/PPS */
    const uint8_t raw_idr[] = { 0x65, 0x88, 0x84, 0x00, 0x11, 0x22, 0x33, 0x44 };
    bool ready = avx_rtsp_demuxer_feed_nalu(demux, raw_idr, sizeof(raw_idr), true, 1000);
    ASSERT_TRUE(ready);

    avx_rtsp_frame_t frame;
    bool ok = avx_rtsp_demuxer_get_frame(demux, &frame);
    ASSERT_TRUE(ok);
    ASSERT_EQ(frame.frame_type, AVX_RTSP_FRAME_TYPE_KEYFRAME);

    /* Verify that the demuxer automatically prepended SPS and PPS into the frame output */
    /* Check for 00 00 00 01 67 */
    ASSERT_EQ(frame.data[0], 0x00);
    ASSERT_EQ(frame.data[1], 0x00);
    ASSERT_EQ(frame.data[2], 0x00);
    ASSERT_EQ(frame.data[3], 0x01);
    ASSERT_EQ(frame.data[4], 0x67); /* SPS */

    avx_rtsp_demuxer_destroy(demux);
    printf("  [+] Orphan IDR Parameter Set Auto-Repair: PASSED\n");
}

/* ========================================================================= */
/* TEST 5: RTSP CLIENT MOCK SESSION (RFC 2326 INTERLEAVED MODE)              */
/* ========================================================================= */

typedef struct {
    int server_fd;
    uint16_t port;
    volatile bool running;
    pthread_t thread;
    bool pli_received;
} mock_rtsp_server_t;

static void* mock_rtsp_server_thread(void *arg) {
    mock_rtsp_server_t *srv = (mock_rtsp_server_t*)arg;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(srv->server_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) return NULL;

    char buf[4096];
    while (srv->running) {
        struct pollfd pfd = { client_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 100) <= 0) continue;

        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        if (strstr(buf, "OPTIONS")) {
            const char *resp = "RTSP/1.0 200 OK\r\nCSeq: 1\r\nPublic: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY\r\n\r\n";
            send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
        } else if (strstr(buf, "DESCRIBE")) {
            const char *sdp =
                "v=0\r\n"
                "o=- 1600000000 1 IN IP4 127.0.0.1\r\n"
                "s=openavx RTSP Camera\r\n"
                "t=0 0\r\n"
                "m=video 0 RTP/AVP 96\r\n"
                "a=rtpmap:96 H264/90000\r\n"
                "a=control:trackID=1\r\n";
            char resp[1024];
            snprintf(resp, sizeof(resp),
                "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
                (int)strlen(sdp), sdp);
            send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
        } else if (strstr(buf, "SETUP")) {
            const char *resp =
                "RTSP/1.0 200 OK\r\nCSeq: 3\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nSession: 98765432\r\n\r\n";
            send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
        } else if (strstr(buf, "PLAY")) {
            const char *resp = "RTSP/1.0 200 OK\r\nCSeq: 4\r\nSession: 98765432\r\n\r\n";
            send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);

            /* Send 1 Interleaved RTP Video Packet on Channel 0: '$' (0x24) */
            uint8_t rtp_video[32] = {
                0x24, 0x00, 0x00, 0x10, /* '$', channel 0, len 16 */
                0x80, 0xE0, 0x00, 0x01, /* V=2, M=1, PT=96, Seq=1 */
                0x00, 0x01, 0x5F, 0x90, /* TS 90000 */
                0x12, 0x34, 0x56, 0x78, /* SSRC */
                0x65, 0x88, 0x84, 0x00  /* NALU payload (IDR slice) */
            };
            send(client_fd, rtp_video, sizeof(rtp_video), MSG_NOSIGNAL);
        } else if ((uint8_t)buf[0] == 0x24 && (uint8_t)buf[1] == 0x01) {
            /* Received RTCP packet on channel 1 (PLI Keyframe request) */
            srv->pli_received = true;
        } else if (strstr(buf, "TEARDOWN")) {
            const char *resp = "RTSP/1.0 200 OK\r\nCSeq: 5\r\n\r\n";
            send(client_fd, resp, strlen(resp), MSG_NOSIGNAL);
            break;
        }
    }

    close(client_fd);
    return NULL;
}

static void test_rtsp_client(void) {
    printf("[*] Testing RTSP 1.0 Client Interleaved Session (RFC 2326)...\n");

    mock_rtsp_server_t srv = {0};
    srv.running = true;
    srv.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(srv.server_fd >= 0);

    int opt = 1;
    setsockopt(srv.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0; /* ephemeral port */
    ASSERT_TRUE(bind(srv.server_fd, (struct sockaddr*)&sin, sizeof(sin)) == 0);
    ASSERT_TRUE(listen(srv.server_fd, 1) == 0);

    socklen_t slen = sizeof(sin);
    ASSERT_TRUE(getsockname(srv.server_fd, (struct sockaddr*)&sin, &slen) == 0);
    srv.port = ntohs(sin.sin_port);

    pthread_create(&srv.thread, NULL, mock_rtsp_server_thread, &srv);

    char rtsp_url[128];
    snprintf(rtsp_url, sizeof(rtsp_url), "rtsp://127.0.0.1:%d/live/ch0", srv.port);

    avx_rtsp_t *client = avx_rtsp_connect(rtsp_url, 1000);
    ASSERT_TRUE(client != NULL);

    /* Test reading interleaved RTP frame */
    avx_rtsp_demuxer_t *demux = avx_rtsp_demuxer_create(AVX_RTSP_CODEC_H264);
    avx_rtsp_frame_t frame = {0};
    int res = avx_rtsp_read_frame(client, demux, &frame, 500);
    ASSERT_EQ(res, 1);
    ASSERT_EQ(frame.codec, AVX_RTSP_CODEC_H264);

    /* Test sending RTCP PLI keyframe request */
    bool pli_sent = avx_rtsp_send_keyframe_request(client);
    ASSERT_TRUE(pli_sent);
    struct timespec ts = { 0, 50000000 };
    nanosleep(&ts, NULL);
    ASSERT_TRUE(srv.pli_received);

    avx_rtsp_close(client);
    avx_rtsp_demuxer_destroy(demux);

    srv.running = false;
    pthread_join(srv.thread, NULL);
    close(srv.server_fd);

    printf("  [+] RTSP 1.0 Client Interleaved Session & PLI: PASSED\n");
}

/* ========================================================================= */
/* TEST 6: COMPRESSED RECEIVER INGESTION & LOOPBACK PASSTHROUGH               */
/* ========================================================================= */

static void test_compressed_recv_pipeline(void) {
    printf("[*] Testing H.264 & HEVC Compressed Passthrough Loopback...\n");

    oavx_context_t *ctx = oavx_context_create();
    ASSERT_TRUE(ctx != NULL);

    oavx_stream_config_t tx_cfg = {
        .name = "HX_PASSTHROUGH_SENDER",
        .groups = "public",
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &tx_cfg);
    ASSERT_TRUE(tx != NULL);

    const oavx_endpoint_t *ep = oavx_stream_get_endpoint(tx);
    ASSERT_TRUE(ep != NULL && ep->address != NULL);

    oavx_stream_config_t rx_cfg = {
        .name = "HX_PASSTHROUGH_RECEIVER",
        .endpoint = ep->address,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_COMPRESSED_V1,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    ASSERT_TRUE(rx != NULL);

    avx_sleep_ms(60);

    /* 1. Transmit H.264 Elementary Bitstream Frame */
    const uint8_t annexb_h264[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x64, 0x00, 0x29, 0xac, 0x2c, 0xa5, 0x01, 0xe0, 0x08,
        0x9f, 0x97, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00,
        0x00, 0x03, 0x03, 0x20, 0xf1, 0x83, 0x19, 0x60,
        0x00, 0x00, 0x00, 0x01,
        0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0,
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x88, 0x84, 0x00, 0x10, 0xff, 0xaa, 0x55
    };
    size_t h264_sz = sizeof(annexb_h264);

    oavx_video_frame_t tx_h264 = {
        .width = 1920,
        .height = 1080,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_H264,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { (uint8_t*)annexb_h264, NULL, NULL, NULL },
        .strides = { h264_sz, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 55443322,
        .dts_ns = 55443322,
        .metadata = "h264-passthrough",
        .metadata_len = 16,
        .user_ctx = NULL
    };

    /* Send frame and receive via loopback */
    oavx_video_frame_t rx_h264;
    memset(&rx_h264, 0, sizeof(rx_h264));
    oavx_frame_type_t type = OAVX_FRAME_TYPE_NONE;

    oavx_stream_send_video(tx, &tx_h264);
    for (int attempt = 0; attempt < 10; ++attempt) {
        type = oavx_stream_receive(rx, &rx_h264, NULL, NULL, 100);
        if (type == OAVX_FRAME_TYPE_VIDEO) break;
    }

    ASSERT_EQ(type, OAVX_FRAME_TYPE_VIDEO);
    ASSERT_EQ(rx_h264.fourcc, OAVX_FOURCC_VIDEO_TYPE_H264);
    ASSERT_EQ(rx_h264.width, 1920);
    ASSERT_EQ(rx_h264.height, 1080);
    ASSERT_EQ(rx_h264.strides[0], h264_sz);
    ASSERT_TRUE(rx_h264.planes[0] != NULL);
    ASSERT_TRUE(memcmp(rx_h264.planes[0], annexb_h264, h264_sz) == 0);
    oavx_stream_release_video(rx, &rx_h264);
    printf("    [+] H.264 Compressed Passthrough Loopback: PASSED (bit-identical)\n");

    /* 2. Transmit HEVC Elementary Bitstream Frame */
    const uint8_t annexb_hevc[] = {
        0x00, 0x00, 0x00, 0x01,
        0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01,
        0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xb0,
        0x00, 0x00, 0x00, 0x01,
        0x44, 0x01, 0xc0, 0xf7, 0xc0,
        0x00, 0x00, 0x00, 0x01,
        0x26, 0x01, 0xaf, 0x08, 0x4a, 0x31
    };
    size_t hevc_sz = sizeof(annexb_hevc);

    oavx_video_frame_t tx_hevc = {
        .width = 3840,
        .height = 2160,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_HEVC,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { (uint8_t*)annexb_hevc, NULL, NULL, NULL },
        .strides = { hevc_sz, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 99887766,
        .dts_ns = 99887766,
        .metadata = "hevc-passthrough",
        .metadata_len = 16,
        .user_ctx = NULL
    };

    oavx_video_frame_t rx_hevc;
    memset(&rx_hevc, 0, sizeof(rx_hevc));
    type = OAVX_FRAME_TYPE_NONE;

    oavx_stream_send_video(tx, &tx_hevc);
    for (int attempt = 0; attempt < 10; ++attempt) {
        type = oavx_stream_receive(rx, &rx_hevc, NULL, NULL, 100);
        if (type == OAVX_FRAME_TYPE_VIDEO) break;
    }

    ASSERT_EQ(type, OAVX_FRAME_TYPE_VIDEO);
    ASSERT_EQ(rx_hevc.fourcc, OAVX_FOURCC_VIDEO_TYPE_HEVC);
    ASSERT_EQ(rx_hevc.width, 3840);
    ASSERT_EQ(rx_hevc.height, 2160);
    ASSERT_EQ(rx_hevc.strides[0], hevc_sz);
    ASSERT_TRUE(rx_hevc.planes[0] != NULL);
    ASSERT_TRUE(memcmp(rx_hevc.planes[0], annexb_hevc, hevc_sz) == 0);
    oavx_stream_release_video(rx, &rx_hevc);
    printf("    [+] HEVC Compressed Passthrough Loopback: PASSED (bit-identical)\n");

    /* 3. Transmit AV1 Elementary Bitstream Frame */
    const uint8_t annexb_av1[] = {
        0x00, 0x00, 0x00, 0x01,
        0x0a, 0x0b, 0x00, 0x00, 0x00, 0x42, 0xab, 0xbf, 0xc3, 0x71, 0xab, 0xe6, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x32, 0x04, 0x14, 0xde, 0xad, 0xbe
    };
    size_t av1_sz = sizeof(annexb_av1);

    oavx_video_frame_t tx_av1 = {
        .width = 1920,
        .height = 1080,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_AV01,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { (uint8_t*)annexb_av1, NULL, NULL, NULL },
        .strides = { av1_sz, 0, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 55443322,
        .dts_ns = 55443322,
        .metadata = "av1-passthrough",
        .metadata_len = 15,
        .user_ctx = NULL
    };

    oavx_video_frame_t rx_av1;
    memset(&rx_av1, 0, sizeof(rx_av1));
    type = OAVX_FRAME_TYPE_NONE;

    oavx_stream_send_video(tx, &tx_av1);
    for (int attempt = 0; attempt < 10; ++attempt) {
        type = oavx_stream_receive(rx, &rx_av1, NULL, NULL, 100);
        if (type == OAVX_FRAME_TYPE_VIDEO) break;
    }

    ASSERT_EQ(type, OAVX_FRAME_TYPE_VIDEO);
    ASSERT_EQ(rx_av1.fourcc, OAVX_FOURCC_VIDEO_TYPE_AV01);
    ASSERT_EQ(rx_av1.width, 1920);
    ASSERT_EQ(rx_av1.height, 1080);
    ASSERT_EQ(rx_av1.strides[0], av1_sz);
    ASSERT_TRUE(rx_av1.planes[0] != NULL);
    ASSERT_TRUE(memcmp(rx_av1.planes[0], annexb_av1, av1_sz) == 0);
    oavx_stream_release_video(rx, &rx_av1);
    printf("    [+] AV1 Compressed Passthrough Loopback: PASSED (bit-identical)\n");

    oavx_stream_close(rx);
    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
}

int main(void) {
    printf("=== OpenAVX RFC 2326 RTP / RTSP Tests ===\n");
    printf("Codecs: H.264 / HEVC / AV1\n\n");

    test_h264_sps_parsing();
    test_hevc_sps_parsing();
    test_av1_sequence_header_parsing();
    test_av1_keyframe_detection();
    test_av1_demuxer_integration();
    test_rtp_h264_depacketization();
    test_rtp_hevc_depacketization();
    test_orphan_idr_repair();
    test_rtsp_client();
    test_compressed_recv_pipeline();

    printf("\n[PASS] openavx_rtsp_test\n");
    return 0;
}
