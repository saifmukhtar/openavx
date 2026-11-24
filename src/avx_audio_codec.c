/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_audio_codec.c - Opus runtime loader and IMA ADPCM codec
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "avx_platform.h"
#include "avx_audio_codec.h"
#include "avx_internal.h"
#include "avx_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* ========================================================================= */
/* 1. OPUS CONSTANTS & DYNAMIC LOADER PROTOTYPES                             */
/* ========================================================================= */

#define OPUS_APPLICATION_VOIP                2048
#define OPUS_APPLICATION_AUDIO               2049
#define OPUS_APPLICATION_RESTRICTED_LOWDELAY 2051
#define OPUS_SIGNAL_MUSIC                    3002
#define OPUS_SET_BITRATE_REQUEST             4002
#define OPUS_SET_COMPLEXITY_REQUEST          4010
#define OPUS_SET_SIGNAL_REQUEST              4024
#define OPUS_AUTO                            -1000
#define OPUS_OK                              0

typedef void* (*fn_opus_ms_encoder_create)(int Fs, int channels, int streams, int coupled_streams,
                                           const unsigned char *mapping, int application, int *error);
typedef void  (*fn_opus_ms_encoder_destroy)(void *st);
typedef int   (*fn_opus_ms_encode_float)(void *st, const float *pcm, int frame_size,
                                         unsigned char *data, int max_data_bytes);
typedef int   (*fn_opus_ms_encoder_ctl)(void *st, int request, ...);

typedef void* (*fn_opus_ms_decoder_create)(int Fs, int channels, int streams, int coupled_streams,
                                           const unsigned char *mapping, int *error);
typedef void  (*fn_opus_ms_decoder_destroy)(void *st);
typedef int   (*fn_opus_ms_decode_float)(void *st, const unsigned char *data, int len,
                                         float *pcm, int frame_size, int decode_fec);

static struct {
    void* handle;
    bool  available;
    bool  probed;
    avx_mutex_t lock;

    fn_opus_ms_encoder_create  ms_encoder_create;
    fn_opus_ms_encoder_destroy ms_encoder_destroy;
    fn_opus_ms_encode_float    ms_encode_float;
    fn_opus_ms_encoder_ctl     ms_encoder_ctl;

    fn_opus_ms_decoder_create  ms_decoder_create;
    fn_opus_ms_decoder_destroy ms_decoder_destroy;
    fn_opus_ms_decode_float    ms_decode_float;
} g_opus = {
    .handle = NULL,
    .available = false,
    .probed = false,
    .lock = AVX_MUTEX_INITIALIZER
};

/* Dynamically probes and binds libopus shared library symbols at runtime with thread-safe locking. */
static void probe_opus_library(void) {
    avx_mutex_lock(&g_opus.lock);
    if (g_opus.probed) {
        avx_mutex_unlock(&g_opus.lock);
        return;
    }
    g_opus.probed = true;

    const char *lib_names[] = {
#if defined(_WIN32)
        "opus.dll", "libopus-0.dll", "libopus.dll"
#elif defined(__APPLE__)
        "libopus.0.dylib", "libopus.dylib",
        "/opt/homebrew/lib/libopus.0.dylib", "/usr/local/lib/libopus.0.dylib"
#else
        "libopus.so.0", "libopus.so",
        "/usr/lib64/libopus.so.0", "/usr/lib/x86_64-linux-gnu/libopus.so.0",
        "/usr/lib/aarch64-linux-gnu/libopus.so.0"
#endif
    };

    void *h = NULL;
    for (size_t i = 0; i < sizeof(lib_names) / sizeof(lib_names[0]); ++i) {
#if defined(_WIN32)
        h = (void*)LoadLibraryA(lib_names[i]);
#else
        h = dlopen(lib_names[i], RTLD_NOW | RTLD_LOCAL);
#endif
        if (h) break;
    }

    if (!h) {
        avx_mutex_unlock(&g_opus.lock);
        return;
    }

#if defined(_WIN32)
#define LOAD_SYM(name) (GetProcAddress((HMODULE)h, #name))
#else
#define LOAD_SYM(name) (dlsym(h, #name))
#endif

    g_opus.ms_encoder_create  = (fn_opus_ms_encoder_create)LOAD_SYM(opus_multistream_encoder_create);
    g_opus.ms_encoder_destroy = (fn_opus_ms_encoder_destroy)LOAD_SYM(opus_multistream_encoder_destroy);
    g_opus.ms_encode_float    = (fn_opus_ms_encode_float)LOAD_SYM(opus_multistream_encode_float);
    g_opus.ms_encoder_ctl     = (fn_opus_ms_encoder_ctl)LOAD_SYM(opus_multistream_encoder_ctl);

    g_opus.ms_decoder_create  = (fn_opus_ms_decoder_create)LOAD_SYM(opus_multistream_decoder_create);
    g_opus.ms_decoder_destroy = (fn_opus_ms_decoder_destroy)LOAD_SYM(opus_multistream_decoder_destroy);
    g_opus.ms_decode_float    = (fn_opus_ms_decode_float)LOAD_SYM(opus_multistream_decode_float);

#undef LOAD_SYM

    if (g_opus.ms_encoder_create && g_opus.ms_encoder_destroy &&
        g_opus.ms_encode_float && g_opus.ms_encoder_ctl &&
        g_opus.ms_decoder_create && g_opus.ms_decoder_destroy &&
        g_opus.ms_decode_float) {
        g_opus.handle = h;
        g_opus.available = true;
    } else {
#if defined(_WIN32)
        FreeLibrary((HMODULE)h);
#else
        dlclose(h);
#endif
    }

    avx_mutex_unlock(&g_opus.lock);
}

