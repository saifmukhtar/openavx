/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/avx_audio_output.c - Audio playback backend (PulseAudio, ALSA, pipe)
 */

#define _GNU_SOURCE
#include "avx_platform.h"
#include "avx_audio_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PulseAudio Simple API Types */
typedef struct pa_sample_spec_t {
    int format;         /* 5 = PA_SAMPLE_FLOAT32LE */
    uint32_t rate;
    uint8_t channels;
} pa_sample_spec_t;

typedef void* (*pa_simple_new_fn)(const char*, const char*, int, const char*, const char*, const pa_sample_spec_t*, const void*, const void*, int*);
typedef int   (*pa_simple_write_fn)(void*, const void*, size_t, int*);
typedef void  (*pa_simple_free_fn)(void*);
typedef int   (*pa_simple_drain_fn)(void*, int*);

/* ALSA PCM API Types */
typedef void snd_pcm_t;
typedef int  (*snd_pcm_open_fn)(snd_pcm_t**, const char*, int, int);
typedef int  (*snd_pcm_set_params_fn)(snd_pcm_t*, int, int, unsigned int, unsigned int, int, unsigned int);
typedef long (*snd_pcm_writei_fn)(snd_pcm_t*, const void*, unsigned long);
typedef int  (*snd_pcm_recover_fn)(snd_pcm_t*, int, int);
typedef int  (*snd_pcm_close_fn)(snd_pcm_t*);

struct avx_audio_output_t {
    avx_audio_backend_e backend;
    char backend_name[64];
    int sample_rate;
    int channels;

    /* Interleave buffer */
    float* interleave_buf;
    size_t interleave_cap;

    /* PulseAudio Context */
    void* pulse_lib;
    void* pulse_handle;
    pa_simple_write_fn pulse_write;
    pa_simple_free_fn  pulse_free;

    /* ALSA Context */
    void* alsa_lib;
    snd_pcm_t* alsa_handle;
    snd_pcm_writei_fn  alsa_writei;
    snd_pcm_recover_fn alsa_recover;
    snd_pcm_close_fn   alsa_close;
    int (*alsa_free_global)(void);

    /* Subprocess Pipe Context */
    FILE* pipe_fp;
};

static bool init_pulse(avx_audio_output_t* ao) {
    const char* libs[] = { "libpulse-simple.so.0", "libpulse-simple.so", NULL };
    for (int i = 0; libs[i]; ++i) {
        ao->pulse_lib = avx_dlopen(libs[i]);
        if (ao->pulse_lib) break;
    }
    if (!ao->pulse_lib) return false;

    pa_simple_new_fn p_new = (pa_simple_new_fn)avx_dlsym(ao->pulse_lib, "pa_simple_new");
    ao->pulse_write = (pa_simple_write_fn)avx_dlsym(ao->pulse_lib, "pa_simple_write");
    ao->pulse_free = (pa_simple_free_fn)avx_dlsym(ao->pulse_lib, "pa_simple_free");

    if (!p_new || !ao->pulse_write || !ao->pulse_free) {
        avx_dlclose(ao->pulse_lib);
        ao->pulse_lib = NULL;
        return false;
    }

    pa_sample_spec_t ss = {
        .format = 5, /* PA_SAMPLE_FLOAT32LE */
        .rate = (uint32_t)ao->sample_rate,
        .channels = (uint8_t)ao->channels
    };

    int err = 0;
    ao->pulse_handle = p_new(NULL, "openavx-monitor", 1 /* PA_STREAM_PLAYBACK */,
                             NULL, "OpenAVX Live Audio", &ss, NULL, NULL, &err);
    if (!ao->pulse_handle) {
        avx_dlclose(ao->pulse_lib);
        ao->pulse_lib = NULL;
        return false;
    }

    snprintf(ao->backend_name, sizeof(ao->backend_name), "PulseAudio (libpulse-simple)");
    ao->backend = AVX_AUDIO_BACKEND_PULSE;
    return true;
}

