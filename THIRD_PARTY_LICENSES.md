# OpenAVX Third-Party Notices & Licenses

OpenAVX incorporates, vendors, or dynamically interfaces with third-party software components. This document provides attribution and notices for those components.

---

## 1. PipeWire & Simple Plugin API (SPA) Headers

- **Location:** `third-party/pipewire/pipewire-0.3/`, `third-party/pipewire/spa-0.2/`
- **Upstream Project:** PipeWire (https://pipewire.org)
- **Copyright:** Copyright (c) 2018 Wim Taymans, Red Hat, Inc., Collabora Ltd., and PipeWire contributors
- **License:** MIT License (`SPDX-License-Identifier: MIT`)
- **Notice:** The headers are vendored for cross-platform compatibility and system-independent compilation. Each header file contains its original SPDX license identifier and copyright header. Distributed under the terms of the MIT License.

---

## 2. Monocypher

- **Location:** `third-party/monocypher/monocypher.c`, `third-party/monocypher/monocypher.h`
- **Upstream Project:** Monocypher (https://monocypher.org)
- **Author:** Loup Vaillant
- **Copyright:** Copyright (c) 2017-2020, Loup Vaillant
- **License:** Dual-licensed under `BSD-2-Clause OR CC0-1.0`
- **Notice:** Used for authenticated encryption (ChaCha20-Poly1305 / XChaCha20). Source code retains original copyright notices and disclaimers in accordance with the BSD 2-Clause License and CC0 Public Domain Dedication.

---

## 3. FFmpeg Runtime Bindings (Optional)

- **Location:** `src/avx_video_avcodec.c`
- **Upstream Project:** FFmpeg (https://ffmpeg.org)
- **Copyright:** FFmpeg developers (Fabrice Bellard, Michael Niedermayer, et al.)
- **License:** LGPL v2.1+ / GPL v2+ (depending on host build)
- **Notice:** OpenAVX does not statically bundle or link against precompiled FFmpeg binaries. It provides optional runtime `dlopen` / `LoadLibrary` bindings to interact with system-installed `libavcodec` / `libavutil` for optional compressed video decoding. The dynamically loaded FFmpeg libraries remain governed by their respective LGPL or GPL licenses.

---

## 4. SpeedHQ Codec Implementation

- **Location:** `src/avx_speedhq.c`, `include/avx_speedhq.h`
- **Upstream Project:** FFmpeg (`libavcodec/speedhq.c`, `libavcodec/speedhqenc.c`)
- **Author / Copyright:** Copyright (c) 2017 Steinar H. Gunderson, Copyright (c) FFmpeg contributors
- **License:** GNU Lesser General Public License v2.1 or later (`SPDX-License-Identifier: LGPL-2.1-or-later`)
- **Notice:** The standalone SpeedHQ intra-frame DCT codec and VLC tables are derived from the FFmpeg project's implementation. Distributed under the terms of the GNU Lesser General Public License version 2.1 or later.

---

## 5. Apple CoreMediaIO Platform SDK Declarations

- **Location:** `third-party/proprietary/cmio/CMIOHardwarePlugIn.h`
- **Copyright:** Copyright (c) 2005-2024 Apple Inc. All rights reserved.
- **License:** Apple Proprietary SDK Interface Definitions
- **Notice:** Contains CoreMediaIO interface types, structures, and identifiers used strictly as an offline fallback for platform compatibility and cross-compilation testing when building on non-macOS platforms. Native macOS builds use official framework headers from the Xcode SDK.
