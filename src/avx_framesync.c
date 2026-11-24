/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_framesync.c - Frame synchronizer and clock drift compensation
 */

#include "openavx.h"
#include "avx_internal.h"
#include "avx_platform.h"
#include "avx_audio.h"
#include "avx_speedhq.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_AUDIO_CHANNELS OAVX_MAX_AUDIO_CHANNELS
#define AUDIO_RING_CAPACITY (192000) /* 4 seconds at 48kHz */

typedef struct avx_framesync_t {
    avx_rx_instance_t p_recv;
    avx_mutex_t drain_lock;
    avx_mutex_t video_lock;
    avx_mutex_t audio_lock;

    /* Cached Video Frame */
    bool has_video;
    oavx_video_frame_t cached_video;
    uint8_t* video_data_buf;
    size_t video_data_buf_size;
    char cached_metadata[1024];

    /* Caller delivery buffer for video */
    uint8_t* delivery_video_buf;
    size_t delivery_video_buf_size;

    /* Cached Audio Jitter Buffer (Planar) */
    int source_sample_rate;
    int source_channels;
    avx_audio_jitter_t* p_jitter_buf;

    /* Active Resampler (if requested rate != source rate) */
    avx_resampler_t* p_resampler;
    int resampler_in_rate;
    int resampler_out_rate;
    int resampler_channels;

    /* Caller delivery buffer for audio */
    float* delivery_audio_buf;
    size_t delivery_audio_buf_size;
} avx_framesync_t;

static inline size_t compute_video_frame_bytes(const oavx_video_frame_t* vf) {
    if (!vf || vf->width == 0 || vf->height == 0) return 0;
    uint32_t w = vf->width;
    uint32_t h = vf->height;

    switch (vf->fourcc) {
        case OAVX_FOURCC_VIDEO_TYPE_H264:
        case OAVX_FOURCC_VIDEO_TYPE_HEVC:
        case OAVX_FOURCC_VIDEO_TYPE_AV01:
            return vf->strides[0];
        case OAVX_FOURCC_VIDEO_TYPE_SHQ0:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ1:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ2:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ3:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ4:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ5:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ7:
        case OAVX_FOURCC_VIDEO_TYPE_SHQ9:
            return vf->strides[0] > 0 ? vf->strides[0] : (size_t)w * h * 2;
        case OAVX_FOURCC_VIDEO_TYPE_UYVA:
            return (size_t)w * h * 3;
        case OAVX_FOURCC_VIDEO_TYPE_P216:
            return (size_t)w * h * 4;
        case OAVX_FOURCC_VIDEO_TYPE_PA16:
            return (size_t)w * h * 6;
        case OAVX_FOURCC_VIDEO_TYPE_NV12:
        case OAVX_FOURCC_VIDEO_TYPE_I420:
        case OAVX_FOURCC_VIDEO_TYPE_YV12:
            return (size_t)w * h + ((size_t)(w + 1) / 2) * ((size_t)(h + 1) / 2) * 2;
        case OAVX_FOURCC_VIDEO_TYPE_RGBA:
        case OAVX_FOURCC_VIDEO_TYPE_RGBX:
        case OAVX_FOURCC_VIDEO_TYPE_BGRA:
        case OAVX_FOURCC_VIDEO_TYPE_BGRX:
            return (size_t)w * h * 4;
        case OAVX_FOURCC_VIDEO_TYPE_UYVY:
        default:
            return (size_t)w * h * 2;
    }
}

