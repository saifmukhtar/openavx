CROSS_COMPILE ?=
CC ?= $(CROSS_COMPILE)gcc
CXX ?= $(CROSS_COMPILE)g++
AR ?= $(CROSS_COMPILE)ar
STRIP ?= $(CROSS_COMPILE)strip

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# BSD Unix detection (FreeBSD, OpenBSD, NetBSD, DragonFly) - dlopen in libc (no -ldl)
ifeq ($(findstring BSD,$(UNAME_S)),BSD)
    LIBS ?= -lpthread -lm
else ifeq ($(UNAME_S),DragonFly)
    LIBS ?= -lpthread -lm
else ifeq ($(UNAME_S),Darwin)
    LIBS ?= -lpthread -lm
else
    LIBS ?= -lpthread -lm -ldl
endif

# ARM 32-bit (armv7l/armhf) NEON hardware float flags
ifeq ($(findstring arm,$(UNAME_M)),arm)
    ifeq ($(findstring aarch64,$(UNAME_M)),)
        ARM_NEON_FLAGS ?= -mfpu=neon -mfloat-abi=hard
    endif
endif

CFLAGS ?= -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -flto -Wall -Wextra -fPIC -Iinclude -Ithird-party/monocypher $(ARM_NEON_FLAGS)
CXXFLAGS ?= -std=c++11 -O3 -Wall -Wextra -fPIC -Iinclude $(ARM_NEON_FLAGS)
LDFLAGS ?= -shared -flto -Wl,-soname,libopenavx.so.1 -Wl,--version-script=openavx.map

PREFIX ?= /usr/local
EXEC_PREFIX ?= $(PREFIX)
LIBDIR ?= $(EXEC_PREFIX)/lib
INCDIR ?= $(PREFIX)/include
BINDIR ?= $(EXEC_PREFIX)/bin
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
VERSION ?= 1.0.0

SRCS = src/avx_instance.c src/avx_find.c src/avx_send.c src/avx_recv.c src/avx_socket.c src/avx_video_codec.c src/avx_video_v4l2.c src/avx_video_avcodec.c src/avx_video_mf.c src/avx_video_vt.c src/avx_speedhq.c src/avx_udp.c src/avx_discovery_server.c src/avx_color.c src/avx_routing.c src/avx_simd.c src/avx_audio.c src/avx_rtsp.c src/avx_hdr.c src/avx_utilities.c src/avx_framesync.c src/avx_recv_ex.c src/avx_config.c src/avx_wire.c src/avx_log.c src/oavx_pipeline.c
OBJS = $(SRCS:.c=.o)
TARGET = build/libopenavx.so.1

all: directories $(TARGET) build/openavx.pc bin/openavx_test bin/openavx_stream_sender bin/openavx_loopback_test bin/openavx_speedhq_test bin/openavx_speedhq_alpha_test bin/openavx_compressed_loopback_test bin/openavx_discovery_udp_test bin/openavx-discovery-server bin/openavx_discovery_server_test bin/openavx_metadata_tally_test bin/openavx_alpha_color_test bin/openavx_routing_test bin/openavx-monitor bin/openavx_monitor_test bin/openavx-find bin/openavx_find_test bin/openavx-test-pattern bin/openavx_test_pattern_test bin/openavx-router bin/openavx_router_test bin/openavx_api_v3_test bin/openavx_config_test bin/openavx_platform_test bin/openavx_cpp_test bin/openavx_bench bin/openavx_audio_test bin/openavx_rtsp_test bin/openavx_v6_hdr_test bin/openavx_framesync_util_test bin/openavx_ptz_compat_test bin/openavx_mdns_discovery_test bin/oavx_stream_test bin/oavx_async_fd_test bin/oavx_telemetry_test bin/oavx_multiclient_test bin/openavx_multinic_test bin/openavx_rudp_test bin/openavx_ipv6_test bin/openavx_congestion_test bin/openavx_failover_test bin/oavx_stream_rudp_test

directories:
	@mkdir -p build bin

$(TARGET): $(OBJS) openavx.map | directories
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@ln -sf libopenavx.so.1 build/libopenavx.so

build/openavx.pc: openavx.pc.in | directories
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCDIR@|$(INCDIR)|g' \
	     -e 's|@VERSION@|$(VERSION)|g' \
	     $< > $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

bin/openavx_test: test/openavx_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_stream_sender: test/openavx_stream_sender.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_loopback_test: test/openavx_loopback_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_speedhq_test: test/openavx_speedhq_test.c src/avx_speedhq.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< src/avx_speedhq.c -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_speedhq_alpha_test: test/openavx_speedhq_alpha_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_compressed_loopback_test: test/openavx_compressed_loopback_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_discovery_udp_test: test/openavx_discovery_udp_test.c src/avx_udp.c src/avx_socket.c src/avx_wire.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< src/avx_udp.c src/avx_socket.c src/avx_wire.c -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx-discovery-server: src/avx_discovery_server_main.c src/avx_discovery_server.c src/avx_socket.c
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $^ $(LIBS) -o $@
	@ln -sf openavx-discovery-server bin/openavx_discovery_server

