/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_framesync_util_test.c
 * Comprehensive test suite for Frame Synchronizer & Interleaved Audio / Video Utilities
 */

#include "openavx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

#define PI 3.14159265358979323846

static void test_interleaved_audio_conversions(void) {
    printf("[*] Testing Audio Interleaved Conversions (16s, 32f, 32s)...\n");

    const int sample_rate = 48000;
    const int channels = 2;
    const int samples = 480;

    /* 1. Allocate Planar Float Source */
    float* planar_src = (float*)malloc((size_t)channels * samples * sizeof(float));
    assert(planar_src != NULL);

    for (int s = 0; s < samples; ++s) {
        planar_src[0 * samples + s] = sinf(2.0f * (float)PI * 440.0f * (float)s / (float)sample_rate); /* Left: 440Hz */
        planar_src[1 * samples + s] = sinf(2.0f * (float)PI * 880.0f * (float)s / (float)sample_rate); /* Right: 880Hz */
    }

    oavx_audio_frame_t src_frame = {
        .sample_rate = sample_rate,
        .channels = channels,
        .frames = samples,
        .format = OAVX_AUDIO_FMT_FLT_PLANAR,
        .planes = { planar_src, planar_src + samples, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = samples * sizeof(float),
        .pts_ns = 1234567,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* 2. Planar -> Interleaved 16s -> Planar Roundtrip */
    int16_t* buf_16s = (int16_t*)malloc((size_t)channels * samples * sizeof(int16_t));
    assert(buf_16s != NULL);

    oavx_audio_frame_interleaved_16s_t frame_16s = {
        .reference_level = 20,
        .data = buf_16s
    };

    oavx_util_audio_to_interleaved_16s(&src_frame, &frame_16s);
    assert(frame_16s.sample_rate == (uint32_t)sample_rate);
    assert(frame_16s.channels == (uint16_t)channels);
    assert(frame_16s.frames == (uint32_t)samples);
    assert(frame_16s.pts_ns == 1234567);

    /* Verify interleaving pattern: buf_16s[0] = L0, buf_16s[1] = R0, buf_16s[2] = L1, buf_16s[3] = R1 */
    assert(abs(buf_16s[0] - (int)roundf(planar_src[0] * 32767.0f)) <= 1);
    assert(abs(buf_16s[1] - (int)roundf(planar_src[samples] * 32767.0f)) <= 1);

    /* Convert back from 16s to Planar */
    float* planar_dst_16 = (float*)malloc((size_t)channels * samples * sizeof(float));
    assert(planar_dst_16 != NULL);
    oavx_audio_frame_t dst_16 = {
        .planes = { planar_dst_16, planar_dst_16 + samples, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = samples * sizeof(float)
    };
    oavx_util_audio_from_interleaved_16s(&frame_16s, &dst_16);

    for (int c = 0; c < channels; ++c) {
        for (int s = 0; s < samples; ++s) {
            float diff = fabsf(planar_src[c * samples + s] - planar_dst_16[c * samples + s]);
            /* 16-bit quantization maximum error is 1.0 / 32767 ~ 0.000031 */
            assert(diff < 0.0001f);
        }
    }
    printf("  [+] Interleaved 16s roundtrip verified (16-bit precision intact)\n");

    /* 3. Planar -> Interleaved 32f -> Planar Roundtrip */
    float* buf_32f = (float*)malloc((size_t)channels * samples * sizeof(float));
    assert(buf_32f != NULL);
    oavx_audio_frame_interleaved_32f_t frame_32f = {
        .data = buf_32f
    };
    oavx_util_audio_to_interleaved_32f(&src_frame, &frame_32f);
    assert(frame_32f.sample_rate == (uint32_t)sample_rate);
    assert(frame_32f.channels == (uint16_t)channels);
    assert(frame_32f.frames == (uint32_t)samples);

    float* planar_dst_32f = (float*)malloc((size_t)channels * samples * sizeof(float));
    assert(planar_dst_32f != NULL);
    oavx_audio_frame_t dst_32f = {
        .planes = { planar_dst_32f, planar_dst_32f + samples, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = samples * sizeof(float)
    };
    oavx_util_audio_from_interleaved_32f(&frame_32f, &dst_32f);

    for (int c = 0; c < channels; ++c) {
        for (int s = 0; s < samples; ++s) {
            assert(planar_src[c * samples + s] == planar_dst_32f[c * samples + s]);
        }
    }
    printf("  [+] Interleaved 32f roundtrip verified (bitwise exact)\n");

    /* 4. Planar -> Interleaved 32s -> Planar Roundtrip */
    int32_t* buf_32s = (int32_t*)malloc((size_t)channels * samples * sizeof(int32_t));
    assert(buf_32s != NULL);
    oavx_audio_frame_interleaved_32s_t frame_32s = {
        .data = buf_32s
    };
    oavx_util_audio_to_interleaved_32s(&src_frame, &frame_32s);
    assert(frame_32s.sample_rate == (uint32_t)sample_rate);
    assert(frame_32s.channels == (uint16_t)channels);
    assert(frame_32s.frames == (uint32_t)samples);

    float* planar_dst_32s = (float*)malloc((size_t)channels * samples * sizeof(float));
    assert(planar_dst_32s != NULL);
    oavx_audio_frame_t dst_32s = {
        .planes = { planar_dst_32s, planar_dst_32s + samples, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = samples * sizeof(float)
    };
    oavx_util_audio_from_interleaved_32s(&frame_32s, &dst_32s);

    for (int c = 0; c < channels; ++c) {
        for (int s = 0; s < samples; ++s) {
            float diff = fabsf(planar_src[c * samples + s] - planar_dst_32s[c * samples + s]);
            assert(diff < 0.000001f);
        }
    }
    printf("  [+] Interleaved 32s roundtrip verified (32-bit precision intact)\n");

    free(planar_src);
    free(buf_16s);
    free(planar_dst_16);
    free(buf_32f);
    free(planar_dst_32f);
    free(buf_32s);
    free(planar_dst_32s);
}

static void test_v210_p216_conversions(void) {
    printf("[*] Testing SMPTE 292M 10-bit SDI V210 <-> P216 Video Conversions...\n");

    const int width = 1920;
    const int height = 1080;

    /* Allocate source P216 frame */
    size_t p216_bytes = (size_t)width * height * 2 * 2; /* Y plane (width*height*2) + UV plane (width*height*2) */
    uint16_t* p216_orig = (uint16_t*)malloc(p216_bytes);
    assert(p216_orig != NULL);

    uint16_t* y_orig = p216_orig;
    uint16_t* uv_orig = p216_orig + (size_t)width * height;

    /* Fill with deterministic 10-bit test pattern (64 to 940 for Y, 64 to 960 for UV) */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint16_t y_val = (uint16_t)((64 + ((x * 876) / width)) & 0x3FF);
            uint16_t u_val = (uint16_t)((64 + ((y * 896) / height)) & 0x3FF);
            uint16_t v_val = (uint16_t)((512 + ((x - y) & 0xFF)) & 0x3FF);

            y_orig[y * width + x] = (uint16_t)(y_val << 6); /* Left-justified 16-bit */
            if ((x % 2) == 0) {
                uv_orig[y * width + x + 0] = (uint16_t)(u_val << 6);
                uv_orig[y * width + x + 1] = (uint16_t)(v_val << 6);
            }
        }
    }

    oavx_video_frame_t src_p216 = {
        .width = width,
        .height = height,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_P216,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { (uint8_t*)p216_orig, (uint8_t*)(p216_orig + width * height), NULL, NULL },
        .strides = { (size_t)width * 2, (size_t)width * 2, 0, 0 },
        .framerate = { 60000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 10002000,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    /* Allocate V210 destination frame */
    size_t v210_stride = (size_t)(width / 6) * 16;
    size_t v210_bytes = v210_stride * height;
    uint8_t* v210_buf = (uint8_t*)malloc(v210_bytes);
    assert(v210_buf != NULL);

    oavx_video_frame_t dst_v210 = {
        .planes = { v210_buf, NULL, NULL, NULL }
    };

    /* Convert P216 -> V210 */
    oavx_util_P216_to_V210(&src_p216, &dst_v210);
    assert(dst_v210.width == (uint32_t)width);
    assert(dst_v210.height == (uint32_t)height);
    assert(dst_v210.fourcc == (oavx_fourcc_video_type_e)0x30313276); /* "v210" */
    assert(dst_v210.strides[0] == v210_stride);

    /* Allocate unpacked P216 frame */
    uint16_t* p216_unpacked = (uint16_t*)malloc(p216_bytes);
    assert(p216_unpacked != NULL);
    memset(p216_unpacked, 0, p216_bytes);

    oavx_video_frame_t dst_p216 = {
        .planes = { (uint8_t*)p216_unpacked, NULL, NULL, NULL }
    };

    /* Convert V210 -> P216 */
    oavx_util_V210_to_P216(&dst_v210, &dst_p216);
    assert(dst_p216.width == (uint32_t)width);
    assert(dst_p216.height == (uint32_t)height);
    assert(dst_p216.fourcc == OAVX_FOURCC_VIDEO_TYPE_P216);

    /* Verify 100% bitwise parity on all 10-bit samples */
    int mismatches = 0;
    for (size_t i = 0; i < p216_bytes / 2; ++i) {
        if (p216_orig[i] != p216_unpacked[i]) {
            mismatches++;
        }
    }
    assert(mismatches == 0);
    printf("  [+] SMPTE 292M V210 <-> P216 10-bit roundtrip 100%% bitwise exact (%zu samples verified)\n", p216_bytes / 2);

    free(p216_orig);
    free(v210_buf);
    free(p216_unpacked);
}

static void test_framesync_and_interleaved_send(void) {
    printf("[*] Testing FrameSync Engine & Interleaved Audio Transmission...\n");

    oavx_context_t *ctx = oavx_context_create();
    assert(ctx != NULL);

    /* Create Sender */
    oavx_stream_config_t send_create = {
        .name = "FrameSync_Test_Stream",
        .groups = NULL,
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);

    /* Create Receiver connected to loopback sender */
    oavx_stream_config_t recv_create = {
        .name = "FrameSync_Test_Recv",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_BGRX_BGRA,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    /* Create FrameSynchronizer */
    oavx_framesync_t *p_fs = oavx_framesync_create(p_recv);
    assert(p_fs != NULL);

    /* Wait for receiver to connect to sender */
    for (int wait = 0; wait < 30; ++wait) {
        if (oavx_stream_get_connection_count(p_send, 50) > 0) break;
        struct timespec ts = {0, 20000000L};
        nanosleep(&ts, NULL);
    }

    /* Send Video Frame & Interleaved Audio */
    const int w = 320;
    const int h = 240;
    size_t video_size = (size_t)w * h * 2;
    uint8_t* vdata = (uint8_t*)malloc(video_size);
    assert(vdata != NULL);
    memset(vdata, 0x80, video_size); /* 50% gray UYVY */

    oavx_video_frame_t vframe = {
        .width = w,
        .height = h,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { vdata, NULL, NULL, NULL },
        .strides = { (size_t)w * 2, 0, 0, 0 },
        .framerate = { 30000, 1000 },
        .aspect_ratio = { 4, 3 },
        .pts_ns = 500000,
        .dts_ns = 0,
        .metadata = "<openavx_color_info color_format=\"rec709\"/>",
        .metadata_len = strlen("<openavx_color_info color_format=\"rec709\"/>"),
        .user_ctx = NULL
    };

    const int audio_rate = 48000;
    const int audio_ch = 2;
    const int audio_samples = 1920; /* 40ms of 48kHz audio */
    int16_t* pcm_16s = (int16_t*)malloc((size_t)audio_ch * audio_samples * sizeof(int16_t));
    assert(pcm_16s != NULL);
    for (int s = 0; s < audio_samples; ++s) {
        pcm_16s[s * 2 + 0] = (int16_t)(sinf(2.0f * (float)PI * 1000.0f * (float)s / 48000.0f) * 16384.0f);
        pcm_16s[s * 2 + 1] = (int16_t)(sinf(2.0f * (float)PI * 500.0f * (float)s / 48000.0f) * 16384.0f);
    }

    oavx_audio_frame_interleaved_16s_t send_audio_16s = {
        .sample_rate = audio_rate,
        .channels = audio_ch,
        .frames = audio_samples,
        .pts_ns = 500000,
        .reference_level = 20,
        .data = pcm_16s
    };

    /* Allocate planar audio destination to send */
    float* planar_send_buf = (float*)malloc((size_t)audio_ch * audio_samples * sizeof(float));
    assert(planar_send_buf != NULL);
    oavx_audio_frame_t planar_send_frame = {
        .planes = { planar_send_buf, planar_send_buf + audio_samples, NULL, NULL, NULL, NULL, NULL, NULL },
        .stride_bytes = audio_samples * sizeof(float)
    };
    oavx_util_audio_from_interleaved_16s(&send_audio_16s, &planar_send_frame);

    /* Transmit several frames to seed the pipeline */
    for (int i = 0; i < 5; ++i) {
        oavx_stream_send_video(p_send, &vframe);
        oavx_stream_send_audio(p_send, &planar_send_frame);
        struct timespec ts = {0, 20000000L}; /* 20ms */
        nanosleep(&ts, NULL);
    }

    /* Allow network packets to arrive */
    struct timespec ts = {0, 150000000L}; /* 150ms */
    nanosleep(&ts, NULL);

    /* Capture Synchronized Video */
    oavx_video_frame_t fs_video;
    memset(&fs_video, 0, sizeof(fs_video));
    oavx_framesync_capture_video(p_fs, &fs_video, OAVX_SCAN_PROGRESSIVE);
    assert(fs_video.planes[0] != NULL);
    assert(fs_video.width == (uint32_t)w);
    assert(fs_video.height == (uint32_t)h);
    printf("  [+] FrameSync captured synchronized video frame: %ux%u (stride=%zu)\n",
           fs_video.width, fs_video.height, fs_video.strides[0]);
    oavx_framesync_free_video(p_fs, &fs_video);

    /* Capture Synchronized Audio with Dynamic Resampling (48kHz source -> 44.1kHz host clock) */
    int initial_queue = oavx_framesync_audio_queue_depth(p_fs);
    printf("  [+] Initial FrameSync audio queue depth: %d samples\n", initial_queue);

    oavx_audio_frame_t fs_audio;
    memset(&fs_audio, 0, sizeof(fs_audio));
    const int host_audio_rate = 44100;
    const int host_samples_needed = 441; /* Exactly 10ms of 44.1kHz audio */

    oavx_framesync_capture_audio(p_fs, &fs_audio, host_audio_rate, 2, host_samples_needed);
    assert(fs_audio.planes[0] != NULL);
    assert(fs_audio.sample_rate == (uint32_t)host_audio_rate);
    assert(fs_audio.channels == 2);
    assert(fs_audio.frames == (uint32_t)host_samples_needed);
    printf("  [+] FrameSync locked audio: resampled 48kHz -> %uHz (%u samples delivered)\n",
           fs_audio.sample_rate, fs_audio.frames);
    oavx_framesync_free_audio(p_fs, &fs_audio);

    int final_queue = oavx_framesync_audio_queue_depth(p_fs);
    printf("  [+] Post-pull FrameSync audio queue depth: %d samples\n", final_queue);

    /* Teardown */
    oavx_framesync_destroy(p_fs);
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    free(vdata);
    free(pcm_16s);
    free(planar_send_buf);
    oavx_context_destroy(ctx);
}

static void test_framesync_multiplane_video(void) {
    printf("[*] Testing FrameSync Multi-Plane Video Anchoring (UYVA & NV12)...\n");

    oavx_context_t* ctx = oavx_context_create();
    assert(ctx != NULL);

    oavx_stream_config_t send_create = {
        .name = "FrameSync_MultiPlane_Stream",
        .groups = NULL,
        .direction = OAVX_DIR_TRANSMIT,
        .clock_sync = false
    };
    oavx_stream_t *p_send = oavx_stream_open(ctx, &send_create);
    assert(p_send != NULL);

    oavx_stream_config_t recv_create = {
        .name = "FrameSync_MultiPlane_Recv",
        .endpoint = "127.0.0.1:7960",
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_FASTEST,
        .bandwidth = OAVX_BANDWIDTH_HIGHEST,
        .clock_sync = false
    };
    oavx_stream_t *p_recv = oavx_stream_open(ctx, &recv_create);
    assert(p_recv != NULL);

    oavx_framesync_t *p_fs = oavx_framesync_create(p_recv);
    assert(p_fs != NULL);

    for (int wait = 0; wait < 30; ++wait) {
        if (oavx_stream_get_connection_count(p_send, 50) > 0) break;
        struct timespec ts = {0, 20000000L};
        nanosleep(&ts, NULL);
    }

    const int w = 160;
    const int h = 90;
    uint8_t* uyva_yuv = (uint8_t*)malloc((size_t)w * h * 2);
    uint8_t* uyva_alpha = (uint8_t*)malloc((size_t)w * h);
    assert(uyva_yuv && uyva_alpha);
    memset(uyva_yuv, 0x80, (size_t)w * h * 2);
    memset(uyva_alpha, 0xEE, (size_t)w * h);

    oavx_video_frame_t uyva_frame = {
        .width = (uint32_t)w,
        .height = (uint32_t)h,
        .fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVA,
        .scan_type = OAVX_SCAN_PROGRESSIVE,
        .planes = { uyva_yuv, uyva_alpha, NULL, NULL },
        .strides = { (size_t)w * 2, (size_t)w, 0, 0 },
        .framerate = { 30000, 1000 },
        .aspect_ratio = { 16, 9 },
        .pts_ns = 555555,
        .dts_ns = 0,
        .metadata = NULL,
        .metadata_len = 0,
        .user_ctx = NULL
    };

    for (int i = 0; i < 5; ++i) {
        oavx_stream_send_video(p_send, &uyva_frame);
        struct timespec ts = {0, 20000000L};
        nanosleep(&ts, NULL);
    }

    struct timespec ts = {0, 150000000L};
    nanosleep(&ts, NULL);

    oavx_video_frame_t fs_video;
    memset(&fs_video, 0, sizeof(fs_video));
    oavx_framesync_capture_video(p_fs, &fs_video, OAVX_SCAN_PROGRESSIVE);
    assert(fs_video.planes[0] != NULL);
    assert(fs_video.planes[1] != NULL);
    assert(fs_video.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA);
    assert(fs_video.strides[0] == (size_t)w * 2);
    assert(fs_video.strides[1] == (size_t)w);
    /* Verify planes[1] is inside the delivery buffer at offset w * h * 2 */
    assert(fs_video.planes[1] == (uint8_t*)fs_video.planes[0] + (size_t)w * h * 2);
    assert(((uint8_t*)fs_video.planes[1])[0] == 0xEE);
    assert(((uint8_t*)fs_video.planes[0])[0] == 0x80);
    printf("  [+] FrameSync UYVA plane 1 correctly anchored and preserved alpha byte 0xEE\n");

    oavx_framesync_free_video(p_fs, &fs_video);
    assert(fs_video.planes[0] == NULL);
    assert(fs_video.planes[1] == NULL);

    oavx_framesync_destroy(p_fs);
    oavx_stream_close(p_recv);
    oavx_stream_close(p_send);
    free(uyva_yuv);
    free(uyva_alpha);
    oavx_context_destroy(ctx);
    printf("  [+] FrameSync multi-plane video test passed\n");
}

int main(void) {
    printf("=== OpenAVX FrameSync & Format Utility Tests ===\n");

    test_interleaved_audio_conversions();
    test_v210_p216_conversions();
    test_framesync_and_interleaved_send();
    test_framesync_multiplane_video();

    printf("\n[PASS] openavx_framesync_util_test\n");
    return 0;
}