bool avx_audio_codec_has_opus(void) {
    if (!g_opus.probed) {
        probe_opus_library();
    }
    return g_opus.available;
}

/* ========================================================================= */
/* 2. CHANNEL MAPPING (SMPTE / ITU-R <-> VORBIS FAMILY 1)                    */
/* ========================================================================= */

typedef struct {
    int streams;
    int coupled_streams;
    unsigned char mapping[16];
} opus_channel_config_t;

/* Maps broadcast SMPTE / ITU-R channel ordering to Vorbis family 1 multistream Opus layout. */
static bool get_opus_channel_config(int channels, opus_channel_config_t *cfg) {
    if (!cfg || channels < 1 || channels > 8) return false;
    memset(cfg, 0, sizeof(*cfg));

    switch (channels) {
        case 1: /* Mono */
            cfg->streams = 1;
            cfg->coupled_streams = 0;
            cfg->mapping[0] = 0;
            break;
        case 2: /* Stereo */
            cfg->streams = 1;
            cfg->coupled_streams = 1;
            cfg->mapping[0] = 0;
            cfg->mapping[1] = 1;
            break;
        case 4: /* Quadraphonic */
            cfg->streams = 2;
            cfg->coupled_streams = 2;
            cfg->mapping[0] = 0;
            cfg->mapping[1] = 1;
            cfg->mapping[2] = 2;
            cfg->mapping[3] = 3;
            break;
        case 6: /* 5.1 Surround */
            cfg->streams = 4;
            cfg->coupled_streams = 2;
            /* Vorbis stream assignment:
             * Stream 0 (coupled): L, R -> slots 0, 1
             * Stream 1 (uncoupled): C  -> slot 2
             * Stream 2 (coupled): BL, BR -> slots 3, 4
             * Stream 3 (uncoupled): LFE -> slot 5
             */
            cfg->mapping[0] = 0; /* FL  */
            cfg->mapping[1] = 2; /* C   */
            cfg->mapping[2] = 1; /* FR  */
            cfg->mapping[3] = 3; /* BL  */
            cfg->mapping[4] = 4; /* BR  */
            cfg->mapping[5] = 5; /* LFE */
            break;
        case 8: /* 7.1 Surround */
            cfg->streams = 5;
            cfg->coupled_streams = 3;
            cfg->mapping[0] = 0; /* FL  */
            cfg->mapping[1] = 2; /* C   */
            cfg->mapping[2] = 1; /* FR  */
            cfg->mapping[3] = 3; /* BL  */
            cfg->mapping[4] = 4; /* BR  */
            cfg->mapping[5] = 5; /* SL  */
            cfg->mapping[6] = 6; /* SR  */
            cfg->mapping[7] = 7; /* LFE */
            break;
        default: {
            int count = (channels > 8) ? 8 : (channels < 1 ? 1 : channels);
            cfg->streams = count;
            cfg->coupled_streams = 0;
            for (int i = 0; i < count; ++i) {
                cfg->mapping[i] = (unsigned char)i;
            }
            break;
        }
    }
    return true;
}

