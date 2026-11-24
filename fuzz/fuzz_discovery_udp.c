/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * fuzz/fuzz_discovery_udp.c - libFuzzer Target for Discovery Wire & DNS-SD Parsing
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "avx_wire.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size == 0) return 0;

    /* 1. Fuzz DNS label encoder and service scanner */
    char *str_input = (char *)malloc(size + 1);
    if (str_input) {
        memcpy(str_input, data, size);
        str_input[size] = '\0';

        uint8_t encoded[256];
        size_t enc_len = avx_wire_encode_dns_service(str_input, encoded, sizeof(encoded));

        /* Exercise bounded output capacities */
        uint8_t small_encoded[32];
        size_t small_cap = (size_t)data[0] % sizeof(small_encoded);
        (void)avx_wire_encode_dns_service(str_input, small_encoded, small_cap);

        size_t match_offset = 0;
        (void)avx_wire_match_service(data, size, str_input, &match_offset);
        (void)avx_wire_match_service(data, size, OAVX_WIRE_MDNS_SERVICE, &match_offset);
        if (enc_len > 0) {
            (void)avx_wire_match_service(encoded, enc_len, str_input, &match_offset);
        }

        free(str_input);
    }

    return 0;
}
