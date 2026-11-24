/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_hdr.h - HDR and color primaries metadata
 */

#ifndef AVX_HDR_H
#define AVX_HDR_H

#include "openavx.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Electro-Optical Transfer Function (EOTF) / Transfer Characteristics.
 */
typedef enum avx_color_transfer_e {
    AVX_COLOR_TRANSFER_SDR        = 0, /* Standard Dynamic Range (BT.709 / BT.601) */
    AVX_COLOR_TRANSFER_BT2100_HLG = 1, /* Hybrid Log-Gamma (ITU-R BT.2100 / ARIB STD-B67) */
    AVX_COLOR_TRANSFER_BT2100_PQ  = 2, /* Perceptual Quantizer (ITU-R BT.2100 / SMPTE ST 2084 / HDR10) */
} avx_color_transfer_e;

/**
 * Matrix Coefficients for YCbCr to RGB mapping.
 */
typedef enum avx_color_matrix_e {
    AVX_COLOR_MATRIX_BT709  = 0, /* ITU-R BT.709 (HD / SDR) */
    AVX_COLOR_MATRIX_BT2020 = 1, /* ITU-R BT.2020 (UHD / HDR non-constant luminance) */
} avx_color_matrix_e;

/**
 * Color Primaries (Gamut).
 */
typedef enum avx_color_primaries_e {
    AVX_COLOR_PRIMARIES_BT709  = 0, /* ITU-R BT.709 (Standard sRGB / Rec.709 gamut) */
    AVX_COLOR_PRIMARIES_BT2020 = 1, /* ITU-R BT.2020 (Wide color gamut) */
    AVX_COLOR_PRIMARIES_DCI_P3 = 2, /* DCI-P3 (Digital Cinema gamut) */
} avx_color_primaries_e;

/**
 * Color and HDR Metadata Structure.
 */
typedef struct avx_color_info_t {
    avx_color_transfer_e  transfer;
    avx_color_matrix_e    matrix;
    avx_color_primaries_e primaries;
    uint32_t              max_cll;  /* Max Content Light Level (nits, e.g. 1000). 0 if unspecified */
    uint32_t              max_fall; /* Max Frame-Average Light Level (nits, e.g. 400). 0 if unspecified */
} avx_color_info_t;

/**
 * Formats a color metadata XML string.
 *
 * @param p_info Pointer to the color info structure.
 * @param out_xml Output buffer to store formatted null-terminated XML string.
 * @param max_len Size in bytes of out_xml buffer.
 * @return true on success, false if parameters or buffer length are insufficient.
 */
OPENAVX_API bool avx_hdr_format_color_info(const avx_color_info_t* p_info, char* out_xml, size_t max_len);

/**
 * Parses a color metadata XML string.
 *
 * @param xml Null-terminated XML string.
 * @param p_info Pointer to destination color info structure.
 * @return true if valid <oavx_color_info> or color tag was parsed, false otherwise.
 */
OPENAVX_API bool avx_hdr_parse_color_info(const char* xml, avx_color_info_t* p_info);

/**
 * Convenience helper to format color metadata and assign it to an openavx video frame's metadata.
 *
 * @param p_frame Video frame to modify.
 * @param p_info Color info specification.
 * @param xml_buf Persistent buffer owned by caller to hold formatted XML.
 * @param xml_buf_len Capacity of xml_buf.
 * @return true on success, false on error.
 */
OPENAVX_API bool avx_hdr_frame_set_color_info(oavx_video_frame_t* p_frame,
                                              const avx_color_info_t* p_info,
                                              char* xml_buf, size_t xml_buf_len);

/**
 * Extracts and parses HDR / color info metadata from an openavx video frame's metadata.
 *
 * @param p_frame Video frame to query.
 * @param p_info Destination color info structure.
 * @return true if HDR/color metadata was detected and parsed, false if absent or invalid.
 */
OPENAVX_API bool avx_hdr_frame_get_color_info(const oavx_video_frame_t* p_frame, avx_color_info_t* p_info);

/**
 * Returns true if color info represents an HDR stream (HLG or PQ).
 */
OPENAVX_API bool avx_hdr_is_hdr(const avx_color_info_t* p_info);

#ifdef __cplusplus
}
#endif

#endif /* AVX_HDR_H */