bin/openavx_discovery_server_test: test/openavx_discovery_server_test.c src/avx_discovery_server.c src/avx_socket.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< src/avx_discovery_server.c src/avx_socket.c -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_metadata_tally_test: test/openavx_metadata_tally_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_alpha_color_test: test/openavx_alpha_color_test.c src/avx_color.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< src/avx_color.c -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_routing_test: test/openavx_routing_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx-monitor: tools/openavx_monitor.c tools/avx_audio_output.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude -Itools $< tools/avx_audio_output.c -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@
	@ln -sf openavx-monitor bin/openavx_monitor

bin/openavx_monitor_test: test/openavx_monitor_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx-find: tools/openavx_find.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@
	@ln -sf openavx-find bin/openavx_find

bin/openavx_find_test: test/openavx_find_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx-test-pattern: tools/openavx_test_pattern.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude -Itest/fixtures $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -lm -o $@
	@ln -sf openavx-test-pattern bin/openavx_test_pattern

bin/openavx_test_pattern_test: test/openavx_test_pattern_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude -Itest $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -lm -o $@

bin/openavx-router: tools/openavx_router.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@
	@ln -sf openavx-router bin/openavx_router

bin/openavx_router_test: test/openavx_router_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_api_v3_test: test/openavx_api_v3_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_config_test: test/openavx_config_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_platform_test: test/openavx_platform_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_cpp_test: test/openavx_cpp_test.cpp $(TARGET)
	$(CXX) $(CXXFLAGS) $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_bench: test/openavx_bench.c $(TARGET)
	$(CC) $(CFLAGS) $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_audio_test: test/openavx_audio_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_rtsp_test: test/openavx_rtsp_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_v6_hdr_test: test/openavx_v6_hdr_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_framesync_util_test: test/openavx_framesync_util_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_ptz_compat_test: test/openavx_ptz_compat_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_mdns_discovery_test: test/openavx_mdns_discovery_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/oavx_stream_test: test/oavx_stream_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/oavx_async_fd_test: test/oavx_async_fd_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/oavx_telemetry_test: test/oavx_telemetry_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/oavx_multiclient_test: test/oavx_multiclient_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_multinic_test: test/openavx_multinic_test.c src/avx_discovery_server.c src/avx_socket.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< src/avx_discovery_server.c src/avx_socket.c -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_rudp_test: test/openavx_rudp_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_ipv6_test: test/openavx_ipv6_test.c src/avx_discovery_server.c src/avx_socket.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< src/avx_discovery_server.c src/avx_socket.c -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_congestion_test: test/openavx_congestion_test.c src/avx_speedhq.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< src/avx_speedhq.c -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/openavx_failover_test: test/openavx_failover_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

bin/oavx_stream_rudp_test: test/oavx_stream_rudp_test.c $(TARGET)
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O3 -Iinclude $< -Lbuild -lopenavx -Wl,-rpath,'$$ORIGIN/../build' $(LIBS) -o $@

clean:
	rm -rf build bin $(OBJS)

test: all
	./bin/openavx_test
	./bin/openavx_loopback_test
	./bin/openavx_speedhq_test
	./bin/openavx_speedhq_alpha_test
	./bin/openavx_compressed_loopback_test
	./bin/openavx_discovery_udp_test
	./bin/openavx_discovery_server_test
	./bin/openavx_metadata_tally_test
	./bin/openavx_alpha_color_test
	./bin/openavx_routing_test
	./bin/openavx_monitor_test
	./bin/openavx_find_test
	./bin/openavx_test_pattern_test
	./bin/openavx_router_test
	./bin/openavx_api_v3_test
	./bin/openavx_config_test
	./bin/openavx_platform_test
	./bin/openavx_cpp_test
	./bin/openavx_bench
	./bin/openavx_audio_test
	./bin/openavx_rtsp_test
	./bin/openavx_v6_hdr_test
	./bin/openavx_framesync_util_test
	./bin/openavx_ptz_compat_test
	./bin/openavx_mdns_discovery_test
	./bin/oavx_stream_test
	./bin/oavx_async_fd_test
	./bin/oavx_telemetry_test
	./bin/oavx_multiclient_test
	./bin/openavx_multinic_test
	./bin/openavx_rudp_test
	./bin/openavx_ipv6_test
	./bin/openavx_congestion_test
	./bin/openavx_failover_test
	./bin/oavx_stream_rudp_test