/* Interleaves planar SMPTE floats into interleaved Vorbis array */
static void smpte_planar_to_vorbis_interleaved(
    const float* const* in_planes,
    float* out_interleaved,
    int channels,
    int frames
) {
    if (channels == 1) {
        memcpy(out_interleaved, in_planes[0], (size_t)frames * sizeof(float));
    } else if (channels == 2) {
        for (int i = 0; i < frames; ++i) {
            out_interleaved[i * 2 + 0] = in_planes[0][i];
            out_interleaved[i * 2 + 1] = in_planes[1][i];
        }
    } else if (channels == 4) {
        for (int i = 0; i < frames; ++i) {
            out_interleaved[i * 4 + 0] = in_planes[0][i];
            out_interleaved[i * 4 + 1] = in_planes[1][i];
            out_interleaved[i * 4 + 2] = in_planes[2][i];
            out_interleaved[i * 4 + 3] = in_planes[3][i];
        }
    } else if (channels == 6) {
        /* SMPTE: [FL, FR, C, LFE, BL, BR] -> Vorbis: [FL, C, FR, BL, BR, LFE] */
        for (int i = 0; i < frames; ++i) {
            out_interleaved[i * 6 + 0] = in_planes[0][i]; /* FL  */
            out_interleaved[i * 6 + 1] = in_planes[2][i]; /* C   */
            out_interleaved[i * 6 + 2] = in_planes[1][i]; /* FR  */
            out_interleaved[i * 6 + 3] = in_planes[4][i]; /* BL  */
            out_interleaved[i * 6 + 4] = in_planes[5][i]; /* BR  */
            out_interleaved[i * 6 + 5] = in_planes[3][i]; /* LFE */
        }
    } else if (channels == 8) {
        /* SMPTE: [FL, FR, C, LFE, BL, BR, SL, SR] -> Vorbis: [FL, C, FR, BL, BR, SL, SR, LFE] */
        for (int i = 0; i < frames; ++i) {
            out_interleaved[i * 8 + 0] = in_planes[0][i]; /* FL  */
            out_interleaved[i * 8 + 1] = in_planes[2][i]; /* C   */
            out_interleaved[i * 8 + 2] = in_planes[1][i]; /* FR  */
            out_interleaved[i * 8 + 3] = in_planes[4][i]; /* BL  */
            out_interleaved[i * 8 + 4] = in_planes[5][i]; /* BR  */
            out_interleaved[i * 8 + 5] = in_planes[6][i]; /* SL  */
            out_interleaved[i * 8 + 6] = in_planes[7][i]; /* SR  */
            out_interleaved[i * 8 + 7] = in_planes[3][i]; /* LFE */
        }
    } else {
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < channels; ++c) {
                out_interleaved[i * channels + c] = in_planes[c][i];
            }
        }
    }
}