static bool init_alsa(avx_audio_output_t* ao) {
    const char* libs[] = { "libasound.so.2", "libasound.so", NULL };
    for (int i = 0; libs[i]; ++i) {
        ao->alsa_lib = avx_dlopen(libs[i]);
        if (ao->alsa_lib) break;
    }
    if (!ao->alsa_lib) return false;

    snd_pcm_open_fn p_open = (snd_pcm_open_fn)avx_dlsym(ao->alsa_lib, "snd_pcm_open");
    snd_pcm_set_params_fn p_params = (snd_pcm_set_params_fn)avx_dlsym(ao->alsa_lib, "snd_pcm_set_params");
    ao->alsa_writei = (snd_pcm_writei_fn)avx_dlsym(ao->alsa_lib, "snd_pcm_writei");
    ao->alsa_recover = (snd_pcm_recover_fn)avx_dlsym(ao->alsa_lib, "snd_pcm_recover");
    ao->alsa_close = (snd_pcm_close_fn)avx_dlsym(ao->alsa_lib, "snd_pcm_close");
    ao->alsa_free_global = (int (*)(void))avx_dlsym(ao->alsa_lib, "snd_config_update_free_global");

    if (!p_open || !p_params || !ao->alsa_writei || !ao->alsa_close) {
        avx_dlclose(ao->alsa_lib);
        ao->alsa_lib = NULL;
        return false;
    }

    /* Open default playback device */
    if (p_open(&ao->alsa_handle, "default", 0 /* SND_PCM_STREAM_PLAYBACK */, 0) != 0) {
        avx_dlclose(ao->alsa_lib);
        ao->alsa_lib = NULL;
        return false;
    }

    /* Format 14 = SND_PCM_FORMAT_FLOAT_LE, Access 3 = SND_PCM_ACCESS_RW_INTERLEAVED */
    int ret = p_params(ao->alsa_handle, 14, 3, (unsigned int)ao->channels,
                       (unsigned int)ao->sample_rate, 1, 50000 /* 50ms latency */);
    if (ret != 0) {
        ao->alsa_close(ao->alsa_handle);
        ao->alsa_handle = NULL;
        avx_dlclose(ao->alsa_lib);
        ao->alsa_lib = NULL;
        return false;
    }

    snprintf(ao->backend_name, sizeof(ao->backend_name), "ALSA (libasound.so.2)");
    ao->backend = AVX_AUDIO_BACKEND_ALSA;
    return true;
}

static bool init_pipe(avx_audio_output_t* ao) {
    char cmd[256];
    if (access("/bin/pw-play", X_OK) == 0 || access("/usr/bin/pw-play", X_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "pw-play --format f32 --rate %d --channels %d - >/dev/null 2>&1",
                 ao->sample_rate, ao->channels);
        ao->pipe_fp = popen(cmd, "w");
        if (ao->pipe_fp) {
            snprintf(ao->backend_name, sizeof(ao->backend_name), "Pipe (pw-play)");
            ao->backend = AVX_AUDIO_BACKEND_PIPE;
            return true;
        }
    }

    if (access("/bin/aplay", X_OK) == 0 || access("/usr/bin/aplay", X_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "aplay -q -t raw -f FLOAT_LE -r %d -c %d - >/dev/null 2>&1",
                 ao->sample_rate, ao->channels);
        ao->pipe_fp = popen(cmd, "w");
        if (ao->pipe_fp) {
            snprintf(ao->backend_name, sizeof(ao->backend_name), "Pipe (aplay)");
            ao->backend = AVX_AUDIO_BACKEND_PIPE;
            return true;
        }
    }

    return false;
}

avx_audio_output_t* avx_audio_output_create(const char* backend_name, int sample_rate, int channels) {
    if (sample_rate <= 0) sample_rate = 48000;
    if (channels <= 0) channels = 2;

    avx_audio_output_t* ao = (avx_audio_output_t*)calloc(1, sizeof(*ao));
    if (!ao) return NULL;

    ao->sample_rate = sample_rate;
    ao->channels = channels;
    ao->interleave_buf = NULL;
    ao->interleave_cap = 0;

    bool req_pulse = backend_name && (strcmp(backend_name, "pulse") == 0 || strcmp(backend_name, "pulseaudio") == 0);
    bool req_alsa  = backend_name && (strcmp(backend_name, "alsa") == 0);
    bool req_pipe  = backend_name && (strcmp(backend_name, "pipe") == 0);
    bool req_none  = backend_name && (strcmp(backend_name, "none") == 0 || strcmp(backend_name, "null") == 0);
    bool req_auto  = !backend_name || strcmp(backend_name, "auto") == 0 || backend_name[0] == '\0';

    if (req_none) {
        ao->backend = AVX_AUDIO_BACKEND_NONE;
        snprintf(ao->backend_name, sizeof(ao->backend_name), "Disabled (Null Sink)");
        return ao;
    }

    if (req_pulse || req_auto) {
        if (init_pulse(ao)) return ao;
    }

    if (req_alsa || req_auto) {
        if (init_alsa(ao)) return ao;
    }

    if (req_pipe || req_auto) {
        if (init_pipe(ao)) return ao;
    }

    /* Fallback to null sink */
    ao->backend = AVX_AUDIO_BACKEND_NONE;
    snprintf(ao->backend_name, sizeof(ao->backend_name), "Disabled (No Audio Device)");
    return ao;
}

