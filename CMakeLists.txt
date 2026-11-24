cmake_minimum_required(VERSION 3.16)

# ==============================================================================
# OpenAVX (Open Audio-Video eXchange)
# Cross-Platform CMake Build System (Linux, macOS, Windows, BSD)
# ==============================================================================
project(openavx
    VERSION 0.2.0
    DESCRIPTION "Open Audio-Video eXchange Library"
    HOMEPAGE_URL "https://github.com/saifmukhtar/openavx"
    LANGUAGES C CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ------------------------------------------------------------------------------
# Language Standards & Global Definitions
# ------------------------------------------------------------------------------
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
    add_compile_options(/W3 /O2)
else()
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
    if(APPLE)
        add_compile_definitions(_DARWIN_C_SOURCE)
    endif()
    add_compile_options(-Wall -Wextra -O3)
    # Enable hardware NEON on 32-bit ARM (armv7l/armhf)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7|armv6|armhf)")
        option(ENABLE_ARM_NEON "Enable ARM NEON hardware acceleration" ON)
        if(ENABLE_ARM_NEON)
            add_compile_options(-mfpu=neon -mfloat-abi=hard)
        endif()
    endif()
endif()

# Standard output directories matching openavx conventions
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build")

foreach(CONFIG_TYPE Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${CONFIG_TYPE} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_CURRENT_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_CURRENT_BINARY_DIR}/build")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_CURRENT_BINARY_DIR}/build")
endforeach()

# ------------------------------------------------------------------------------
# System Dependencies (Zero External Third-Party Runtime Bloat)
# ------------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(SYSTEM_LIBS "")

if(WIN32)
    list(APPEND SYSTEM_LIBS ws2_32)
elseif(UNIX)
    # Math library
    find_library(LIBM m)
    if(LIBM)
        list(APPEND SYSTEM_LIBS ${LIBM})
    endif()

    # Dynamic loading library (glibc < 2.34 requires libdl; macOS/BSD has it in libc)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_library(LIBDL dl)
        if(LIBDL)
            list(APPEND SYSTEM_LIBS ${LIBDL})
        endif()
    endif()
endif()

# ------------------------------------------------------------------------------
# RPATH Configuration (Ensures build tree binaries load our built libopenavx)
# ------------------------------------------------------------------------------
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

