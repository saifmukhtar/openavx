[← Back to OpenAVX Main README](../../README.md)

# OpenAVX Docker Container

Minimal, hardened multi-arch Alpine Linux container (`linux/amd64`, `linux/arm64`) running under an unprivileged user (`openavx`, UID 1000) for headless servers and broadcast networks.

## Quickstart

### 1. Run with Docker Compose (Recommended)
Runs the centralized discovery server daemon on host network (`7959`):

```bash
docker compose -f distribution/docker/docker-compose.yml up -d
```

> **Note on Networking:** Broadcast IP video relies on LAN multicast UDP (`224.0.0.251:5353` mDNS) and dynamic port allocation. Running with `--net=host` (or `network_mode: host` in Compose) is strongly recommended so the container can discover and announce to the local physical subnet without complex NAT routing.

### 2. Build & Run CLI Tools Directly

```bash
# Build image locally
docker build -t openavx:latest -f distribution/docker/Dockerfile .

# Run any OpenAVX CLI utility (find, route, pattern, gateway, virtcam)
docker run --rm -it --net=host openavx:latest find
docker run --rm -it --net=host openavx:latest gateway -s "CAM1" --web 8080 --rtsp 8554
docker run --rm -it --net=host openavx:latest pattern -n "Studio Bars" -p bars100
```
