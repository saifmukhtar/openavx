# Changelog

All notable changes to **OpenAVX** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-11-24

### Added
- **Tri-Platform Virtual Camera Suite**:
  - Linux: Native user-space PipeWire virtual camera sink (`src/avx_pipewire_sink.c`) and V4L2 loopback sink (`src/avx_v4l2_sink.c`).
  - Windows: DirectShow in-process COM virtual camera filter (`src/avx_dshow_filter.c`, `src/avx_dshow_sink.c`) exposing `IBaseFilter`, `IPin`, `IAMStreamConfig`, `IKsPropertySet`, and self-registration COM exports.
  - macOS: CoreMediaIO DAL hardware plug-in (`src/avx_dal_plugin.c`, `src/avx_dal_sink.c`) exposing `CMIOHardwarePlugInInterface`.
  - Cross-platform shared memory ring buffer IPC (`src/avx_virtcam_ipc.c`).
  - `openavx virtcam` CLI with `doctor`, `install`, and embedded HTTP/WebSocket web receiver (`--web 8080`) bridging smartphone cameras into desktop virtual cameras.
- **WAN Transport, RUDP & Pro-Grade Resilience**:
  - SMPTE ST 2022-1 2D XOR Forward Error Correction (FEC) engine for packet recovery over lossy links.
  - Reliable UDP (RUDP) with selective NACK retransmission, dynamic NACK pacing, live RTT telemetry, and BDP queue expansion.
  - Hybrid GCC/BBR adaptive bitrate (ABR) and dynamic FEC rate controller.
  - Centralized WAN discovery and UDP relay server with STUN reflection.
- **End-to-End Encryption (E2EE)**:
  - Native ChaCha20-Poly1305 / XChaCha20 stream encryption with Blake2b key derivation for zero-latency encrypted broadcast links.
- **Pro Audio Engine (Up to 32 Channels)**:
  - 16 and 32-channel pro-audio matrix transport across TCP, RUDP, and IMA ADPCM.
  - Audio channel matrix routing, broadcast Lt/Rt downmixing, and 5.1 surround upmixing.
- **Hardware Video Codecs & Simulcast**:
  - Hardware encoder backends and auto-detection: NVIDIA NVENC, Intel QSV, AMD AMF on Linux/Windows, and Apple VideoToolbox on macOS.
  - On-demand lazy hardware simulcast and dynamic in-flight codec re-negotiation.
  - Native 10-bit `P216` video decoder pipeline with automatic Annex-B / AVCC length-prefix conversion.
- **Live Broadcast Tally & Control**:
  - Tally Command & Control API with Server-Sent Events (SSE) and REST streaming (`openavx tally`).
  - Program (red), Preview (green), and Recording (amber) live tally states with smartphone browser border feedback.
- **Native RFC 2326 RTSP / RTP Subsystem**:
  - Clean-break native implementation of RFC 2326 RTSP 1.0 client/server and RFC 3550/6184/7798/9584 RTP packetization/demuxing (`src/avx_rtsp.c`, `include/avx_rtsp.h`).
- **Alpine Linux & musl Distribution Support**:
  - Full CI build & test container workflow (`alpine:3.20`) and release packaging job generating `openavx-${VERSION}-linux-x86_64-musl.tar.gz`.
  - Configured POSIX thread stack sizes in receiver workers to accommodate musl's default 128KB stack limit.
- **Packaging Standardization & CLI Symlinks**:
  - Standardized cross-platform artifact naming formula to `{project}-{version}-{os}-{arch}[-{variant}].{ext}` with canonical `x86_64` architecture tokens.
  - Added missing `openavx-virtcam` and `openavx-tally` convenience symlinks to CMake install and CPack packaging targets.
- **C99 Portability Compliance**:
  - Refactored 64-bit discrete audio channel mask (`OAVX_AUDIO_MASK_DISCRETE`) to a macro for ISO C99 `-Wpedantic` compliance.
- **Licensing, Attribution & Contributor Governance**:
  - Relicensed project under **GNU AGPLv3 with Section 7 Linking Exception** (`AGPL-3.0-or-later WITH Classpath-exception-2.0`).
  - Established Contributor License Agreement ([`CLA.md`](CLA.md)) with comprehensive multi-tier sublicensing and dual-licensing rights.
  - Integrated automated GitHub Actions CLA Assistant bot ([`.github/workflows/cla.yml`](.github/workflows/cla.yml)) with isolated `cla-signatures` branch storage.
  - Standardized SPDX and Copyright headers across 100% of native source, header, test, and fuzz files.
  - Formalized upstream attribution for FFmpeg SpeedHQ under `LGPL-2.1-or-later` in [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
  - Uniform `.clang-format` configuration for C99/C++11.
  - Expanded test suite to 54 CTest test targets with 100% pass rate.

### Changed
- Segregated third-party dependencies and platform fallbacks into dedicated `third-party/` directories (`third-party/monocypher/`, `third-party/pipewire/`, `third-party/proprietary/cmio/`).
- Refactored transmitter pipeline into modular companion files (`avx_send_aux.c`, `avx_send_profiles.c`, `avx_send_client.c`).
- Optimized SpeedHQ codec with parallel slice threading and reciprocal fixed-point quantization.
- Expanded WebSocket and HTTP ingress buffer to 16KB for high-framerate smartphone video streaming.

## [0.1.0] - 2026-11-11 (Initial Testing Preview)

> Note: Early `v0.1.0` testing release (not production-ready; API and wire behavior may evolve prior to `1.0.0`).

### Added
- Native C99 library (`libopenavx`) and header-only C++11 RAII wrapper ([`include/openavx.h`](include/openavx.h), [`include/openavx.hpp`](include/openavx.hpp)).
- Zero-dependency IPv4 mDNS/DNS-SD discovery, unicast registry fallback, and standalone TCP discovery server daemon.
- Low-latency TCP framing and fragmented UDP / RUDP selective-NACK transport.
- Standalone C99 SpeedHQ (`SHQ0`–`SHQ5`, `SHQ7`, `SHQ9`, `UYVA`) codecs plus optional runtime `dlopen()` H.264/HEVC/AV1 RTSP decode and Opus audio codec support.
- Runtime-dispatched x86_64 AVX2/FMA and ARM NEON color/alpha/DCT kernels with scalar C99 fallbacks, HDR `<oavx_color_info>` XML metadata, 10-bit `V210`/`P216` packing, and audio resampling/BS.775-3 downmixing.
- Clock-decoupled `avx_framesync`, crosspoint matrix router with tally feedback, HTTP/1.1 fMP4 web preview, and RTSP/RTP bridge.
- Public logging controls in `openavx.h` (`oavx_log_level_e`, `oavx_log_callback_t`, `oavx_set_log_level`, `oavx_get_log_level`, `oavx_set_log_callback`, `oavx_log_write`) backed by [`src/avx_log.c`](src/avx_log.c).
- Unified `openavx` CLI (`find`, `monitor`, `route`, `pattern`, `gateway`, `server`) with shared banners, `stdout`/`stderr` separation, POSIX TTY color gating, live status tickers, `-q`/`--quiet` support, compatibility symlinks, 43 CTest targets, and 11 libFuzzer harnesses.
- Multi-OS CI workflow (`ubuntu-22.04`, `macos-15`, `windows-2025`), multi-platform release automation, issue/PR templates, and governance docs (`SECURITY.md`, `MAINTAINERS.md`, `CODE_OF_CONDUCT.md`, `.github/CODEOWNERS`).