bool avx_audio_output_write(avx_audio_output_t* ao, const oavx_audio_frame_t* aframe) {
    if (!ao || !aframe || !aframe->planes[0] || aframe->frames == 0 || aframe->channels == 0) {
        return false;
    }
    if (ao->backend == AVX_AUDIO_BACKEND_NONE) {
        return true; /* Silently discard */
    }

    /* Reallocate scratch interleave buffer */
    size_t total_samples = (size_t)aframe->frames * aframe->channels;
    if (ao->interleave_cap < total_samples) {
        float* nb = (float*)realloc(ao->interleave_buf, total_samples * sizeof(float));
        if (!nb) return false;
        ao->interleave_buf = nb;
        ao->interleave_cap = total_samples;
    }

    /* Interleave if planar */
    if (aframe->format == OAVX_AUDIO_FMT_FLT_INTERLEAVED) {
        memcpy(ao->interleave_buf, aframe->planes[0], total_samples * sizeof(float));
    } else {
        for (uint32_t s = 0; s < aframe->frames; ++s) {
            for (uint16_t ch = 0; ch < aframe->channels; ++ch) {
                const float* ch_ptr;
                if (ch < 8 && aframe->planes[ch]) {
                    ch_ptr = (const float*)aframe->planes[ch];
                    ao->interleave_buf[s * aframe->channels + ch] = ch_ptr[s];
                } else if (aframe->stride_bytes > 0) {
                    ch_ptr = (const float*)((const char*)aframe->planes[0] + ch * aframe->stride_bytes);
                    ao->interleave_buf[s * aframe->channels + ch] = ch_ptr[s];
                } else {
                    ao->interleave_buf[s * aframe->channels + ch] = 0.0f;
                }
            }
        }
    }

    size_t byte_count = total_samples * sizeof(float);

    if (ao->backend == AVX_AUDIO_BACKEND_PULSE && ao->pulse_handle && ao->pulse_write) {
        int err = 0;
        int r = ao->pulse_write(ao->pulse_handle, ao->interleave_buf, byte_count, &err);
        return (r == 0);
    } else if (ao->backend == AVX_AUDIO_BACKEND_ALSA && ao->alsa_handle && ao->alsa_writei) {
        long written = ao->alsa_writei(ao->alsa_handle, ao->interleave_buf, (unsigned long)aframe->frames);
        if (written < 0 && ao->alsa_recover) {
            written = ao->alsa_recover(ao->alsa_handle, (int)written, 1);
            if (written == 0) {
                ao->alsa_writei(ao->alsa_handle, ao->interleave_buf, (unsigned long)aframe->frames);
            }
        }
        return (written >= 0);
    } else if (ao->backend == AVX_AUDIO_BACKEND_PIPE && ao->pipe_fp) {
        size_t w = fwrite(ao->interleave_buf, sizeof(float), total_samples, ao->pipe_fp);
        return (w == total_samples);
    }

    return true;
}

const char* avx_audio_output_get_backend_name(const avx_audio_output_t* ao) {
    if (!ao) return "None";
    return ao->backend_name;
}

void avx_audio_output_destroy(avx_audio_output_t* ao) {
    if (!ao) return;

    if (ao->pulse_handle && ao->pulse_free) {
        ao->pulse_free(ao->pulse_handle);
        ao->pulse_handle = NULL;
    }
    if (ao->pulse_lib) {
        avx_dlclose(ao->pulse_lib);
        ao->pulse_lib = NULL;
    }

    if (ao->alsa_handle && ao->alsa_close) {
        ao->alsa_close(ao->alsa_handle);
        ao->alsa_handle = NULL;
    }
    if (ao->alsa_free_global) {
        ao->alsa_free_global();
    }
    if (ao->alsa_lib) {
        avx_dlclose(ao->alsa_lib);
        ao->alsa_lib = NULL;
    }

    if (ao->pipe_fp) {
        pclose(ao->pipe_fp);
        ao->pipe_fp = NULL;
    }

    if (ao->interleave_buf) {
        free(ao->interleave_buf);
        ao->interleave_buf = NULL;
        ao->interleave_cap = 0;
    }

    free(ao);
}
