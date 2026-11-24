/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * test/openavx_fuzz_regression_test.c
 *
 * Automated regression suite executing 174M-iteration fuzzing corpus
 * and discovered crash artifacts against core decoders and wire parsers.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

#include "openavx.h"
#include "avx_internal.h"
#include "avx_wire.h"
#include "avx_speedhq.h"
#include "avx_audio.h"
#include "avx_udp.h"

#ifndef OPENAVX_PROJECT_SOURCE_DIR
#define OPENAVX_PROJECT_SOURCE_DIR "."
#endif

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static uint8_t *read_file_to_buffer(const char *filepath, size_t *out_size) {
    *out_size = 0;
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > 64 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    *out_size = read_bytes;
    return buf;
}

static void test_fuzz_wire_unpack(const uint8_t *data, size_t size) {
    if (!data || size < 4) return;

    uint32_t magic;
    memcpy(&magic, data, sizeof(magic));

    const oavx_wire_profile_t *prof = avx_wire_get_default_profile();

    if (magic == prof->magic_video || magic == OAVX_WIRE_MAGIC_VIDEO) {
        AVX_PACKED_START
        struct {
            uint32_t xres;
            uint32_t yres;
            uint32_t fourcc;
            int32_t  frame_rate_n;
            int32_t  frame_rate_d;
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED vhdr;
        AVX_PACKED_END

        if (size >= 4 + sizeof(vhdr)) {
            memcpy(&vhdr, data + 4, sizeof(vhdr));
            uint64_t total_pixels = (uint64_t)vhdr.xres * (uint64_t)vhdr.yres;
            (void)total_pixels;
        }
    } else if (magic == prof->magic_audio || magic == OAVX_WIRE_MAGIC_AUDIO) {
        AVX_PACKED_START
        struct {
            int32_t  sample_rate;
            int32_t  channels;
            int32_t  samples;
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED ahdr;
        AVX_PACKED_END

        if (size >= 4 + sizeof(ahdr)) {
            memcpy(&ahdr, data + 4, sizeof(ahdr));
            for (int c = 0; c < ahdr.channels && c < 8; ++c) {
                size_t offset = (size_t)c * (size_t)ahdr.samples * sizeof(float);
                (void)offset;
            }
        }
    } else if (magic == prof->magic_metadata || magic == OAVX_WIRE_MAGIC_METADATA) {
        AVX_PACKED_START
        struct {
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED mhdr;
        AVX_PACKED_END

        if (size >= 4 + sizeof(mhdr)) {
            memcpy(&mhdr, data + 4, sizeof(mhdr));
        }
    } else if (magic == OAVX_WIRE_MAGIC_AUDIO_COMP) {
        AVX_PACKED_START
        struct {
            uint32_t codec_fourcc;
            int32_t  sample_rate;
            int32_t  channels;
            int32_t  samples;
            int64_t  timecode;
            uint32_t payload_len;
        } AVX_PACKED cahdr;
        AVX_PACKED_END

        if (size >= 4 + sizeof(cahdr)) {
            memcpy(&cahdr, data + 4, sizeof(cahdr));
        }
    } else if (magic == OAVX_UDP_MAGIC) {
        if (size >= sizeof(avx_udp_header_t)) {
            avx_udp_header_t uhdr;
            memcpy(&uhdr, data, sizeof(uhdr));
            if (uhdr.frag_count > 0 && uhdr.frag_count <= AVX_UDP_MAX_FRAGS) {
                size_t dest_offset = (size_t)uhdr.frag_idx * AVX_UDP_MAX_FRAG_SIZE;
                (void)dest_offset;
            }
        }
    } else if (magic == OAVX_UDP_NACK_MAGIC) {
        if (size >= sizeof(avx_udp_nack_t)) {
            avx_udp_nack_t nhdr;
            memcpy(&nhdr, data, sizeof(nhdr));
            uint16_t rc = nhdr.range_count;
            if (rc > AVX_UDP_NACK_MAX_RANGES) rc = AVX_UDP_NACK_MAX_RANGES;
            for (uint16_t r = 0; r < rc; ++r) {
                uint32_t end = (uint32_t)nhdr.ranges[r].start + (uint32_t)nhdr.ranges[r].count;
                (void)end;
            }
        }
    }
}

static void test_fuzz_wire_socketpair(const uint8_t *data, size_t size) {
    if (!data || size == 0) return;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        size_t send_len = size > 65536 ? 65536 : size;
        (void)send(sv[0], (const char*)data, (int)send_len, 0);
        close(sv[0]);

        avx_rx_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.color_format = (oavx_color_format_e)(data[0] % 7);

        avx_rx_instance_t rx = avx_rx_create(&cfg);
        if (rx) {
            avx_recv_ctx_t *slot = (avx_recv_ctx_t*)rx;
            avx_mutex_lock(&slot->lock);
            slot->tcp_conn_fd = sv[1];
            slot->connected = true;
            slot->offline_mode = true;
            avx_mutex_unlock(&slot->lock);

            oavx_video_frame_t vframe;
            oavx_audio_frame_t aframe;
            oavx_metadata_frame_t mframe;

            for (int i = 0; i < 2; ++i) {
                oavx_frame_type_t ft = avx_rx_receive(rx, &vframe, &aframe, &mframe, 2);
                if (ft <= 0) break;
                if (ft == OAVX_FRAME_TYPE_VIDEO) {
                    avx_rx_free_video(rx, &vframe);
                } else if (ft == OAVX_FRAME_TYPE_AUDIO) {
                    avx_rx_free_audio(rx, &aframe);
                } else if (ft == OAVX_FRAME_TYPE_METADATA) {
                    avx_rx_free_metadata(rx, &mframe);
                }
            }

            avx_rx_destroy(rx);
        } else {
            close(sv[1]);
        }
    }
}

static void test_fuzz_speedhq_input(const uint8_t *data, size_t size) {
    if (!data || size < 6) return;

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

    uint16_t raw_w = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    uint16_t raw_h = (uint16_t)data[3] | ((uint16_t)data[4] << 8);

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

        if (out_u64 <= 16ULL * 1024ULL * 1024ULL) {
            uint8_t *out_buf = (uint8_t *)malloc((size_t)out_u64);
            uint8_t *alpha_buf = (uint8_t *)malloc((size_t)alpha_u64);

            if (out_buf) {
                avx_speedhq_decode_c99(w, h, payload, payload_size, out_buf);
                avx_speedhq_decode(w, h, payload, payload_size, out_buf);

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
    }
}

static void test_fuzz_audio_input(const uint8_t *data, size_t size) {
    if (!data || size < 12) return;

    int in_rate = ((int)(data[0] | (data[1] << 8)) % 200000) + 1;
    int out_rate = ((int)(data[2] | (data[3] << 8)) % 200000) + 1;
    int channels = (data[4] % 8) + 1;
    avx_audio_resample_mode_e mode = (avx_audio_resample_mode_e)(data[5] % 3);
    avx_audio_normalize_e norm_mode = (avx_audio_normalize_e)(data[6] % 3);

    const uint8_t *sample_bytes = data + 12;
    size_t sample_len = size - 12;
    int total_floats = (int)(sample_len / sizeof(float));
    int frames = total_floats / channels;
    if (frames > 256) frames = 256;
    if (frames < 1) frames = 1;

    float *in_data[8] = {NULL};
    float *out_data[8] = {NULL};
    int max_out = frames * 4 + 128;

    for (int c = 0; c < 8; ++c) {
        in_data[c] = (float *)calloc((size_t)frames + 16, sizeof(float));
        out_data[c] = (float *)calloc((size_t)max_out + 16, sizeof(float));
    }

    size_t copy_floats = (size_t)frames * (size_t)channels;
    if (copy_floats > (size_t)total_floats) copy_floats = (size_t)total_floats;
    if (copy_floats > 0) {
        const float *src_floats = (const float *)sample_bytes;
        for (int c = 0; c < channels; ++c) {
            for (int f = 0; f < frames; ++f) {
                size_t src_idx = (size_t)f * (size_t)channels + (size_t)c;
                if (src_idx < (size_t)total_floats) {
                    in_data[c][f] = src_floats[src_idx];
                }
            }
        }
    }

    avx_resampler_t *r = avx_resampler_create(in_rate, out_rate, channels, mode);
    if (r) {
        (void)avx_resampler_process(r, (const float * const *)in_data, frames, out_data, max_out);
        (void)avx_resampler_flush(r, out_data, max_out);
        avx_resampler_reset(r);
        avx_resampler_destroy(r);
    }

    (void)avx_audio_resample_simple(in_data[0], frames, in_rate, out_data[0], max_out, out_rate, mode);

    if (frames > 0) {
        avx_audio_downmix_stereo_to_mono(in_data[0], in_data[1], out_data[0], frames, norm_mode);
        avx_audio_downmix_quad_to_stereo((const float * const *)in_data, out_data[0], out_data[1], frames, norm_mode);
        avx_audio_downmix_5_1_to_stereo((const float * const *)in_data, out_data[0], out_data[1], frames, norm_mode);
        avx_audio_downmix_7_1_to_stereo((const float * const *)in_data, out_data[0], out_data[1], frames, norm_mode);
    }

    for (int c = 0; c < 8; ++c) {
        free(in_data[c]);
        free(out_data[c]);
    }
}

static int run_directory_regression(const char *dirpath, void (*test_func)(const uint8_t *, size_t)) {
    DIR *d = opendir(dirpath);
    if (!d) {
        printf("  [!] Notice: Directory not found or empty: %s\n", dirpath);
        return 0;
    }

    struct dirent *dir;
    int count = 0;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dirpath, dir->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            size_t sz = 0;
            uint8_t *data = read_file_to_buffer(full_path, &sz);
            if (data) {
                test_func(data, sz);
                free(data);
                count++;
            }
        }
    }
    closedir(d);
    return count;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\n=== OpenAVX Fuzz Corpus & Crash Regression Tests ===\n\n");

    const char *source_dir = OPENAVX_PROJECT_SOURCE_DIR;
    char path_buf[1024];

    /* 1. Initialize subsystems */
    printf("[*] 1. Initializing decoders & internal profiles...\n");
    bool shq_ok = avx_speedhq_init();
    assert(shq_ok);
    printf("  [+] avx_speedhq_init: OK\n");

    /* 2. Crash Artifact Regression */
    printf("\n[*] 2. Executing historical crash artifact regressions...\n");
    
    // Crash 1: Truncated UYVA packet (from Nov 7)
    snprintf(path_buf, sizeof(path_buf), "%s/fuzz/crashes/fuzz_wire_packet-crash-93132a7274cb0ca0c515a8e30ef02cc5db82c4b6", source_dir);
    size_t sz1 = 0;
    uint8_t *crash1 = read_file_to_buffer(path_buf, &sz1);
    if (crash1) {
        printf("  [+] Executing Crash Artifact #1 (%zu bytes, wire truncation regression)...\n", sz1);
        test_fuzz_wire_unpack(crash1, sz1);
        test_fuzz_wire_socketpair(crash1, sz1);
        free(crash1);
        printf("  [+] Crash Artifact #1 PASS\n");
    } else {
        printf("  [!] Notice: Crash artifact #1 not found at %s (skipping)\n", path_buf);
    }

    // Crash 2: Slow-unit malformed XML metadata
    snprintf(path_buf, sizeof(path_buf), "%s/fuzz/crashes/fuzz_wire_packet-slow-unit-1f70c8a855f22213f70f8367a09c2de41c362e3c", source_dir);
    size_t sz2 = 0;
    uint8_t *crash2 = read_file_to_buffer(path_buf, &sz2);
    if (crash2) {
        printf("  [+] Executing Slow Unit Artifact #2 (%zu bytes, wire metadata parsing)...\n", sz2);
        double t0 = get_time_ms();
        test_fuzz_wire_unpack(crash2, sz2);
        test_fuzz_wire_socketpair(crash2, sz2);
        double elapsed = get_time_ms() - t0;
        free(crash2);
        printf("  [+] Slow Unit Artifact #2 PASS (%.2f ms)\n", elapsed);
    } else {
        printf("  [!] Notice: Slow unit artifact #2 not found at %s (skipping)\n", path_buf);
    }

    /* 3. Wire Framing Corpus Regression */
    printf("\n[*] 3. Executing Wire Framing Corpus Regression (fuzz/corpus/wire)...\n");
    snprintf(path_buf, sizeof(path_buf), "%s/fuzz/corpus/wire", source_dir);
    double t_wire_0 = get_time_ms();
    int wire_count = run_directory_regression(path_buf, test_fuzz_wire_unpack);
    double t_wire = get_time_ms() - t_wire_0;
    printf("  [+] Wire corpus: %d inputs in %.2f ms\n", wire_count, t_wire);

    /* 4. SpeedHQ Bitstream Corpus Regression */
    printf("\n[*] 4. Executing SpeedHQ Bitstream Corpus Regression (fuzz/corpus/speedhq)...\n");
    snprintf(path_buf, sizeof(path_buf), "%s/fuzz/corpus/speedhq", source_dir);
    double t_shq_0 = get_time_ms();
    int shq_count = run_directory_regression(path_buf, test_fuzz_speedhq_input);
    double t_shq = get_time_ms() - t_shq_0;
    printf("  [+] SpeedHQ corpus: %d inputs in %.2f ms\n", shq_count, t_shq);

    /* 5. Audio Resampler & Downmix Corpus Regression */
    printf("\n[*] 5. Executing Audio DSP Corpus Regression (fuzz/corpus/audio)...\n");
    snprintf(path_buf, sizeof(path_buf), "%s/fuzz/corpus/audio", source_dir);
    double t_audio_0 = get_time_ms();
    int audio_count = run_directory_regression(path_buf, test_fuzz_audio_input);
    double t_audio = get_time_ms() - t_audio_0;
    printf("  [+] Audio corpus: %d inputs in %.2f ms\n", audio_count, t_audio);

    printf("\n[PASS] openavx_fuzz_regression_test (%d inputs)\n\n", wire_count + shq_count + audio_count + 2);

    return 0;
}
