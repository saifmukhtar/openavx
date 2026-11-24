# OpenAVX

[![CI](https://github.com/saifmukhtar/openavx/actions/workflows/ci.yml/badge.svg)](https://github.com/saifmukhtar/openavx/actions/workflows/ci.yml)
[![Release](https://github.com/saifmukhtar/openavx/actions/workflows/release.yml/badge.svg)](https://github.com/saifmukhtar/openavx/actions/workflows/release.yml)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/License-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Standard: C99 / C++11](https://img.shields.io/badge/Standard-C99%20%7C%20C%2B%2B11-informational.svg)](CONTRIBUTING.md)
[![Platforms: Linux | macOS | Windows | BSD](https://img.shields.io/badge/Platforms-Linux%20%7C%20macOS%20%7C%20Windows%20%7C%20BSD-lightgrey.svg)](.github/workflows/ci.yml)

Open Audio-Video eXchange (`openavx`) is an open-standard IP audio and video transport library and command-line tool suite written in portable C99.

It is designed for live broadcast production, studio routing, and software video pipelines requiring sub-frame latency. OpenAVX provides standalone intra-frame compression (SpeedHQ), zero external build dependencies, and native virtual webcam drivers to feed live feeds directly into OBS Studio, video switchers, or web browsers.

---

## Quick Start

OpenAVX requires no external runtime services. You can start streaming and monitoring video immediately using the CLI tools:

```bash
# 1. Generate a broadcast test pattern (1080p60 SMPTE color bars with audio tone)
openavx pattern -n "Studio CAM" -p bars100 --res 1080p --fps 60

# 2. In another terminal, view the active stream live (Unicode Braille or TrueColor RGB)
openavx monitor -s "Studio CAM"

# 3. Output the stream as an OS virtual webcam for OBS Studio, Zoom, or Teams
openavx virtcam -s "Studio CAM" -P
```

---

## Common Use Cases

### Feed Video into OBS Studio or Video Conferencing
Send any network stream to an OS virtual webcam device:
* **Linux:** Uses PipeWire or V4L2 loopback (`/dev/videoX`).
* **Windows:** Uses DirectShow filter (`OpenAVX Virtual Camera`).
* **macOS:** Uses CoreMediaIO DAL plugin.

```bash
# Ingest an OpenAVX LAN stream into your virtual webcam
openavx virtcam -s "CAM1" -P

# Or turn any smartphone into a wireless studio webcam (zero phone apps needed)
openavx virtcam --web 8080
# Open http://<lan-ip>:8080/camera on your phone to feed directly into Zoom, Meet, or OBS
```

### Route Feeds with Live Tally
Route video feeds between sources and destinations with Program (PGM), Preview (PVW), and Recording tally signaling:

```bash
# Start an interactive matrix router
openavx route -n "Program Out" -s "CAM1"

# Update tally state from a switcher or script
openavx tally "CAM1" --set pgm
```

### Bridge to Web Browsers and Media Players
Serve live streams over HTTP fragmented MP4 (fMP4) for zero-plugin playback in modern web browsers, or stream via RTSP 1.0:

```bash
openavx gateway -s "CAM1" --web 8080 --rtsp 8554
```

* **Browser playback:** `http://localhost:8080/stream.mp4`
* **Media players (VLC, FFmpeg):** `rtsp://localhost:8554/live/CAM1`

### Discover Active Streams
Find all active transmitters on the local subnet via mDNS:

```bash
openavx find
```

For networks spanning multiple subnets or VLANs where multicast is blocked, run the lightweight TCP directory daemon:

```bash
openavx server --port 7959
```

---

## Features

* **Zero External Build Dependencies**: Written in strict C99 and C++11 using standard system libraries (`libc`, `pthreads`, `libm`, `libdl` / `ws2_32`). Builds with GCC, Clang, or MSVC in seconds.
* **Low Latency Transport**: Low-overhead TCP stream framing and UDP/RUDP with selective NACK retransmission. Achieves 3.29 ms median transit latency over local networks.
* **Standalone SpeedHQ Codec**: Integrated intra-frame compression (`SHQ0` through `SHQ5`, `SHQ7`, `SHQ9`) with alpha channel support. Decodes 1080p at 262 FPS in software, reducing 2 Gbps raw video to 50 Mbps.
* **Hardware SIMD Acceleration**: Runtime AVX2/FMA and ARM NEON vector routines for color format conversions (`UYVY`, `RGBA`, `BGRA`, `NV12`), alpha blending, and 10-bit HDR metadata (`BT.709`, `BT.2020`, `HLG`, `PQ`).
* **Packet Loss Protection (FEC)**: SMPTE ST 2022-1 2D XOR forward error correction processes parity at 8.43 GB/s, reconstructing lost packets without retransmission lag.
* **Native End-to-End Encryption**: Built-in XChaCha20-Poly1305 authenticated encryption (RFC 8439) with session salt derivation and zero external crypto libraries.
* **Broadcast Audio Engine**: Up to 32 audio channels, ITU-R BS.775 surround downmixing (7.1/5.1 to Stereo, 5.1 to Lt/Rt matrix), and Catmull-Rom/Blackman-Sinc sample rate conversion.
* **Virtual Webcam Drivers**: Direct integration with Linux V4L2/PipeWire, Windows DirectShow, and macOS CoreMediaIO DAL.
* **Automatic Discovery**: Local discovery via mDNS/DNS-SD (`224.0.0.251:5353`), plus a centralized TCP directory server for cross-subnet infrastructure.

---

## Protocol Comparison

| Property | OpenAVX | WebRTC | SRT | RTSP |
|---|---|---|---|---|
| **Median Latency** | **3.29 ms** (Sub-frame) | 100–300 ms | 120–500 ms | 500–2000 ms |
| **License** | Open Source (AGPLv3 + Exception) | Open specification | Open Source (MPL-2.0) | Open (IETF) |
| **Build Dependencies** | **Zero** (libc + pthreads) | Heavy C++ dependencies | OpenSSL / Libs | FFmpeg / Live555 |
| **Compression** | SpeedHQ Intra-frame (or Raw) | Inter-frame (H.264/VP9) | Passthrough | Passthrough |
| **Virtual Webcam** | Built-in (V4L2, DirectShow, DAL) | Browser-only | None | None |
| **Discovery** | mDNS + TCP Directory | STUN / TURN / ICE | Manual configuration | Manual configuration |

---

## Installation

### Pre-built Packages & Binaries

Download pre-compiled binaries and native packages from [GitHub Releases](https://github.com/saifmukhtar/openavx/releases):

* **Debian / Ubuntu (`.deb`):**
  ```bash
  sudo dpkg -i openavx-0.2.1-linux-x86_64.deb
  ```
* **Fedora / RHEL / Rocky (`.rpm`):**
  ```bash
  sudo rpm -Uvh openavx-0.2.1-linux-x86_64.rpm
  ```
* **Standalone Tarball (Linux glibc / Alpine musl / macOS):**
  ```bash
  tar -xzf openavx-0.2.1-linux-x86_64.tar.gz
  sudo cp -r openavx-0.2.1-linux-x86_64/* /usr/local/
  ```
* **Windows (`.zip`):** Extract and add `bin/` to your system `PATH`.

### Homebrew (macOS & Linux)

```bash
brew install saifmukhtar/openavx/openavx
```

### Build from Source

**Prerequisites:** C99 compiler (`gcc`, `clang`, or MSVC) and CMake `>= 3.16`.

```bash
git clone https://github.com/saifmukhtar/openavx.git
cd openavx
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

### Docker

Run the discovery directory server in an Alpine container:

```bash
docker run -d --name openavx-discovery --net=host openavx:latest --port 7959
```

---

## Benchmarks

See [`BENCHMARK.md`](BENCHMARK.md) for full empirical benchmarks, glass-to-glass latency distributions (P50/P95/P99), SpeedHQ codec throughput, and reproduction commands.

---

## Documentation

* [`BENCHMARK.md`](BENCHMARK.md) — Comprehensive performance benchmarks, latency distributions, and reproduction guide.
* [`DERIVATION.md`](DERIVATION.md) — Technical specifications, packet framing, and standards reference.
* [`CONTRIBUTING.md`](CONTRIBUTING.md) — Coding conventions, formatting rules, and pull request steps.
* [`CLA.md`](CLA.md) — Contributor License Agreement terms and automated sign-off instructions.
* [`CHANGELOG.md`](CHANGELOG.md) — Release notes and unreleased development updates.
* [`SECURITY.md`](SECURITY.md) — Vulnerability disclosure policy.
* [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) — Attribution and notices for vendored and dynamically loaded components.

---

## License

OpenAVX is licensed under the **GNU Affero General Public License v3 with the Linking Exception** ([AGPL-3.0-or-later WITH Classpath-exception-2.0](LICENSE)).

* **Application Linking**: You may freely link `libopenavx` with independent applications, plugins (including OBS Studio), and commercial software without your own code becoming subject to the AGPL.
* **Core Engine**: Any modifications or improvements made directly to OpenAVX itself must remain open-source under the AGPLv3.
* **Third-Party Notices**: For third-party libraries and compatibility headers included or dynamically referenced by OpenAVX, see [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).