list(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${CMAKE_INSTALL_FULL_LIBDIR}" is_system_dir)
if(is_system_dir EQUAL -1)
    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
else()
    set(CMAKE_INSTALL_RPATH "")
endif()

# ------------------------------------------------------------------------------
# Core Shared Library Target: libopenavx
# ------------------------------------------------------------------------------
set(OPENAVX_CORE_SOURCES
    src/avx_instance.c
    src/avx_find.c
    src/avx_send.c
    src/avx_send_client.c
    src/avx_send_profiles.c
    src/avx_send_aux.c
    src/avx_recv.c
    src/avx_socket.c
    src/avx_video_codec.c
    src/avx_video_v4l2.c
    src/avx_video_avcodec.c
    src/avx_video_mf.c
    src/avx_video_vt.c
    src/avx_speedhq.c
    src/avx_fec.c
    src/avx_rate_control.c
    third-party/monocypher/monocypher.c
    src/avx_crypto.c
    src/avx_udp.c
    src/avx_v4l2_sink.c
    src/avx_pipewire_sink.c
    src/avx_virtcam_ipc.c
    src/avx_dshow_sink.c
    src/avx_dal_sink.c
    src/avx_discovery_server.c
    src/avx_color.c
    src/avx_routing.c
    src/avx_simd.c
    src/avx_audio.c
    src/avx_audio_codec.c
    src/avx_rtsp.c
    src/avx_web.c
    src/avx_hdr.c
    src/avx_utilities.c
    src/avx_framesync.c
    src/avx_recv_ex.c
    src/avx_config.c
    src/avx_wire.c
    src/avx_log.c
    src/oavx_pipeline.c
)

add_library(openavx SHARED ${OPENAVX_CORE_SOURCES})
add_library(openavx::openavx ALIAS openavx)

target_include_directories(openavx
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/monocypher
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/pipewire/pipewire-0.3
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/pipewire/spa-0.2
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/proprietary
)

target_compile_definitions(openavx
    PRIVATE
        OPENAVX_EXPORTS
)

target_link_libraries(openavx
    PRIVATE
        Threads::Threads
        ${SYSTEM_LIBS}
)

# Platform-specific naming and symbols
if(WIN32)
    set_target_properties(openavx PROPERTIES
        OUTPUT_NAME "openavx"
    )
elseif(APPLE)
    set_target_properties(openavx PROPERTIES
        OUTPUT_NAME "openavx"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
    target_link_libraries(openavx PRIVATE
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreFoundation"
    )
else()
    set_target_properties(openavx PROPERTIES
        OUTPUT_NAME "openavx"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(openavx PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/openavx.map")
    endif()
endif()

# ------------------------------------------------------------------------------
# Unified CLI Broadcast Utilities Suite
# ------------------------------------------------------------------------------
option(BUILD_TOOLS "Build OpenAVX CLI utilities" ON)

if(BUILD_TOOLS)
    add_executable(openavx_cli
        tools/openavx_main.c
        tools/openavx_find.c
        tools/openavx_monitor.c
        tools/avx_audio_output.c
        tools/openavx_router.c
        tools/openavx_test_pattern.c
        tools/openavx_server.c
        tools/openavx_gateway.c
        tools/openavx_virtcam.c
        tools/openavx_tally.c
    )
    set_target_properties(openavx_cli PROPERTIES
        OUTPUT_NAME "openavx"
    )
    target_include_directories(openavx_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/pipewire/pipewire-0.3
        ${CMAKE_CURRENT_SOURCE_DIR}/third-party/pipewire/spa-0.2
    )
    target_link_libraries(openavx_cli PRIVATE openavx Threads::Threads ${SYSTEM_LIBS})

    # UNIX Build-tree binary symlinks for CLI subcommand entry points
    if(UNIX)
        add_custom_target(openavx_bin_symlinks ALL
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-find"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx_find"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-monitor"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx_monitor"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-router"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx_router"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-test-pattern"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx_test_pattern"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-discovery-server"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx_discovery_server"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-gateway"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-web-gateway"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx_web_gateway"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-virtcam"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx_virtcam"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx-tally"
            COMMAND ${CMAKE_COMMAND} -E create_symlink openavx "${CMAKE_CURRENT_BINARY_DIR}/bin/openavx_tally"
            DEPENDS openavx_cli
        )
    endif()
endif()

# ------------------------------------------------------------------------------
# Verification Test Suites
# ------------------------------------------------------------------------------
option(BUILD_TESTING "Build OpenAVX automated test suites" ON)

if(BUILD_TESTING)
    enable_testing()

    macro(add_openavx_test test_name test_source)
        add_executable(${test_name} ${test_source} ${ARGN})
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tools
            ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures
            ${CMAKE_CURRENT_SOURCE_DIR}/third-party/monocypher
            ${CMAKE_CURRENT_SOURCE_DIR}/third-party/pipewire/pipewire-0.3
            ${CMAKE_CURRENT_SOURCE_DIR}/third-party/pipewire/spa-0.2
            ${CMAKE_CURRENT_SOURCE_DIR}/third-party/proprietary
        )
        target_link_libraries(${test_name} PRIVATE openavx Threads::Threads ${SYSTEM_LIBS})
        if(NOT MSVC)
            target_compile_options(${test_name} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${test_name}
            COMMAND "${CMAKE_CURRENT_BINARY_DIR}/bin/${test_name}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        )
    endmacro()

    # 1. Foundation and loopback tests
    add_executable(openavx_stream_sender test/openavx_stream_sender.c)
    target_include_directories(openavx_stream_sender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(openavx_stream_sender PRIVATE openavx Threads::Threads ${SYSTEM_LIBS})

    add_openavx_test(openavx_test test/openavx_test.c)
    add_openavx_test(openavx_loopback_test test/openavx_loopback_test.c)

    # 2. Codec and alpha verification
    add_openavx_test(openavx_speedhq_test test/openavx_speedhq_test.c src/avx_speedhq.c)
    target_link_libraries(openavx_speedhq_test PRIVATE ${CMAKE_DL_LIBS})
    add_openavx_test(openavx_speedhq_alpha_test test/openavx_speedhq_alpha_test.c)
    add_openavx_test(openavx_compressed_loopback_test test/openavx_compressed_loopback_test.c)

    # 3. Discovery and networking
    add_openavx_test(openavx_discovery_udp_test test/openavx_discovery_udp_test.c src/avx_udp.c src/avx_socket.c)
    add_openavx_test(openavx_discovery_server_test test/openavx_discovery_server_test.c src/avx_discovery_server.c src/avx_socket.c)
    add_openavx_test(openavx_mdns_discovery_test test/openavx_mdns_discovery_test.c)

    # 4. Metadata, tally, routing, and color
    add_openavx_test(openavx_metadata_tally_test test/openavx_metadata_tally_test.c)
    add_openavx_test(openavx_alpha_color_test test/openavx_alpha_color_test.c src/avx_color.c)
    add_openavx_test(openavx_routing_test test/openavx_routing_test.c)

    # 5. CLI tools verification
    add_openavx_test(openavx_monitor_test test/openavx_monitor_test.c)
    add_openavx_test(openavx_find_test test/openavx_find_test.c)
    add_openavx_test(openavx_test_pattern_test test/openavx_test_pattern_test.c)
    add_openavx_test(openavx_router_test test/openavx_router_test.c)
    add_openavx_test(openavx_gateway_test test/openavx_gateway_test.c)
    add_openavx_test(openavx_tally_test test/openavx_tally_test.c)
    if(BUILD_TOOLS AND UNIX)
        add_dependencies(openavx_monitor_test openavx_bin_symlinks)
        add_dependencies(openavx_find_test openavx_bin_symlinks)
        add_dependencies(openavx_gateway_test openavx_bin_symlinks)
        add_dependencies(openavx_tally_test openavx_bin_symlinks)
    endif()

    # 6. Advanced ecosystem tests
    add_openavx_test(openavx_api_v3_test test/openavx_api_v3_test.c)
    add_openavx_test(openavx_config_test test/openavx_config_test.c)
    add_openavx_test(openavx_platform_test test/openavx_platform_test.c)
    add_openavx_test(openavx_cpp_test test/openavx_cpp_test.cpp)
    add_openavx_test(openavx_bench test/openavx_bench.c)
    add_executable(openavx_stream_bench test/openavx_stream_bench.c)
    target_include_directories(openavx_stream_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(openavx_stream_bench PRIVATE openavx Threads::Threads ${SYSTEM_LIBS})
    add_openavx_test(openavx_audio_test test/openavx_audio_test.c)
    add_openavx_test(openavx_rtsp_test test/openavx_rtsp_test.c)
    add_openavx_test(openavx_hdr_test test/openavx_hdr_test.c)
    add_openavx_test(openavx_framesync_util_test test/openavx_framesync_util_test.c)
    add_openavx_test(openavx_ptz_compat_test test/openavx_ptz_compat_test.c)
    add_openavx_test(oavx_stream_test test/oavx_stream_test.c)
    add_openavx_test(oavx_async_fd_test test/oavx_async_fd_test.c)
    add_openavx_test(oavx_telemetry_test test/oavx_telemetry_test.c)
    add_openavx_test(oavx_multiclient_test test/oavx_multiclient_test.c)
    add_openavx_test(openavx_multinic_test test/openavx_multinic_test.c src/avx_discovery_server.c src/avx_socket.c)
    add_openavx_test(openavx_rudp_test test/openavx_rudp_test.c)
    add_openavx_test(openavx_fec_test test/openavx_fec_test.c)
    add_openavx_test(openavx_rate_control_test test/openavx_rate_control_test.c)
    add_openavx_test(openavx_relay_test test/openavx_relay_test.c)
    add_openavx_test(openavx_crypto_test test/openavx_crypto_test.c)
    add_openavx_test(openavx_v4l2_sink_test test/openavx_v4l2_sink_test.c)
    add_openavx_test(openavx_pipewire_sink_test test/openavx_pipewire_sink_test.c)
    add_openavx_test(openavx_virtcam_ipc_test test/openavx_virtcam_ipc_test.c)
    add_openavx_test(openavx_dal_test test/openavx_dal_test.c src/avx_dal_plugin.c)
    if(WIN32 OR MINGW)
        add_library(openavx_dshow_filter SHARED
            src/avx_dshow_filter.c
            src/avx_dshow_sink.c
            src/avx_virtcam_ipc.c
            src/avx_log.c
        )
        target_include_directories(openavx_dshow_filter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(openavx_dshow_filter PRIVATE OPENAVX_EXPORTS)
        target_link_libraries(openavx_dshow_filter PRIVATE ole32 oleaut32 uuid)
        add_openavx_test(openavx_dshow_test test/openavx_dshow_test.c)
        target_link_libraries(openavx_dshow_test PRIVATE ole32 oleaut32 uuid)
    endif()
    add_openavx_test(openavx_ipv6_test test/openavx_ipv6_test.c src/avx_discovery_server.c src/avx_socket.c)
    add_openavx_test(openavx_congestion_test test/openavx_congestion_test.c src/avx_speedhq.c)
    add_openavx_test(openavx_failover_test test/openavx_failover_test.c)
    add_openavx_test(oavx_stream_rudp_test test/oavx_stream_rudp_test.c)
    add_openavx_test(openavx_audio_codec_test test/openavx_audio_codec_test.c)
    add_openavx_test(openavx_rtsp_server_test test/openavx_rtsp_server_test.c)
    add_openavx_test(openavx_web_server_test test/openavx_web_server_test.c)
    add_openavx_test(openavx_video_decoder_test test/openavx_video_decoder_test.c)
    add_openavx_test(openavx_simulcast_test test/openavx_simulcast_test.c)

    # 7. Permanent 174M Fuzzing Corpus & Crash Artifact Regression Suite
    add_openavx_test(openavx_fuzz_regression_test test/openavx_fuzz_regression_test.c src/avx_speedhq.c)
    target_compile_definitions(openavx_fuzz_regression_test PRIVATE OPENAVX_PROJECT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    # 8. Socket Resilience, Signal Safety & Concurrency Audit Proof Suite
    add_openavx_test(openavx_socket_resilience_audit_test test/openavx_socket_resilience_audit_test.c)
    add_openavx_test(openavx_concurrency_audit_test test/openavx_concurrency_audit_test.c)
    add_openavx_test(openavx_simd_audit_test test/openavx_simd_audit_test.c)

    # Prevent network/socket contention during parallel ctest (-j)
    set_tests_properties(
        openavx_test
        openavx_loopback_test
        openavx_speedhq_alpha_test
        openavx_compressed_loopback_test
        openavx_discovery_udp_test
        openavx_discovery_server_test
        openavx_mdns_discovery_test
        openavx_metadata_tally_test
        openavx_alpha_color_test
        openavx_monitor_test
        openavx_find_test
        openavx_test_pattern_test
        openavx_router_test
        openavx_cpp_test
        openavx_hdr_test
        openavx_framesync_util_test
        openavx_ptz_compat_test
        openavx_socket_resilience_audit_test
        openavx_concurrency_audit_test
        openavx_simd_audit_test
        oavx_stream_test
        openavx_multinic_test
        openavx_ipv6_test
        openavx_audio_codec_test
        openavx_rtsp_server_test
        openavx_web_server_test
        openavx_video_decoder_test
        openavx_fec_test
        openavx_rate_control_test
        openavx_simulcast_test
        openavx_relay_test
        openavx_crypto_test
        openavx_virtcam_ipc_test
        openavx_dal_test
        PROPERTIES RUN_SERIAL TRUE
    )
endif()

# ------------------------------------------------------------------------------
# libFuzzer Coverage-Guided Fuzzing Test Targets
# ------------------------------------------------------------------------------
option(ENABLE_FUZZING "Build LLVM libFuzzer security fuzz targets (requires Clang)" OFF)

if(ENABLE_FUZZING)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "libFuzzer requires Clang compiler (pass -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)")
    endif()

    set(FUZZ_COMPILE_FLAGS "-fsanitize=fuzzer-no-link,address,undefined" "-g" "-O1" "-fno-omit-frame-pointer")
    set(FUZZ_LINK_FLAGS "-fsanitize=fuzzer,address,undefined")

    # Enable coverage-guidance instrumentation across the core library
    target_compile_options(openavx PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(openavx PUBLIC "-fsanitize=fuzzer-no-link,address,undefined")

    macro(add_openavx_fuzz_target target_name target_source)
        add_executable(${target_name} ${target_source} ${ARGN})
        target_include_directories(${target_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_compile_options(${target_name} PRIVATE ${FUZZ_COMPILE_FLAGS})
        target_link_options(${target_name} PRIVATE ${FUZZ_LINK_FLAGS})
        target_link_libraries(${target_name} PRIVATE openavx Threads::Threads ${SYSTEM_LIBS})

        # Add 5-second sanity fuzz run to ctest when testing is enabled
        if(BUILD_TESTING)
            add_test(NAME ${target_name}
                COMMAND "${CMAKE_CURRENT_BINARY_DIR}/bin/${target_name}" -max_total_time=5
            )
        endif()
    endmacro()

    add_openavx_fuzz_target(fuzz_rtsp_sps fuzz/fuzz_rtsp_sps.c)
    add_openavx_fuzz_target(fuzz_rtsp_demuxer fuzz/fuzz_rtsp_demuxer.c)
    add_openavx_fuzz_target(fuzz_speedhq fuzz/fuzz_speedhq.c src/avx_speedhq.c)
    add_openavx_fuzz_target(fuzz_config fuzz/fuzz_config.c)
    add_openavx_fuzz_target(fuzz_hdr_xml fuzz/fuzz_hdr_xml.c)
    add_openavx_fuzz_target(fuzz_discovery_udp fuzz/fuzz_discovery_udp.c)
    add_openavx_fuzz_target(fuzz_audio_resampler fuzz/fuzz_audio_resampler.c)
    add_openavx_fuzz_target(fuzz_wire_packet fuzz/fuzz_wire_packet.c)
    add_openavx_fuzz_target(fuzz_audio_codec fuzz/fuzz_audio_codec.c)
    add_openavx_fuzz_target(fuzz_rtsp_packetizer fuzz/fuzz_rtsp_packetizer.c)
    add_openavx_fuzz_target(fuzz_fmp4_generator fuzz/fuzz_fmp4_generator.c)
endif()

# ------------------------------------------------------------------------------
# Packaging & Installation
# ------------------------------------------------------------------------------
install(TARGETS openavx
    EXPORT openavxTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(BUILD_TOOLS)
    install(TARGETS openavx_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# Install public headers
install(FILES
    include/openavx.h
    include/openavx.hpp
    include/avx_color.h
    include/avx_routing.h
    include/avx_simd.h
    include/avx_audio.h
    include/avx_audio_codec.h
    include/avx_rtsp.h
    include/avx_hdr.h
    include/avx_config.h
    include/avx_fec.h
    include/avx_rate_control.h
    include/avx_platform.h
    include/avx_crypto.h
    include/avx_v4l2_sink.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install JSON configuration schemas
install(FILES
    schema/v1.0.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/openavx/schema
)

# Install pkg-config (.pc) specification
set(PREFIX "${CMAKE_INSTALL_PREFIX}")
set(LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}")
set(INCDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
set(VERSION "${PROJECT_VERSION}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/openavx.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/openavx.pc"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openavx.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

# UNIX Symlink aliases for CLI tools
if(UNIX)
    if(BUILD_TOOLS)
        install(CODE "
            macro(create_bin_symlink target link)
                execute_process(
                    COMMAND \${CMAKE_COMMAND} -E create_symlink \${target} \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/\${link}\"
                )
            endmacro()

            create_bin_symlink(openavx openavx-find)
            create_bin_symlink(openavx openavx_find)
            create_bin_symlink(openavx openavx-monitor)
            create_bin_symlink(openavx openavx_monitor)
            create_bin_symlink(openavx openavx-router)
            create_bin_symlink(openavx openavx_router)
            create_bin_symlink(openavx openavx-test-pattern)
            create_bin_symlink(openavx openavx_test_pattern)
            create_bin_symlink(openavx openavx-discovery-server)
            create_bin_symlink(openavx openavx_discovery_server)
            create_bin_symlink(openavx openavx-gateway)
            create_bin_symlink(openavx openavx_gateway)
            create_bin_symlink(openavx openavx-web-gateway)
            create_bin_symlink(openavx openavx_web_gateway)
            create_bin_symlink(openavx openavx-virtcam)
            create_bin_symlink(openavx openavx_virtcam)
            create_bin_symlink(openavx openavx-tally)
            create_bin_symlink(openavx openavx_tally)
        ")
    endif()
endif()

# CMake Package Export
install(EXPORT openavxTargets
    FILE openavxTargets.cmake
    NAMESPACE openavx::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/openavx
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/openavxConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/openavxConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/openavxConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/openavx
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/openavxConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/openavxConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/openavx
)

# ------------------------------------------------------------------------------
# CPack Packaging Configuration (.deb, .rpm, .tar.gz)
# ------------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "openavx")
set(CPACK_PACKAGE_VENDOR "openavx")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Open Audio-Video eXchange Library")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Saif Mukhtar <saifmukhtar20@gmail.com>")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/saifmukhtar/openavx")

# Standard package filename convention: {project}-{version}-{os}-{arch}[-{variant}]
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OPENAVX_OS "linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(OPENAVX_OS "macos")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(OPENAVX_OS "windows")
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" OPENAVX_OS)
endif()

if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    set(OPENAVX_ARCH "universal")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|x64)$")
    set(OPENAVX_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(OPENAVX_ARCH "arm64")
else()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" OPENAVX_ARCH)
endif()

if(NOT CPACK_PACKAGE_FILE_NAME)
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${OPENAVX_OS}-${OPENAVX_ARCH}")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ;productbuild")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_SOURCE_GENERATOR "TGZ;TXZ")

include(CPack)
