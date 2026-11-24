/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_dshow_sink.h - Windows DirectShow Virtual Camera COM Filter
 */

#ifndef AVX_DSHOW_SINK_H
#define AVX_DSHOW_SINK_H

#include <stdint.h>
#include <stdbool.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Registers the OpenAVX DirectShow Virtual Camera in the user's registry (HKCU).
 * Zero administrator rights required.
 * dll_path: Path to openavx_dshow_filter.dll (or NULL to auto-detect current module directory).
 */
OPENAVX_API bool avx_dshow_register_filter(const char *dll_path);

/**
 * Unregisters the OpenAVX DirectShow Virtual Camera from HKCU.
 */
OPENAVX_API bool avx_dshow_unregister_filter(void);

/**
 * Checks if the OpenAVX DirectShow Virtual Camera is registered on the system.
 */
OPENAVX_API bool avx_dshow_is_registered(void);

#ifdef __cplusplus
}
#endif

#endif /* AVX_DSHOW_SINK_H */
