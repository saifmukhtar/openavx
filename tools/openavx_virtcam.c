/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * tools/openavx_virtcam.c - Linux V4L2 Virtual Webcam Sink CLI
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

#include "avx_platform.h"
#include "openavx.h"
#include "openavx_cli.h"
#include "avx_v4l2_sink.h"
#include "avx_pipewire_sink.h"
#include "avx_virtcam_ipc.h"
#include "avx_dshow_sink.h"
#include "avx_dal_sink.h"
#include "avx_web.h"
#include "avx_video_codec.h"
#include "avx_internal.h"

#if defined(AVX_OS_LINUX)
    #include <linux/videodev2.h>
    #include <sys/utsname.h>
    #include <unistd.h>
#endif

static volatile bool g_running = true;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = false;
}

typedef struct {
    avx_v4l2_sink_t        *v4l2_sink;
    avx_pipewire_sink_t    *pw_sink;
    avx_virtcam_producer_t *ipc_sink;
    avx_video_decoder_t    *decoder;
    avx_video_decoder_t    *mjpeg_dec;
    oavx_stream_t          *tx_stream;
    avx_web_server_t       *web_srv;
    volatile uint64_t       frames_received;
    uint32_t                last_width;
    uint32_t                last_height;
    char                    last_codec[32];
    avx_mutex_t             lock;
} virtcam_web_ctx_t;

static void on_virtcam_web_frame_ingress(
    const uint8_t *frame_data,
    size_t frame_size,
    uint32_t fourcc,
    uint32_t width,
    uint32_t height,
    bool is_keyframe,
    void *user_ctx
) {
    (void)is_keyframe;
    virtcam_web_ctx_t *ctx = (virtcam_web_ctx_t*)user_ctx;
    if (!ctx || (!ctx->v4l2_sink && !ctx->pw_sink && !ctx->ipc_sink && !ctx->web_srv && !ctx->tx_stream) || !frame_data || frame_size == 0) return;

    avx_mutex_lock(&ctx->lock);
    ctx->last_width = width;
    ctx->last_height = height;
    ctx->frames_received++;

    avx_video_decoder_t *active_dec = NULL;
    if (fourcc == OAVX_FOURCC_VIDEO_TYPE_H264) {
        snprintf(ctx->last_codec, sizeof(ctx->last_codec), "H.264");
        if (ctx->web_srv) {
            avx_web_server_feed_frame(ctx->web_srv, frame_data, frame_size, 0, is_keyframe);
        }
        active_dec = ctx->decoder;
    } else if (fourcc == 0x47504A4D || fourcc == 0x4A50474D) { /* 'MJPG' / 'MPGJ' */
        snprintf(ctx->last_codec, sizeof(ctx->last_codec), "JPEG");
        if (!ctx->mjpeg_dec) {
            avx_video_dec_cfg_t mjpg_cfg = {
                .fourcc = 0x47504A4D,
                .preferred_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY
            };
            ctx->mjpeg_dec = avx_video_decoder_create(&mjpg_cfg);
        }
        active_dec = ctx->mjpeg_dec;
    }

    if (active_dec) {
        oavx_video_frame_t out_frame;
        memset(&out_frame, 0, sizeof(out_frame));
        if (avx_video_decoder_decode(active_dec, frame_data, frame_size, &out_frame) == 0) {
            if (ctx->pw_sink) {
                size_t sz = out_frame.strides[0] > 0 ? out_frame.strides[0] * out_frame.height : (size_t)out_frame.width * out_frame.height * 2;
                avx_pipewire_sink_push_frame(
                    ctx->pw_sink,
                    out_frame.planes[0],
                    sz,
                    out_frame.width,
                    out_frame.height,
                    out_frame.fourcc
                );
            }
            if (ctx->v4l2_sink) {
                avx_v4l2_sink_write_frame(ctx->v4l2_sink, &out_frame);
            }
            if (ctx->ipc_sink) {
                size_t sz = out_frame.strides[0] > 0 ? out_frame.strides[0] * out_frame.height : (size_t)out_frame.width * out_frame.height * 2;
                avx_virtcam_producer_write_frame(
                    ctx->ipc_sink,
                    out_frame.planes[0],
                    sz,
                    out_frame.width,
                    out_frame.height,
                    out_frame.strides[0],
                    out_frame.fourcc,
                    30,
                    1
                );
            }
            if (ctx->tx_stream) {
                oavx_stream_send_video(ctx->tx_stream, &out_frame);
            }
        }
    }
    avx_mutex_unlock(&ctx->lock);
}

