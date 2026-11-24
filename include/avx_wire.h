/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_wire.h - Wire protocol framing and profile definitions
 */

#ifndef AVX_WIRE_H
#define AVX_WIRE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native OpenAVX Protocol Defaults */
#define OAVX_WIRE_MAGIC_VIDEO          0x56585641  /* 'AVXV' */
#define OAVX_WIRE_MAGIC_AUDIO          0x41585641  /* 'AVXA' */
#define OAVX_WIRE_MAGIC_AUDIO_COMP     0x4F585641  /* 'AVXO' */
#define OAVX_WIRE_MAGIC_METADATA       0x4D585641  /* 'AVXM' */
#define OAVX_WIRE_MAGIC_UDP            0x55585641  /* 'AVXU' */
#define OAVX_WIRE_DEFAULT_PORT         7960
#define OAVX_WIRE_MDNS_SERVICE         "_openavx._tcp.local"
#define OAVX_WIRE_HANDSHAKE_TAG        "oavx_stream"
#define OAVX_WIRE_TALLY_TAG            "oavx_tally"
#define OAVX_WIRE_COLOR_INFO_TAG       "oavx_color_info"
#define OAVX_WIRE_PTZ_TAG              "oavx_ptz"
#define OAVX_WIRE_RECORD_TAG           "oavx_record"

#ifndef OAVX_WIRE_PROFILE_DEFINED
#define OAVX_WIRE_PROFILE_DEFINED
typedef struct oavx_wire_profile {
    uint32_t magic_video;
    uint32_t magic_audio;
    uint32_t magic_metadata;
    uint32_t magic_udp;
    uint32_t compat_magic_video;
    uint32_t compat_magic_audio;
    uint32_t compat_magic_metadata;
    uint32_t compat_magic_udp;
    uint16_t base_port;
    const char* mdns_service;
    const char* handshake_tag;
    const char* tally_tag;
    const char* color_info_tag;
    const char* ptz_tag;
    const char* record_tag;
} oavx_wire_profile_t;
#endif

/* Return pointer to default native OpenAVX profile */
OPENAVX_API const oavx_wire_profile_t* avx_wire_get_default_profile(void);

/* Return pointer to active profile (process-wide fallback) */
OPENAVX_API const oavx_wire_profile_t* avx_wire_get_active_profile(void);

/* Set process-wide active wire profile (e.g. called by compatibility layer) */
OPENAVX_API void avx_wire_set_active_profile(const oavx_wire_profile_t* profile);

/* Encode a DNS-SD service string (e.g. "_openavx._tcp.local") into DNS label wire format */
OPENAVX_API size_t avx_wire_encode_dns_service(const char* service_str, uint8_t* out_buf, size_t max_len);

/* Scan packet for matching DNS service label sequence */
OPENAVX_API bool avx_wire_match_service(const uint8_t* pkt, size_t pkt_len, const char* service_str, size_t* out_offset);

#ifdef __cplusplus
}
#endif

#endif /* AVX_WIRE_H */