install: all
	@echo "[*] Installing OpenAVX (openavx) to $(DESTDIR)$(PREFIX)..."
	@install -d $(DESTDIR)$(LIBDIR)
	@install -d $(DESTDIR)$(INCDIR)
	@install -d $(DESTDIR)$(BINDIR)
	@install -d $(DESTDIR)$(PKGCONFIGDIR)
	@install -m 755 $(TARGET) $(DESTDIR)$(LIBDIR)/libopenavx.so.1
	@ln -sf libopenavx.so.1 $(DESTDIR)$(LIBDIR)/libopenavx.so
	@install -m 644 include/openavx.h $(DESTDIR)$(INCDIR)/
	@install -m 644 include/openavx.hpp $(DESTDIR)$(INCDIR)/
	@install -m 644 include/avx_color.h $(DESTDIR)$(INCDIR)/
	@install -m 644 include/avx_routing.h $(DESTDIR)$(INCDIR)/
	@install -m 644 include/avx_simd.h $(DESTDIR)$(INCDIR)/
	@install -m 644 include/avx_audio.h $(DESTDIR)$(INCDIR)/
	@install -m 644 include/avx_rtsp.h $(DESTDIR)$(INCDIR)/
	@install -m 644 include/avx_hdr.h $(DESTDIR)$(INCDIR)/
	@install -m 644 include/avx_config.h $(DESTDIR)$(INCDIR)/
	@install -m 644 include/avx_platform.h $(DESTDIR)$(INCDIR)/
	@install -m 755 bin/openavx-discovery-server $(DESTDIR)$(BINDIR)/
	@ln -sf openavx-discovery-server $(DESTDIR)$(BINDIR)/openavx_discovery_server
	@install -m 755 bin/openavx-monitor $(DESTDIR)$(BINDIR)/
	@ln -sf openavx-monitor $(DESTDIR)$(BINDIR)/openavx_monitor
	@install -m 755 bin/openavx-find $(DESTDIR)$(BINDIR)/
	@ln -sf openavx-find $(DESTDIR)$(BINDIR)/openavx_find
	@install -m 755 bin/openavx-test-pattern $(DESTDIR)$(BINDIR)/
	@ln -sf openavx-test-pattern $(DESTDIR)$(BINDIR)/openavx_test_pattern
	@install -m 755 bin/openavx-router $(DESTDIR)$(BINDIR)/
	@ln -sf openavx-router $(DESTDIR)$(BINDIR)/openavx_router
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCDIR@|$(INCDIR)|g' \
	     -e 's|@VERSION@|$(VERSION)|g' \
	     openavx.pc.in > $(DESTDIR)$(PKGCONFIGDIR)/openavx.pc
	@chmod 644 $(DESTDIR)$(PKGCONFIGDIR)/openavx.pc
	@echo "[+] Successfully installed OpenAVX (openavx)."

package:
	@echo "[*] Generating native distribution packages (.deb, .rpm, .tar.gz)..."
	@mkdir -p build-cpack dist
	@cmake -B build-cpack -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
	@cmake --build build-cpack -j$$(nproc)
	@cpack --config build-cpack/CPackConfig.cmake -B dist
	@rm -rf build-cpack
	@echo "[+] Distribution packages generated in dist/:"
	@ls -lh dist/

uninstall:
	@echo "[*] Uninstalling OpenAVX (openavx) from $(DESTDIR)$(PREFIX)..."
	@rm -f $(DESTDIR)$(LIBDIR)/libopenavx.so.1
	@rm -f $(DESTDIR)$(LIBDIR)/libopenavx.so
	@rm -f $(DESTDIR)$(INCDIR)/openavx.h
	@rm -f $(DESTDIR)$(INCDIR)/openavx.hpp
	@rm -f $(DESTDIR)$(INCDIR)/avx_color.h
	@rm -f $(DESTDIR)$(INCDIR)/avx_routing.h
	@rm -f $(DESTDIR)$(INCDIR)/avx_simd.h
	@rm -f $(DESTDIR)$(INCDIR)/avx_audio.h
	@rm -f $(DESTDIR)$(INCDIR)/avx_rtsp.h
	@rm -f $(DESTDIR)$(INCDIR)/avx_hdr.h
	@rm -f $(DESTDIR)$(INCDIR)/avx_config.h
	@rm -f $(DESTDIR)$(INCDIR)/avx_platform.h
	@rm -f $(DESTDIR)$(BINDIR)/openavx-discovery-server
	@rm -f $(DESTDIR)$(BINDIR)/openavx_discovery_server
	@rm -f $(DESTDIR)$(BINDIR)/openavx-monitor
	@rm -f $(DESTDIR)$(BINDIR)/openavx_monitor
	@rm -f $(DESTDIR)$(BINDIR)/openavx-find
	@rm -f $(DESTDIR)$(BINDIR)/openavx_find
	@rm -f $(DESTDIR)$(BINDIR)/openavx-test-pattern
	@rm -f $(DESTDIR)$(BINDIR)/openavx_test_pattern
	@rm -f $(DESTDIR)$(BINDIR)/openavx-router
	@rm -f $(DESTDIR)$(BINDIR)/openavx_router
	@rm -f $(DESTDIR)$(PKGCONFIGDIR)/openavx.pc
	@echo "[+] Successfully uninstalled OpenAVX (openavx)."

.PHONY: all clean test directories install uninstall package