#if defined(AVX_OS_LINUX)
static int virtcam_run_doctor(void) {
    printf("\n=======================================================\n");
    printf(" OpenAVX Virtual Camera Diagnostic (Linux System Doctor)\n");
    printf("=======================================================\n\n");

    /* 1. OS & Kernel */
    char os_name[128] = "Linux";
    FILE *f_os = fopen("/etc/os-release", "r");
    if (f_os) {
        char line[256];
        while (fgets(line, sizeof(line), f_os)) {
            if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                char *val = line + 12;
                if (*val == '"') val++;
                size_t l = strlen(val);
                if (l > 0 && val[l - 1] == '\n') val[--l] = '\0';
                if (l > 0 && val[l - 1] == '"') val[--l] = '\0';
                snprintf(os_name, sizeof(os_name), "%s", val);
                break;
            }
        }
        fclose(f_os);
    }
    printf("[*] Operating System : %s\n", os_name);

    char kernel_rel[256] = "Unknown";
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(kernel_rel, sizeof(kernel_rel), "%s (%s)", u.release, u.machine);
    }
    printf("[*] Kernel Release   : %s\n", kernel_rel);

    /* 2. Secure Boot Status */
    bool sb_enabled = false;
    FILE *f_sb = popen("mokutil --sb-state 2>/dev/null", "r");
    if (f_sb) {
        char sb_buf[128] = "";
        if (fgets(sb_buf, sizeof(sb_buf), f_sb)) {
            if (strcasestr(sb_buf, "enabled")) {
                sb_enabled = true;
            }
        }
        pclose(f_sb);
    }
    printf("[*] Secure Boot      : %s\n", sb_enabled ? "\033[33mENABLED (Requires MOK signing for out-of-tree modules)\033[0m" : "\033[32mDisabled / Inactive (Module loading unblocked)\033[0m");

    /* 3. PipeWire User-Space Video */
    bool pw_avail = avx_pipewire_is_available();
    printf("[*] PipeWire Daemon  : %s\n", pw_avail ? "\033[32mACTIVE (User-space virtual camera is supported)\033[0m" : "\033[33mUnavailable (libpipewire not loaded or daemon stopped)\033[0m");

    /* 4. V4L2 Loopback Module */
    bool v4l2_mod_loaded = (access("/sys/module/v4l2loopback", F_OK) == 0);
    printf("[*] V4L2 Loopback Mod: %s\n", v4l2_mod_loaded ? "\033[32mLOADED\033[0m" : "\033[31mNOT LOADED\033[0m");

    if (v4l2_mod_loaded) {
        char exc_caps[64] = "unknown";
        FILE *f_exc = fopen("/sys/module/v4l2loopback/parameters/exclusive_caps", "r");
        if (f_exc) {
            if (fgets(exc_caps, sizeof(exc_caps), f_exc)) {
                size_t l = strlen(exc_caps);
                if (l > 0 && exc_caps[l - 1] == '\n') exc_caps[l - 1] = '\0';
            }
            fclose(f_exc);
        }
        printf("    -> exclusive_caps: %s (%s)\n", exc_caps, (exc_caps[0] == 'Y' || exc_caps[0] == '1') ? "\033[32mOptimal for browsers\033[0m" : "\033[33mWarning: May trigger 'in-use' lockouts\033[0m");
    }

    /* 5. Virtual Camera Device Node */
    char dev_path[64] = "/dev/video10";
    bool dev_exists = (access(dev_path, F_OK) == 0);
    if (!dev_exists) {
        char probed[64] = "";
        if (avx_v4l2_sink_probe_loopback(probed, sizeof(probed))) {
            snprintf(dev_path, sizeof(dev_path), "%s", probed);
            dev_exists = true;
        }
    }

    if (dev_exists) {
        char card_name[128] = "Unknown";
        char sys_name_path[128];
        const char *base_dev = strrchr(dev_path, '/');
        if (base_dev) base_dev++; else base_dev = dev_path;
        snprintf(sys_name_path, sizeof(sys_name_path), "/sys/class/video4linux/%s/name", base_dev);
        FILE *f_card = fopen(sys_name_path, "r");
        if (f_card) {
            if (fgets(card_name, sizeof(card_name), f_card)) {
                size_t l = strlen(card_name);
                if (l > 0 && card_name[l - 1] == '\n') card_name[l - 1] = '\0';
            }
            fclose(f_card);
        }
        bool can_write = (access(dev_path, W_OK) == 0);
        printf("[*] Device Node      : \033[32m%s\033[0m (%s)\n", dev_path, card_name);
        printf("[*] Write Permission : %s\n", can_write ? "\033[32mYES (Ready for virtual camera streaming)\033[0m" : "\033[31mNO (Permission denied - add user to 'video' group)\033[0m");
    } else {
        printf("[*] Device Node      : \033[31mNONE FOUND (/dev/video10 does not exist)\033[0m\n");
    }

    /* 6. Persistence */
    bool perm_load = (access("/etc/modules-load.d/openavx-v4l2loopback.conf", F_OK) == 0 ||
                      access("/etc/modules-load.d/v4l2loopback.conf", F_OK) == 0);
    bool perm_opts = (access("/etc/modprobe.d/openavx-v4l2loopback.conf", F_OK) == 0 ||
                      access("/etc/modprobe.d/v4l2loopback.conf", F_OK) == 0);
    printf("[*] Boot Persistence : %s\n", (perm_load && perm_opts) ? "\033[32mCONFIGURED (Persists across reboots)\033[0m" : "\033[33mEphemeral only (Will vanish after reboot)\033[0m");

    printf("\n-------------------------------------------------------\n");
    if (dev_exists && v4l2_mod_loaded) {
        printf("\033[32mVerdict: Fully Ready for Chrome, Firefox, Meet, Zoom & Teams!\033[0m\n");
        printf("Launch feed: openavx cam --web 8080 -d %s\n", dev_path);
    } else {
        printf("\033[33mVerdict: V4L2 kernel device is not ready.\033[0m\n");
        printf("To install and configure permanently, run:\n");
        printf("  \033[1mopenavx cam install\033[0m\n");
    }
    printf("=======================================================\n\n");
    return 0;
}

