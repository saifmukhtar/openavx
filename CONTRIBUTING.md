# Contributing to OpenAVX

Thanks for contributing to **OpenAVX**. Keep pull requests focused, small, and tested before submitting.

## Build & Test

OpenAVX uses CMake (`>= 3.16`) as its supported build system:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

* **Parsers / Codecs / Wire Framing:** Also test under Clang with sanitizers and libFuzzer enabled:
  ```bash
  cmake -B build_fuzz -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DENABLE_FUZZING=ON -DBUILD_TESTING=ON
  cmake --build build_fuzz -j
  ```
* **SIMD Changes:** Any edits to [`src/avx_simd.c`](src/avx_simd.c) or [`src/avx_audio.c`](src/avx_audio.c) must match scalar C99 results bit-for-bit (`./build/bin/openavx_bench` and `./build/bin/openavx_simd_audit_test`).

## Project Rules

1. **C99 Portability:** Code must compile cleanly under `-std=c99 -D_POSIX_C_SOURCE=200809L` with `-Wall -Wextra` across Linux (`glibc` and `musl`), ARM64 (`aarch64` NEON), ARM32, x86_64, macOS, Windows, and BSD.
2. **No New Compile-Time Dependencies:** `libopenavx` links only OS/runtime libraries (`libc`, `pthread`, `libm`, `libdl` / `ws2_32`). Optional external integrations (such as host codecs or desktop audio drivers) must load at runtime via `dlopen` / `LoadLibraryA` without `#include`-ing third-party headers.
3. **Code Provenance:** Never copy code or headers from copyleft or proprietary third-party SDKs. Keep SpeedHQ ([`src/avx_speedhq.c`](src/avx_speedhq.c)) standalone with zero FFmpeg dependency ([`src/avx_video_avcodec.c`](src/avx_video_avcodec.c) is strictly for optional runtime H.264/HEVC/AV1 compressed stream decoding).
4. **Public Symbols & Headers:**
   * Mark public library exports with `OPENAVX_API` and register them in [`openavx.map`](openavx.map).
   * Add the standard `SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0` + copyright header across `include/`, `src/`, `tools/`, `test/`, and `fuzz/` (vendored code in `third-party/` and SpeedHQ in `src/avx_speedhq.c` retain their respective upstream licenses per [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md)).
5. **Code Formatting:** Format C and C++ source files using `clang-format -i <file>` with the root [`.clang-format`](.clang-format) configuration before opening a pull request.

## Pull Request & CLA Sign-Off

Every external contributor must accept the [Contributor License Agreement](CLA.md) by posting the following comment on their pull request (add `Entity: <Company Name>` if submitting on behalf of an employer):

```text
I have read the CLA Document and I hereby sign the CLA
Name: <Full Legal Name>
Email: <email@example.com>
```
