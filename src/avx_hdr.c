/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_hdr.c - HDR and color primaries metadata parser and serializer
 */

#include "avx_hdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* Helper to parse an attribute value from an XML tag */
static bool get_xml_attribute(const char* tag_start, const char* tag_end,
                              const char* attr_name, char* out_val, size_t max_val_len) {
    if (!tag_start || !tag_end || !attr_name || !out_val || max_val_len == 0) return false;
    out_val[0] = '\0';

    size_t attr_len = strlen(attr_name);
    const char* p = tag_start;

    while (p < tag_end) {
        /* Search for attribute name */
        p = strstr(p, attr_name);
        if (!p || p >= tag_end) return false;

        /* Verify word boundary before attribute name */
        if (p > tag_start && !isspace((unsigned char)*(p - 1))) {
            p += attr_len;
            continue;
        }

        const char* eq = p + attr_len;
        while (eq < tag_end && isspace((unsigned char)*eq)) eq++;
        if (eq >= tag_end || *eq != '=') {
            p = eq;
            continue;
        }

        const char* val_start = eq + 1;
        while (val_start < tag_end && isspace((unsigned char)*val_start)) val_start++;
        if (val_start >= tag_end) return false;

        char quote = *val_start;
        if (quote == '"' || quote == '\'') {
            val_start++;
            const char* val_end = memchr(val_start, quote, tag_end - val_start);
            if (!val_end) return false;
            size_t len = (size_t)(val_end - val_start);
            if (len >= max_val_len) len = max_val_len - 1;
            memcpy(out_val, val_start, len);
            out_val[len] = '\0';
            return true;
        } else {
            /* Unquoted attribute */
            const char* val_end = val_start;
            while (val_end < tag_end && !isspace((unsigned char)*val_end) && *val_end != '>' && *val_end != '/') {
                val_end++;
            }
            size_t len = (size_t)(val_end - val_start);
            if (len >= max_val_len) len = max_val_len - 1;
            memcpy(out_val, val_start, len);
            out_val[len] = '\0';
            return true;
        }
    }
    return false;
}

/* Serializes HDR colorimetry parameters (HLG, PQ, BT.2020, DCI-P3, MaxCLL, MaxFALL) into XML metadata tag. */
OPENAVX_API bool avx_hdr_format_color_info(const avx_color_info_t* p_info, char* out_xml, size_t max_len) {
    if (!p_info || !out_xml || max_len < 32) return false;

    const char* transfer_str = "sdr";
    if (p_info->transfer == AVX_COLOR_TRANSFER_BT2100_HLG) {
        transfer_str = "bt_2100_hlg";
    } else if (p_info->transfer == AVX_COLOR_TRANSFER_BT2100_PQ) {
        transfer_str = "bt_2100_pq";
    }

    const char* matrix_str = "bt_709";
    if (p_info->matrix == AVX_COLOR_MATRIX_BT2020) {
        matrix_str = "bt_2020";
    }

    const char* primaries_str = "bt_709";
    if (p_info->primaries == AVX_COLOR_PRIMARIES_BT2020) {
        primaries_str = "bt_2020";
    } else if (p_info->primaries == AVX_COLOR_PRIMARIES_DCI_P3) {
        primaries_str = "dci_p3";
    }

    if (p_info->max_cll > 0 || p_info->max_fall > 0) {
        int written = snprintf(out_xml, max_len,
                               "<oavx_color_info transfer=\"%s\" matrix=\"%s\" primaries=\"%s\" max_cll=\"%u\" max_fall=\"%u\"/>",
                               transfer_str, matrix_str, primaries_str,
                               (unsigned)p_info->max_cll, (unsigned)p_info->max_fall);
        return (written > 0 && (size_t)written < max_len);
    } else {
        int written = snprintf(out_xml, max_len,
                               "<oavx_color_info transfer=\"%s\" matrix=\"%s\" primaries=\"%s\"/>",
                               transfer_str, matrix_str, primaries_str);
        return (written > 0 && (size_t)written < max_len);
    }
}