static int virtcam_run_install(void) {
    printf("\n=======================================================\n");
    printf(" OpenAVX Virtual Camera Auto-Installer & Configurator\n");
    printf("=======================================================\n\n");

    /* 1. Detect Linux Distro */
    char distro_id[64] = "unknown";
    FILE *f_os = fopen("/etc/os-release", "r");
    if (f_os) {
        char line[256];
        while (fgets(line, sizeof(line), f_os)) {
            if (strncmp(line, "ID=", 3) == 0) {
                char *val = line + 3;
                if (*val == '"') val++;
                size_t l = strlen(val);
                if (l > 0 && val[l - 1] == '\n') val[--l] = '\0';
                if (l > 0 && val[l - 1] == '"') val[--l] = '\0';
                snprintf(distro_id, sizeof(distro_id), "%s", val);
                break;
            }
        }
        fclose(f_os);
    }
    printf("[*] Detected Linux Distribution: \033[1m%s\033[0m\n", distro_id);

    /* 2. Determine Package Manager & Commands */
    const char *check_pkg_cmd = NULL;
    const char *install_pkg_cmd = NULL;
    const char *build_mod_cmd = NULL;

    if (strcmp(distro_id, "fedora") == 0 || strcmp(distro_id, "ultramarine") == 0 ||
        strcmp(distro_id, "rhel") == 0 || strcmp(distro_id, "centos") == 0 ||
        strcmp(distro_id, "rocky") == 0 || strcmp(distro_id, "almalinux") == 0) {
        check_pkg_cmd = "rpm -q akmod-v4l2loopback >/dev/null 2>&1";
        install_pkg_cmd = "sudo dnf install -y akmod-v4l2loopback kernel-devel help2man";
        build_mod_cmd = "sudo akmods --akmod v4l2loopback --force";
    } else if (strcmp(distro_id, "ubuntu") == 0 || strcmp(distro_id, "debian") == 0 ||
               strcmp(distro_id, "linuxmint") == 0 || strcmp(distro_id, "pop") == 0) {
        check_pkg_cmd = "dpkg -s v4l2loopback-dkms >/dev/null 2>&1";
        install_pkg_cmd = "sudo apt-get update && sudo apt-get install -y v4l2loopback-dkms linux-headers-$(uname -r)";
    } else if (strcmp(distro_id, "arch") == 0 || strcmp(distro_id, "manjaro") == 0 ||
               strcmp(distro_id, "endeavouros") == 0) {
        check_pkg_cmd = "pacman -Q v4l2loopback-dkms >/dev/null 2>&1";
        install_pkg_cmd = "sudo pacman -S --noconfirm v4l2loopback-dkms linux-headers";
    } else {
        printf("\033[33m[!] Generic Linux distro detected. Assuming standard dkms or package manager.\033[0m\n");
    }

    /* 3. Check if module is already on disk */
    bool mod_installed = (system("modinfo v4l2loopback >/dev/null 2>&1") == 0);
    if (!mod_installed && check_pkg_cmd) {
        mod_installed = (system(check_pkg_cmd) == 0);
    }

    if (!mod_installed) {
        if (!install_pkg_cmd) {
            fprintf(stderr, "\033[31m[ERROR] Please install 'v4l2loopback' using your distribution's package manager.\033[0m\n");
            return 1;
        }
        printf("[*] Package 'v4l2loopback' not found on system.\n");
        printf("[*] Installing required kernel packages and build tools...\n");
        printf("    Executing: \033[36m%s\033[0m\n", install_pkg_cmd);
        int rc = system(install_pkg_cmd);
        if (rc != 0) {
            fprintf(stderr, "\033[31m[ERROR] Package installation failed with exit code %d.\033[0m\n", rc);
            if (strcmp(distro_id, "fedora") == 0 || strcmp(distro_id, "ultramarine") == 0) {
                fprintf(stderr, "Hint: On Fedora, ensure RPM Fusion is enabled: 'sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %%fedora).noarch.rpm'\n");
            }
            return 1;
        }

        if (build_mod_cmd) {
            printf("[*] Building kernel module for current kernel...\n");
            printf("    Executing: \033[36m%s\033[0m\n", build_mod_cmd);
            int brc = system(build_mod_cmd);
            if (brc != 0) {
                fprintf(stderr, "\033[31m[ERROR] Kernel module compilation failed. Check kernel headers.\033[0m\n");
                return 1;
            }
        }
    } else {
        printf("[*] Kernel module 'v4l2loopback' is present on disk.\n");
    }

    /* 4. Write Persistence Configuration */
    printf("[*] Configuring boot persistence...\n");
    int w1 = system("echo 'v4l2loopback' | sudo tee /etc/modules-load.d/openavx-v4l2loopback.conf >/dev/null");
    int w2 = system("echo 'options v4l2loopback devices=1 video_nr=10 card_label=\"OpenAVX Virtual Camera\" exclusive_caps=1' | sudo tee /etc/modprobe.d/openavx-v4l2loopback.conf >/dev/null");
    if (w1 != 0 || w2 != 0) {
        fprintf(stderr, "\033[33m[!] Warning: Could not write /etc/modules-load.d or /etc/modprobe.d (sudo failed).\033[0m\n");
    } else {
        printf("    -> Created \033[32m/etc/modules-load.d/openavx-v4l2loopback.conf\033[0m\n");
        printf("    -> Created \033[32m/etc/modprobe.d/openavx-v4l2loopback.conf\033[0m\n");
    }

    /* 5. Load Module */
    printf("[*] Loading v4l2loopback into Linux kernel...\n");
    system("sudo modprobe -r v4l2loopback 2>/dev/null || true");
    int lrc = system("sudo modprobe v4l2loopback devices=1 video_nr=10 card_label=\"OpenAVX Virtual Camera\" exclusive_caps=1");
    if (lrc != 0) {
        fprintf(stderr, "\033[31m[ERROR] Failed to load v4l2loopback module into kernel.\033[0m\n");
        return 1;
    }

    /* 6. Verify Device Node Exists */
    if (access("/dev/video10", F_OK) == 0) {
        printf("\n\033[32m=======================================================\n");
        printf(" [SUCCESS] OpenAVX Virtual Camera is installed and ready!\n");
        printf("=======================================================\033[0m\n");
        printf("  Device Node   : \033[1m/dev/video10\033[0m\n");
        printf("  Card Label    : \033[1mOpenAVX Virtual Camera\033[0m\n");
        printf("  Browser Ready : Chrome, Chromium, Firefox, Zoom, Teams, Meet\n");
        printf("  Persistence   : Module will load automatically on every system boot.\n\n");
        printf("To start streaming, run:\n");
        printf("  \033[36mopenavx cam --web\033[0m\n\n");
        return 0;
    } else {
        fprintf(stderr, "\033[31m[ERROR] Module loaded but /dev/video10 did not appear.\033[0m\n");
        return 1;
    }
}
#else
static int virtcam_run_doctor(void) {
    printf("Virtcam doctor is currently supported on Linux.\n");
    return 0;
}
static int virtcam_run_install(void) {
    printf("Virtcam automatic installation is currently configured for Linux.\n");
    return 0;
}
#endif

