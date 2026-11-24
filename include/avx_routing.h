/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_routing.h - Virtual matrix routing interface
 */

#ifndef AVX_ROUTING_H
#define AVX_ROUTING_H

#include "openavx.h"
#include <stdbool.h>

#ifndef AVX_ROUTER_INSTANCE_T
#define AVX_ROUTER_INSTANCE_T
typedef void* avx_router_instance_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns the active advertised source for downstream clients connecting to this router.
 */
OPENAVX_API oavx_endpoint_t avx_router_get_source(avx_router_instance_t p_instance);

#ifdef __cplusplus
}
#endif

#endif /* AVX_ROUTING_H */
