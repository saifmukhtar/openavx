# OpenAVX

[![CI](https://github.com/saifmukhtar/openavx/actions/workflows/ci.yml/badge.svg)](https://github.com/saifmukhtar/openavx/actions/workflows/ci.yml)
[![Release](https://github.com/saifmukhtar/openavx/actions/workflows/release.yml/badge.svg)](https://github.com/saifmukhtar/openavx/actions/workflows/release.yml)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/License-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![Standard: C99 / C++11](https://img.shields.io/badge/Standard-C99%20%7C%20C%2B%2B11-informational.svg)](CONTRIBUTING.md)
[![Platforms: Linux | macOS | Windows](https://img.shields.io/badge/Platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](.github/workflows/ci.yml)
[![GitHub Stars](https://img.shields.io/github/stars/saifmukhtar/openavx?style=social)](https://github.com/saifmukhtar/openavx)

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

### Architecture & CLI Overview

```text
  Sender (pattern / SDK) ──▶ [ TCP or RUDP + 2D XOR FEC + XChaCha20-Poly1305 ] ──▶ Receiver
                                                                                     ├──▶ monitor (terminal)
                                                                                     ├──▶ virtcam (V4L2 / DirectShow / DAL)
                                                                                     ├──▶ gateway (HTTP fMP4 / RTSP 1.0)
                                                                                     └──▶ route / tally (matrix switcher)
```

| Subcommand | Purpose |
|---|---|
| `openavx pattern` | Generate SMPTE test patterns & audio tones as a live stream |
| `openavx monitor` | Preview any active stream live in the terminal |
| `openavx virtcam` | Bridge a stream (or smartphone camera via `--web`) to an OS virtual webcam |
| `openavx route` | Matrix crosspoint router between sources and outputs |
| `openavx tally` | Set or query PGM / PVW / Recording tally state |
| `openavx gateway` | Serve a stream over HTTP `fMP4` and RTSP 1.0 |
| `openavx find` | Discover active transmitters on the LAN via mDNS |
| `openavx server` | Run the TCP discovery directory server for cross-subnet routing |

### Minimal C99 Library Example

```c
#include "openavx.h"

int main(void) {
    oavx_context_t *ctx = oavx_context_create();
    oavx_stream_config_t cfg = {
        .name      = "MySource",
        .direction = OAVX_DIR_TRANSMIT,
        .transport = OAVX_TRANSPORT_TCP
    };
    oavx_stream_t *tx = oavx_stream_open(ctx, &cfg);

    /* Send frames with oavx_stream_send_video(tx, &frame); */

    oavx_stream_close(tx);
    oavx_context_destroy(ctx);
    return 0;
}
```

Compile and link against `libopenavx`:

```bash
cc -std=c99 app.c -I/usr/local/include -L/usr/local/lib -lopenavx -o app
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
* **Low Latency Transport**: Low-overhead TCP stream framing and UDP/RUDP with selective NACK retransmission (measured at ~3.29 ms median loopback transit latency; see [`BENCHMARK.md`](BENCHMARK.md)).
* **Automatic Failover & Auto-Revert**: Instant socket-disconnect failover, configurable stream-stall timeout detection, sender-advised failover metadata, and debounced auto-revert to prevent flapping.
* **Standalone SpeedHQ Codec**: Integrated intra-frame compression (`SHQ0` through `SHQ5`, `SHQ7`, `SHQ9`) with alpha channel support (measured up to 262 FPS software decode at 1080p on reference hardware).
* **Hardware Video Codecs & Simulcast**: H.264, HEVC, and AV1 hardware encode/decode via VideoToolbox (macOS), Media Foundation (Windows), V4L2 M2M (Linux), or optional `libavcodec`, with WAN simulcast support.
* **Hardware SIMD & Color Processing**: Runtime AVX2/FMA and ARM NEON acceleration for `UYVY`, `RGBA`, `BGRA`, and alpha blending, plus `NV12`, 16-bit `P216`, and 10-bit HDR metadata (`BT.709`, `BT.2020`, `HLG`, `PQ`).
* **Frame Synchronizer & Adaptive Rate Control**: Jitter-buffered A/V frame synchronization and congestion-aware bitrate/FEC rate control.
* **Packet Loss Protection (FEC)**: SMPTE ST 2022-1 2D XOR forward error correction (measured up to 8.43 GB/s parity throughput on reference hardware) to reconstruct lost packets without retransmission lag.
* **Native End-to-End Encryption**: Built-in XChaCha20-Poly1305 authenticated encryption (RFC 8439) with session salt derivation and zero external crypto libraries.
* **Broadcast Audio Engine**: Up to 32 audio channels, ITU-R BS.775 surround downmixing (7.1/5.1 to Stereo, 5.1 to Lt/Rt matrix), and Catmull-Rom/Blackman-Sinc sample rate conversion.
* **RTSP 1.0 & HTTP fMP4 Gateway**: Built-in RTSP 1.0 server, zero-plugin browser fragmented MP4 (`fMP4`) playback, and browser smartphone camera ingest.
* **Matrix Routing, Tally, PTZ & Recording**: Crosspoint source switching, PGM/PVW/Recording tally signaling, PTZ camera control, and local disk recording.
* **Virtual Webcam Drivers**: Direct integration with Linux V4L2/PipeWire, Windows DirectShow, and macOS CoreMediaIO DAL.
* **Automatic Discovery**: Local discovery via mDNS/DNS-SD (`224.0.0.251:5353`), plus a centralized TCP directory server for cross-subnet infrastructure.

---

## Installation

### Pre-built Packages & Binaries

Download pre-compiled binaries and native packages from [GitHub Releases](https://github.com/saifmukhtar/openavx/releases):

* **Debian / Ubuntu (`.deb`):**
  ```bash
  sudo dpkg -i openavx-<version>-linux-x86_64.deb
  ```
* **Fedora / RHEL / Rocky (`.rpm`):**
  ```bash
  sudo rpm -Uvh openavx-<version>-linux-x86_64.rpm
  ```
* **Standalone Tarball:**
  * Linux glibc: `openavx-<version>-linux-x86_64.tar.gz`
  * Alpine musl: `openavx-<version>-linux-x86_64-musl.tar.gz`
  * macOS Universal 2 (`arm64` + `x86_64`): `openavx-<version>-macos-universal.tar.gz`
  ```bash
  sudo tar -xzf openavx-<version>-<platform>.tar.gz --strip-components=1 -C /usr/local/
  ```
* **Windows (`.zip`):** Extract and add `bin/` to your system `PATH`.

### Build from Source

**Prerequisites:** C99 compiler (`gcc`, `clang`, or MSVC) and CMake `>= 3.16`.
*(Optional: install `libavcodec` / `ffmpeg-dev` if you want FFmpeg software codec fallback alongside native VideoToolbox, Media Foundation, and V4L2 M2M.)*

```bash
git clone https://github.com/saifmukhtar/openavx.git
cd openavx
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo ldconfig   # Linux: refresh shared library cache for /usr/local/lib
```

### Docker

Run the discovery directory server in an Alpine container:

```bash
docker run -d --name openavx-discovery --net=host ghcr.io/saifmukhtar/openavx:latest --port 7959
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
* [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) — Community standards and behavior expectations.
* [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) — Attribution and notices for vendored and dynamically loaded components.

---

## Contributing

Contributions, bug reports, and feature requests are welcome!

1. Read [`CONTRIBUTING.md`](CONTRIBUTING.md) for C99/C++11 style rules, build checks, and pull request guidelines.
2. Follow the [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).
3. Sign off on the Contributor License Agreement in [`CLA.md`](CLA.md) when submitting pull requests.

---

## License

OpenAVX is licensed under the **GNU Affero General Public License v3 with the Linking Exception** ([AGPL-3.0-or-later WITH Classpath-exception-2.0](LICENSE)).

* **Application Linking**: You may freely link `libopenavx` with independent applications, plugins (including OBS Studio), and commercial software without your own code becoming subject to the AGPL.
* **Core Engine**: Any modifications or improvements made directly to OpenAVX itself must remain open-source under the AGPLv3.
* **Third-Party Notices**: For third-party libraries and compatibility headers included or dynamically referenced by OpenAVX, see [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md).

---

## Star History

<a href="https://www.star-history.com/#saifmukhtar/openavx&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=saifmukhtar/openavx&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=saifmukhtar/openavx&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=saifmukhtar/openavx&type=Date" />
 </picture>
</a>