static void print_usage(const char* prog) {
    printf("OpenAVX Virtual Camera (Zero-Sudo PipeWire, DirectShow & DAL Webcam Sink)\n");
    printf("Feeds smartphone cameras or OpenAVX video streams into Zoom, Teams, Meet & Discord.\n\n");
    printf("Usage: %s [options|subcommand]\n\n", prog);
    printf("Subcommands:\n");
    printf("  doctor                      Diagnose virtual camera drivers, PipeWire & Secure Boot\n");
    printf("  install, setup              Install & configure persistent V4L2 virtual camera (/dev/video10)\n\n");
    printf("Options:\n");
    printf("  -W, --web [PORT]            Start Web Camera Ingress hub for phone streaming (default: 8080)\n");
    printf("  -s, --source <SOURCE>       Upstream source name or IP:port to ingest (default: auto-discover)\n");
    printf("  -P, --pipewire              Force PipeWire virtual camera sink (default on modern Linux)\n");
    printf("  -d, --device <DEVICE>       V4L2 loopback device path (e.g. /dev/video10)\n");
    printf("  -p, --passphrase <KEY>      Passphrase for native E2EE encrypted stream\n");
    printf("  -f, --format <FORMAT>       Output format: yuyv (default), uyvy, bgr24\n");
    printf("  -g, --groups <GROUPS>       Discovery / finder groups (default: public)\n");
    printf("      --doctor                Run virtual camera diagnostic\n");
    printf("      --install, --setup      Install & configure persistent Linux kernel loopback\n");
    printf("      --register              (Windows) Register DirectShow COM filter into HKCU (Zero Admin)\n");
    printf("      --unregister            (Windows) Unregister DirectShow COM filter from HKCU\n");
    printf("      --install-dal           (macOS) Install CoreMediaIO DAL virtual camera plugin (Zero Root)\n");
    printf("      --uninstall-dal         (macOS) Uninstall CoreMediaIO DAL virtual camera plugin\n");
    printf("  -q, --quiet                 Suppress info banners and live progress logs\n");
    printf("  -h, --help                  Display this help message and exit\n\n");
    printf("Driverless / Zero-Sudo Architecture:\n");
    printf("  - Linux:   Publishes user-space PipeWire video node (\"OpenAVX Virtual Camera\").\n");
    printf("  - Windows: Self-registers in HKCU DirectShow Video Input Category without admin rights.\n");
    printf("  - macOS:   Installs into ~/Library/CoreMediaIO/Plug-Ins/DAL/ without sudo or root.\n\n");
    printf("Examples:\n");
    printf("  %s doctor                                 # Diagnose webcam subsystems\n", prog);
    printf("  %s install                                # Install and configure persistent /dev/video10\n", prog);
    printf("  %s --web                                  # Host phone camera capture page at http://<ip>:8080/camera\n", prog);
    printf("  %s --web 8088 --pipewire                  # Explicitly run on PipeWire port 8088\n", prog);
    printf("  %s --source \"Phone_Camera\"               # Ingest LAN OpenAVX stream into Virtual Camera\n", prog);
}

