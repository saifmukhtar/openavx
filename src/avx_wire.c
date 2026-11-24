/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_wire.c - Wire protocol framing and profile selection
 */

#include "avx_wire.h"
#include "avx_platform.h"
#include <string.h>

static const oavx_wire_profile_t s_default_wire_profile = {
    .magic_video           = OAVX_WIRE_MAGIC_VIDEO,
    .magic_audio           = OAVX_WIRE_MAGIC_AUDIO,
    .magic_metadata        = OAVX_WIRE_MAGIC_METADATA,
    .magic_udp             = OAVX_WIRE_MAGIC_UDP,
    .compat_magic_video    = 0,
    .compat_magic_audio    = 0,
    .compat_magic_metadata = 0,
    .compat_magic_udp      = 0,
    .base_port             = OAVX_WIRE_DEFAULT_PORT,
    .mdns_service          = OAVX_WIRE_MDNS_SERVICE,
    .handshake_tag         = OAVX_WIRE_HANDSHAKE_TAG,
    .tally_tag             = OAVX_WIRE_TALLY_TAG,
    .color_info_tag        = OAVX_WIRE_COLOR_INFO_TAG,
    .ptz_tag               = OAVX_WIRE_PTZ_TAG,
    .record_tag            = OAVX_WIRE_RECORD_TAG
};

static oavx_wire_profile_t s_wire_profiles[2] = {
    {
        .magic_video           = OAVX_WIRE_MAGIC_VIDEO,
        .magic_audio           = OAVX_WIRE_MAGIC_AUDIO,
        .magic_metadata        = OAVX_WIRE_MAGIC_METADATA,
        .magic_udp             = OAVX_WIRE_MAGIC_UDP,
        .compat_magic_video    = 0,
        .compat_magic_audio    = 0,
        .compat_magic_metadata = 0,
        .compat_magic_udp      = 0,
        .base_port             = OAVX_WIRE_DEFAULT_PORT,
        .mdns_service          = OAVX_WIRE_MDNS_SERVICE,
        .handshake_tag         = OAVX_WIRE_HANDSHAKE_TAG,
        .tally_tag             = OAVX_WIRE_TALLY_TAG,
        .color_info_tag        = OAVX_WIRE_COLOR_INFO_TAG,
        .ptz_tag               = OAVX_WIRE_PTZ_TAG,
        .record_tag            = OAVX_WIRE_RECORD_TAG
    },
    {
        .magic_video           = OAVX_WIRE_MAGIC_VIDEO,
        .magic_audio           = OAVX_WIRE_MAGIC_AUDIO,
        .magic_metadata        = OAVX_WIRE_MAGIC_METADATA,
        .magic_udp             = OAVX_WIRE_MAGIC_UDP,
        .compat_magic_video    = 0,
        .compat_magic_audio    = 0,
        .compat_magic_metadata = 0,
        .compat_magic_udp      = 0,
        .base_port             = OAVX_WIRE_DEFAULT_PORT,
        .mdns_service          = OAVX_WIRE_MDNS_SERVICE,
        .handshake_tag         = OAVX_WIRE_HANDSHAKE_TAG,
        .tally_tag             = OAVX_WIRE_TALLY_TAG,
        .color_info_tag        = OAVX_WIRE_COLOR_INFO_TAG,
        .ptz_tag               = OAVX_WIRE_PTZ_TAG,
        .record_tag            = OAVX_WIRE_RECORD_TAG
    }
};
static volatile int s_active_profile_idx = 0;
static avx_mutex_t s_wire_profile_lock = AVX_MUTEX_INITIALIZER;

OPENAVX_API const oavx_wire_profile_t* avx_wire_get_default_profile(void) {
    return &s_default_wire_profile;
}

OPENAVX_API const oavx_wire_profile_t* avx_wire_get_active_profile(void) {
    int idx = s_active_profile_idx;
    return &s_wire_profiles[idx];
}