/* De-interleaves Vorbis array into planar SMPTE floats */
static void vorbis_interleaved_to_smpte_planar(
    const float* in_interleaved,
    float* const* out_planes,
    int channels,
    int frames
) {
    if (channels == 1) {
        memcpy(out_planes[0], in_interleaved, (size_t)frames * sizeof(float));
    } else if (channels == 2) {
        for (int i = 0; i < frames; ++i) {
            out_planes[0][i] = in_interleaved[i * 2 + 0];
            out_planes[1][i] = in_interleaved[i * 2 + 1];
        }
    } else if (channels == 4) {
        for (int i = 0; i < frames; ++i) {
            out_planes[0][i] = in_interleaved[i * 4 + 0];
            out_planes[1][i] = in_interleaved[i * 4 + 1];
            out_planes[2][i] = in_interleaved[i * 4 + 2];
            out_planes[3][i] = in_interleaved[i * 4 + 3];
        }
    } else if (channels == 6) {
        /* Vorbis: [FL, C, FR, BL, BR, LFE] -> SMPTE: [FL, FR, C, LFE, BL, BR] */
        for (int i = 0; i < frames; ++i) {
            out_planes[0][i] = in_interleaved[i * 6 + 0]; /* FL  */
            out_planes[1][i] = in_interleaved[i * 6 + 2]; /* FR  */
            out_planes[2][i] = in_interleaved[i * 6 + 1]; /* C   */
            out_planes[3][i] = in_interleaved[i * 6 + 5]; /* LFE */
            out_planes[4][i] = in_interleaved[i * 6 + 3]; /* BL  */
            out_planes[5][i] = in_interleaved[i * 6 + 4]; /* BR  */
        }
    } else if (channels == 8) {
        /* Vorbis: [FL, C, FR, BL, BR, SL, SR, LFE] -> SMPTE: [FL, FR, C, LFE, BL, BR, SL, SR] */
        for (int i = 0; i < frames; ++i) {
            out_planes[0][i] = in_interleaved[i * 8 + 0]; /* FL  */
            out_planes[1][i] = in_interleaved[i * 8 + 2]; /* FR  */
            out_planes[2][i] = in_interleaved[i * 8 + 1]; /* C   */
            out_planes[3][i] = in_interleaved[i * 8 + 7]; /* LFE */
            out_planes[4][i] = in_interleaved[i * 8 + 3]; /* BL  */
            out_planes[5][i] = in_interleaved[i * 8 + 4]; /* BR  */
            out_planes[6][i] = in_interleaved[i * 8 + 5]; /* SL  */
            out_planes[7][i] = in_interleaved[i * 8 + 6]; /* SR  */
        }
    } else {
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < channels; ++c) {
                out_planes[c][i] = in_interleaved[i * channels + c];
            }
        }
    }
}

/* ========================================================================= */
/* 3. PURE C99 IMA ADPCM ENCODER / DECODER                                   */
/* ========================================================================= */

static const int ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int16_t float_to_s16(float f) {
    if (isnan(f)) return 0;
    if (f <= -1.0f) return -32767;
    if (f >= 1.0f) return 32767;
    return (int16_t)roundf(f * 32767.0f);
}

static inline float s16_to_float(int16_t s) {
    return (float)s / 32767.0f;
}

static inline uint8_t encode_ima_nibble(int16_t sample, int *predicted, int *index) {
    int diff = sample - *predicted;
    int sign = 0;
    if (diff < 0) {
        sign = 8;
        diff = -diff;
    }
    int step = ima_step_table[*index];
    int delta = 0;
    int vpdiff = step >> 3;

    if (diff >= step) {
        delta |= 4;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        delta |= 2;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        delta |= 1;
        vpdiff += step;
    }

    if (sign) *predicted -= vpdiff;
    else      *predicted += vpdiff;

    if (*predicted > 32767)  *predicted = 32767;
    if (*predicted < -32768) *predicted = -32768;

    *index += ima_index_table[delta];
    if (*index < 0)  *index = 0;
    if (*index > 88) *index = 88;

    return (uint8_t)(delta | sign);
}

static inline int16_t decode_ima_nibble(uint8_t nibble, int *predicted, int *index) {
    int step = ima_step_table[*index];
    int delta = nibble & 7;
    int vpdiff = step >> 3;

    if (delta & 4) vpdiff += step;
    if (delta & 2) vpdiff += (step >> 1);
    if (delta & 1) vpdiff += (step >> 2);

    if (nibble & 8) *predicted -= vpdiff;
    else            *predicted += vpdiff;

    if (*predicted > 32767)  *predicted = 32767;
    if (*predicted < -32768) *predicted = -32768;

    *index += ima_index_table[delta];
    if (*index < 0)  *index = 0;
    if (*index > 88) *index = 88;

    return (int16_t)*predicted;
}

