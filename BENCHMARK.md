<!--
SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
Copyright (c) 2026 Saif Mukhtar
-->

# OpenAVX Performance & Benchmark Specification

This document details empirical benchmarks and performance characteristics for **OpenAVX (Open Audio-Video eXchange)**. All data presented in this document was directly measured on physical hardware running native binaries; no numbers are estimated or synthetic.

---

## 1. Test Rig & Methodology

* **Processor**: `11th Gen Intel(R) Core(TM) i5-11400H @ 2.70GHz` (6 physical cores, 12 logical threads), `x86_64`
* **Operating System**: `Ultramarine Linux 44 (Plasma Edition)` (Linux Kernel `7.3.3-200.fc44.x86_64`)
* **Active SIMD Target**: Hand-vectorized `AVX2 + FMA` vector kernels (ARM64 targets compile equivalent `<arm_neon.h>` vector kernels automatically)
* **Compiler**: `GCC 16.2.1` (`-O3 -DNDEBUG`, `CMAKE_BUILD_TYPE=Release`)
* **Bitwise Verification**: 100% bitwise parity verified against scalar C99 references (`openavx_simd_audit_test` and `openavx_bench`).
* **Cache Isolation**: Full-frame codec tests employ multi-frame cold buffer rings exceeding L3 CPU cache size (16 frames for 1080p, 8 frames for 4K) to prevent synthetic L1/L2 cache pre-warming.

---

## 2. SIMD Vectorization & Pixel Throughput

Measured via `./build/bin/openavx_bench`. Evaluates raw color space conversion, channel swapping, alpha blending, and discrete cosine transform (DCT) performance against scalar C99 reference code.

### 2.1 Video Frame Color Transforms & Compositing

| Operation | Workload / Resolution | Scalar C99 | SIMD (AVX2+FMA) | Speedup | SIMD Frame Time | SIMD Memory Throughput |
|---|---|---:|---:|---:|---:|---:|
| `UYVY -> RGBA` | 1080p Full HD (2.07 MP) | 89.4 FPS | 270.8 FPS | **3.03x** | 3.69 ms | 3.14 GB/s |
| `UYVY -> BGRA` | 1080p Full HD (2.07 MP) | 94.7 FPS | 249.5 FPS | **2.63x** | 4.01 ms | 2.89 GB/s |
| `RGBA -> UYVY` | 1080p Full HD (2.07 MP) | 267.6 FPS | 322.2 FPS | **1.20x** | 3.10 ms | 3.73 GB/s |
| `BGRA -> UYVY` | 1080p Full HD (2.07 MP) | 249.1 FPS | 316.4 FPS | **1.27x** | 3.16 ms | 3.67 GB/s |
| `RGBA <-> BGRA Swap` | 1080p Full HD (2.07 MP) | 536.4 FPS | 525.2 FPS | **0.98x** | 1.90 ms | 8.11 GB/s |
| `RGBA Alpha Blend` | 1080p Full HD (2.07 MP) | 66.9 FPS | 268.3 FPS | **4.01x** | 3.73 ms | 6.22 GB/s |
| `UYVY -> RGBA` | 4K Ultra HD (8.29 MP) | 23.8 FPS | 64.8 FPS | **2.72x** | 15.44 ms | 3.00 GB/s |
| `UYVY -> BGRA` | 4K Ultra HD (8.29 MP) | 23.8 FPS | 65.3 FPS | **2.74x** | 15.32 ms | 3.02 GB/s |
| `RGBA -> UYVY` | 4K Ultra HD (8.29 MP) | 61.6 FPS | 78.9 FPS | **1.28x** | 12.67 ms | 3.66 GB/s |
| `BGRA -> UYVY` | 4K Ultra HD (8.29 MP) | 62.7 FPS | 84.9 FPS | **1.35x** | 11.77 ms | 3.94 GB/s |
| `RGBA <-> BGRA Swap` | 4K Ultra HD (8.29 MP) | 130.7 FPS | 124.5 FPS | **0.95x** | 8.03 ms | 7.69 GB/s |
| `RGBA Alpha Blend` | 4K Ultra HD (8.29 MP) | 16.5 FPS | 71.9 FPS | **4.37x** | 13.91 ms | 6.67 GB/s |

### 2.2 SpeedHQ-2 Discrete Cosine Transform (8x8 Blocks)

Each 1080p video frame consists of 65,280 blocks ($8 \times 8$ coefficients).

| Operation | Block Count | Scalar C99 | SIMD (AVX2+FMA) | Speedup | Latency / Block | 1080p Frame Time |
|---|---|---:|---:|---:|---:|---:|
| `SpeedHQ-2 Forward DCT` | 65,280 blocks (1080p) | 5.09 Mblocks/s | 8.92 Mblocks/s | **1.75x** | 112.1 ns | 7.32 ms |
| `SpeedHQ-2 Inverse DCT` | 65,280 blocks (1080p) | 2.21 Mblocks/s | 8.44 Mblocks/s | **3.81x** | 118.4 ns | 7.73 ms |