/* Drains incoming video and audio packets non-blocking from network receiver into jitter buffers. */
static void drain_receiver_frames(avx_framesync_t* fs) {
    if (!fs->p_recv) return;

    avx_mutex_lock(&fs->drain_lock);

    oavx_video_frame_t vf;
    oavx_audio_frame_t af;
    oavx_metadata_frame_t mf;

    while (true) {
        oavx_frame_type_t ftype = avx_rx_receive(fs->p_recv, &vf, &af, &mf, 0);
        if (ftype == OAVX_FRAME_TYPE_NONE || ftype == OAVX_FRAME_TYPE_ERROR) {
            break;
        }

        if (ftype == OAVX_FRAME_TYPE_VIDEO && vf.planes[0]) {
            size_t payload_bytes = compute_video_frame_bytes(&vf);

            avx_mutex_lock(&fs->video_lock);
            if (payload_bytes > 0 && fs->video_data_buf_size < payload_bytes) {
                uint8_t* nb = (uint8_t*)realloc(fs->video_data_buf, payload_bytes);
                if (nb) {
                    fs->video_data_buf = nb;
                    fs->video_data_buf_size = payload_bytes;
                }
            }

            if (payload_bytes > 0 && fs->video_data_buf && fs->video_data_buf_size >= payload_bytes) {
                if (vf.planes[1] && vf.planes[1] != (uint8_t*)vf.planes[0] + (size_t)vf.width * vf.height * 2 &&
                    vf.fourcc == OAVX_FOURCC_VIDEO_TYPE_UYVA) {
                    memcpy(fs->video_data_buf, vf.planes[0], (size_t)vf.width * vf.height * 2);
                    memcpy(fs->video_data_buf + (size_t)vf.width * vf.height * 2, vf.planes[1], (size_t)vf.width * vf.height);
                } else if (vf.planes[1] && vf.planes[1] != (uint8_t*)vf.planes[0] + (size_t)vf.width * vf.height &&
                           vf.fourcc == OAVX_FOURCC_VIDEO_TYPE_NV12) {
                    memcpy(fs->video_data_buf, vf.planes[0], (size_t)vf.width * vf.height);
                    memcpy(fs->video_data_buf + (size_t)vf.width * vf.height, vf.planes[1], (size_t)vf.width * ((vf.height + 1) / 2));
                } else {
                    memcpy(fs->video_data_buf, vf.planes[0], payload_bytes);
                }
                fs->cached_video = vf;
                avx_setup_raw_video_planes(&fs->cached_video, fs->video_data_buf, payload_bytes);

                if (vf.metadata && vf.metadata[0] != '\0') {
                    snprintf(fs->cached_metadata, sizeof(fs->cached_metadata), "%s", vf.metadata);
                    fs->cached_video.metadata = fs->cached_metadata;
                } else {
                    fs->cached_metadata[0] = '\0';
                    fs->cached_video.metadata = NULL;
                }
                fs->has_video = true;
            }
            avx_mutex_unlock(&fs->video_lock);
            avx_rx_free_video(fs->p_recv, &vf);
        } else if (ftype == OAVX_FRAME_TYPE_AUDIO && af.planes[0]) {
            int chs = (int)af.channels;
            if (chs > MAX_AUDIO_CHANNELS) chs = MAX_AUDIO_CHANNELS;
            int samples = (int)af.frames;

            avx_mutex_lock(&fs->audio_lock);
            if (!fs->p_jitter_buf || fs->source_sample_rate != (int)af.sample_rate || fs->source_channels != chs) {
                if (fs->p_jitter_buf) avx_audio_jitter_destroy(fs->p_jitter_buf);
                avx_audio_jitter_config_t jcfg = {
                    .sample_rate = (int)af.sample_rate,
                    .channels = chs,
                    .target_cushion_ms = 15,
                    .min_cushion_ms = 5,
                    .max_cushion_ms = 35
                };
                fs->p_jitter_buf = avx_audio_jitter_create(&jcfg);
                fs->source_sample_rate = (int)af.sample_rate;
                fs->source_channels = chs;
            }

            size_t stride = (af.stride_bytes > 0) ? (af.stride_bytes / sizeof(float)) : (size_t)samples;
            const float* in_ch[MAX_AUDIO_CHANNELS];
            for (int c = 0; c < chs; ++c) {
                in_ch[c] = af.planes[c] ? (const float*)af.planes[c] : ((const float*)af.planes[0] + c * stride);
            }
            for (int c = chs; c < MAX_AUDIO_CHANNELS; ++c) {
                in_ch[c] = NULL;
            }

            if (fs->p_jitter_buf) {
                avx_audio_jitter_push(fs->p_jitter_buf, in_ch, samples);
            }
            avx_mutex_unlock(&fs->audio_lock);
            avx_rx_free_audio(fs->p_recv, &af);
        } else if (ftype == OAVX_FRAME_TYPE_METADATA) {
            avx_rx_free_metadata(fs->p_recv, &mf);
        }
    }

    avx_mutex_unlock(&fs->drain_lock);
}

