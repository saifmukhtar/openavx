# Security Policy

## Supported Versions

OpenAVX follows semantic versioning. Security fixes are applied to the latest minor release line and `main`:

| Version | Supported |
| --- | --- |
| `0.2.x` (`main`) | Yes |
| `0.1.x` | No |
| `< 0.1.0` | No |

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public GitHub issues, pull requests, or discussions.**

Because OpenAVX parses untrusted network framing, headers, XML color metadata, and compressed bitstreams in C99, we handle memory-safety, integer-overflow, DoS, and parser vulnerabilities privately until a fix is ready.

Report vulnerabilities using either of these private channels:

1. **GitHub Private Vulnerability Reporting (Preferred):**
   Submit a draft advisory at <https://github.com/saifmukhtar/openavx/security/advisories/new>.
2. **Email:**
   Send details to **saifmukhtar20@gmail.com** with subject prefix `[SECURITY] OpenAVX: <short summary>`.

### What to Include

To help reproduce and patch quickly, please include:

- Affected commit SHA or release tag (output of `openavx --version` if applicable)
- Operating system, architecture, libc (`glibc`, `musl`, macOS, Windows), and compiler flags
- Affected component (`libopenavx` wire parser, discovery, SpeedHQ, RTSP demuxer, HDR XML, web/RTSP server, or CLI)
- Minimal proof-of-concept input, command, or libFuzzer testcase (`crash-*` / AddressSanitizer / UBSan trace)
- Potential impact (e.g., heap buffer over-read/write, infinite loop, remote crash, unauthenticated state change)

## Response & Disclosure Process

- **Acknowledgment:** We will acknowledge receipt of your report in a timely manner following initial review.
- **Triage & Fix:** We validate the reproducer against existing sanitizers and [`fuzz/`](fuzz) targets, develop a patch, and add a regression test before publishing.
- **Coordinated Disclosure:** Advisories and fixes are released together once patched across supported branches. Reporters are credited in the advisory and release notes unless anonymity is requested.