/* Pure C99 4-bit IMA ADPCM encoder using 89-step adaptive delta quantization table. */
size_t avx_adpcm_encode_planar_float(
    const float* const* in_planes,
    int channels,
    int samples,
    uint8_t* out_buf,
    size_t max_out_bytes
) {
    if (!in_planes || channels < 1 || channels > OAVX_MAX_AUDIO_CHANNELS || samples <= 0 || !out_buf) return 0;

    /* Header:
     * 2 bytes magic (0x4441 = "AD")
     * 2 bytes channels
     * 4 bytes samples
     * Per channel: 2 bytes initial valprev + 1 byte initial index + 1 byte padding (4 bytes per ch)
     */
    size_t hdr_size = 8 + (size_t)channels * 4;
    size_t nibbles_per_ch = (samples > 1) ? (size_t)(samples - 1) : 0;
    size_t bytes_per_ch = (nibbles_per_ch + 1) / 2;
    size_t needed = hdr_size + (size_t)channels * bytes_per_ch;

    if (needed > max_out_bytes) return 0;

    out_buf[0] = 0x41; /* 'A' */
    out_buf[1] = 0x44; /* 'D' */
    out_buf[2] = (uint8_t)(channels & 0xFF);
    out_buf[3] = (uint8_t)((channels >> 8) & 0xFF);
    out_buf[4] = (uint8_t)(samples & 0xFF);
    out_buf[5] = (uint8_t)((samples >> 8) & 0xFF);
    out_buf[6] = (uint8_t)((samples >> 16) & 0xFF);
    out_buf[7] = (uint8_t)((samples >> 24) & 0xFF);

    size_t payload_offset = hdr_size;

    for (int c = 0; c < channels; ++c) {
        int predicted = float_to_s16(in_planes[c][0]);
        int index = 0;
        if (samples > 1) {
            int init_diff = abs(float_to_s16(in_planes[c][1]) - predicted);
            while (index < 88 && ima_step_table[index + 1] <= init_diff) {
                index++;
            }
        }

        /* Write initial state */
        size_t st_off = 8 + (size_t)c * 4;
        out_buf[st_off + 0] = (uint8_t)(predicted & 0xFF);
        out_buf[st_off + 1] = (uint8_t)((predicted >> 8) & 0xFF);
        out_buf[st_off + 2] = (uint8_t)index;
        out_buf[st_off + 3] = 0;

        /* Encode remaining samples as packed 4-bit nibbles */
        size_t ch_out = payload_offset + (size_t)c * bytes_per_ch;
        for (int s = 1; s < samples; s += 2) {
            uint8_t n0 = encode_ima_nibble(float_to_s16(in_planes[c][s]), &predicted, &index);
            uint8_t n1 = 0;
            if (s + 1 < samples) {
                n1 = encode_ima_nibble(float_to_s16(in_planes[c][s + 1]), &predicted, &index);
            }
            out_buf[ch_out++] = (uint8_t)(n0 | (n1 << 4));
        }
    }

    return needed;
}

int avx_adpcm_decode_to_planar_float(
    const uint8_t* in_buf,
    size_t in_bytes,
    int channels,
    float* const* out_planes,
    int max_samples
) {
    if (!in_buf || in_bytes < 8 || channels < 1 || channels > OAVX_MAX_AUDIO_CHANNELS || !out_planes || max_samples <= 0) return -1;

    if (in_buf[0] != 0x41 || in_buf[1] != 0x44) return -1;
    uint32_t pkt_ch = (uint32_t)in_buf[2] | ((uint32_t)in_buf[3] << 8);
    uint32_t samples_u = (uint32_t)in_buf[4] | ((uint32_t)in_buf[5] << 8) |
                         ((uint32_t)in_buf[6] << 16) | ((uint32_t)in_buf[7] << 24);

    if (pkt_ch != (uint32_t)channels || samples_u == 0 || samples_u > (uint32_t)max_samples) return -1;
    int samples = (int)samples_u;

    size_t hdr_size = 8 + (size_t)channels * 4;
    size_t nibbles_per_ch = (samples > 1) ? (size_t)(samples - 1) : 0;
    size_t bytes_per_ch = (nibbles_per_ch + 1) / 2;
    size_t expected_size = hdr_size + (size_t)channels * bytes_per_ch;

    if (in_bytes < expected_size) return -1;

    size_t payload_offset = hdr_size;

    for (int c = 0; c < channels; ++c) {
        size_t st_off = 8 + (size_t)c * 4;
        int predicted = (int16_t)(in_buf[st_off + 0] | (in_buf[st_off + 1] << 8));
        int index = in_buf[st_off + 2];
        if (index > 88) index = 88;

        out_planes[c][0] = s16_to_float((int16_t)predicted);

        size_t ch_in = payload_offset + (size_t)c * bytes_per_ch;
        for (int s = 1; s < samples; s += 2) {
            uint8_t byte = in_buf[ch_in++];
            uint8_t n0 = byte & 0x0F;
            uint8_t n1 = (byte >> 4) & 0x0F;

            out_planes[c][s] = s16_to_float(decode_ima_nibble(n0, &predicted, &index));
            if (s + 1 < samples) {
                out_planes[c][s + 1] = s16_to_float(decode_ima_nibble(n1, &predicted, &index));
            }
        }
    }

    return samples;
}