OPENAVX_API avx_sync_instance_t avx_sync_create(avx_rx_instance_t p_receiver) {
    if (!p_receiver) return NULL;

    avx_framesync_t* fs = (avx_framesync_t*)calloc(1, sizeof(avx_framesync_t));
    if (!fs) return NULL;

    fs->p_recv = p_receiver;
    avx_mutex_init(&fs->drain_lock);
    avx_mutex_init(&fs->video_lock);
    avx_mutex_init(&fs->audio_lock);
    fs->has_video = false;
    fs->source_sample_rate = 48000;
    fs->source_channels = 2;

    avx_audio_jitter_config_t jcfg = {
        .sample_rate = 48000,
        .channels = 2,
        .target_cushion_ms = 15,
        .min_cushion_ms = 5,
        .max_cushion_ms = 35
    };
    fs->p_jitter_buf = avx_audio_jitter_create(&jcfg);
    if (!fs->p_jitter_buf) {
        avx_sync_destroy(fs);
        return NULL;
    }

    return (avx_sync_instance_t)fs;
}

OPENAVX_API void avx_sync_destroy(avx_sync_instance_t p_instance) {
    if (!p_instance) return;
    avx_framesync_t* fs = (avx_framesync_t*)p_instance;

    avx_mutex_lock(&fs->drain_lock);
    avx_mutex_lock(&fs->video_lock);
    avx_mutex_lock(&fs->audio_lock);
    if (fs->video_data_buf) {
        free(fs->video_data_buf);
        fs->video_data_buf = NULL;
    }
    if (fs->delivery_video_buf) {
        free(fs->delivery_video_buf);
        fs->delivery_video_buf = NULL;
    }
    if (fs->p_jitter_buf) {
        avx_audio_jitter_destroy(fs->p_jitter_buf);
        fs->p_jitter_buf = NULL;
    }
    if (fs->p_resampler) {
        avx_resampler_destroy(fs->p_resampler);
        fs->p_resampler = NULL;
    }
    if (fs->delivery_audio_buf) {
        free(fs->delivery_audio_buf);
        fs->delivery_audio_buf = NULL;
    }
    avx_mutex_unlock(&fs->audio_lock);
    avx_mutex_unlock(&fs->video_lock);
    avx_mutex_unlock(&fs->drain_lock);

    avx_mutex_destroy(&fs->audio_lock);
    avx_mutex_destroy(&fs->video_lock);
    avx_mutex_destroy(&fs->drain_lock);
    free(fs);
}

/*
 * Delivers video frame on hardware display ticks, gracefully duplicating the previous
 * frame on network packet drop or transmission jitter to maintain constant output rate.
 */
