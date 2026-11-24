# ==============================================================================
# OpenAVX (Open Audio-Video eXchange)
# Multi-Stage Alpine Linux Minimal Production Container
# ==============================================================================

# Stage 1: Build & Automated Verification
FROM alpine:3.20 AS builder

RUN apk add --no-cache \
    build-base \
    cmake \
    linux-headers

WORKDIR /src
COPY . .

# Configure, compile, and run CTest suite verification
RUN cmake -B build -S . \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_TESTING=ON && \
    cmake --build build -j$(nproc) && \
    ctest --test-dir build --output-on-failure && \
    DESTDIR=/staging cmake --install build

# Stage 2: Minimal Alpine Runtime Image
FROM alpine:3.20 AS runtime

RUN apk add --no-cache \
    libgcc \
    libstdc++ && \
    addgroup -g 1000 openavx && \
    adduser -u 1000 -G openavx -s /bin/sh -D openavx

# Copy compiled shared library and CLI utilities
COPY --from=builder /staging/usr/lib/libopenavx* /usr/lib/
COPY --from=builder /staging/usr/bin/openavx* /usr/bin/

# Default working directory
WORKDIR /home/openavx
USER openavx

# Standard Broadcast Ports:
# 7959/tcp: OpenAVX Discovery Server
# 7960/tcp: Base stream communication port
# 5353/udp: LAN Multicast DNS (RFC 6762)
EXPOSE 7959/tcp 7960/tcp 5353/udp

# Default to running openavx-discovery-server
ENTRYPOINT ["/usr/bin/openavx-discovery-server"]
CMD ["--port", "7959"]