---

## 3. Audio Digital Signal Processing (DSP)

Measured via `./build/bin/openavx_bench`. Evaluates sample rate conversion (resampling) and broadcast multi-channel downmixing across 48 kHz 32-bit floating-point planar audio buffers.

### 3.1 Sample Rate Conversion (Resampling: 48 kHz $\to$ 44.1 kHz)

| Algorithm / Filter Mode | Audio Specification | Throughput | Time / 1 sec Audio | Real-Time Factor |
|---|---|---:|---:|---:|
| **Linear Interpolation** | 2-point low-overhead | **61.47 MSamples/s** | 0.78 ms | **1,280.5x Real-Time** |
| **Cubic Catmull-Rom** | 4-point broadcast spline | **43.71 MSamples/s** | 1.10 ms | **910.6x Real-Time** |
| **Windowed Blackman Sinc** | 32-point studio master grade | **0.71 MSamples/s** | 68.04 ms | **14.7x Real-Time** |

### 3.2 Broadcast Multi-Channel Downmixing

Evaluates ITU-R BS.775 and ATSC A/52 compliant downmixing across multi-channel surround tracks (48,000 frames per channel = 1.0 second):

| Downmix Configuration | Channel Mapping | Processing Throughput | Memory Bandwidth | Execution Time | Real-Time Factor |
|---|---|---:|---:|---:|---:|
| **7.1 Surround $\to$ Stereo** | 8 ch $\to$ 2 ch (BS.775) | **1,544.71 MSamples/s** | 5.75 GB/s | 0.249 ms | **4,022.7x Real-Time** |
| **5.1 Surround $\to$ Stereo** | 6 ch $\to$ 2 ch (BS.775) | **3,683.59 MSamples/s** | 13.72 GB/s | 0.078 ms | **12,790.2x Real-Time** |
| **5.1 Surround $\to$ Lt/Rt Matrix** | 6 ch $\to$ 2 ch (Dolby ProLogic) | **4,250.79 MSamples/s** | 15.84 GB/s | 0.068 ms | **14,759.7x Real-Time** |
| **Quadraphonic $\to$ Stereo** | 4 ch $\to$ 2 ch | **874.99 MSamples/s** | 3.26 GB/s | 0.219 ms | **4,557.2x Real-Time** |
| **Stereo $\to$ Mono** | 2 ch $\to$ 1 ch | **789.24 MSamples/s** | 2.94 GB/s | 0.122 ms | **8,221.2x Real-Time** |

---

## 4. Network Reliability & Cryptographic Security

Measured via `./build/bin/openavx_bench`. Evaluates SMPTE ST 2022-1 2D XOR Forward Error Correction (FEC) and native XChaCha20-Poly1305 End-to-End Encryption (E2EE).

### 4.1 SMPTE ST 2022-1 2D XOR Forward Error Correction (FEC)

Evaluates parity generation and iterative matrix reconstruction on blocks of 50 fragments ($D=10, L=5$), 1,316 bytes per fragment (65.8 KB media block):

| Operation | Matrix Configuration | Processing Throughput | Latency / 50-Packet Block | Unit Latency |
|---|---|---:|---:|---:|
| **1D Column Parity Generation** | $D=10, L=5$ (10 parity packets) | **8.43 GB/s** | 7.27 $\mu$s | 145.39 ns / fragment |
| **2D Column + Row Parity Generation** | $D=10, L=5$ (15 parity packets) | **4.22 GB/s** | 14.51 $\mu$s | 290.11 ns / fragment |
| **2D Packet Reconstruction** | 10% Burst Packet Loss (5 lost) | **100% Recovery** | 8.20 $\mu$s / block | **1.64 $\mu$s / recovered packet** |

### 4.2 Native End-to-End Encryption (XChaCha20-Poly1305 AEAD)

Evaluates authenticated cipher throughput on standard 1,376-byte media fragments with 32-byte authenticated additional data (AD):

| Cryptographic Operation | Packet Payload Size | Cipher Throughput | Latency / Packet | Packet Processing Rate |
|---|---|---:|---:|---:|
| **XChaCha20-Poly1305 Encrypt** | 1,376 bytes | **0.12 GB/s** | 10.64 $\mu$s | **93.95 Kpkts/sec** |
| **XChaCha20-Poly1305 Decrypt + MAC Auth** | 1,376 bytes | **0.11 GB/s** | 11.62 $\mu$s | **86.05 Kpkts/sec** |

---

## 5. Full-Frame SpeedHQ-2 Codec Throughput

Measured via `./build/bin/openavx_stream_bench --layer 1`. Evaluates cold multi-frame ring buffers rotating through RAM exceeding CPU L3 cache capacity:

