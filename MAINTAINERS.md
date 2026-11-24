# OpenAVX Maintainers

This file lists the active maintainers responsible for reviewing changes, enforcing architecture and C99 portability invariants, and cutting releases for **OpenAVX** (`saifmukhtar/openavx`).

## Lead Maintainer

| Name | GitHub | Scope | Contact |
| --- | --- | --- | --- |
| Saif Mukhtar | [@saifmukhtar](https://github.com/saifmukhtar) | Overall architecture, `libopenavx` core/wire/discovery, codecs, DSP/SIMD, CLI suite, releases & governance | `saifmukhtar20@gmail.com` |

## Review & Governance

- Pull requests trigger automated checks in [`.github/workflows/ci.yml`](.github/workflows/ci.yml) (SPDX headers, `openavx.map` symbol parity, and CMake/CTest across Linux, macOS, and Windows) and require review via [`.github/CODEOWNERS`](.github/CODEOWNERS).
- External contributions require accepting the [Contributor License Agreement](CLA.md) before merge.
- Security issues must be reported privately per [`SECURITY.md`](SECURITY.md).

## Becoming a Maintainer

Contributors with sustained, high-quality pull requests, test/fuzz coverage improvements, or subsystem reviews may be invited to join subsystem maintainership for specific modules (such as codecs, transport, platform ports, or packaging).