/* ========================================================================= */
/* 4. HIGH-LEVEL AUDIO ENCODER & DECODER IMPLEMENTATION                      */
/* ========================================================================= */

#define OPUS_FRAME_SIZE_48K 960 /* 20 ms @ 48 kHz */
#define MAX_AUDIO_CHANNELS  OAVX_MAX_AUDIO_CHANNELS
#define ACCUM_CAPACITY      19200 /* 400 ms cushion */

struct avx_audio_encoder_t {
    int sample_rate;
    int channels;
    avx_audio_codec_type_e codec_type;
    uint32_t fourcc;
    int bitrate_kbps;

    /* Opus Multistream State */
    void *opus_ms_enc;
    float *interleaved_pcm;

    /* Sample Accumulator for irregular frame sizes */
    float *accum_planes[MAX_AUDIO_CHANNELS];
    int accum_samples;
    int64_t accum_start_pts;
};

avx_audio_encoder_t* avx_audio_encoder_create(
    int sample_rate,
    int channels,
    avx_audio_codec_type_e preferred_codec,
    int bitrate_kbps
) {
    if (sample_rate <= 0 || channels < 1 || channels > MAX_AUDIO_CHANNELS) return NULL;

    avx_audio_encoder_t *enc = (avx_audio_encoder_t*)calloc(1, sizeof(*enc));
    if (!enc) return NULL;

    enc->sample_rate = sample_rate;
    enc->channels = channels;
    enc->bitrate_kbps = bitrate_kbps;

    for (int c = 0; c < channels; ++c) {
        enc->accum_planes[c] = (float*)malloc((size_t)ACCUM_CAPACITY * sizeof(float));
        if (!enc->accum_planes[c]) {
            avx_audio_encoder_destroy(enc);
            return NULL;
        }
    }

    /* Decide codec */
    bool want_opus = (preferred_codec == AVX_AUDIO_CODEC_OPUS);
    bool opus_ok = false;

    if (want_opus && avx_audio_codec_has_opus()) {
        opus_channel_config_t cfg;
        if (get_opus_channel_config(channels, &cfg)) {
            int err = OPUS_OK;
            enc->opus_ms_enc = g_opus.ms_encoder_create(
                sample_rate, channels, cfg.streams, cfg.coupled_streams,
                cfg.mapping, OPUS_APPLICATION_AUDIO, &err
            );
            if (enc->opus_ms_enc && err == OPUS_OK) {
                opus_ok = true;
                int target_br = bitrate_kbps > 0 ? (bitrate_kbps * 1000) : (64000 * channels);
                g_opus.ms_encoder_ctl(enc->opus_ms_enc, OPUS_SET_BITRATE_REQUEST, target_br);
                g_opus.ms_encoder_ctl(enc->opus_ms_enc, OPUS_SET_COMPLEXITY_REQUEST, 10);
                g_opus.ms_encoder_ctl(enc->opus_ms_enc, OPUS_SET_SIGNAL_REQUEST, OPUS_SIGNAL_MUSIC);

                enc->interleaved_pcm = (float*)malloc((size_t)OPUS_FRAME_SIZE_48K * (size_t)channels * sizeof(float));
                if (!enc->interleaved_pcm) {
                    avx_audio_encoder_destroy(enc);
                    return NULL;
                }
            }
        }
    }

    if (opus_ok) {
        enc->codec_type = AVX_AUDIO_CODEC_OPUS;
        enc->fourcc = OAVX_FOURCC_AUDIO_TYPE_OPUS;
    } else {
        enc->codec_type = AVX_AUDIO_CODEC_ADPCM;
        enc->fourcc = OAVX_FOURCC_AUDIO_TYPE_ADPC;
    }

    return enc;
}

void avx_audio_encoder_destroy(avx_audio_encoder_t* enc) {
    if (!enc) return;
    if (enc->opus_ms_enc && g_opus.ms_encoder_destroy) {
        g_opus.ms_encoder_destroy(enc->opus_ms_enc);
    }
    free(enc->interleaved_pcm);
    for (int c = 0; c < MAX_AUDIO_CHANNELS; ++c) {
        free(enc->accum_planes[c]);
    }
    free(enc);
}

