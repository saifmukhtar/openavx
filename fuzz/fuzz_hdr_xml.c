/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_hdr_xml.c - libFuzzer Target for HDR & Color Metadata XML Parser
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "avx_hdr.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size == 0) return 0;

    /* Ensure payload is null-terminated */
    char *xml_str = (char *)malloc(size + 1);
    if (!xml_str) return 0;
    memcpy(xml_str, data, size);
    xml_str[size] = '\0';

    avx_color_info_t info;
    if (avx_hdr_parse_color_info(xml_str, &info)) {
        (void)avx_hdr_is_hdr(&info);

        char out_xml[512];
        (void)avx_hdr_format_color_info(&info, out_xml, sizeof(out_xml));

        /* Verify frame helpers */
        oavx_video_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        char xml_buf[512];
        if (avx_hdr_frame_set_color_info(&frame, &info, xml_buf, sizeof(xml_buf))) {
            avx_color_info_t parsed_info;
            (void)avx_hdr_frame_get_color_info(&frame, &parsed_info);
        }
    }

    free(xml_str);
    return 0;
}
