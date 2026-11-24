/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_cli.h - CLI subcommand entry points and shared UI/logging helpers
 */

#ifndef OPENAVX_CLI_H
#define OPENAVX_CLI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void openavx_cli_init(void);
void openavx_cli_set_quiet(bool quiet);
void openavx_cli_header(const char* tool_name);
void openavx_cli_kv(const char* key, const char* fmt, ...);
void openavx_cli_status(const char* module, const char* fmt, ...);
void openavx_cli_status_end(void);

int openavx_cmd_find(int argc, char** argv);
int openavx_cmd_monitor(int argc, char** argv);
int openavx_cmd_route(int argc, char** argv);
int openavx_cmd_pattern(int argc, char** argv);
int openavx_cmd_server(int argc, char** argv);
int openavx_cmd_gateway(int argc, char** argv);
int openavx_cmd_virtcam(int argc, char** argv);
int openavx_cmd_tally(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif /* OPENAVX_CLI_H */
