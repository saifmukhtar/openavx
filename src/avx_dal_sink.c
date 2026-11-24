/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * src/avx_dal_sink.c - macOS CoreMediaIO DAL Virtual Camera Plugin
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "openavx.h"
#include "avx_dal_sink.h"
#include "avx_virtcam_ipc.h"
#include "avx_platform.h"

#if defined(AVX_OS_MACOS)
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

static bool get_user_dal_dir(char *out_path, size_t out_max) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return false;
    snprintf(out_path, out_max, "%s/Library/CoreMediaIO/Plug-Ins/DAL", home);
    return true;
}

/* Deploys user-space CoreMediaIO DAL bundle structure and Info.plist into ~/Library. */
bool avx_dal_install_plugin(void) {
    char dal_dir[512];
    if (!get_user_dal_dir(dal_dir, sizeof(dal_dir))) return false;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s/OpenAVX.plugin/Contents/MacOS\"", dal_dir);
    if (system(cmd) != 0) return false;

    /* Write minimal Info.plist defining the CoreMediaIO DAL Assistant bundle */
    char plist_path[512];
    snprintf(plist_path, sizeof(plist_path), "%s/OpenAVX.plugin/Contents/Info.plist", dal_dir);
    FILE *f = fopen(plist_path, "w");
    if (!f) return false;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\">\n<dict>\n"
               "    <key>CFBundleName</key><string>OpenAVX</string>\n"
               "    <key>CFBundleIdentifier</key><string>org.openavx.dal</string>\n"
               "    <key>CFBundleVersion</key><string>1.0</string>\n"
               "    <key>CMIOHardwarePlugInFactory</key><string>OpenAVXDALPluginFactory</string>\n"
               "</dict>\n</plist>\n");
    fclose(f);

    oavx_log_write(OAVX_LOG_INFO, "dal", "Installed user CoreMediaIO DAL virtual camera plugin into %s/OpenAVX.plugin", dal_dir);
    return true;
}

bool avx_dal_uninstall_plugin(void) {
    char dal_dir[512];
    if (!get_user_dal_dir(dal_dir, sizeof(dal_dir))) return false;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s/OpenAVX.plugin\"", dal_dir);
    return (system(cmd) == 0);
}

bool avx_dal_is_installed(void) {
    char dal_dir[512];
    if (!get_user_dal_dir(dal_dir, sizeof(dal_dir))) return false;

    char plugin_path[512];
    snprintf(plugin_path, sizeof(plugin_path), "%s/OpenAVX.plugin", dal_dir);
    struct stat st;
    return (stat(plugin_path, &st) == 0 && S_ISDIR(st.st_mode));
}

#else /* Non-macOS Stubs */

bool avx_dal_install_plugin(void) { return false; }
bool avx_dal_uninstall_plugin(void) { return false; }
bool avx_dal_is_installed(void) { return false; }

#endif
