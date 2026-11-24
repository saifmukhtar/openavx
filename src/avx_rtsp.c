/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_rtsp.c - RFC 2326 RTSP 1.0 Client/Server & RFC 6184/7798 RTP Streaming Engine
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "avx_platform.h"
#include "avx_rtsp.h"
#include "avx_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ========================================================================= */
/* 1. BITSTREAM READER & EXP-GOLOMB DECODER                                  */
/* ========================================================================= */

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bit_pos;
} bit_reader_t;

static inline uint32_t br_read_bit(bit_reader_t *br) {
    if (br->bit_pos >= br->size * 8) return 0;
    size_t byte_idx = br->bit_pos / 8;
    int bit_offset = 7 - (int)(br->bit_pos % 8);
    br->bit_pos++;
    return (br->data[byte_idx] >> bit_offset) & 1;
}

static inline uint32_t br_read_bits(bit_reader_t *br, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; ++i) {
        val = (val << 1) | br_read_bit(br);
    }
    return val;
}

static inline uint32_t br_read_ue(bit_reader_t *br) {
    int zeros = 0;
    while (br_read_bit(br) == 0 && br->bit_pos < br->size * 8) {
        zeros++;
        if (zeros >= 31) return 0;
    }
    if (zeros == 0) return 0;
    uint32_t info = br_read_bits(br, zeros);
    return (1U << zeros) - 1 + info;
}

static inline int32_t br_read_se(bit_reader_t *br) {
    uint32_t ue = br_read_ue(br);
    if (ue == 0) return 0;
    int32_t val = (int32_t)((ue + 1) >> 1);
    return (ue & 1) ? val : -val;
}

static inline uint32_t br_read_uvlc(bit_reader_t *br) {
    int leading_zeros = 0;
    while (br_read_bit(br) == 0 && br->bit_pos < br->size * 8) {
        leading_zeros++;
        if (leading_zeros >= 32) return 0xFFFFFFFFU;
    }
    if (leading_zeros == 0) return 0;
    uint32_t info = br_read_bits(br, leading_zeros);
    return (1U << leading_zeros) - 1 + info;
}

static inline bool av1_read_leb128(const uint8_t *buf, size_t size, size_t *offset, uint64_t *out_val) {
    uint64_t val = 0;
    size_t shift = 0;
    for (int i = 0; i < 8; ++i) {
        if (*offset >= size) return false;
        uint8_t byte = buf[(*offset)++];
        val |= ((uint64_t)(byte & 0x7F)) << shift;
        shift += 7;
        if (!(byte & 0x80)) {
            if (out_val) *out_val = val;
            return true;
        }
    }
    return false;
}

/* Strips 0x000003 emulation prevention bytes from NAL unit byte stream to produce raw RBSP. */
static size_t remove_emulation_prevention(const uint8_t *src, size_t src_len, uint8_t *dst) {
    size_t dst_len = 0;
    for (size_t i = 0; i < src_len; ++i) {
        if (i + 2 < src_len && src[i] == 0 && src[i+1] == 0 && src[i+2] == 3) {
            dst[dst_len++] = src[i++];
            dst[dst_len++] = src[i++];
            /* skip 0x03 */
        } else {
            dst[dst_len++] = src[i];
        }
    }
    return dst_len;
}

/* ========================================================================= */
/* 2. PARAMETER SET PARSING (H.264 & HEVC SPS)                               */
/* ========================================================================= */