void avx_sync_capture_video(
    avx_sync_instance_t p_instance,
    oavx_video_frame_t* p_video_data,
    int field_type
) {
    (void)field_type;
    if (!p_instance || !p_video_data) return;
    avx_framesync_t* fs = (avx_framesync_t*)p_instance;

    drain_receiver_frames(fs);

    avx_mutex_lock(&fs->video_lock);
    if (!fs->has_video || !fs->cached_video.planes[0]) {
        memset(p_video_data, 0, sizeof(*p_video_data));
        avx_mutex_unlock(&fs->video_lock);
        return;
    }

    size_t payload_bytes = compute_video_frame_bytes(&fs->cached_video);
    if (payload_bytes > 0 && fs->delivery_video_buf_size < payload_bytes) {
        uint8_t* nb = (uint8_t*)realloc(fs->delivery_video_buf, payload_bytes);
        if (nb) {
            fs->delivery_video_buf = nb;
            fs->delivery_video_buf_size = payload_bytes;
        }
    }

    if (payload_bytes > 0 && fs->delivery_video_buf && fs->delivery_video_buf_size >= payload_bytes) {
        memcpy(fs->delivery_video_buf, fs->cached_video.planes[0], payload_bytes);
        *p_video_data = fs->cached_video;
        avx_setup_raw_video_planes(p_video_data, fs->delivery_video_buf, payload_bytes);
    } else {
        memset(p_video_data, 0, sizeof(*p_video_data));
    }

    avx_mutex_unlock(&fs->video_lock);
}

void avx_sync_free_video(
    avx_sync_instance_t p_instance,
    oavx_video_frame_t* p_video_data
) {
    (void)p_instance;
    if (p_video_data) {
        p_video_data->planes[0] = NULL;
        p_video_data->planes[1] = NULL;
        p_video_data->planes[2] = NULL;
        p_video_data->planes[3] = NULL;
    }
}

/*
 * Pulls audio samples synchronized to DAC render clock, performing dynamic resampling
 * and jitter buffer drift slewing (+-500 PPM) to prevent buffer underrun/overflow.
 */