avx_audio_codec_type_e avx_audio_encoder_get_codec(const avx_audio_encoder_t* enc) {
    return enc ? enc->codec_type : AVX_AUDIO_CODEC_NONE;
}

uint32_t avx_audio_encoder_get_fourcc(const avx_audio_encoder_t* enc) {
    return enc ? enc->fourcc : 0;
}

/* Buffers planar PCM samples into accumulation queue with timestamp alignment for 20 ms frames. */
bool avx_audio_encoder_feed(
    avx_audio_encoder_t* enc,
    const float* const* planes,
    int frames,
    int64_t pts_ns
) {
    if (!enc || !planes || frames <= 0) return false;

    if (enc->accum_samples + frames > ACCUM_CAPACITY) {
        /* Drop oldest or clamp to prevent overflow */
        enc->accum_samples = 0;
    }

    if (enc->accum_samples == 0) {
        enc->accum_start_pts = pts_ns;
    }

    for (int c = 0; c < enc->channels; ++c) {
        memcpy(enc->accum_planes[c] + enc->accum_samples, planes[c], (size_t)frames * sizeof(float));
    }
    enc->accum_samples += frames;
    return true;
}

/* Encodes accumulated 20 ms PCM frames (960 samples @ 48kHz) via multistream Opus or IMA ADPCM. */
bool avx_audio_encoder_pull(
    avx_audio_encoder_t* enc,
    uint8_t* out_pkt,
    size_t max_pkt_size,
    size_t* out_pkt_size,
    int* out_samples,
    int64_t* out_pts_ns,
    uint32_t* out_codec_fourcc
) {
    if (!enc || !out_pkt || !out_pkt_size || !out_samples || !out_pts_ns || !out_codec_fourcc) {
        return false;
    }

    if (enc->codec_type == AVX_AUDIO_CODEC_ADPCM) {
        if (enc->accum_samples <= 0) return false;

        size_t encoded = avx_adpcm_encode_planar_float(
            (const float* const*)enc->accum_planes,
            enc->channels,
            enc->accum_samples,
            out_pkt,
            max_pkt_size
        );
        if (encoded == 0) return false;

        *out_pkt_size = encoded;
        *out_samples = enc->accum_samples;
        *out_pts_ns = enc->accum_start_pts;
        *out_codec_fourcc = enc->fourcc;

        enc->accum_samples = 0;
        return true;
    }

    if (enc->codec_type == AVX_AUDIO_CODEC_OPUS) {
        if (enc->accum_samples < OPUS_FRAME_SIZE_48K) return false;

        /* Interleave 960 samples with SMPTE -> Vorbis ordering */
        smpte_planar_to_vorbis_interleaved(
            (const float* const*)enc->accum_planes,
            enc->interleaved_pcm,
            enc->channels,
            OPUS_FRAME_SIZE_48K
        );

        int bytes = g_opus.ms_encode_float(
            enc->opus_ms_enc,
            enc->interleaved_pcm,
            OPUS_FRAME_SIZE_48K,
            out_pkt,
            (int)max_pkt_size
        );
        if (bytes <= 0) return false;

        *out_pkt_size = (size_t)bytes;
        *out_samples = OPUS_FRAME_SIZE_48K;
        *out_pts_ns = enc->accum_start_pts;
        *out_codec_fourcc = enc->fourcc;

        /* Shift remaining samples down */
        enc->accum_samples -= OPUS_FRAME_SIZE_48K;
        if (enc->accum_samples > 0) {
            for (int c = 0; c < enc->channels; ++c) {
                memmove(enc->accum_planes[c],
                        enc->accum_planes[c] + OPUS_FRAME_SIZE_48K,
                        (size_t)enc->accum_samples * sizeof(float));
            }
            int64_t step_ns = (int64_t)((double)OPUS_FRAME_SIZE_48K * 1000000000.0 / enc->sample_rate);
            enc->accum_start_pts += step_ns;
        }

        return true;
    }

    return false;
}

