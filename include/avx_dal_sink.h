/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/avx_dal_sink.h - macOS CoreMediaIO DAL Virtual Camera Plugin
 */

#ifndef AVX_DAL_SINK_H
#define AVX_DAL_SINK_H

#include <stdint.h>
#include <stdbool.h>
#include "openavx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Installs the OpenAVX CoreMediaIO DAL Virtual Camera plugin into:
 *   ~/Library/CoreMediaIO/Plug-Ins/DAL/OpenAVX.plugin
 * Zero sudo / zero root required.
 */
OPENAVX_API bool avx_dal_install_plugin(void);

/**
 * Uninstalls the OpenAVX DAL plugin from ~/Library/CoreMediaIO/Plug-Ins/DAL/.
 */
OPENAVX_API bool avx_dal_uninstall_plugin(void);

/**
 * Checks if the OpenAVX DAL plugin is currently installed in the user Library.
 */
OPENAVX_API bool avx_dal_is_installed(void);

#ifdef __cplusplus
}
#endif

#endif /* AVX_DAL_SINK_H */