/* Performs atomic double-buffered swap of wire protocol profile using full memory barrier. */
OPENAVX_API void avx_wire_set_active_profile(const oavx_wire_profile_t* profile) {
    avx_mutex_lock(&s_wire_profile_lock);
    int next_idx = 1 - s_active_profile_idx;
    if (profile) {
        s_wire_profiles[next_idx] = *profile;
        if (!s_wire_profiles[next_idx].magic_video)    s_wire_profiles[next_idx].magic_video = OAVX_WIRE_MAGIC_VIDEO;
        if (!s_wire_profiles[next_idx].magic_audio)    s_wire_profiles[next_idx].magic_audio = OAVX_WIRE_MAGIC_AUDIO;
        if (!s_wire_profiles[next_idx].magic_metadata) s_wire_profiles[next_idx].magic_metadata = OAVX_WIRE_MAGIC_METADATA;
        if (!s_wire_profiles[next_idx].base_port)      s_wire_profiles[next_idx].base_port = OAVX_WIRE_DEFAULT_PORT;
        if (!s_wire_profiles[next_idx].mdns_service)   s_wire_profiles[next_idx].mdns_service = OAVX_WIRE_MDNS_SERVICE;
        if (!s_wire_profiles[next_idx].handshake_tag)  s_wire_profiles[next_idx].handshake_tag = OAVX_WIRE_HANDSHAKE_TAG;
        if (!s_wire_profiles[next_idx].tally_tag)      s_wire_profiles[next_idx].tally_tag = OAVX_WIRE_TALLY_TAG;
        if (!s_wire_profiles[next_idx].color_info_tag) s_wire_profiles[next_idx].color_info_tag = OAVX_WIRE_COLOR_INFO_TAG;
        if (!s_wire_profiles[next_idx].ptz_tag)        s_wire_profiles[next_idx].ptz_tag = OAVX_WIRE_PTZ_TAG;
        if (!s_wire_profiles[next_idx].record_tag)     s_wire_profiles[next_idx].record_tag = OAVX_WIRE_RECORD_TAG;
    } else {
        s_wire_profiles[next_idx] = s_default_wire_profile;
    }
    __sync_synchronize();
    s_active_profile_idx = next_idx;
    avx_mutex_unlock(&s_wire_profile_lock);
}

/* Encodes dotted DNS service name into RFC 1035 length-prefixed label sequence with trailing zero. */
OPENAVX_API size_t avx_wire_encode_dns_service(const char* service_str, uint8_t* out_buf, size_t max_len) {
    if (!service_str || !out_buf || max_len == 0) return 0;

    size_t out_idx = 0;
    const char* start = service_str;

    while (*start != '\0') {
        const char* dot = strchr(start, '.');
        size_t len = dot ? (size_t)(dot - start) : strlen(start);

        if (len > 63 || out_idx + 1 + len >= max_len) return 0;

        out_buf[out_idx++] = (uint8_t)len;
        memcpy(&out_buf[out_idx], start, len);
        out_idx += len;

        if (!dot) break;
        start = dot + 1;
    }

    if (out_idx < max_len) {
        out_buf[out_idx++] = 0; /* Null terminator label */
    } else {
        return 0;
    }

    return out_idx;
}

/* Scans packet payload for target DNS service label sequence without trailing null byte. */
OPENAVX_API bool avx_wire_match_service(const uint8_t* pkt, size_t pkt_len, const char* service_str, size_t* out_offset) {
    if (!pkt || !service_str || pkt_len < 12) return false;

    uint8_t target_labels[128];
    size_t target_len = avx_wire_encode_dns_service(service_str, target_labels, sizeof(target_labels));
    if (target_len <= 1) return false;

    /* Don't include the trailing zero in substring match for flexible label matching */
    size_t match_len = target_len - 1;

    for (size_t i = 12; i + match_len < pkt_len; ++i) {
        if (memcmp(&pkt[i], target_labels, match_len) == 0) {
            if (out_offset) *out_offset = i;
            return true;
        }
    }
    return false;
}