bool avx_audio_encoder_flush(
    avx_audio_encoder_t* enc,
    uint8_t* out_pkt,
    size_t max_pkt_size,
    size_t* out_pkt_size,
    int* out_samples,
    int64_t* out_pts_ns,
    uint32_t* out_codec_fourcc
) {
    if (!enc || enc->accum_samples <= 0) return false;

    if (enc->codec_type == AVX_AUDIO_CODEC_ADPCM) {
        return avx_audio_encoder_pull(enc, out_pkt, max_pkt_size, out_pkt_size, out_samples, out_pts_ns, out_codec_fourcc);
    }

    if (enc->codec_type == AVX_AUDIO_CODEC_OPUS) {
        /* Zero pad to 960 */
        int pad = OPUS_FRAME_SIZE_48K - enc->accum_samples;
        if (pad > 0) {
            for (int c = 0; c < enc->channels; ++c) {
                memset(enc->accum_planes[c] + enc->accum_samples, 0, (size_t)pad * sizeof(float));
            }
            enc->accum_samples = OPUS_FRAME_SIZE_48K;
        }
        return avx_audio_encoder_pull(enc, out_pkt, max_pkt_size, out_pkt_size, out_samples, out_pts_ns, out_codec_fourcc);
    }

    return false;
}

/* ========================================================================= */
/* 5. AUDIO DECODER IMPLEMENTATION                                           */
/* ========================================================================= */

struct avx_audio_decoder_t {
    int sample_rate;
    int channels;
    uint32_t fourcc;

    /* Opus Multistream State */
    void *opus_ms_dec;
    float *interleaved_pcm;
};

avx_audio_decoder_t* avx_audio_decoder_create(
    int sample_rate,
    int channels,
    uint32_t codec_fourcc
) {
    if (sample_rate <= 0 || channels < 1 || channels > MAX_AUDIO_CHANNELS) return NULL;

    avx_audio_decoder_t *dec = (avx_audio_decoder_t*)calloc(1, sizeof(*dec));
    if (!dec) return NULL;

    dec->sample_rate = sample_rate;
    dec->channels = channels;
    dec->fourcc = codec_fourcc;

    if (codec_fourcc == OAVX_FOURCC_AUDIO_TYPE_OPUS && avx_audio_codec_has_opus()) {
        opus_channel_config_t cfg;
        if (get_opus_channel_config(channels, &cfg)) {
            int err = OPUS_OK;
            dec->opus_ms_dec = g_opus.ms_decoder_create(
                sample_rate, channels, cfg.streams, cfg.coupled_streams,
                cfg.mapping, &err
            );
            if (dec->opus_ms_dec && err == OPUS_OK) {
                /* Max frame size: 5760 samples (120ms) */
                dec->interleaved_pcm = (float*)malloc(5760 * (size_t)channels * sizeof(float));
                if (!dec->interleaved_pcm) {
                    avx_audio_decoder_destroy(dec);
                    return NULL;
                }
            }
        }
    }

    return dec;
}

void avx_audio_decoder_destroy(avx_audio_decoder_t* dec) {
    if (!dec) return;
    if (dec->opus_ms_dec && g_opus.ms_decoder_destroy) {
        g_opus.ms_decoder_destroy(dec->opus_ms_dec);
    }
    free(dec->interleaved_pcm);
    free(dec);
}

int avx_audio_decoder_decode(
    avx_audio_decoder_t* dec,
    uint32_t codec_fourcc,
    const uint8_t* pkt_data,
    size_t pkt_size,
    float* const* out_planes,
    int max_samples
) {
    if (!dec || !pkt_data || pkt_size == 0 || !out_planes || max_samples <= 0) return -1;

    if (codec_fourcc == OAVX_FOURCC_AUDIO_TYPE_ADPC) {
        return avx_adpcm_decode_to_planar_float(pkt_data, pkt_size, dec->channels, out_planes, max_samples);
    }

    if (codec_fourcc == OAVX_FOURCC_AUDIO_TYPE_OPUS) {
        if (!dec->opus_ms_dec || !g_opus.ms_decode_float || !dec->interleaved_pcm) return -1;

        int decoded_frames = g_opus.ms_decode_float(
            dec->opus_ms_dec,
            pkt_data,
            (int)pkt_size,
            dec->interleaved_pcm,
            max_samples,
            0
        );
        if (decoded_frames <= 0) return -1;

        vorbis_interleaved_to_smpte_planar(
            dec->interleaved_pcm,
            out_planes,
            dec->channels,
            decoded_frames
        );
        return decoded_frames;
    }

    return -1;
}