/* Parses XML colorimetry attributes and populates color transfer, matrix, and mastering luminance limits. */
OPENAVX_API bool avx_hdr_parse_color_info(const char* xml, avx_color_info_t* p_info) {
    if (!xml || !p_info) return false;

    /* Initialize to defaults */
    p_info->transfer = AVX_COLOR_TRANSFER_SDR;
    p_info->matrix = AVX_COLOR_MATRIX_BT709;
    p_info->primaries = AVX_COLOR_PRIMARIES_BT709;
    p_info->max_cll = 0;
    p_info->max_fall = 0;

    const char* tag_start = strstr(xml, "color_info");
    if (!tag_start) return false;
    while (tag_start > xml && *(tag_start - 1) != '<') {
        tag_start--;
    }
    if (*tag_start == '<') tag_start++;

    const char* tag_end = strchr(tag_start, '>');
    if (!tag_end) return false;

    char val[64];

    /* Parse transfer */
    if (get_xml_attribute(tag_start, tag_end, "transfer", val, sizeof(val))) {
        if (strcasecmp(val, "bt_2100_hlg") == 0 || strcasecmp(val, "hlg") == 0) {
            p_info->transfer = AVX_COLOR_TRANSFER_BT2100_HLG;
        } else if (strcasecmp(val, "bt_2100_pq") == 0 || strcasecmp(val, "pq") == 0 || strcasecmp(val, "smpte2084") == 0) {
            p_info->transfer = AVX_COLOR_TRANSFER_BT2100_PQ;
        } else {
            p_info->transfer = AVX_COLOR_TRANSFER_SDR;
        }
    }

    /* Parse matrix */
    if (get_xml_attribute(tag_start, tag_end, "matrix", val, sizeof(val))) {
        if (strcasecmp(val, "bt_2020") == 0 || strcasecmp(val, "bt2020") == 0) {
            p_info->matrix = AVX_COLOR_MATRIX_BT2020;
        } else {
            p_info->matrix = AVX_COLOR_MATRIX_BT709;
        }
    }

    /* Parse primaries */
    if (get_xml_attribute(tag_start, tag_end, "primaries", val, sizeof(val))) {
        if (strcasecmp(val, "bt_2020") == 0 || strcasecmp(val, "bt2020") == 0) {
            p_info->primaries = AVX_COLOR_PRIMARIES_BT2020;
        } else if (strcasecmp(val, "dci_p3") == 0 || strcasecmp(val, "p3") == 0) {
            p_info->primaries = AVX_COLOR_PRIMARIES_DCI_P3;
        } else {
            p_info->primaries = AVX_COLOR_PRIMARIES_BT709;
        }
    }

    /* Parse optional max_cll and max_fall */
    if (get_xml_attribute(tag_start, tag_end, "max_cll", val, sizeof(val))) {
        p_info->max_cll = (uint32_t)strtoul(val, NULL, 10);
    }
    if (get_xml_attribute(tag_start, tag_end, "max_fall", val, sizeof(val))) {
        p_info->max_fall = (uint32_t)strtoul(val, NULL, 10);
    }

    return true;
}

OPENAVX_API bool avx_hdr_frame_set_color_info(oavx_video_frame_t* p_frame,
                                              const avx_color_info_t* p_info,
                                              char* xml_buf, size_t xml_buf_len) {
    if (!p_frame || !p_info || !xml_buf || xml_buf_len < 32) return false;
    if (!avx_hdr_format_color_info(p_info, xml_buf, xml_buf_len)) return false;
    p_frame->metadata = xml_buf;
    p_frame->metadata_len = strlen(xml_buf);
    return true;
}

OPENAVX_API bool avx_hdr_frame_get_color_info(const oavx_video_frame_t* p_frame, avx_color_info_t* p_info) {
    if (!p_frame || !p_frame->metadata || !p_info) return false;
    return avx_hdr_parse_color_info(p_frame->metadata, p_info);
}

OPENAVX_API bool avx_hdr_is_hdr(const avx_color_info_t* p_info) {
    if (!p_info) return false;
    return (p_info->transfer == AVX_COLOR_TRANSFER_BT2100_HLG ||
            p_info->transfer == AVX_COLOR_TRANSFER_BT2100_PQ);
}
