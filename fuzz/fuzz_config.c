/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_config.c - libFuzzer Target for JSON Configuration Parser
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "avx_config.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size == 0) return 0;

    /* Ensure payload is null-terminated string */
    char *json_str = (char *)malloc(size + 1);
    if (!json_str) return 0;
    memcpy(json_str, data, size);
    json_str[size] = '\0';

    avx_config_t cfg;
    avx_config_parse_json(json_str, &cfg);

    free(json_str);
    return 0;
}