void avx_sync_capture_audio(
    avx_sync_instance_t p_instance,
    oavx_audio_frame_t* p_audio_data,
    int target_rate,
    int target_channels,
    int target_samples
) {
    if (!p_instance || !p_audio_data || target_rate <= 0 || target_channels <= 0 || target_samples <= 0) {
        return;
    }
    if (target_channels > MAX_AUDIO_CHANNELS) {
        target_channels = MAX_AUDIO_CHANNELS;
    }
    avx_framesync_t* fs = (avx_framesync_t*)p_instance;

    drain_receiver_frames(fs);

    avx_mutex_lock(&fs->audio_lock);

    /* Allocate or grow delivery audio buffer */
    size_t needed_bytes = (size_t)target_channels * target_samples * sizeof(float);
    if (fs->delivery_audio_buf_size < needed_bytes) {
        float* nb = (float*)realloc(fs->delivery_audio_buf, needed_bytes);
        if (nb) {
            fs->delivery_audio_buf = nb;
            fs->delivery_audio_buf_size = needed_bytes;
        }
    }

    if (!fs->delivery_audio_buf) {
        avx_mutex_unlock(&fs->audio_lock);
        return;
    }

    /* Check if sample rate conversion is required */
    if (target_rate != fs->source_sample_rate) {
        /* Setup / update resampler */
        if (!fs->p_resampler ||
            fs->resampler_in_rate != fs->source_sample_rate ||
            fs->resampler_out_rate != target_rate ||
            fs->resampler_channels != target_channels) {
            if (fs->p_resampler) {
                avx_resampler_destroy(fs->p_resampler);
            }
            fs->p_resampler = avx_resampler_create(
                fs->source_sample_rate, target_rate, target_channels, AVX_AUDIO_RESAMPLE_CUBIC
            );
            fs->resampler_in_rate = fs->source_sample_rate;
            fs->resampler_out_rate = target_rate;
            fs->resampler_channels = target_channels;
        }

        /* Calculate how many source samples correspond to target_samples */
        int needed_source_samples = (int)ceilf((float)target_samples * (float)fs->source_sample_rate / (float)target_rate);

        /* Prepare input channel pointers */
        float* in_ch[MAX_AUDIO_CHANNELS];
        for (int c = 0; c < target_channels; ++c) {
            in_ch[c] = (float*)malloc((size_t)needed_source_samples * sizeof(float));
        }
        for (int c = target_channels; c < MAX_AUDIO_CHANNELS; ++c) {
            in_ch[c] = NULL;
        }

        if (fs->p_jitter_buf) {
            avx_audio_jitter_pull(fs->p_jitter_buf, in_ch, needed_source_samples);
        } else {
            for (int c = 0; c < target_channels; ++c) {
                memset(in_ch[c], 0, (size_t)needed_source_samples * sizeof(float));
            }
        }

        /* Prepare output channel pointers */
        float* out_ch[MAX_AUDIO_CHANNELS];
        for (int c = 0; c < target_channels; ++c) {
            out_ch[c] = fs->delivery_audio_buf + (size_t)c * target_samples;
        }

        int produced = 0;
        if (fs->p_resampler && needed_source_samples > 0) {
            produced = avx_resampler_process(
                fs->p_resampler,
                (const float* const*)in_ch,
                needed_source_samples,
                out_ch,
                target_samples
            );
        }

        /* Pad remainder with silence if underflow */
        for (int c = 0; c < target_channels; ++c) {
            if (produced < target_samples) {
                memset(out_ch[c] + produced, 0, (size_t)(target_samples - produced) * sizeof(float));
            }
            free(in_ch[c]);
        }
    } else {
        /* Direct pull from adaptive jitter buffer (with micro-slewing & concealment) */
        float* out_ch[MAX_AUDIO_CHANNELS];
        for (int c = 0; c < target_channels; ++c) {
            out_ch[c] = fs->delivery_audio_buf + (size_t)c * target_samples;
        }
        for (int c = target_channels; c < MAX_AUDIO_CHANNELS; ++c) {
            out_ch[c] = NULL;
        }

        if (fs->p_jitter_buf) {
            avx_audio_jitter_pull(fs->p_jitter_buf, out_ch, target_samples);
        } else {
            for (int c = 0; c < target_channels; ++c) {
                memset(out_ch[c], 0, (size_t)(target_samples) * sizeof(float));
            }
        }
    }

    memset(p_audio_data, 0, sizeof(*p_audio_data));
    p_audio_data->sample_rate = (uint32_t)target_rate;
    p_audio_data->channels    = (uint16_t)target_channels;
    p_audio_data->frames      = (uint32_t)target_samples;
    p_audio_data->format      = OAVX_AUDIO_FMT_FLT_PLANAR;
    p_audio_data->stride_bytes = (size_t)target_samples * sizeof(float);
    p_audio_data->planes[0]   = fs->delivery_audio_buf;
    p_audio_data->channel_mask = avx_audio_layout_to_mask(avx_audio_layout_from_channels((uint16_t)target_channels));
    for (int c = 0; c < target_channels && c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
        p_audio_data->planes[c] = fs->delivery_audio_buf + (size_t)c * target_samples;
    }
    for (int c = target_channels; c < OAVX_MAX_AUDIO_CHANNELS; ++c) {
        p_audio_data->planes[c] = NULL;
    }
    p_audio_data->metadata    = NULL;
    p_audio_data->metadata_len = 0;
    p_audio_data->pts_ns      = 0;
    p_audio_data->user_ctx    = NULL;

    avx_mutex_unlock(&fs->audio_lock);
}

void avx_sync_free_audio(
    avx_sync_instance_t p_instance,
    oavx_audio_frame_t* p_audio_data
) {
    (void)p_instance;
    if (p_audio_data) {
        p_audio_data->planes[0] = NULL;
    }
}

int avx_sync_audio_queue_depth(avx_sync_instance_t p_instance) {
    if (!p_instance) return 0;
    avx_framesync_t* fs = (avx_framesync_t*)p_instance;

    drain_receiver_frames(fs);
    avx_mutex_lock(&fs->audio_lock);
    int depth = fs->p_jitter_buf ? avx_audio_jitter_get_depth_samples(fs->p_jitter_buf) : 0;
    avx_mutex_unlock(&fs->audio_lock);

    return depth;
}