| Workload | Quality Profile | Encode Rate | Encode Wall Time | Decode Rate | Decode Wall Time | Compressed Frame Size | Compression Ratio | Wire Bitrate @ 60 FPS |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| **1080p UYVY** | `Q=96` (Studio High) | **43.8 FPS** | 22.82 ms | **109.5 FPS** | 9.13 ms | 477.3 KB | **8.5 : 1** | **234.6 Mbps** |
| **1080p UYVY** | `Q=50` (Balanced) | **100.5 FPS** | 9.95 ms | **262.2 FPS** | 3.81 ms | 102.0 KB | **39.7 : 1** | **50.1 Mbps** |
| **4K UYVY** | `Q=96` (Studio High) | **11.4 FPS** | 87.62 ms | **22.9 FPS** | 43.74 ms | 1840.4 KB | **8.8 : 1** | **904.6 Mbps** |
| **4K UYVY** | `Q=50` (Balanced) | **26.8 FPS** | 37.29 ms | **58.6 FPS** | 17.07 ms | 370.1 KB | **43.8 : 1** | **181.9 Mbps** |

---

## 6. Real-World Pipeline Socket Latency & Wire Bandwidth

Measured via `./build/bin/openavx_stream_bench --layer 2`. Evaluates end-to-end transmit over local loopback (`127.0.0.1`), socket serialization, transport framing, receive decoding, and delivery with synchronous 48 kHz stereo audio:

| Pipeline Scenario | Delivery Rate | Wire Bandwidth | Drops | CPU Load | Jitter | Latency P50 | Latency P95 | Latency P99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **1080p60 Uncompressed (Raw)** | **60.3 FPS** | 2,002.2 Mbps | **0** | 24% | 0.43 ms | **3.29 ms** | 3.92 ms | 6.09 ms |
| **1080p60 UYVY -> BGRA (SIMD)** | **60.3 FPS** | 2,002.8 Mbps | **0** | 25% | 0.29 ms | **3.36 ms** | 3.90 ms | 6.86 ms |
| **1080p60 SpeedHQ-2 -> BGRA (Codec)** | **60.0 FPS** | **52.1 Mbps** | **0** | 326% | 0.65 ms | **15.86 ms** | 21.27 ms | 53.19 ms |
| **4K60 Uncompressed (Raw)** | **60.1 FPS** | 7,980.8 Mbps | **0** | 83% | 1.44 ms | **11.49 ms** | 14.00 ms | 28.78 ms |
| **1080p Raw Line-Rate Burst** | **336.3 FPS** | **11,926.8 Mbps** | 118 | 206% | 13.81 ms | **11.44 ms** | 17.10 ms | 18.21 ms |
| **1080p SpeedHQ-2 Burst** | **87.4 FPS** | **75.9 Mbps** | **0** | 457% | 5.81 ms | **24.79 ms** | 29.13 ms | 30.14 ms |

---

## 7. Multi-Viewer Fanout & Slow-Client Isolation

Measured via `./build/bin/openavx_stream_bench --layer 4`. Evaluates one sender broadcasting simultaneously to multiple independent receivers over TCP sockets:

| Fanout Scenario | Concurrent Viewers | Receiver Delivery | Wire Bandwidth | CPU Load | Dropped Frames |
|---|---:|---:|---:|---:|---:|
| **1080p60 SpeedHQ-2** | 1 Client | 60.0 FPS | 52.1 Mbps | 347% | 0 |
| **1080p60 SpeedHQ-2** | 2 Clients | 59.8 FPS | 103.9 Mbps | 401% | 0 |
| **1080p60 SpeedHQ-2** | 4 Clients | 59.5 FPS | 206.7 Mbps | 630% | 0 |
| **1080p60 SpeedHQ-2** | 8 Clients | 52.7 FPS | 366.0 Mbps | 937% | 0 |
| **Slow-Client Isolation Proof** | **3 Fast + 1 Stalled** | **59.7 FPS (Fast)** | 198.5 Mbps | 503% | **0 on Fast Clients** |

> **Non-blocking Client Isolation**: When one receiver freezes or halts socket read operations, OpenAVX's per-client ring buffer drops packets destined for the stalled client without blocking or lagging any of the other concurrent viewers.

---

## 8. How to Reproduce

All benchmarks are part of the OpenAVX test suite and can be built and run on any Linux, macOS, or Windows system:

```bash
# 1. Build OpenAVX with Release optimizations
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j$(nproc)

# 2. Run the SIMD, Audio DSP, FEC, and E2EE micro-benchmarks
./bin/openavx_bench

# 3. Run the Full-Frame Codec cold-cache benchmark
./bin/openavx_stream_bench --layer 1

# 4. Run the Real Socket Single-Stream latency benchmark
./bin/openavx_stream_bench --layer 2

# 5. Run the Transport comparison (TCP vs RUDP)
./bin/openavx_stream_bench --layer 3

# 6. Run the Multi-Viewer Fanout & Slow-Client Isolation benchmark
./bin/openavx_stream_bench --layer 4

# 7. Run all stream benchmark layers in quick validation mode
./bin/openavx_stream_bench --quick --all
```