/* Parses H.264 SPS NAL unit via Exp-Golomb decoding, extracting picture resolution, profile, and level. */
bool avx_rtsp_parse_sps_h264(
    const uint8_t* sps,
    size_t size,
    int* out_width,
    int* out_height,
    int* out_profile,
    int* out_level
) {
    if (!sps || size < 4 || !out_width || !out_height) return false;

    size_t offset = 0;
    if ((sps[0] & 0x1F) == 7) {
        offset = 1;
    }

    uint8_t *rbsp = (uint8_t*)malloc(size);
    if (!rbsp) return false;
    size_t rbsp_len = remove_emulation_prevention(sps + offset, size - offset, rbsp);

    bit_reader_t br = { rbsp, rbsp_len, 0 };
    int profile = (int)br_read_bits(&br, 8);
    br_read_bits(&br, 8); /* constraint flags */
    int level = (int)br_read_bits(&br, 8);
    br_read_ue(&br); /* seq_parameter_set_id */

    if (out_profile) *out_profile = profile;
    if (out_level) *out_level = level;

    int chroma_format_idc = 1; /* Default 4:2:0 */
    if (profile == 100 || profile == 110 || profile == 122 ||
        profile == 244 || profile == 44  || profile == 83  ||
        profile == 86  || profile == 118 || profile == 128 ||
        profile == 138) {
        chroma_format_idc = (int)br_read_ue(&br);
        if (chroma_format_idc == 3) {
            br_read_bits(&br, 1); /* separate_colour_plane_flag */
        }
        br_read_ue(&br); /* bit_depth_luma_minus8 */
        br_read_ue(&br); /* bit_depth_chroma_minus8 */
        br_read_bits(&br, 1); /* qpprime_y_zero_transform_bypass_flag */
        int seq_scaling_matrix_present = (int)br_read_bits(&br, 1);
        if (seq_scaling_matrix_present) {
            int num_lists = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < num_lists; ++i) {
                if (br_read_bit(&br)) {
                    int size_of_list = (i < 6) ? 16 : 64;
                    int last_scale = 8, next_scale = 8;
                    for (int j = 0; j < size_of_list; ++j) {
                        if (next_scale != 0) {
                            int delta = br_read_se(&br);
                            next_scale = (last_scale + delta) & 0xff;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    }

    br_read_ue(&br); /* log2_max_frame_num_minus4 */
    int pic_order_cnt_type = (int)br_read_ue(&br);
    if (pic_order_cnt_type == 0) {
        br_read_ue(&br); /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (pic_order_cnt_type == 1) {
        br_read_bits(&br, 1); /* delta_pic_order_always_zero_flag */
        br_read_se(&br); /* offset_for_non_ref_pic */
        br_read_se(&br); /* offset_for_top_to_bottom_field */
        int num_ref_frames_in_cycle = (int)br_read_ue(&br);
        if (num_ref_frames_in_cycle < 0 || num_ref_frames_in_cycle > 255) {
            free(rbsp);
            return false;
        }
        for (int i = 0; i < num_ref_frames_in_cycle; ++i) {
            if (br.bit_pos >= br.size * 8) {
                free(rbsp);
                return false;
            }
            br_read_se(&br);
        }
    }

    br_read_ue(&br); /* max_num_ref_frames */
    br_read_bits(&br, 1); /* gaps_in_frame_num_value_allowed_flag */
    int pic_width_in_mbs_minus1 = (int)br_read_ue(&br);
    int pic_height_in_map_units_minus1 = (int)br_read_ue(&br);
    int frame_mbs_only_flag = (int)br_read_bits(&br, 1);
    if (!frame_mbs_only_flag) {
        br_read_bits(&br, 1); /* mb_adaptive_frame_field_flag */
    }
    br_read_bits(&br, 1); /* direct_8x8_inference_flag */

    int64_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    int frame_cropping_flag = (int)br_read_bits(&br, 1);
    if (frame_cropping_flag) {
        crop_left   = (int64_t)br_read_ue(&br);
        crop_right  = (int64_t)br_read_ue(&br);
        crop_top    = (int64_t)br_read_ue(&br);
        crop_bottom = (int64_t)br_read_ue(&br);
    }

    if (pic_width_in_mbs_minus1 < 0 || pic_width_in_mbs_minus1 > 2048 ||
        pic_height_in_map_units_minus1 < 0 || pic_height_in_map_units_minus1 > 2048) {
        free(rbsp);
        return false;
    }

    int width = (pic_width_in_mbs_minus1 + 1) * 16;
    int height = (2 - frame_mbs_only_flag) * (pic_height_in_map_units_minus1 + 1) * 16;

    int crop_unit_x = 2; /* 4:2:0 */
    int crop_unit_y = 2 * (2 - frame_mbs_only_flag);
    if (chroma_format_idc == 0 || chroma_format_idc == 3) {
        crop_unit_x = 1;
        crop_unit_y = 2 - frame_mbs_only_flag;
    }

    if (crop_left < 0 || crop_right < 0 || crop_top < 0 || crop_bottom < 0) {
        free(rbsp);
        return false;
    }

    int64_t total_crop_x = (crop_left + crop_right) * crop_unit_x;
    int64_t total_crop_y = (crop_top + crop_bottom) * crop_unit_y;
    if (total_crop_x >= width || total_crop_y >= height) {
        free(rbsp);
        return false;
    }

    width -= (int)total_crop_x;
    height -= (int)total_crop_y;

    *out_width = width;
    *out_height = height;
    free(rbsp);
    return (width > 0 && height > 0);
}

/* Parses HEVC/H.265 SPS NAL unit, extracting conformance window cropped resolution and bit depth. */
bool avx_rtsp_parse_sps_hevc(
    const uint8_t* sps,
    size_t size,
    int* out_width,
    int* out_height,
    int* out_profile,
    int* out_level
) {
    if (!sps || size < 6 || !out_width || !out_height) return false;

    size_t offset = 0;
    int nal_type = (sps[0] >> 1) & 0x3F;
    if (nal_type == 33) {
        offset = 2; /* Skip 2-byte HEVC NAL header */
    }

    uint8_t *rbsp = (uint8_t*)malloc(size);
    if (!rbsp) return false;
    size_t rbsp_len = remove_emulation_prevention(sps + offset, size - offset, rbsp);

    bit_reader_t br = { rbsp, rbsp_len, 0 };
    br_read_bits(&br, 4); /* vps_id */
    int max_sub_layers = (int)br_read_bits(&br, 3);
    br_read_bits(&br, 1); /* temporal_id_nesting_flag */

    /* profile_tier_level */
    br_read_bits(&br, 2); /* profile_space */
    br_read_bits(&br, 1); /* tier_flag */
    int profile_idc = (int)br_read_bits(&br, 5);
    br_read_bits(&br, 32); /* profile_compatibility_flags */
    br_read_bits(&br, 48); /* constraint flags */
    int level_idc = (int)br_read_bits(&br, 8);

    if (out_profile) *out_profile = profile_idc;
    if (out_level) *out_level = level_idc;

    bool sub_layer_profile[8] = { false };
    bool sub_layer_level[8] = { false };
    for (int i = 0; i < max_sub_layers; ++i) {
        sub_layer_profile[i] = (br_read_bit(&br) != 0);
        sub_layer_level[i] = (br_read_bit(&br) != 0);
    }
    if (max_sub_layers > 0) {
        for (int i = max_sub_layers; i < 8; ++i) br_read_bits(&br, 2);
    }
    for (int i = 0; i < max_sub_layers; ++i) {
        if (sub_layer_profile[i]) br_read_bits(&br, 2 + 1 + 5 + 32 + 48);
        if (sub_layer_level[i]) br_read_bits(&br, 8);
    }

    br_read_ue(&br); /* seq_parameter_set_id */
    int chroma_format_idc = (int)br_read_ue(&br);
    if (chroma_format_idc == 3) {
        br_read_bits(&br, 1); /* separate_colour_plane_flag */
    }

    int pic_width = (int)br_read_ue(&br);
    int pic_height = (int)br_read_ue(&br);
    if (pic_width <= 0 || pic_width > 32768 || pic_height <= 0 || pic_height > 32768) {
        free(rbsp);
        return false;
    }

    int conformance_win = (int)br_read_bit(&br);
    if (conformance_win) {
        int64_t cl = (int64_t)br_read_ue(&br);
        int64_t cr = (int64_t)br_read_ue(&br);
        int64_t ct = (int64_t)br_read_ue(&br);
        int64_t cb = (int64_t)br_read_ue(&br);
        if (cl < 0 || cr < 0 || ct < 0 || cb < 0) {
            free(rbsp);
            return false;
        }
        int sub_width_c = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
        int sub_height_c = (chroma_format_idc == 1) ? 2 : 1;
        int64_t crop_w = (cl + cr) * sub_width_c;
        int64_t crop_h = (ct + cb) * sub_height_c;
        if (crop_w >= pic_width || crop_h >= pic_height) {
            free(rbsp);
            return false;
        }
        pic_width -= (int)crop_w;
        pic_height -= (int)crop_h;
    }

    *out_width = pic_width;
    *out_height = pic_height;
    free(rbsp);
    return (pic_width > 0 && pic_height > 0);
}

bool avx_rtsp_parse_sequence_header_av1(
    const uint8_t* obu,
    size_t size,
    int* out_width,
    int* out_height,
    int* out_profile,
    int* out_level
) {
    if (!obu || size < 2 || !out_width || !out_height) return false;

    size_t offset = 0;

    /* 1. Skip start codes (00 00 01 or 00 00 00 01) if present */
    if (size >= 4 && obu[0] == 0 && obu[1] == 0) {
        if (obu[2] == 1) {
            offset = 3;
        } else if (obu[2] == 0 && obu[3] == 1) {
            offset = 4;
        }
    }
    if (offset >= size) return false;

    /* 2. Skip Temporal Delimiter OBU (type 2) if it precedes the Sequence Header */
    if (size - offset >= 2 && ((obu[offset] >> 3) & 0x0F) == 2 && (obu[offset] & 0x80) == 0) {
        size_t td_offset = offset + 1;
        if (obu[offset] & 0x04) td_offset++;
        if (obu[offset] & 0x02) {
            uint64_t td_sz = 0;
            if (av1_read_leb128(obu, size, &td_offset, &td_sz)) {
                td_offset += (size_t)td_sz;
                if (td_offset < size) {
                    offset = td_offset;
                }
            }
        }
    }

    if (offset >= size) return false;

    /* 3. Validate AV1 OBU Header */
    uint8_t header = obu[offset++];
    if ((header & 0x80) != 0) {
        return false; /* obu_forbidden_bit must be 0 */
    }

    int obu_type = (header >> 3) & 0x0F;
    if (obu_type != 1) {
        return false; /* Must be OBU_SEQUENCE_HEADER (1) */
    }

    if (header & 0x04) {
        /* obu_extension_flag: 1 byte (temporal_id, spatial_id) */
        if (offset >= size) return false;
        offset++;
    }

    size_t payload_len = size - offset;
    if (header & 0x02) {
        /* obu_has_size_field */
        uint64_t obu_size = 0;
        if (!av1_read_leb128(obu, size, &offset, &obu_size)) {
            return false;
        }
        if (obu_size < payload_len) {
            payload_len = (size_t)obu_size;
        }
    }

    if (offset + payload_len > size || payload_len == 0) {
        return false;
    }

    bit_reader_t br = { obu + offset, payload_len, 0 };

    int seq_profile = (int)br_read_bits(&br, 3);
    br_read_bit(&br); /* still_picture */
    int reduced_still_picture_header = (int)br_read_bit(&br);

    int seq_level = 0;

    if (reduced_still_picture_header) {
        seq_level = (int)br_read_bits(&br, 5);
    } else {
        int timing_info_present = (int)br_read_bit(&br);
        int decoder_model_info_present = 0;
        int buffer_delay_length_minus_1 = 0;

        if (timing_info_present) {
            br_read_bits(&br, 32); /* num_units_in_display_tick */
            br_read_bits(&br, 32); /* time_scale */
            int equal_picture_interval = (int)br_read_bit(&br);
            if (equal_picture_interval) {
                br_read_uvlc(&br); /* num_ticks_per_picture_minus_1 */
            }
            decoder_model_info_present = (int)br_read_bit(&br);
            if (decoder_model_info_present) {
                buffer_delay_length_minus_1 = (int)br_read_bits(&br, 5);
                br_read_bits(&br, 32); /* num_units_in_decoding_tick */
                br_read_bits(&br, 5);  /* buffer_removal_time_length_minus_1 */
                br_read_bits(&br, 5);  /* frame_presentation_time_length_minus_1 */
            }
        }

        int initial_display_delay_present = (int)br_read_bit(&br);
        int operating_points_cnt_minus_1 = (int)br_read_bits(&br, 5);

        for (int i = 0; i <= operating_points_cnt_minus_1; ++i) {
            br_read_bits(&br, 12); /* operating_point_idc */
            int level = (int)br_read_bits(&br, 5);
            if (i == 0) seq_level = level;
            if (level > 7) {
                br_read_bit(&br); /* seq_tier */
            }
            if (decoder_model_info_present) {
                int decoder_model_present = (int)br_read_bit(&br);
                if (decoder_model_present) {
                    br_read_bits(&br, buffer_delay_length_minus_1 + 1); /* decoder_buffer_delay */
                    br_read_bits(&br, buffer_delay_length_minus_1 + 1); /* encoder_buffer_delay */
                    br_read_bit(&br); /* low_delay_mode_flag */
                }
            }
            if (initial_display_delay_present) {
                int initial_display_delay_present_for_op = (int)br_read_bit(&br);
                if (initial_display_delay_present_for_op) {
                    br_read_bits(&br, 4); /* initial_display_delay_minus_1 */
                }
            }
        }
    }

    int frame_width_bits = (int)br_read_bits(&br, 4) + 1;
    int frame_height_bits = (int)br_read_bits(&br, 4) + 1;

    int max_frame_width = (int)br_read_bits(&br, frame_width_bits) + 1;
    int max_frame_height = (int)br_read_bits(&br, frame_height_bits) + 1;

    if (br.bit_pos > payload_len * 8) {
        return false;
    }

    if (max_frame_width <= 0 || max_frame_width > 65536 ||
        max_frame_height <= 0 || max_frame_height > 65536) {
        return false;
    }

    *out_width = max_frame_width;
    *out_height = max_frame_height;
    if (out_profile) *out_profile = seq_profile;
    if (out_level) *out_level = seq_level;

    return true;
}

bool avx_rtsp_is_keyframe(avx_rtsp_codec_e codec, const uint8_t* nalu, size_t size) {
    if (!nalu || size == 0) return false;

    if (codec == AVX_RTSP_CODEC_H264) {
        int nal_type = nalu[0] & 0x1F;
        return (nal_type == 5); /* IDR */
    } else if (codec == AVX_RTSP_CODEC_HEVC) {
        if (size < 2) return false;
        int nal_type = (nalu[0] >> 1) & 0x3F;
        return (nal_type == 19 || nal_type == 20 || /* IDR_W_RADL, IDR_N_LP */
                nal_type == 16 || nal_type == 17 || nal_type == 18 || /* BLA */
                nal_type == 21); /* CRA */
    } else if (codec == AVX_RTSP_CODEC_AV1) {
        size_t pos = 0;
        /* Skip start codes if present */
        if (size >= 4 && nalu[0] == 0 && nalu[1] == 0) {
            if (nalu[2] == 1) pos = 3;
            else if (nalu[2] == 0 && nalu[3] == 1) pos = 4;
        }

        while (pos < size) {
            uint8_t h = nalu[pos++];
            if ((h & 0x80) != 0) return false; /* forbidden_bit must be 0 */

            int obu_type = (h >> 3) & 0x0F;
            bool has_ext = (h & 0x04) != 0;
            bool has_sz  = (h & 0x02) != 0;

            if (has_ext) {
                if (pos >= size) return false;
                pos++;
            }

            size_t obu_size = 0;
            if (has_sz) {
                uint64_t sz_val = 0;
                if (!av1_read_leb128(nalu, size, &pos, &sz_val)) {
                    return false;
                }
                obu_size = (size_t)sz_val;
            } else {
                obu_size = size - pos;
            }

            if (pos + obu_size > size) return false;

            if (obu_type == 1) {
                /* Sequence Header OBU is always a key / configuration anchor */
                return true;
            } else if (obu_type == 3 || obu_type == 6) {
                /* OBU_FRAME_HEADER or OBU_FRAME */
                if (obu_size > 0 && pos < size) {
                    uint8_t b0 = nalu[pos];
                    int show_existing = (b0 >> 7) & 1;
                    if (show_existing == 0) {
                        int frame_type = (b0 >> 5) & 3;
                        return (frame_type == 0 || frame_type == 2); /* KEY_FRAME or INTRA_ONLY_FRAME */
                    }
                }
                return false;
            } else if (obu_type == 2) {
                /* Temporal Delimiter: advance to inspect next OBU */
                pos += obu_size;
                continue;
            } else {
                /* Other OBUs (metadata, tile group, etc.) */
                if (has_sz) {
                    pos += obu_size;
                    continue;
                }
                break;
            }
        }
        return false;
    }
    return false;
}

/* ========================================================================= */
/* 3. ELEMENTARY STREAM DEMUXER IMPLEMENTATION                               */
/* ========================================================================= */

struct avx_rtsp_demuxer_t {
    avx_rtsp_codec_e codec;

    /* Reassembly buffer for current Access Unit (video frame) */
    uint8_t *frame_buf;
    size_t frame_cap;
    size_t frame_size;

    /* Reassembly buffer for active Fragmented Unit (FU-A / HEVC FU) */
    uint8_t *fu_buf;
    size_t fu_cap;
    size_t fu_size;
    bool fu_in_progress;
    uint8_t fu_orig_header[2];
    size_t fu_orig_header_len;

    /* Cached parameter sets for orphan IDR keyframe repair */
    uint8_t *sps;
    size_t sps_len;
    uint8_t *pps;
    size_t pps_len;
    uint8_t *vps;
    size_t vps_len;

    /* Stream video geometry */
    int width;
    int height;
    int frame_rate_n;
    int frame_rate_d;

    /* Active timestamp and frame type */
    uint32_t active_rtp_ts;
    bool has_active_ts;
    avx_rtsp_frame_type_e active_frame_type;
    bool frame_ready;

    /* Completed frame cache */
    avx_rtsp_frame_t completed_frame;
};

static void ensure_buf_capacity(uint8_t **buf, size_t *cap, size_t needed) {
    if (needed > *cap) {
        size_t new_cap = (*cap == 0) ? 65536 : (*cap * 2);
        while (new_cap < needed) new_cap *= 2;
        uint8_t *nb = (uint8_t*)realloc(*buf, new_cap);
        if (nb) {
            *buf = nb;
            *cap = new_cap;
        }
    }
}

avx_rtsp_demuxer_t* avx_rtsp_demuxer_create(avx_rtsp_codec_e default_codec) {
    avx_rtsp_demuxer_t *d = (avx_rtsp_demuxer_t*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->codec = (default_codec != AVX_RTSP_CODEC_UNKNOWN) ? default_codec : AVX_RTSP_CODEC_H264;
    d->frame_rate_n = 60000;
    d->frame_rate_d = 1000;
    return d;
}

void avx_rtsp_demuxer_destroy(avx_rtsp_demuxer_t* demux) {
    if (!demux) return;
    free(demux->frame_buf);
    free(demux->fu_buf);
    free(demux->sps);
    free(demux->pps);
    free(demux->vps);
    free(demux);
}

void avx_rtsp_demuxer_reset(avx_rtsp_demuxer_t* demux) {
    if (!demux) return;
    demux->frame_size = 0;
    demux->fu_size = 0;
    demux->fu_in_progress = false;
    demux->has_active_ts = false;
    demux->frame_ready = false;
    demux->active_frame_type = AVX_RTSP_FRAME_TYPE_UNKNOWN;
}

bool avx_rtsp_demuxer_set_parameter_sets(
    avx_rtsp_demuxer_t* demux,
    avx_rtsp_codec_e codec,
    const uint8_t* sps, size_t sps_len,
    const uint8_t* pps, size_t pps_len,
    const uint8_t* vps, size_t vps_len
) {
    if (!demux) return false;
    demux->codec = codec;

    if (sps && sps_len > 0) {
        free(demux->sps);
        demux->sps = (uint8_t*)malloc(sps_len);
        if (demux->sps) {
            memcpy(demux->sps, sps, sps_len);
            demux->sps_len = sps_len;
            if (codec == AVX_RTSP_CODEC_H264) {
                avx_rtsp_parse_sps_h264(sps, sps_len, &demux->width, &demux->height, NULL, NULL);
            } else if (codec == AVX_RTSP_CODEC_HEVC) {
                avx_rtsp_parse_sps_hevc(sps, sps_len, &demux->width, &demux->height, NULL, NULL);
            } else if (codec == AVX_RTSP_CODEC_AV1) {
                avx_rtsp_parse_sequence_header_av1(sps, sps_len, &demux->width, &demux->height, NULL, NULL);
            }
        }
    }

    if (pps && pps_len > 0) {
        free(demux->pps);
        demux->pps = (uint8_t*)malloc(pps_len);
        if (demux->pps) {
            memcpy(demux->pps, pps, pps_len);
            demux->pps_len = pps_len;
        }
    }

    if (vps && vps_len > 0) {
        free(demux->vps);
        demux->vps = (uint8_t*)malloc(vps_len);
        if (demux->vps) {
            memcpy(demux->vps, vps, vps_len);
            demux->vps_len = vps_len;
        }
    }

    return true;
}

static void append_nalu_to_frame(avx_rtsp_demuxer_t *demux, const uint8_t *nalu, size_t size, bool force_4byte) {
    if (!demux || !nalu || size == 0) return;

    /* Start code: 0x00 0x00 0x00 0x01 (4-byte) or 0x00 0x00 0x01 (3-byte) */
    size_t sc_len = force_4byte ? 4 : 3;
    uint8_t sc[4] = { 0, 0, 0, 1 };
    if (!force_4byte) {
        sc[0] = 0; sc[1] = 0; sc[2] = 1;
    }

    ensure_buf_capacity(&demux->frame_buf, &demux->frame_cap, demux->frame_size + sc_len + size);
    if (!demux->frame_buf) return;

    memcpy(demux->frame_buf + demux->frame_size, sc, sc_len);
    demux->frame_size += sc_len;
    memcpy(demux->frame_buf + demux->frame_size, nalu, size);
    demux->frame_size += size;
}

static void finalize_frame(avx_rtsp_demuxer_t *demux) {
    if (demux->frame_size == 0) return;

    /* If frame is Keyframe and lacks SPS/PPS, prepend cached parameter sets */
    if (demux->active_frame_type == AVX_RTSP_FRAME_TYPE_KEYFRAME && demux->sps && demux->sps_len > 0) {
        /* Check if SPS already present in frame_buf */
        bool has_sps = false;
        if (demux->codec == AVX_RTSP_CODEC_H264) {
            for (size_t i = 0; i < demux->frame_size - 4; ++i) {
                if (demux->frame_buf[i] == 0 && demux->frame_buf[i+1] == 0 &&
                    demux->frame_buf[i+2] == 1 && (demux->frame_buf[i+3] & 0x1F) == 7) {
                    has_sps = true;
                    break;
                }
            }
        } else if (demux->codec == AVX_RTSP_CODEC_HEVC) {
            for (size_t i = 0; i < demux->frame_size - 4; ++i) {
                if (demux->frame_buf[i] == 0 && demux->frame_buf[i+1] == 0 &&
                    demux->frame_buf[i+2] == 1 && ((demux->frame_buf[i+3] >> 1) & 0x3F) == 33) {
                    has_sps = true;
                    break;
                }
            }
        } else if (demux->codec == AVX_RTSP_CODEC_AV1) {
            for (size_t i = 0; i < demux->frame_size - 4; ++i) {
                if (demux->frame_buf[i] == 0 && demux->frame_buf[i+1] == 0 &&
                    demux->frame_buf[i+2] == 1 && ((demux->frame_buf[i+3] >> 3) & 0x0F) == 1) {
                    has_sps = true;
                    break;
                }
            }
        }
        if (!has_sps) {
            size_t extra = demux->sps_len + 4 + (demux->pps ? demux->pps_len + 4 : 0) + (demux->vps ? demux->vps_len + 4 : 0);
            uint8_t *new_buf = (uint8_t*)malloc(demux->frame_size + extra);
            if (new_buf) {
                size_t p = 0;
                if (demux->vps && demux->vps_len > 0) {
                    new_buf[p++] = 0; new_buf[p++] = 0; new_buf[p++] = 0; new_buf[p++] = 1;
                    memcpy(new_buf + p, demux->vps, demux->vps_len);
                    p += demux->vps_len;
                }
                new_buf[p++] = 0; new_buf[p++] = 0; new_buf[p++] = 0; new_buf[p++] = 1;
                memcpy(new_buf + p, demux->sps, demux->sps_len);
                p += demux->sps_len;
                if (demux->pps && demux->pps_len > 0) {
                    new_buf[p++] = 0; new_buf[p++] = 0; new_buf[p++] = 0; new_buf[p++] = 1;
                    memcpy(new_buf + p, demux->pps, demux->pps_len);
                    p += demux->pps_len;
                }
                memcpy(new_buf + p, demux->frame_buf, demux->frame_size);
                p += demux->frame_size;
                free(demux->frame_buf);
                demux->frame_buf = new_buf;
                demux->frame_cap = p;
                demux->frame_size = p;
            }
        }
    }

    uint32_t fourcc = OAVX_FOURCC_VIDEO_TYPE_H264;
    if (demux->codec == AVX_RTSP_CODEC_HEVC) fourcc = OAVX_FOURCC_VIDEO_TYPE_HEVC;
    else if (demux->codec == AVX_RTSP_CODEC_AV1) fourcc = OAVX_FOURCC_VIDEO_TYPE_AV01;

    demux->completed_frame.codec = demux->codec;
    demux->completed_frame.frame_type = demux->active_frame_type;
    demux->completed_frame.fourcc = fourcc;
    demux->completed_frame.width = (demux->width > 0) ? demux->width : 1920;
    demux->completed_frame.height = (demux->height > 0) ? demux->height : 1080;
    demux->completed_frame.frame_rate_n = demux->frame_rate_n;
    demux->completed_frame.frame_rate_d = demux->frame_rate_d;
    demux->completed_frame.rtp_timestamp = demux->active_rtp_ts;
    demux->completed_frame.timecode = (int64_t)demux->active_rtp_ts;
    demux->completed_frame.data = demux->frame_buf;
    demux->completed_frame.size = demux->frame_size;

    demux->frame_ready = true;
}

bool avx_rtsp_demuxer_feed_nalu(
    avx_rtsp_demuxer_t* demux,
    const uint8_t* nalu_data,
    size_t nalu_size,
    bool is_last_slice,
    uint32_t timestamp
) {
    if (!demux || !nalu_data || nalu_size == 0) return false;

    if (!demux->has_active_ts || demux->active_rtp_ts != timestamp) {
        if (demux->has_active_ts && demux->frame_size > 0) {
            finalize_frame(demux);
        }
        demux->frame_size = 0;
        demux->active_rtp_ts = timestamp;
        demux->has_active_ts = true;
        demux->active_frame_type = AVX_RTSP_FRAME_TYPE_DELTA;
    }

    bool is_kf = avx_rtsp_is_keyframe(demux->codec, nalu_data, nalu_size);
    if (is_kf) {
        demux->active_frame_type = AVX_RTSP_FRAME_TYPE_KEYFRAME;
    }

    /* Cache parameter sets */
    if (demux->codec == AVX_RTSP_CODEC_H264) {
        int nt = nalu_data[0] & 0x1F;
        if (nt == 7) {
            avx_rtsp_demuxer_set_parameter_sets(demux, demux->codec, nalu_data, nalu_size, NULL, 0, NULL, 0);
        } else if (nt == 8) {
            avx_rtsp_demuxer_set_parameter_sets(demux, demux->codec, NULL, 0, nalu_data, nalu_size, NULL, 0);
        }
    } else if (demux->codec == AVX_RTSP_CODEC_HEVC && nalu_size >= 2) {
        int nt = (nalu_data[0] >> 1) & 0x3F;
        if (nt == 32) {
            avx_rtsp_demuxer_set_parameter_sets(demux, demux->codec, NULL, 0, NULL, 0, nalu_data, nalu_size);
        } else if (nt == 33) {
            avx_rtsp_demuxer_set_parameter_sets(demux, demux->codec, nalu_data, nalu_size, NULL, 0, NULL, 0);
        } else if (nt == 34) {
            avx_rtsp_demuxer_set_parameter_sets(demux, demux->codec, NULL, 0, nalu_data, nalu_size, NULL, 0);
        }
    } else if (demux->codec == AVX_RTSP_CODEC_AV1 && nalu_size >= 1) {
        size_t sc = 0;
        if (nalu_size >= 4 && nalu_data[0] == 0 && nalu_data[1] == 0) {
            if (nalu_data[2] == 1) sc = 3;
            else if (nalu_data[2] == 0 && nalu_data[3] == 1) sc = 4;
        }
        if (nalu_size > sc && (nalu_data[sc] & 0x80) == 0) {
            int obu_type = (nalu_data[sc] >> 3) & 0x0F;
            if (obu_type == 1) {
                avx_rtsp_demuxer_set_parameter_sets(demux, demux->codec, nalu_data + sc, nalu_size - sc, NULL, 0, NULL, 0);
            }
        }
    }

    append_nalu_to_frame(demux, nalu_data, nalu_size, is_kf);

    if (is_last_slice) {
        finalize_frame(demux);
        return true;
    }
    return false;
}

/* Reassembles fragmented RTP packets (RFC 6184 FU-A, RFC 7798 HEVC FU) into Annex-B Access Units. */
bool avx_rtsp_demuxer_feed_rtp(
    avx_rtsp_demuxer_t* demux,
    const uint8_t* rtp_packet,
    size_t packet_size
) {
    if (!demux || !rtp_packet || packet_size < 12) return false;

    /* RFC 3550 RTP Header */
    int version = (rtp_packet[0] >> 6) & 0x03;
    if (version != 2) return false;

    int padding = (rtp_packet[0] >> 5) & 0x01;
    int extension = (rtp_packet[0] >> 4) & 0x01;
    int csrc_count = rtp_packet[0] & 0x0F;
    int marker = (rtp_packet[1] >> 7) & 0x01;
    uint32_t timestamp = ((uint32_t)rtp_packet[4] << 24) | ((uint32_t)rtp_packet[5] << 16) |
                         ((uint32_t)rtp_packet[6] << 8)  | (uint32_t)rtp_packet[7];

    size_t header_len = 12 + (size_t)csrc_count * 4;
    if (header_len > packet_size) return false;

    if (extension) {
        if (header_len + 4 > packet_size) return false;
        size_t ext_words = ((size_t)rtp_packet[header_len + 2] << 8) | (size_t)rtp_packet[header_len + 3];
        header_len += 4 + ext_words * 4;
        if (header_len > packet_size) return false;
    }

    size_t payload_len = packet_size - header_len;
    if (padding && payload_len > 0) {
        uint8_t pad_count = rtp_packet[packet_size - 1];
        if (pad_count <= payload_len) payload_len -= pad_count;
    }

    if (payload_len == 0) return false;
    const uint8_t *payload = rtp_packet + header_len;

    /* Check if timestamp advanced */
    if (!demux->has_active_ts || demux->active_rtp_ts != timestamp) {
        if (demux->has_active_ts && demux->frame_size > 0) {
            finalize_frame(demux);
        }
        demux->frame_size = 0;
        demux->active_rtp_ts = timestamp;
        demux->has_active_ts = true;
        demux->active_frame_type = AVX_RTSP_FRAME_TYPE_DELTA;
    }

    /* Depacketize payload by codec */
    if (demux->codec == AVX_RTSP_CODEC_H264) {
        uint8_t nal_header = payload[0];
        int nal_type = nal_header & 0x1F;

        if (nal_type >= 1 && nal_type <= 23) {
            /* Single NAL unit packet */
            avx_rtsp_demuxer_feed_nalu(demux, payload, payload_len, false, timestamp);
        } else if (nal_type == 24) {
            /* STAP-A Aggregation Packet */
            size_t p = 1;
            while (p + 2 <= payload_len) {
                size_t nalu_len = ((size_t)payload[p] << 8) | (size_t)payload[p+1];
                p += 2;
                if (p + nalu_len > payload_len) break;
                avx_rtsp_demuxer_feed_nalu(demux, payload + p, nalu_len, false, timestamp);
                p += nalu_len;
            }
        } else if (nal_type == 28) {
            /* FU-A Fragmentation Unit */
            if (payload_len < 2) return false;
            uint8_t fu_indicator = payload[0];
            uint8_t fu_header = payload[1];
            int start_bit = (fu_header >> 7) & 1;
            int end_bit = (fu_header >> 6) & 1;
            uint8_t orig_type = fu_header & 0x1F;
            uint8_t orig_header = (fu_indicator & 0xE0) | orig_type;

            if (start_bit) {
                demux->fu_size = 0;
                ensure_buf_capacity(&demux->fu_buf, &demux->fu_cap, payload_len + 1);
                if (demux->fu_buf) {
                    demux->fu_buf[0] = orig_header;
                    memcpy(demux->fu_buf + 1, payload + 2, payload_len - 2);
                    demux->fu_size = 1 + (payload_len - 2);
                    demux->fu_in_progress = true;
                }
            } else if (demux->fu_in_progress) {
                ensure_buf_capacity(&demux->fu_buf, &demux->fu_cap, demux->fu_size + (payload_len - 2));
                if (demux->fu_buf) {
                    memcpy(demux->fu_buf + demux->fu_size, payload + 2, payload_len - 2);
                    demux->fu_size += (payload_len - 2);
                }
            }

            if (end_bit && demux->fu_in_progress) {
                avx_rtsp_demuxer_feed_nalu(demux, demux->fu_buf, demux->fu_size, false, timestamp);
                demux->fu_size = 0;
                demux->fu_in_progress = false;
            }
        }
    } else if (demux->codec == AVX_RTSP_CODEC_HEVC) {
        if (payload_len < 2) return false;
        int nal_type = (payload[0] >> 1) & 0x3F;

        if (nal_type <= 47) {
            /* Single NAL Unit */
            avx_rtsp_demuxer_feed_nalu(demux, payload, payload_len, false, timestamp);
        } else if (nal_type == 48) {
            /* AP (Aggregation Packet) */
            size_t p = 2;
            while (p + 2 <= payload_len) {
                size_t nalu_len = ((size_t)payload[p] << 8) | (size_t)payload[p+1];
                p += 2;
                if (p + nalu_len > payload_len) break;
                avx_rtsp_demuxer_feed_nalu(demux, payload + p, nalu_len, false, timestamp);
                p += nalu_len;
            }
        } else if (nal_type == 49) {
            /* FU (Fragmentation Unit) */
            if (payload_len < 3) return false;
            uint8_t fu_header = payload[2];
            int start_bit = (fu_header >> 7) & 1;
            int end_bit = (fu_header >> 6) & 1;
            uint8_t fu_type = fu_header & 0x3F;

            uint8_t orig_h0 = (payload[0] & 0x81) | (fu_type << 1);
            uint8_t orig_h1 = payload[1];

            if (start_bit) {
                demux->fu_size = 0;
                ensure_buf_capacity(&demux->fu_buf, &demux->fu_cap, payload_len + 2);
                if (demux->fu_buf) {
                    demux->fu_buf[0] = orig_h0;
                    demux->fu_buf[1] = orig_h1;
                    memcpy(demux->fu_buf + 2, payload + 3, payload_len - 3);
                    demux->fu_size = 2 + (payload_len - 3);
                    demux->fu_in_progress = true;
                }
            } else if (demux->fu_in_progress) {
                ensure_buf_capacity(&demux->fu_buf, &demux->fu_cap, demux->fu_size + (payload_len - 3));
                if (demux->fu_buf) {
                    memcpy(demux->fu_buf + demux->fu_size, payload + 3, payload_len - 3);
                    demux->fu_size += (payload_len - 3);
                }
            }

            if (end_bit && demux->fu_in_progress) {
                avx_rtsp_demuxer_feed_nalu(demux, demux->fu_buf, demux->fu_size, false, timestamp);
                demux->fu_size = 0;
                demux->fu_in_progress = false;
            }
        }
    } else if (demux->codec == AVX_RTSP_CODEC_AV1) {
        avx_rtsp_demuxer_feed_nalu(demux, payload, payload_len, false, timestamp);
    }

    if (marker) {
        finalize_frame(demux);
        return true;
    }
    return false;
}

bool avx_rtsp_demuxer_get_frame(
    avx_rtsp_demuxer_t* demux,
    avx_rtsp_frame_t* out_frame
) {
    if (!demux || !out_frame || !demux->frame_ready) return false;
    *out_frame = demux->completed_frame;
    demux->frame_ready = false;
    return true;
}

/* ========================================================================= */
/* 4. RTSP 1.0 CLIENT ENGINE (RFC 2326)                                      */
/* ========================================================================= */

struct avx_rtsp_t {
    avx_socket_t tcp_fd;
    char url[256];
    char host[128];
    uint16_t port;
    char path[128];
    char session_id[64];
    int cseq;
    avx_rtsp_codec_e codec;
    char control_url[512];

    /* Socket read buffer */
    uint8_t rx_buf[65536];
    size_t rx_len;
    size_t rx_pos;
};

static bool parse_rtsp_url(const char *url, char *host, size_t host_sz, uint16_t *port, char *path, size_t path_sz) {
    if (!url || strncmp(url, "rtsp://", 7) != 0) return false;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    *port = 554;
    if (colon && (!slash || colon < slash)) {
        hlen = (size_t)(colon - p);
        *port = (uint16_t)atoi(colon + 1);
    }
    if (hlen >= host_sz) hlen = host_sz - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    if (slash) {
        size_t plen = strlen(slash);
        if (plen >= path_sz) plen = path_sz - 1;
        memcpy(path, slash, plen);
        path[plen] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    return true;
}

static ssize_t rtsp_send_request(avx_rtsp_t *r, const char *req) {
    if (!r || !avx_socket_is_valid(r->tcp_fd) || !req) return -1;
    size_t len = strlen(req);
    return send(r->tcp_fd, (const char*)req, (int)len, MSG_NOSIGNAL);
}

static int rtsp_read_response(avx_rtsp_t *r, char *out_resp, size_t max_resp, int timeout_ms) {
    if (!r || !avx_socket_is_valid(r->tcp_fd) || !out_resp || max_resp == 0) return -1;

    avx_pollfd_t pfd = { r->tcp_fd, POLLIN, 0 };
    int pr = avx_poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return -1;

    size_t total = 0;
    while (total < max_resp - 1) {
        ssize_t n = recv(r->tcp_fd, out_resp + total, 1, 0);
        if (n <= 0) break;
        total++;
        if (total >= 4 && memcmp(out_resp + total - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
    }
    out_resp[total] = '\0';

    /* Parse Content-Length for SDP body */
    const char *cl = strstr(out_resp, "Content-Length:");
    if (!cl) cl = strstr(out_resp, "content-length:");
    if (cl) {
        int body_len = atoi(cl + 15);
        if (body_len > 0 && total + body_len < max_resp - 1) {
            size_t body_read = 0;
            while (body_read < (size_t)body_len) {
                ssize_t n = recv(r->tcp_fd, out_resp + total + body_read, (size_t)body_len - body_read, 0);
                if (n <= 0) break;
                body_read += n;
            }
            total += body_read;
            out_resp[total] = '\0';
        }
    }

    return (int)total;
}

/* Connects RTSP 1.0 client (RFC 2326), performs DESCRIBE/SETUP/PLAY handshake, and configures interleaved TCP. */
avx_rtsp_t* avx_rtsp_connect(const char* url, int timeout_ms) {
    if (!url) return NULL;
    avx_rtsp_t *r = (avx_rtsp_t*)calloc(1, sizeof(*r));
    if (!r) return NULL;

    strncpy(r->url, url, sizeof(r->url) - 1);
    if (!parse_rtsp_url(url, r->host, sizeof(r->host), &r->port, r->path, sizeof(r->path))) {
        free(r);
        return NULL;
    }

    r->tcp_fd = avx_socket_connect_tcp(r->host, r->port);
    if (!avx_socket_is_valid(r->tcp_fd)) {
        free(r);
        return NULL;
    }

    char req[1024];
    char resp[8192];

    /* 1. OPTIONS */
    r->cseq = 1;
    snprintf(req, sizeof(req), "OPTIONS %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: OpenAVX/1.0\r\n\r\n", r->url, r->cseq++);
    rtsp_send_request(r, req);
    if (rtsp_read_response(r, resp, sizeof(resp), timeout_ms) <= 0) {
        avx_socket_close(r->tcp_fd);
        free(r);
        return NULL;
    }

    /* 2. DESCRIBE */
    snprintf(req, sizeof(req), "DESCRIBE %s RTSP/1.0\r\nCSeq: %d\r\nAccept: application/sdp\r\nUser-Agent: OpenAVX/1.0\r\n\r\n", r->url, r->cseq++);
    rtsp_send_request(r, req);
    if (rtsp_read_response(r, resp, sizeof(resp), timeout_ms) <= 0) {
        avx_socket_close(r->tcp_fd);
        free(r);
        return NULL;
    }

    /* Parse SDP for codec and control track */
    r->codec = AVX_RTSP_CODEC_H264;
    if (strstr(resp, "H265") || strstr(resp, "hevc") || strstr(resp, "HEVC")) {
        r->codec = AVX_RTSP_CODEC_HEVC;
    }

    /* Find control track URL */
    const char *ctrl = strstr(resp, "a=control:");
    if (ctrl) {
        const char *eol = strstr(ctrl, "\r\n");
        if (eol) {
            size_t clen = (size_t)(eol - (ctrl + 10));
            if (clen < sizeof(r->control_url)) {
                strncpy(r->control_url, ctrl + 10, clen);
                r->control_url[clen] = '\0';
            }
        }
    }
    if (r->control_url[0] == '\0' || strcmp(r->control_url, "*") == 0) {
        snprintf(r->control_url, sizeof(r->control_url), "%s", r->url);
    } else if (strncmp(r->control_url, "rtsp://", 7) != 0) {
        /* Relative control track: append to url */
        char rel_track[128];
        snprintf(rel_track, sizeof(rel_track), "%s", r->control_url);
        snprintf(r->control_url, sizeof(r->control_url), "%s/%s", r->url, rel_track);
    }

    /* 3. SETUP (RTP over TCP Interleaved channel 0-1) */
    snprintf(req, sizeof(req),
        "SETUP %s RTSP/1.0\r\nCSeq: %d\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nUser-Agent: OpenAVX/1.0\r\n\r\n",
        r->control_url, r->cseq++);
    rtsp_send_request(r, req);
    if (rtsp_read_response(r, resp, sizeof(resp), timeout_ms) <= 0) {
        avx_socket_close(r->tcp_fd);
        free(r);
        return NULL;
    }

    /* Extract Session ID */
    const char *sess = strstr(resp, "Session:");
    if (!sess) sess = strstr(resp, "session:");
    if (sess) {
        sess += 8;
        while (*sess == ' ') sess++;
        const char *e = sess;
        while (*e && *e != ';' && *e != '\r' && *e != '\n' && *e != ' ') e++;
        size_t slen = (size_t)(e - sess);
        if (slen < sizeof(r->session_id)) {
            strncpy(r->session_id, sess, slen);
            r->session_id[slen] = '\0';
        }
    }

    /* 4. PLAY */
    snprintf(req, sizeof(req),
        "PLAY %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\nRange: npt=0.000-\r\nUser-Agent: OpenAVX/1.0\r\n\r\n",
        r->url, r->cseq++, r->session_id);
    rtsp_send_request(r, req);
    rtsp_read_response(r, resp, sizeof(resp), timeout_ms);

    return r;
}

void avx_rtsp_close(avx_rtsp_t* rtsp) {
    if (!rtsp) return;
    if (avx_socket_is_valid(rtsp->tcp_fd)) {
        if (rtsp->session_id[0] != '\0') {
            char req[512];
            snprintf(req, sizeof(req),
                "TEARDOWN %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                rtsp->url, rtsp->cseq++, rtsp->session_id);
            rtsp_send_request(rtsp, req);
        }
        avx_socket_close(rtsp->tcp_fd);
        rtsp->tcp_fd = AVX_INVALID_SOCKET;
    }
    free(rtsp);
}

bool avx_rtsp_send_keyframe_request(avx_rtsp_t* rtsp) {
    if (!rtsp || !avx_socket_is_valid(rtsp->tcp_fd)) return false;

    /* RFC 4585 RTCP PLI (Picture Loss Indication) packet */
    /* Interleaved header: '$' (0x24), channel 1, 2-byte length (12 bytes) */
    uint8_t pli[16] = {
        0x24, 0x01, 0x00, 0x0C, /* Interleaved framing: channel 1, len 12 */
        0x81, 0xCE, 0x00, 0x02, /* V=2, P=0, FMT=1 (PLI), PT=206 (Payload-Specific FB), length=2 words */
        0x00, 0x00, 0x00, 0x01, /* Sender SSRC = 1 */
        0x00, 0x00, 0x00, 0x00  /* Media source SSRC = 0 (all) */
    };

    ssize_t s = send(rtsp->tcp_fd, (const char*)pli, sizeof(pli), MSG_NOSIGNAL);
    return (s == (ssize_t)sizeof(pli));
}

int avx_rtsp_read_frame(
    avx_rtsp_t* rtsp,
    avx_rtsp_demuxer_t* demux,
    avx_rtsp_frame_t* out_frame,
    int timeout_ms
) {
    if (!rtsp || !avx_socket_is_valid(rtsp->tcp_fd) || !demux || !out_frame) return -1;

    int64_t start_ms = avx_time_ms();

    while (1) {
        int rem_ms = timeout_ms;
        if (timeout_ms > 0) {
            int64_t elapsed = avx_time_ms() - start_ms;
            if (elapsed >= timeout_ms) return 0; /* Timed out */
            rem_ms = timeout_ms - (int)elapsed;
        }

        avx_pollfd_t pfd = { rtsp->tcp_fd, POLLIN, 0 };
        int pr = avx_poll(&pfd, 1, rem_ms);
        if (pr <= 0) return 0; /* Timeout */

        /* Read interleaved framing: '$' + channel (1 byte) + length (2 bytes) */
        uint8_t hdr[4];
        size_t rd = 0;
        while (rd < 4) {
            ssize_t n = recv(rtsp->tcp_fd, (char*)hdr + rd, (int)(4 - rd), 0);
            if (n <= 0) return -1;
            rd += (size_t)n;
        }

        if (hdr[0] != 0x24) {
            /* Out of sync, scan for next '$' */
            continue;
        }

        uint8_t channel = hdr[1];
        uint16_t pkt_len = ((uint16_t)hdr[2] << 8) | (uint16_t)hdr[3];

        uint8_t *pkt = (uint8_t*)malloc(pkt_len);
        if (!pkt) return -1;

        size_t pkt_rd = 0;
        while (pkt_rd < pkt_len) {
            ssize_t n = recv(rtsp->tcp_fd, (char*)pkt + pkt_rd, (int)((size_t)pkt_len - pkt_rd), 0);
            if (n <= 0) {
                free(pkt);
                return -1;
            }
            pkt_rd += (size_t)n;
        }

        bool frame_completed = false;
        if (channel == 0) {
            /* Video RTP packet */
            frame_completed = avx_rtsp_demuxer_feed_rtp(demux, pkt, pkt_len);
            if (frame_completed) {
                avx_rtsp_demuxer_get_frame(demux, out_frame);
            }
        }

        free(pkt);
        if (frame_completed) {
            return 1;
        }
    }
}

/* ========================================================================= */
/* 5. BASE64 ENCODER (RFC 4648)                                              */
/* ========================================================================= */

OPENAVX_API size_t avx_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_max) {
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!dst || dst_max == 0) return 0;
    if (!src || src_len == 0) {
        dst[0] = '\0';
        return 0;
    }

    size_t out_len = ((src_len + 2) / 3) * 4;
    if (dst_max <= out_len) return 0; /* Insufficient destination capacity */

    size_t di = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        uint32_t b0 = src[i];
        uint32_t b1 = (i + 1 < src_len) ? src[i + 1] : 0;
        uint32_t b2 = (i + 2 < src_len) ? src[i + 2] : 0;
        uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        dst[di++] = b64_table[(triple >> 18) & 0x3F];
        dst[di++] = b64_table[(triple >> 12) & 0x3F];
        dst[di++] = (i + 1 < src_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        dst[di++] = (i + 2 < src_len) ? b64_table[triple & 0x3F] : '=';
    }
    dst[di] = '\0';
    return di;
}

/* ========================================================================= */
/* 6. RTP PACKETIZER (RFC 6184, RFC 7798, RFC 9584)                          */
/* ========================================================================= */

#define AVX_RTP_MAX_PAYLOAD 1400

static inline void avx_write_rtp_header(
    uint8_t *hdr,
    bool marker,
    uint8_t payload_type,
    uint16_t seq,
    uint32_t timestamp,
    uint32_t ssrc
) {
    hdr[0] = 0x80; /* V=2, P=0, X=0, CC=0 */
    hdr[1] = (marker ? 0x80 : 0x00) | (payload_type & 0x7F);
    hdr[2] = (uint8_t)(seq >> 8);
    hdr[3] = (uint8_t)(seq & 0xFF);
    hdr[4] = (uint8_t)(timestamp >> 24);
    hdr[5] = (uint8_t)((timestamp >> 16) & 0xFF);
    hdr[6] = (uint8_t)((timestamp >> 8) & 0xFF);
    hdr[7] = (uint8_t)(timestamp & 0xFF);
    hdr[8] = (uint8_t)(ssrc >> 24);
    hdr[9] = (uint8_t)((ssrc >> 16) & 0xFF);
    hdr[10] = (uint8_t)((ssrc >> 8) & 0xFF);
    hdr[11] = (uint8_t)(ssrc & 0xFF);
}

/* Fragments NALU into MTU-safe RTP packets with RFC-compliant FU headers and sets marker bit on final slice. */
OPENAVX_API size_t avx_rtsp_rtp_packetize_nalu(
    avx_rtsp_codec_e codec,
    const uint8_t *nalu,
    size_t nalu_len,
    uint32_t rtp_timestamp,
    uint16_t *seq_num,
    uint32_t ssrc,
    bool is_last_nalu_in_frame,
    avx_rtsp_rtp_packet_cb cb,
    void *user_data
) {
    if (!nalu || nalu_len == 0 || !seq_num || !cb) return 0;

    /* Skip leading zero byte or prefix if passed by accident */
    while (nalu_len > 0 && nalu[0] == 0) {
        nalu++;
        nalu_len--;
    }
    if (nalu_len == 0) return 0;

    if (codec == AVX_RTSP_CODEC_H264) {
        if (nalu_len <= AVX_RTP_MAX_PAYLOAD) {
            /* Single NAL Unit Packet (RFC 6184 Section 5.6) */
            uint8_t pkt[12 + AVX_RTP_MAX_PAYLOAD];
            avx_write_rtp_header(pkt, is_last_nalu_in_frame, 96, (*seq_num)++, rtp_timestamp, ssrc);
            memcpy(pkt + 12, nalu, nalu_len);
            cb(pkt, 12 + nalu_len, user_data);
            return 1;
        } else {
            /* Fragmentation Unit A (FU-A) (RFC 6184 Section 5.8) */
            uint8_t orig_header = nalu[0];
            uint8_t fu_indicator = (orig_header & 0xE0) | 28; /* type 28 = FU-A */
            const uint8_t *payload_data = nalu + 1;
            size_t payload_rem = nalu_len - 1;
            const size_t max_chunk = AVX_RTP_MAX_PAYLOAD - 2;
            size_t offset = 0;
            size_t pkt_count = 0;

            while (offset < payload_rem) {
                size_t chunk_len = payload_rem - offset;
                if (chunk_len > max_chunk) chunk_len = max_chunk;

                bool is_start = (offset == 0);
                bool is_end = (offset + chunk_len == payload_rem);
                bool marker = is_end && is_last_nalu_in_frame;

                uint8_t fu_header = (orig_header & 0x1F);
                if (is_start) fu_header |= 0x80;
                if (is_end)   fu_header |= 0x40;

                uint8_t pkt[12 + 2 + AVX_RTP_MAX_PAYLOAD];
                avx_write_rtp_header(pkt, marker, 96, (*seq_num)++, rtp_timestamp, ssrc);
                pkt[12] = fu_indicator;
                pkt[13] = fu_header;
                memcpy(pkt + 14, payload_data + offset, chunk_len);

                cb(pkt, 14 + chunk_len, user_data);
                pkt_count++;
                offset += chunk_len;
            }
            return pkt_count;
        }
    } else if (codec == AVX_RTSP_CODEC_HEVC) {
        if (nalu_len < 2) return 0;
        int nal_unit_type = (nalu[0] >> 1) & 0x3F;

        if (nalu_len <= AVX_RTP_MAX_PAYLOAD) {
            /* Single NAL Unit Packet (RFC 7798 Section 4.4.1) */
            uint8_t pkt[12 + AVX_RTP_MAX_PAYLOAD];
            avx_write_rtp_header(pkt, is_last_nalu_in_frame, 96, (*seq_num)++, rtp_timestamp, ssrc);
            memcpy(pkt + 12, nalu, nalu_len);
            cb(pkt, 12 + nalu_len, user_data);
            return 1;
        } else {
            /* Fragmentation Unit (FU) (RFC 7798 Section 4.4.3) */
            uint8_t ph0 = (nalu[0] & 0x81) | (49 << 1); /* Type 49 = FU */
            uint8_t ph1 = nalu[1];
            const uint8_t *payload_data = nalu + 2;
            size_t payload_rem = nalu_len - 2;
            const size_t max_chunk = AVX_RTP_MAX_PAYLOAD - 3;
            size_t offset = 0;
            size_t pkt_count = 0;

            while (offset < payload_rem) {
                size_t chunk_len = payload_rem - offset;
                if (chunk_len > max_chunk) chunk_len = max_chunk;

                bool is_start = (offset == 0);
                bool is_end = (offset + chunk_len == payload_rem);
                bool marker = is_end && is_last_nalu_in_frame;

                uint8_t fu_header = (uint8_t)(nal_unit_type & 0x3F);
                if (is_start) fu_header |= 0x80;
                if (is_end)   fu_header |= 0x40;

                uint8_t pkt[12 + 3 + AVX_RTP_MAX_PAYLOAD];
                avx_write_rtp_header(pkt, marker, 96, (*seq_num)++, rtp_timestamp, ssrc);
                pkt[12] = ph0;
                pkt[13] = ph1;
                pkt[14] = fu_header;
                memcpy(pkt + 15, payload_data + offset, chunk_len);

                cb(pkt, 15 + chunk_len, user_data);
                pkt_count++;
                offset += chunk_len;
            }
            return pkt_count;
        }
    } else {
        /* AV1 / Generic OBU (RFC 9584) */
        if (nalu_len <= AVX_RTP_MAX_PAYLOAD) {
            uint8_t pkt[12 + AVX_RTP_MAX_PAYLOAD];
            avx_write_rtp_header(pkt, is_last_nalu_in_frame, 96, (*seq_num)++, rtp_timestamp, ssrc);
            memcpy(pkt + 12, nalu, nalu_len);
            cb(pkt, 12 + nalu_len, user_data);
            return 1;
        } else {
            size_t offset = 0;
            size_t pkt_count = 0;
            while (offset < nalu_len) {
                size_t chunk_len = nalu_len - offset;
                if (chunk_len > AVX_RTP_MAX_PAYLOAD) chunk_len = AVX_RTP_MAX_PAYLOAD;
                bool is_end = (offset + chunk_len == nalu_len);
                bool marker = is_end && is_last_nalu_in_frame;

                uint8_t pkt[12 + AVX_RTP_MAX_PAYLOAD];
                avx_write_rtp_header(pkt, marker, 96, (*seq_num)++, rtp_timestamp, ssrc);
                memcpy(pkt + 12, nalu + offset, chunk_len);

                cb(pkt, 12 + chunk_len, user_data);
                pkt_count++;
                offset += chunk_len;
            }
            return pkt_count;
        }
    }
}

/* ========================================================================= */
/* 7. NATIVE RTSP 1.0 SERVER ENGINE (RFC 2326, RFC 4566)                     */
/* ========================================================================= */

#define AVX_RTSP_MAX_CLIENTS 16

typedef struct {
    avx_socket_t fd;
    bool active;
    bool playing;
    uint8_t rtp_channel;
    uint8_t rtcp_channel;
    char session_id[32];
    char req_buf[4096];
    size_t req_len;
} avx_rtsp_client_t;

struct avx_rtsp_server_t {
    avx_socket_t listen_fd;
    uint16_t port;
    char path[128];
    avx_rtsp_codec_e codec;
    void (*on_keyframe_request)(void *user_ctx);
    void *user_ctx;

    avx_thread_t thread;
    volatile bool running;
    avx_mutex_t lock;

    avx_rtsp_client_t clients[AVX_RTSP_MAX_CLIENTS];

    uint8_t *sps;
    size_t sps_len;
    uint8_t *pps;
    size_t pps_len;
    uint8_t *vps;
    size_t vps_len;
    int width;
    int height;
    int profile;
    int level;

    uint16_t rtp_seq;
    uint32_t ssrc;
    uint32_t last_rtp_ts;
};

static void generate_sdp(avx_rtsp_server_t *srv, char *sdp, size_t sdp_max, const char *url_prefix) {
    (void)url_prefix;
    char fmtp[1024] = {0};

    if (srv->codec == AVX_RTSP_CODEC_H264) {
        if (srv->sps && srv->pps && srv->sps_len > 0 && srv->pps_len > 0) {
            char sps_b64[256] = {0};
            char pps_b64[256] = {0};
            avx_base64_encode(srv->sps, srv->sps_len, sps_b64, sizeof(sps_b64));
            avx_base64_encode(srv->pps, srv->pps_len, pps_b64, sizeof(pps_b64));

            uint8_t p0 = srv->sps_len > 1 ? srv->sps[1] : 0x42;
            uint8_t p1 = srv->sps_len > 2 ? srv->sps[2] : 0xE0;
            uint8_t p2 = srv->sps_len > 3 ? srv->sps[3] : 0x1F;

            snprintf(fmtp, sizeof(fmtp),
                "a=fmtp:96 packetization-mode=1;profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s\r\n",
                p0, p1, p2, sps_b64, pps_b64);
        } else {
            snprintf(fmtp, sizeof(fmtp), "a=fmtp:96 packetization-mode=1\r\n");
        }
        snprintf(sdp, sdp_max,
            "v=0\r\n"
            "o=- %u 1 IN IP4 127.0.0.1\r\n"
            "s=OpenAVX Live Stream\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "b=AS:10000\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "%s"
            "a=control:trackID=0\r\n",
            (uint32_t)srv->ssrc, fmtp);
    } else if (srv->codec == AVX_RTSP_CODEC_HEVC) {
        if (srv->sps && srv->pps && srv->sps_len > 0 && srv->pps_len > 0) {
            char sps_b64[256] = {0};
            char pps_b64[256] = {0};
            char vps_b64[256] = {0};
            avx_base64_encode(srv->sps, srv->sps_len, sps_b64, sizeof(sps_b64));
            avx_base64_encode(srv->pps, srv->pps_len, pps_b64, sizeof(pps_b64));
            if (srv->vps && srv->vps_len > 0) {
                avx_base64_encode(srv->vps, srv->vps_len, vps_b64, sizeof(vps_b64));
                snprintf(fmtp, sizeof(fmtp), "a=fmtp:96 sprop-vps=%s;sprop-sps=%s;sprop-pps=%s\r\n",
                         vps_b64, sps_b64, pps_b64);
            } else {
                snprintf(fmtp, sizeof(fmtp), "a=fmtp:96 sprop-sps=%s;sprop-pps=%s\r\n",
                         sps_b64, pps_b64);
            }
        } else {
            snprintf(fmtp, sizeof(fmtp), "a=fmtp:96\r\n");
        }
        snprintf(sdp, sdp_max,
            "v=0\r\n"
            "o=- %u 1 IN IP4 127.0.0.1\r\n"
            "s=OpenAVX Live Stream\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "b=AS:10000\r\n"
            "a=rtpmap:96 H265/90000\r\n"
            "%s"
            "a=control:trackID=0\r\n",
            (uint32_t)srv->ssrc, fmtp);
    } else {
        snprintf(sdp, sdp_max,
            "v=0\r\n"
            "o=- %u 1 IN IP4 127.0.0.1\r\n"
            "s=OpenAVX Live Stream\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "b=AS:10000\r\n"
            "a=rtpmap:96 AV1/90000\r\n"
            "a=fmtp:96 profile=0;level-idx-0=0\r\n"
            "a=control:trackID=0\r\n",
            (uint32_t)srv->ssrc);
    }
}

static void handle_rtsp_client_request(
    avx_rtsp_server_t *srv,
    avx_rtsp_client_t *c,
    const char *req,
    size_t req_len,
    bool *out_trigger_keyframe
) {
    (void)req_len;
    char method[32] = {0};
    char url[256] = {0};
    int cseq = 0;

    if (out_trigger_keyframe) *out_trigger_keyframe = false;

    if (sscanf(req, "%31s %255s", method, url) < 2) return;
    const char *cs = strstr(req, "CSeq:");
    if (!cs) cs = strstr(req, "cseq:");
    if (cs) {
        cseq = atoi(cs + 5);
    }

    if (strcmp(method, "OPTIONS") == 0) {
        char resp[512];
        snprintf(resp, sizeof(resp),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n",
            cseq);
        send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
    } else if (strcmp(method, "DESCRIBE") == 0) {
        char sdp[2048] = {0};
        generate_sdp(srv, sdp, sizeof(sdp), url);
        size_t sdp_len = strlen(sdp);
        char resp[4096];
        snprintf(resp, sizeof(resp),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Base: %s/\r\n"
            "Content-Length: %zu\r\n\r\n"
            "%s",
            cseq, url, sdp_len, sdp);
        send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
    } else if (strcmp(method, "SETUP") == 0) {
        const char *il = strstr(req, "interleaved=");
        if (il) {
            int ch0 = 0, ch1 = 1;
            if (sscanf(il + 12, "%d-%d", &ch0, &ch1) >= 1) {
                c->rtp_channel = (uint8_t)ch0;
                c->rtcp_channel = (uint8_t)ch1;
            }
        }
        snprintf(c->session_id, sizeof(c->session_id), "%08X", (uint32_t)(c->fd ^ 0xA5A5A5A5));
        char resp[512];
        snprintf(resp, sizeof(resp),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
            "Session: %s\r\n\r\n",
            cseq, c->rtp_channel, c->rtcp_channel, c->session_id);
        send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
    } else if (strcmp(method, "PLAY") == 0) {
        c->playing = true;
        char resp[512];
        snprintf(resp, sizeof(resp),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %s\r\n"
            "Range: npt=0.000-\r\n"
            "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=%u\r\n\r\n",
            cseq, c->session_id, url, srv->rtp_seq, srv->last_rtp_ts);
        send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);

        if (out_trigger_keyframe) *out_trigger_keyframe = true;
    } else if (strcmp(method, "PAUSE") == 0) {
        c->playing = false;
        char resp[512];
        snprintf(resp, sizeof(resp),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %s\r\n\r\n",
            cseq, c->session_id);
        send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
    } else if (strcmp(method, "TEARDOWN") == 0) {
        char resp[512];
        snprintf(resp, sizeof(resp),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %s\r\n\r\n",
            cseq, c->session_id);
        send(c->fd, resp, strlen(resp), MSG_NOSIGNAL);
        avx_socket_close(c->fd);
        c->fd = AVX_INVALID_SOCKET;
        c->active = false;
        c->playing = false;
    }
}

static void* avx_rtsp_server_worker(void *arg) {
    avx_rtsp_server_t *srv = (avx_rtsp_server_t*)arg;

    while (srv->running) {
        avx_pollfd_t pfds[AVX_RTSP_MAX_CLIENTS + 1];
        int client_indices[AVX_RTSP_MAX_CLIENTS + 1];
        int nfds = 0;

        avx_mutex_lock(&srv->lock);
        if (avx_socket_is_valid(srv->listen_fd)) {
            pfds[nfds].fd = srv->listen_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            client_indices[nfds] = -1;
            nfds++;
        }

        for (int i = 0; i < AVX_RTSP_MAX_CLIENTS; ++i) {
            if (srv->clients[i].active && avx_socket_is_valid(srv->clients[i].fd)) {
                pfds[nfds].fd = srv->clients[i].fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                client_indices[nfds] = i;
                nfds++;
            }
        }
        avx_mutex_unlock(&srv->lock);

        if (nfds == 0) {
            avx_sleep_ms(10);
            continue;
        }

        int pr = avx_poll(pfds, (avx_nfds_t)nfds, 50);
        if (pr <= 0) continue;

        bool trigger_keyframe = false;

        for (int j = 0; j < nfds; ++j) {
            if (!(pfds[j].revents & POLLIN)) continue;

            if (client_indices[j] == -1) {
                /* New connection on listener */
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                avx_socket_t cfd = accept(srv->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
                if (avx_socket_is_valid(cfd)) {
                    avx_socket_set_tcp_nodelay(cfd, true);
                    avx_mutex_lock(&srv->lock);
                    int slot = -1;
                    for (int i = 0; i < AVX_RTSP_MAX_CLIENTS; ++i) {
                        if (!srv->clients[i].active) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot >= 0) {
                        srv->clients[slot].fd = cfd;
                        srv->clients[slot].active = true;
                        srv->clients[slot].playing = false;
                        srv->clients[slot].rtp_channel = 0;
                        srv->clients[slot].rtcp_channel = 1;
                        srv->clients[slot].req_len = 0;
                        srv->clients[slot].session_id[0] = '\0';
                    } else {
                        avx_socket_close(cfd);
                    }
                    avx_mutex_unlock(&srv->lock);
                }
            } else {
                /* Data from existing client */
                int idx = client_indices[j];
                avx_mutex_lock(&srv->lock);
                if (srv->clients[idx].active && srv->clients[idx].fd == pfds[j].fd) {
                    avx_rtsp_client_t *c = &srv->clients[idx];
                    size_t avail = sizeof(c->req_buf) - 1 - c->req_len;
                    if (avail > 0) {
                        ssize_t n = recv(c->fd, c->req_buf + c->req_len, avail, 0);
                        if (n <= 0) {
                            avx_socket_close(c->fd);
                            c->fd = AVX_INVALID_SOCKET;
                            c->active = false;
                            c->playing = false;
                        } else {
                            c->req_len += (size_t)n;
                            c->req_buf[c->req_len] = '\0';

                            /* Process incoming commands / packets in req_buf */
                            while (c->req_len > 0) {
                                if ((uint8_t)c->req_buf[0] == 0x24) { /* '$' interleaved framing */
                                    if (c->req_len < 4) break;
                                    uint16_t plen = ((uint16_t)(uint8_t)c->req_buf[2] << 8) | (uint16_t)(uint8_t)c->req_buf[3];
                                    if (c->req_len < (size_t)(4 + plen)) break;

                                    uint8_t ch = (uint8_t)c->req_buf[1];
                                    if (ch == c->rtcp_channel && plen >= 12) {
                                        /* RTCP packet: Check for PLI (PT=206, FMT=1) */
                                        uint8_t pt = (uint8_t)c->req_buf[5];
                                        uint8_t fmt = (uint8_t)c->req_buf[4] & 0x1F;
                                        if ((pt == 206 && fmt == 1) || pt == 205) {
                                            trigger_keyframe = true;
                                        }
                                    }
                                    size_t total_pkt = 4 + plen;
                                    memmove(c->req_buf, c->req_buf + total_pkt, c->req_len - total_pkt);
                                    c->req_len -= total_pkt;
                                    c->req_buf[c->req_len] = '\0';
                                } else {
                                    /* RTSP text message: search for \r\n\r\n */
                                    char *hdr_end = strstr(c->req_buf, "\r\n\r\n");
                                    if (!hdr_end) break;

                                    size_t msg_len = (size_t)(hdr_end - c->req_buf) + 4;
                                    bool kf = false;
                                    handle_rtsp_client_request(srv, c, c->req_buf, msg_len, &kf);
                                    if (kf) trigger_keyframe = true;

                                    memmove(c->req_buf, c->req_buf + msg_len, c->req_len - msg_len);
                                    c->req_len -= msg_len;
                                    c->req_buf[c->req_len] = '\0';
                                }
                            }
                        }
                    }
                }
                avx_mutex_unlock(&srv->lock);
            }
        }

        if (trigger_keyframe && srv->on_keyframe_request) {
            srv->on_keyframe_request(srv->user_ctx);
        }
    }

    return NULL;
}

OPENAVX_API avx_rtsp_server_t* avx_rtsp_server_create(const avx_rtsp_server_config_t *config) {
    avx_rtsp_server_t *srv = (avx_rtsp_server_t*)calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->port = (config && config->port != 0) ? config->port : 8554;
    const char *p = (config && config->path && config->path[0]) ? config->path : "/live";
    strncpy(srv->path, p, sizeof(srv->path) - 1);
    srv->codec = (config && config->codec != AVX_RTSP_CODEC_UNKNOWN) ? config->codec : AVX_RTSP_CODEC_H264;
    if (config) {
        srv->on_keyframe_request = config->on_keyframe_request;
        srv->user_ctx = config->user_ctx;
    }

    srv->ssrc = 0x12345678;
    srv->rtp_seq = 1;
    avx_mutex_init(&srv->lock);

    for (int i = 0; i < AVX_RTSP_MAX_CLIENTS; ++i) {
        srv->clients[i].fd = AVX_INVALID_SOCKET;
    }

    /* Create TCP Listener Socket */
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!avx_socket_is_valid(srv->listen_fd)) {
        avx_mutex_destroy(&srv->lock);
        free(srv);
        return NULL;
    }

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(srv->port);

    if (bind(srv->listen_fd, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        /* If port was 8554 and busy, try ephemeral port 0 */
        if (srv->port == 8554) {
            sin.sin_port = 0;
            if (bind(srv->listen_fd, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
                avx_socket_close(srv->listen_fd);
                avx_mutex_destroy(&srv->lock);
                free(srv);
                return NULL;
            }
        } else {
            avx_socket_close(srv->listen_fd);
            avx_mutex_destroy(&srv->lock);
            free(srv);
            return NULL;
        }
    }

    if (listen(srv->listen_fd, 8) != 0) {
        avx_socket_close(srv->listen_fd);
        avx_mutex_destroy(&srv->lock);
        free(srv);
        return NULL;
    }

    /* Retrieve actual bound port */
    socklen_t slen = sizeof(sin);
    if (getsockname(srv->listen_fd, (struct sockaddr*)&sin, &slen) == 0) {
        srv->port = ntohs(sin.sin_port);
    }

    srv->running = true;
    if (avx_thread_create(&srv->thread, avx_rtsp_server_worker, srv) != 0) {
        srv->running = false;
        avx_socket_close(srv->listen_fd);
        avx_mutex_destroy(&srv->lock);
        free(srv);
        return NULL;
    }

    return srv;
}

OPENAVX_API void avx_rtsp_server_destroy(avx_rtsp_server_t *srv) {
    if (!srv) return;

    srv->running = false;
    if (avx_socket_is_valid(srv->listen_fd)) {
        avx_socket_close(srv->listen_fd);
        srv->listen_fd = AVX_INVALID_SOCKET;
    }

    avx_thread_join(srv->thread);

    avx_mutex_lock(&srv->lock);
    for (int i = 0; i < AVX_RTSP_MAX_CLIENTS; ++i) {
        if (srv->clients[i].active && avx_socket_is_valid(srv->clients[i].fd)) {
            avx_socket_close(srv->clients[i].fd);
            srv->clients[i].fd = AVX_INVALID_SOCKET;
            srv->clients[i].active = false;
        }
    }
    free(srv->sps);
    free(srv->pps);
    free(srv->vps);
    avx_mutex_unlock(&srv->lock);
    avx_mutex_destroy(&srv->lock);
    free(srv);
}

OPENAVX_API int avx_rtsp_server_get_client_count(avx_rtsp_server_t *srv) {
    if (!srv) return 0;
    avx_mutex_lock(&srv->lock);
    int count = 0;
    for (int i = 0; i < AVX_RTSP_MAX_CLIENTS; ++i) {
        if (srv->clients[i].active && srv->clients[i].playing) {
            count++;
        }
    }
    avx_mutex_unlock(&srv->lock);
    return count;
}

OPENAVX_API uint16_t avx_rtsp_server_get_port(avx_rtsp_server_t *srv) {
    return srv ? srv->port : 0;
}

typedef struct {
    avx_rtsp_server_t *srv;
    uint8_t channel;
} rtp_send_arg_t;

static void rtp_broadcast_cb(const uint8_t *rtp_pkt, size_t rtp_len, void *user_data) {
    rtp_send_arg_t *arg = (rtp_send_arg_t*)user_data;
    avx_rtsp_server_t *srv = arg->srv;

    uint8_t prefix[4];
    prefix[0] = 0x24; /* '$' */
    prefix[1] = 0x00; /* channel 0 for video */
    prefix[2] = (uint8_t)(rtp_len >> 8);
    prefix[3] = (uint8_t)(rtp_len & 0xFF);

    for (int i = 0; i < AVX_RTSP_MAX_CLIENTS; ++i) {
        if (!srv->clients[i].active || !srv->clients[i].playing) continue;
        prefix[1] = srv->clients[i].rtp_channel;

        ssize_t s1 = send(srv->clients[i].fd, (const char*)prefix, 4, MSG_NOSIGNAL);
        ssize_t s2 = send(srv->clients[i].fd, (const char*)rtp_pkt, (int)rtp_len, MSG_NOSIGNAL);
        if (s1 <= 0 || s2 <= 0) {
            avx_socket_close(srv->clients[i].fd);
            srv->clients[i].fd = AVX_INVALID_SOCKET;
            srv->clients[i].active = false;
            srv->clients[i].playing = false;
        }
    }
}

OPENAVX_API bool avx_rtsp_server_feed_frame(
    avx_rtsp_server_t *srv,
    const uint8_t *data,
    size_t size,
    int64_t pts_ns,
    bool is_keyframe
) {
    (void)is_keyframe;
    if (!srv || !data || size == 0) return false;

    avx_mutex_lock(&srv->lock);

    /* 1. Extract NALUs from Annex-B bitstream */
    typedef struct {
        const uint8_t *ptr;
        size_t len;
    } nalu_entry_t;
    nalu_entry_t nalus[128];
    size_t num_nalus = 0;

    size_t pos = 0;
    while (pos + 3 <= size && num_nalus < 128) {
        size_t sc_len = 0;
        if (data[pos] == 0 && data[pos+1] == 0) {
            if (data[pos+2] == 1) sc_len = 3;
            else if (pos + 4 <= size && data[pos+2] == 0 && data[pos+3] == 1) sc_len = 4;
        }

        if (sc_len > 0) {
            size_t nalu_start = pos + sc_len;
            size_t next_pos = nalu_start;
            while (next_pos + 3 <= size) {
                if (data[next_pos] == 0 && data[next_pos+1] == 0 &&
                    (data[next_pos+2] == 1 || (next_pos + 4 <= size && data[next_pos+2] == 0 && data[next_pos+3] == 1))) {
                    break;
                }
                next_pos++;
            }
            if (next_pos + 3 > size) next_pos = size;

            size_t nalu_len = next_pos - nalu_start;
            if (nalu_len > 0) {
                nalus[num_nalus].ptr = data + nalu_start;
                nalus[num_nalus].len = nalu_len;
                num_nalus++;
            }
            pos = next_pos;
        } else {
            pos++;
        }
    }

    if (num_nalus == 0) {
        nalus[0].ptr = data;
        nalus[0].len = size;
        num_nalus = 1;
    }

    /* 2. Inspect and cache parameter sets */
    for (size_t i = 0; i < num_nalus; ++i) {
        const uint8_t *np = nalus[i].ptr;
        size_t nl = nalus[i].len;
        if (nl == 0) continue;

        if (srv->codec == AVX_RTSP_CODEC_H264) {
            uint8_t type = np[0] & 0x1F;
            if (type == 7) { /* SPS */
                free(srv->sps);
                srv->sps = (uint8_t*)malloc(nl);
                if (srv->sps) {
                    memcpy(srv->sps, np, nl);
                    srv->sps_len = nl;
                    avx_rtsp_parse_sps_h264(srv->sps, srv->sps_len, &srv->width, &srv->height, &srv->profile, &srv->level);
                }
            } else if (type == 8) { /* PPS */
                free(srv->pps);
                srv->pps = (uint8_t*)malloc(nl);
                if (srv->pps) {
                    memcpy(srv->pps, np, nl);
                    srv->pps_len = nl;
                }
            }
        } else if (srv->codec == AVX_RTSP_CODEC_HEVC) {
            uint8_t type = (np[0] >> 1) & 0x3F;
            if (type == 32) { /* VPS */
                free(srv->vps);
                srv->vps = (uint8_t*)malloc(nl);
                if (srv->vps) {
                    memcpy(srv->vps, np, nl);
                    srv->vps_len = nl;
                }
            } else if (type == 33) { /* SPS */
                free(srv->sps);
                srv->sps = (uint8_t*)malloc(nl);
                if (srv->sps) {
                    memcpy(srv->sps, np, nl);
                    srv->sps_len = nl;
                    avx_rtsp_parse_sps_hevc(srv->sps, srv->sps_len, &srv->width, &srv->height, &srv->profile, &srv->level);
                }
            } else if (type == 34) { /* PPS */
                free(srv->pps);
                srv->pps = (uint8_t*)malloc(nl);
                if (srv->pps) {
                    memcpy(srv->pps, np, nl);
                    srv->pps_len = nl;
                }
            }
        } else if (srv->codec == AVX_RTSP_CODEC_AV1) {
            uint8_t type = (np[0] >> 3) & 0x0F;
            if (type == 1) { /* Sequence Header */
                free(srv->sps);
                srv->sps = (uint8_t*)malloc(nl);
                if (srv->sps) {
                    memcpy(srv->sps, np, nl);
                    srv->sps_len = nl;
                    avx_rtsp_parse_sequence_header_av1(srv->sps, srv->sps_len, &srv->width, &srv->height, &srv->profile, &srv->level);
                }
            }
        }
    }

    /* 3. Check if any client is active and playing */
    bool has_playing = false;
    for (int i = 0; i < AVX_RTSP_MAX_CLIENTS; ++i) {
        if (srv->clients[i].active && srv->clients[i].playing) {
            has_playing = true;
            break;
        }
    }

    if (!has_playing) {
        avx_mutex_unlock(&srv->lock);
        return true;
    }

    /* 4. Timestamp in 90kHz units */
    uint32_t rtp_ts;
    if (pts_ns >= 0) {
        rtp_ts = (uint32_t)(((uint64_t)pts_ns * 90ULL) / 1000000ULL);
    } else {
        rtp_ts = srv->last_rtp_ts + 1500;
    }
    srv->last_rtp_ts = rtp_ts;

    /* 5. Packetize and broadcast each NALU */
    rtp_send_arg_t send_arg = { srv, 0 };
    for (size_t i = 0; i < num_nalus; ++i) {
        bool is_last_nalu = (i == num_nalus - 1);
        avx_rtsp_rtp_packetize_nalu(
            srv->codec,
            nalus[i].ptr,
            nalus[i].len,
            rtp_ts,
            &srv->rtp_seq,
            srv->ssrc,
            is_last_nalu,
            rtp_broadcast_cb,
            &send_arg
        );
    }

    avx_mutex_unlock(&srv->lock);
    return true;
}