int openavx_cmd_virtcam(int argc, char** argv) {
    if (argc >= 2) {
        if (strcmp(argv[1], "doctor") == 0 || strcmp(argv[1], "check") == 0 ||
            strcmp(argv[1], "--doctor") == 0) {
            return virtcam_run_doctor();
        }
        if (strcmp(argv[1], "install") == 0 || strcmp(argv[1], "setup") == 0 ||
            strcmp(argv[1], "--install") == 0 || strcmp(argv[1], "--setup") == 0) {
            return virtcam_run_install();
        }
    }

    optind = 1;
    g_running = true;

    char source_arg[256] = "";
    char device_arg[128] = "";
    char passphrase_arg[128] = "";
    char format_arg[32] = "yuyv";
    char groups[256] = "public";
    bool quiet = false;
    bool enable_web = false;
    bool force_pipewire = false;
    uint16_t web_port = 8080;

    static struct option long_opts[] = {
        { "web",           optional_argument, 0, 'W' },
        { "source",        required_argument, 0, 's' },
        { "pipewire",      no_argument,       0, 'P' },
        { "device",        required_argument, 0, 'd' },
        { "passphrase",    required_argument, 0, 'p' },
        { "format",        required_argument, 0, 'f' },
        { "groups",        required_argument, 0, 'g' },
        { "doctor",        no_argument,       0, 1005 },
        { "install",       no_argument,       0, 1006 },
        { "setup",         no_argument,       0, 1006 },
        { "register",      no_argument,       0, 1001 },
        { "unregister",    no_argument,       0, 1002 },
        { "install-dal",   no_argument,       0, 1003 },
        { "uninstall-dal", no_argument,       0, 1004 },
        { "quiet",         no_argument,       0, 'q' },
        { "help",          no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "W::s:Pd:p:f:g:qh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'W':
            enable_web = true;
            if (optarg && optarg[0] != '\0') {
                web_port = (uint16_t)atoi(optarg);
            } else if (optind < argc && argv[optind] && argv[optind][0] != '-') {
                web_port = (uint16_t)atoi(argv[optind++]);
            }
            if (web_port == 0) web_port = 8080;
            break;
        case 's':
            snprintf(source_arg, sizeof(source_arg), "%s", optarg);
            break;
        case 'P':
            force_pipewire = true;
            break;
        case 'd':
            snprintf(device_arg, sizeof(device_arg), "%s", optarg);
            break;
        case 'p':
            snprintf(passphrase_arg, sizeof(passphrase_arg), "%s", optarg);
            break;
        case 'f':
            snprintf(format_arg, sizeof(format_arg), "%s", optarg);
            break;
        case 'g':
            snprintf(groups, sizeof(groups), "%s", optarg);
            break;
        case 1001:
            if (avx_dshow_register_filter(NULL)) {
                printf("Successfully registered OpenAVX Virtual Camera (DirectShow COM) in HKCU (Zero Admin).\n");
                return 0;
            } else {
                fprintf(stderr, "Failed to register OpenAVX Virtual Camera DirectShow filter.\n");
                return 1;
            }
        case 1002:
            if (avx_dshow_unregister_filter()) {
                printf("Successfully unregistered OpenAVX Virtual Camera from HKCU.\n");
                return 0;
            } else {
                fprintf(stderr, "Failed to unregister OpenAVX Virtual Camera DirectShow filter.\n");
                return 1;
            }
        case 1003:
            if (avx_dal_install_plugin()) {
                printf("Successfully installed OpenAVX CoreMediaIO DAL virtual camera plugin (Zero Root).\n");
                return 0;
            } else {
                fprintf(stderr, "Failed to install OpenAVX DAL virtual camera plugin.\n");
                return 1;
            }
        case 1004:
            if (avx_dal_uninstall_plugin()) {
                printf("Successfully uninstalled OpenAVX CoreMediaIO DAL virtual camera plugin.\n");
                return 0;
            } else {
                fprintf(stderr, "Failed to uninstall OpenAVX DAL virtual camera plugin.\n");
                return 1;
            }
        case 1005:
            return virtcam_run_doctor();
        case 1006:
            return virtcam_run_install();
        case 'q':
            quiet = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    openavx_cli_set_quiet(quiet);

    /* Setup signal handling */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* 1. Resolve Sink Backend: PipeWire (Linux), DirectShow (Windows), DAL (macOS), or V4L2 */
    avx_pipewire_sink_t    *pw_sink = NULL;
    avx_v4l2_sink_t        *v4l2_sink = NULL;
    avx_virtcam_producer_t *ipc_sink = avx_virtcam_producer_create();
    char resolved_sink_desc[128] = "";
    char resolved_dev[128] = "";

#if defined(AVX_OS_WINDOWS)
    if (!avx_dshow_is_registered()) {
        avx_dshow_register_filter(NULL);
    }
    snprintf(resolved_sink_desc, sizeof(resolved_sink_desc), "DirectShow (HKCU Zero-Admin)");
#elif defined(AVX_OS_MACOS)
    if (!avx_dal_is_installed()) {
        avx_dal_install_plugin();
    }
    snprintf(resolved_sink_desc, sizeof(resolved_sink_desc), "CoreMediaIO DAL (User-Space)");
#elif defined(AVX_OS_LINUX)
    if (device_arg[0] != '\0' && !force_pipewire) {
        /* User explicitly passed --device /dev/videoX */
        snprintf(resolved_dev, sizeof(resolved_dev), "%s", device_arg);
        uint32_t sink_pixfmt = V4L2_PIX_FMT_YUYV;
        if (strcasecmp(format_arg, "uyvy") == 0) sink_pixfmt = V4L2_PIX_FMT_UYVY;
        else if (strcasecmp(format_arg, "bgr24") == 0) sink_pixfmt = V4L2_PIX_FMT_BGR24;

        avx_v4l2_sink_config_t sink_cfg = {
            .device_path = resolved_dev,
            .width = 1920,
            .height = 1080,
            .pixelformat = sink_pixfmt,
            .fps_num = 30,
            .fps_den = 1
        };
        v4l2_sink = avx_v4l2_sink_create(&sink_cfg);
        if (!v4l2_sink) {
            oavx_log_write(OAVX_LOG_ERROR, "virtcam", "Failed to open V4L2 loopback device '%s' (check permissions or sudo usermod -aG video $USER)", resolved_dev);
            if (ipc_sink) avx_virtcam_producer_destroy(ipc_sink);
            return 1;
        }
        snprintf(resolved_sink_desc, sizeof(resolved_sink_desc), "V4L2 (%s)", resolved_dev);
    } else {
        /* Default: Try modern user-space PipeWire virtual camera (zero sudo, driverless) */
        if (avx_pipewire_is_available()) {
            avx_pipewire_sink_config_t pw_cfg = {
                .node_name = "OpenAVX Virtual Camera",
                .node_description = "OpenAVX Virtual Camera Feed",
                .width = 1920,
                .height = 1080,
                .fps_num = 30,
                .fps_den = 1,
                .preferred_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY
            };
            pw_sink = avx_pipewire_sink_create(&pw_cfg);
            if (pw_sink) {
                snprintf(resolved_sink_desc, sizeof(resolved_sink_desc), "PipeWire (User-Space, Driverless)");
            }
        }

        /* If PipeWire not available and user didn't force it, try V4L2 loopback probe fallback */
        if (!pw_sink && !force_pipewire) {
            if (avx_v4l2_sink_probe_loopback(resolved_dev, sizeof(resolved_dev))) {
                avx_v4l2_sink_config_t sink_cfg = {
                    .device_path = resolved_dev,
                    .width = 1920,
                    .height = 1080,
                    .pixelformat = V4L2_PIX_FMT_YUYV,
                    .fps_num = 30,
                    .fps_den = 1
                };
                v4l2_sink = avx_v4l2_sink_create(&sink_cfg);
                if (v4l2_sink) {
                    snprintf(resolved_sink_desc, sizeof(resolved_sink_desc), "V4L2 (%s)", resolved_dev);
                }
            }
        }

        if (!pw_sink && !v4l2_sink && ipc_sink) {
            snprintf(resolved_sink_desc, sizeof(resolved_sink_desc), "IPC Shared Memory (/openavx_virtcam_shm)");
        }
    }
#else
    if (ipc_sink) {
        snprintf(resolved_sink_desc, sizeof(resolved_sink_desc), "IPC Shared Memory");
    }
#endif

    if (!pw_sink && !v4l2_sink && !ipc_sink) {
        openavx_cli_header("virtcam");
        oavx_log_write(OAVX_LOG_ERROR, "virtcam",
                       "No virtual camera sink available:\n"
                       "  - PipeWire: daemon not responding or library missing.\n"
                       "  - V4L2 Loopback: no device found in /dev/video*.\n");
        return 1;
    }

    /* If neither source nor web specified, default to web camera hub */
    if (source_arg[0] == '\0') {
        enable_web = true;
    }

    /* ===================================================================== */
    /* MODE A: SMARTPHONE WEB CAMERA INGRESS HUB                             */
    /* ===================================================================== */
    if (enable_web) {
        char local_ip[64] = "127.0.0.1";
        (void)avx_net_get_default_ip(local_ip, sizeof(local_ip));

        avx_video_codec_init();
        avx_video_dec_cfg_t dec_cfg = {
            .fourcc = OAVX_FOURCC_VIDEO_TYPE_H264,
            .preferred_fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY
        };
        avx_video_decoder_t *dec = avx_video_decoder_create(&dec_cfg);

        virtcam_web_ctx_t web_ctx;
        memset(&web_ctx, 0, sizeof(web_ctx));
        web_ctx.v4l2_sink = v4l2_sink;
        web_ctx.pw_sink = pw_sink;
        web_ctx.ipc_sink = ipc_sink;
        web_ctx.decoder = dec;
        avx_mutex_init(&web_ctx.lock);

        oavx_context_t *ctx = oavx_context_create();
        if (ctx) {
            oavx_stream_config_t tx_cfg = {
                .name = "Phone Camera",
                .direction = OAVX_DIR_TRANSMIT,
                .color_format = OAVX_COLOR_FORMAT_FASTEST,
                .groups = groups,
                .passphrase = passphrase_arg[0] ? passphrase_arg : NULL
            };
            web_ctx.tx_stream = oavx_stream_open(ctx, &tx_cfg);
        }

        avx_web_server_config_t web_cfg = {
            .port = web_port,
            .stream_name = "OpenAVX Phone Camera",
            .codec = AVX_RTSP_CODEC_H264,
            .on_ingress_frame = on_virtcam_web_frame_ingress,
            .user_ctx = &web_ctx
        };
        avx_web_server_t *web_srv = avx_web_server_create(&web_cfg);
        if (!web_srv) {
            oavx_log_write(OAVX_LOG_ERROR, "virtcam", "Failed to start web server on port %u", web_port);
            if (dec) avx_video_decoder_destroy(dec);
        if (web_ctx.mjpeg_dec) avx_video_decoder_destroy(web_ctx.mjpeg_dec);
            if (v4l2_sink) avx_v4l2_sink_destroy(v4l2_sink);
            if (pw_sink) avx_pipewire_sink_destroy(pw_sink);
            if (ipc_sink) avx_virtcam_producer_destroy(ipc_sink);
            if (ctx) oavx_context_destroy(ctx);
            return 1;
        }
        web_port = avx_web_server_get_port(web_srv);
        avx_mutex_lock(&web_ctx.lock);
        web_ctx.web_srv = web_srv;
        avx_mutex_unlock(&web_ctx.lock);

        openavx_cli_header("virtcam");
        openavx_cli_kv("sink backend", "%s", resolved_sink_desc);
        openavx_cli_kv("format", "%s", format_arg);
        openavx_cli_kv("phone camera url", "http://%s:%u/camera", local_ip, web_port);
        openavx_cli_kv("pc preview url", "http://localhost:%u/", web_port);
        openavx_cli_kv("usb cable (adb)", "adb reverse tcp:%u tcp:%u -> http://localhost:%u/camera", web_port, web_port, web_port);
        if (passphrase_arg[0]) {
            openavx_cli_kv("e2ee", "ChaCha20-Poly1305 Active (LAN restream)");
        }
        oavx_log_write(OAVX_LOG_INFO, "virtcam", "Virtual Camera Hub ready! Open http://%s:%u/camera on your phone (Ctrl+C to stop)", local_ip, web_port);

        int64_t last_stat_time = avx_time_ms();
        while (g_running) {
            avx_sleep_ms(100);
            if (oavx_get_log_level() >= OAVX_LOG_INFO) {
                int64_t now = avx_time_ms();
                if (now - last_stat_time >= 1000) {
                    avx_mutex_lock(&web_ctx.lock);
                    uint64_t fc = web_ctx.frames_received;
                    uint32_t w = web_ctx.last_width;
                    uint32_t h = web_ctx.last_height;
                    const char *c = web_ctx.last_codec[0] ? web_ctx.last_codec : "waiting";
                    avx_mutex_unlock(&web_ctx.lock);

                    openavx_cli_status("virtcam", "sink=%s web=%u frames=%llu %ux%u codec=%s",
                                       resolved_sink_desc, web_port, (unsigned long long)fc, w, h, c);
                    last_stat_time = now;
                }
            }
        }

        openavx_cli_status_end();
        oavx_log_write(OAVX_LOG_INFO, "virtcam", "Releasing Virtual Camera Hub...");
        avx_mutex_lock(&web_ctx.lock);
        web_ctx.web_srv = NULL;
        avx_mutex_unlock(&web_ctx.lock);
        avx_web_server_destroy(web_srv);
        if (web_ctx.tx_stream) oavx_stream_close(web_ctx.tx_stream);
        if (dec) avx_video_decoder_destroy(dec);
        if (web_ctx.mjpeg_dec) avx_video_decoder_destroy(web_ctx.mjpeg_dec);
        if (v4l2_sink) avx_v4l2_sink_destroy(v4l2_sink);
        if (pw_sink) avx_pipewire_sink_destroy(pw_sink);
        if (ipc_sink) avx_virtcam_producer_destroy(ipc_sink);
        if (ctx) oavx_context_destroy(ctx);
        avx_mutex_destroy(&web_ctx.lock);
        oavx_log_write(OAVX_LOG_INFO, "virtcam", "Virtual camera hub stopped cleanly");
        return 0;
    }

    /* ===================================================================== */
    /* MODE B: UPSTREAM OPENAVX STREAM SINK                                  */
    /* ===================================================================== */
    oavx_context_t *ctx = oavx_context_create();
    if (!ctx) {
        oavx_log_write(OAVX_LOG_ERROR, "virtcam", "Failed to initialize OpenAVX context");
        if (v4l2_sink) avx_v4l2_sink_destroy(v4l2_sink);
        if (pw_sink) avx_pipewire_sink_destroy(pw_sink);
        if (ipc_sink) avx_virtcam_producer_destroy(ipc_sink);
        return 1;
    }

    char target_name[256] = "";
    char target_url[256] = "";

    if (source_arg[0] != '\0') {
        if (strchr(source_arg, '.') || strchr(source_arg, ':')) {
            snprintf(target_url, sizeof(target_url), "%s", source_arg);
        } else {
            snprintf(target_name, sizeof(target_name), "%s", source_arg);
        }
    } else {
        /* Auto-discover first available OpenAVX stream */
        openavx_cli_header("virtcam");
        oavx_log_write(OAVX_LOG_INFO, "virtcam", "Scanning network for active OpenAVX streams...");

        oavx_discovery_t *p_find = oavx_discovery_create(ctx, groups);
        if (!p_find) {
            oavx_log_write(OAVX_LOG_ERROR, "virtcam", "Failed to start stream discovery");
            if (v4l2_sink) avx_v4l2_sink_destroy(v4l2_sink);
            if (pw_sink) avx_pipewire_sink_destroy(pw_sink);
            if (ipc_sink) avx_virtcam_producer_destroy(ipc_sink);
            oavx_context_destroy(ctx);
            return 1;
        }

        oavx_endpoint_t sources[32];
        size_t count = 0;
        int retries = 30; /* Wait up to 3 seconds */
        while (retries-- > 0 && count == 0 && g_running) {
            oavx_discovery_wait(p_find, 100);
            count = oavx_discovery_get_sources(p_find, sources, 32);
        }

        if (count == 0) {
            oavx_log_write(OAVX_LOG_ERROR, "virtcam", "No active OpenAVX streams found on local network or groups '%s'", groups);
            oavx_discovery_destroy(p_find);
            if (v4l2_sink) avx_v4l2_sink_destroy(v4l2_sink);
            if (pw_sink) avx_pipewire_sink_destroy(pw_sink);
            if (ipc_sink) avx_virtcam_producer_destroy(ipc_sink);
            oavx_context_destroy(ctx);
            return 1;
        }

        if (sources[0].name) snprintf(target_name, sizeof(target_name), "%s", sources[0].name);
        if (sources[0].address) snprintf(target_url, sizeof(target_url), "%s", sources[0].address);
        oavx_log_write(OAVX_LOG_INFO, "virtcam", "Auto-selected upstream stream: %s (%s)",
                       target_name, target_url[0] ? target_url : "mDNS");
        oavx_discovery_destroy(p_find);
    }

    oavx_stream_config_t rx_cfg = {
        .name = target_name[0] ? target_name : "OpenAVX Virtual Camera Ingress",
        .endpoint = target_url[0] ? target_url : NULL,
        .direction = OAVX_DIR_RECEIVE,
        .color_format = OAVX_COLOR_FORMAT_FASTEST,
        .groups = groups,
        .passphrase = passphrase_arg[0] ? passphrase_arg : NULL
    };

    oavx_stream_t *rx = oavx_stream_open(ctx, &rx_cfg);
    if (!rx) {
        oavx_log_write(OAVX_LOG_ERROR, "virtcam", "Failed to connect to stream '%s'",
                       target_name[0] ? target_name : target_url);
        if (v4l2_sink) avx_v4l2_sink_destroy(v4l2_sink);
        if (pw_sink) avx_pipewire_sink_destroy(pw_sink);
        if (ipc_sink) avx_virtcam_producer_destroy(ipc_sink);
        oavx_context_destroy(ctx);
        return 1;
    }

    openavx_cli_header("virtcam");
    openavx_cli_kv("sink backend", "%s", resolved_sink_desc);
    openavx_cli_kv("format", "%s", format_arg);
    openavx_cli_kv("source", "%s%s%s",
                   target_name[0] ? target_name : target_url,
                   (target_name[0] && target_url[0]) ? " @ " : "",
                   (target_name[0] && target_url[0]) ? target_url : "");
    if (passphrase_arg[0]) {
        openavx_cli_kv("e2ee", "ChaCha20-Poly1305 Active");
    }
    oavx_log_write(OAVX_LOG_INFO, "virtcam", "Virtual camera active: %s is feeding video to Zoom/Teams/Meet (Ctrl+C to stop)", resolved_sink_desc);

    uint64_t frame_count = 0;
    int64_t last_stat_time = avx_time_ms();
    uint32_t cur_w = 0, cur_h = 0;

    while (g_running) {
        oavx_video_frame_t vf;
        oavx_audio_frame_t af;
        oavx_metadata_frame_t mf;

        oavx_frame_type_t ft = oavx_stream_receive(rx, &vf, &af, &mf, 100);
        if (ft == OAVX_FRAME_TYPE_VIDEO) {
            if (pw_sink) {
                size_t sz = vf.strides[0] > 0 ? vf.strides[0] * vf.height : (size_t)vf.width * vf.height * 2;
                avx_pipewire_sink_push_frame(
                    pw_sink,
                    vf.planes[0],
                    sz,
                    vf.width,
                    vf.height,
                    vf.fourcc
                );
            }
            if (v4l2_sink) {
                avx_v4l2_sink_write_frame(v4l2_sink, &vf);
            }
            if (ipc_sink) {
                size_t sz = vf.strides[0] > 0 ? vf.strides[0] * vf.height : (size_t)vf.width * vf.height * 2;
                avx_virtcam_producer_write_frame(
                    ipc_sink,
                    vf.planes[0],
                    sz,
                    vf.width,
                    vf.height,
                    vf.strides[0],
                    vf.fourcc,
                    30,
                    1
                );
            }
            cur_w = vf.width;
            cur_h = vf.height;
            frame_count++;
            oavx_stream_release_video(rx, &vf);
        } else if (ft == OAVX_FRAME_TYPE_AUDIO) {
            oavx_stream_release_audio(rx, &af);
        } else if (ft == OAVX_FRAME_TYPE_METADATA) {
            oavx_stream_release_metadata(rx, &mf);
        }

        if (oavx_get_log_level() >= OAVX_LOG_INFO) {
            int64_t now = avx_time_ms();
            if (now - last_stat_time >= 1000) {
                openavx_cli_status("virtcam", "sink=%s frames=%llu %ux%u format=%s",
                                   resolved_sink_desc, (unsigned long long)frame_count, cur_w, cur_h, format_arg);
                last_stat_time = now;
            }
        }
    }

    openavx_cli_status_end();
    oavx_log_write(OAVX_LOG_INFO, "virtcam", "Stopping virtual camera...");
    oavx_stream_close(rx);
    if (v4l2_sink) avx_v4l2_sink_destroy(v4l2_sink);
    if (pw_sink) avx_pipewire_sink_destroy(pw_sink);
    if (ipc_sink) avx_virtcam_producer_destroy(ipc_sink);
    oavx_context_destroy(ctx);
    oavx_log_write(OAVX_LOG_INFO, "virtcam", "Virtual camera released cleanly");

    return 0;
}
