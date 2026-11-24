[← Back to OpenAVX Main README](../../README.md)

# OpenAVX Homebrew Formula

Official Homebrew formula for macOS (Apple Silicon & Intel) and Linux (`x86_64` & `arm64`).

## Installation

```bash
# Install tagged release
brew install saifmukhtar/openavx/openavx

# Or install latest commit from main
brew install --HEAD saifmukhtar/openavx/openavx
```

## What Gets Installed
* **Shared Library**: `libopenavx.dylib` (macOS) / `libopenavx.so` (Linux)
* **Headers**: `openavx.h` (C99), `openavx.hpp` (C++11 RAII)
* **Pkg-Config**: `openavx.pc`
* **CLI Suite**: `openavx`, `openavx-find`, `openavx-router`, `openavx-virtcam`, `openavx-gateway`, `openavx-test-pattern`, `openavx-monitor`, `openavx-discovery-server`, `openavx-tally`

## Background Discovery Service
Run the centralized TCP directory daemon (`7959`) via Homebrew Services (managed by `launchd` on macOS or `systemd` on Linux):

```bash
brew services start openavx    # Start on login
brew services list             # Check running status
brew services stop openavx     # Stop service
```
