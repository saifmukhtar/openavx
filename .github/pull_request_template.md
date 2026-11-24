## Summary

<!-- Describe what changed and why. -->
Closes # <!-- optional -->

## Environment

- **Tested on (OS / Arch / libc):** <!-- e.g. Ubuntu 22.04 x86_64 (glibc), Alpine 3.20 aarch64 (musl), macOS 15 arm64, Windows x64 -->

## Checks

- [ ] `cmake -B build -DCMAKE_C_EXTENSIONS=OFF && cmake --build build -j && ctest --test-dir build --output-on-failure` passes
- [ ] C99 portable (`-std=c99 -D_POSIX_C_SOURCE=200809L`), no new compile-time third-party dependencies
- [ ] New `include/` / `src/` / `tools/` files include `SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0`; original native code only (no copied copyleft/proprietary code)
- [ ] New `OPENAVX_API` exports in `include/` are added to `openavx.map`
- [ ] SIMD/audio changes match scalar C99 results (`./build/bin/openavx_bench` and `openavx_simd_audit_test`)
- [ ] Fuzz/sanitizer targets tested if modifying parsers, codecs, or wire framing
- [ ] Formatted C/C++ source code with `clang-format -i` (using root `.clang-format`)

## Contributor License Agreement

By opening this pull request, confirm acceptance of [CLA.md](CLA.md) (see [CONTRIBUTING.md](CONTRIBUTING.md)) by commenting with your Git commit name and email (`Entity:` optional if submitting for an employer):

```text
I have read the CLA Document and I hereby sign the CLA
Name: <Full Legal Name>
Email: <email@example.com>
```
