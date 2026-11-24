/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange
 * include/openavx.hpp - C++11 RAII wrapper for OpenAVX
 */

#pragma once
#ifndef OPENAVX_HPP
#define OPENAVX_HPP

#include "openavx.h"
#include <string>
#include <vector>
#include <utility>

namespace oavx {

/**
 * Free query functions matching clean modern oavx_* C API
 */
inline const char* version() {
    return oavx_version();
}

inline bool is_supported_cpu() {
    return oavx_is_supported_cpu();
}

inline const char* status_to_string(oavx_status_t status) {
    return oavx_status_to_string(status);
}

typedef oavx_telemetry_t Telemetry;

/**
 * RAII Context Engine Guard
 */
class Context {
public:
    Context() : m_ctx(oavx_context_create()) {}
    ~Context() {
        if (m_ctx) {
            oavx_context_destroy(m_ctx);
            m_ctx = nullptr;
        }
    }

    bool is_valid() const { return m_ctx != nullptr; }
    explicit operator bool() const { return is_valid(); }
    oavx_context_t* get() const { return m_ctx; }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    Context(Context&& other) noexcept : m_ctx(other.m_ctx) {
        other.m_ctx = nullptr;
    }

    Context& operator=(Context&& other) noexcept {
        if (this != &other) {
            if (m_ctx) {
                oavx_context_destroy(m_ctx);
            }
            m_ctx = other.m_ctx;
            other.m_ctx = nullptr;
        }
        return *this;
    }

    /* Wire Profile Configuration */
    void set_wire_profile(const oavx_wire_profile_t& profile) {
        oavx_context_set_wire_profile(m_ctx, &profile);
    }

    void get_wire_profile(oavx_wire_profile_t& out_profile) const {
        oavx_context_get_wire_profile(m_ctx, &out_profile);
    }

private:
    oavx_context_t* m_ctx;
};

/**
 * RAII Unified Media Stream (Transmit & Receive)
 */
class Stream {
public:
    Stream() : m_stream(nullptr) {}

    /* Config Constructor */
    Stream(Context& ctx, const oavx_stream_config_t& config) {
        m_stream = oavx_stream_open(ctx.get(), &config);
    }

    /* Named Direction Constructor */
    Stream(Context& ctx, const std::string& name, oavx_direction_t direction,
           const std::string& endpoint = "", const std::string& groups = "") {
        oavx_stream_config_t cfg = {};
        cfg.name = name.c_str();
        cfg.direction = direction;
        cfg.endpoint = endpoint.empty() ? nullptr : endpoint.c_str();
        cfg.groups = groups.empty() ? nullptr : groups.c_str();
        cfg.color_format = OAVX_COLOR_FORMAT_UYVY_RGBA;
        cfg.bandwidth = OAVX_BANDWIDTH_HIGHEST;
        m_stream = oavx_stream_open(ctx.get(), &cfg);
    }

    /* Static Factory Methods */
    static Stream create_transmitter(Context& ctx, const std::string& name, const std::string& groups = "") {
        return Stream(ctx, name, OAVX_DIR_TRANSMIT, "", groups);
    }

    static Stream create_receiver(Context& ctx, const std::string& name, const std::string& endpoint,
                                  oavx_color_format_e color = OAVX_COLOR_FORMAT_UYVY_RGBA,
                                  oavx_bandwidth_e bw = OAVX_BANDWIDTH_HIGHEST,
                                  const std::string& groups = "") {
        oavx_stream_config_t cfg = {};
        cfg.name = name.c_str();
        cfg.endpoint = endpoint.c_str();
        cfg.direction = OAVX_DIR_RECEIVE;
        cfg.color_format = color;
        cfg.bandwidth = bw;
        cfg.groups = groups.empty() ? nullptr : groups.c_str();
        return Stream(ctx, cfg);
    }

    ~Stream() {
        if (m_stream) {
            oavx_stream_close(m_stream);
            m_stream = nullptr;
        }
    }

    bool is_valid() const { return m_stream != nullptr; }
    explicit operator bool() const { return is_valid(); }
    oavx_stream_t* get() const { return m_stream; }

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    Stream(Stream&& other) noexcept : m_stream(other.m_stream) {
        other.m_stream = nullptr;
    }

    Stream& operator=(Stream&& other) noexcept {
        if (this != &other) {
            if (m_stream) {
                oavx_stream_close(m_stream);
            }
            m_stream = other.m_stream;
            other.m_stream = nullptr;
        }
        return *this;
    }

    /* Data Transmission */
    oavx_status_t send_video(const oavx_video_frame_t& frame) {
        return oavx_stream_send_video(m_stream, &frame);
    }

    oavx_status_t send_audio(const oavx_audio_frame_t& frame) {
        return oavx_stream_send_audio(m_stream, &frame);
    }

    oavx_status_t send_metadata(const oavx_metadata_frame_t& meta) {
        return oavx_stream_send_metadata(m_stream, &meta);
    }

    /* Audio Compression Engine */
    bool set_audio_compression(oavx_audio_compression_mode_e mode, int target_bitrate_kbps = 128) {
        return oavx_stream_set_audio_compression(m_stream, mode, target_bitrate_kbps) == OAVX_SUCCESS;
    }

    /* Video Compression Engine */
    bool set_video_compression(uint32_t target_fourcc, uint32_t bitrate_kbps = 0) {
        return oavx_stream_set_video_compression(m_stream, target_fourcc, bitrate_kbps) == OAVX_SUCCESS;
    }

    uint32_t get_video_compression() const {
        return oavx_stream_get_video_compression(m_stream);
    }

    bool set_video_quality(uint8_t quality) {
        return oavx_stream_set_video_quality(m_stream, quality) == OAVX_SUCCESS;
    }

    uint8_t get_video_quality() const {
        return oavx_stream_get_video_quality(m_stream);
    }

    /* Native RTSP 1.0 Server */
    bool enable_rtsp_server(uint16_t port = 0, const std::string& path = "/live") {
        return oavx_stream_enable_rtsp_server(m_stream, port, path.c_str()) == OAVX_SUCCESS;
    }

    void disable_rtsp_server() {
        oavx_stream_disable_rtsp_server(m_stream);
    }

    uint16_t get_rtsp_port() const {
        return oavx_stream_get_rtsp_port(m_stream);
    }

    int get_rtsp_client_count() const {
        return oavx_stream_get_rtsp_client_count(m_stream);
    }

    /* Embedded HTTP / fMP4 Low-Latency Web Preview */
    bool enable_web_preview(uint16_t port = 0) {
        return oavx_stream_enable_web_preview(m_stream, port) == OAVX_SUCCESS;
    }

    void disable_web_preview() {
        oavx_stream_disable_web_preview(m_stream);
    }

    uint16_t get_web_preview_port() const {
        return oavx_stream_get_web_preview_port(m_stream);
    }

    int get_web_client_count() const {
        return oavx_stream_get_web_client_count(m_stream);
    }

    std::string get_web_preview_url() const {
        char buf[256] = {0};
        const char* u = oavx_stream_get_web_preview_url(m_stream, buf, sizeof(buf));
        return u ? std::string(u) : std::string();
    }

    /* Data Reception */
    /**
     * Receives next available frame. Call release_video() after consuming returned video frames.
     */
    oavx_frame_type_t receive(oavx_video_frame_t* v,
                              oavx_audio_frame_t* a,
                              oavx_metadata_frame_t* m,
                              uint32_t timeout_ms) {
        return oavx_stream_receive(m_stream, v, a, m, timeout_ms);
    }

    void release_video(const oavx_video_frame_t& frame) {
        oavx_stream_release_video(m_stream, &frame);
    }

    void release_audio(const oavx_audio_frame_t& frame) {
        oavx_stream_release_audio(m_stream, &frame);
    }

    void release_metadata(const oavx_metadata_frame_t& meta) {
        oavx_stream_release_metadata(m_stream, &meta);
    }

    /* PTZ & Camera Controls */
    bool set_ptz(const oavx_ptz_state_t& ptz) {
        return oavx_stream_set_ptz(m_stream, &ptz);
    }

    /* Tally Controls */
    bool set_tally(const oavx_tally_t& tally) {
        return oavx_stream_set_tally(m_stream, &tally);
    }

    bool get_tally(oavx_tally_t& tally, uint32_t timeout_ms = 0) const {
        return oavx_stream_get_tally(m_stream, &tally, timeout_ms);
    }

    /* Recording Triggers */
    bool record_trigger(const std::string& action, const std::string& filename_hint = "") {
        return oavx_stream_record_trigger(m_stream, action.c_str(), filename_hint.empty() ? nullptr : filename_hint.c_str());
    }

    bool record_status(bool& is_recording, std::string& filename, std::string& error) const {
        const char* fn = nullptr;
        const char* err = nullptr;
        bool ok = oavx_stream_record_status(m_stream, &is_recording, &fn, &err);
        filename = fn ? fn : "";
        error = err ? err : "";
        return ok;
    }

    bool record_set_audio_level(float level_db) {
        return oavx_stream_record_set_audio_level(m_stream, level_db);
    }

    /* Endpoint & Connection Control */
    void connect(const oavx_endpoint_t& endpoint) {
        oavx_stream_connect(m_stream, &endpoint);
    }

    const oavx_endpoint_t* get_endpoint() const {
        return oavx_stream_get_endpoint(m_stream);
    }

    int get_connection_count(uint32_t timeout_ms = 0) const {
        return oavx_stream_get_connection_count(m_stream, timeout_ms);
    }

    bool get_performance(oavx_performance_t& total, oavx_performance_t& dropped) const {
        return oavx_stream_get_performance(m_stream, &total, &dropped);
    }

    bool get_queue(oavx_queue_t& queue) const {
        return oavx_stream_get_queue(m_stream, &queue);
    }

    bool get_telemetry(oavx_telemetry_t& telemetry) const {
        return oavx_stream_get_telemetry(m_stream, &telemetry);
    }

    std::string get_telemetry_json() const {
        oavx_telemetry_t telem = {};
        if (!oavx_stream_get_telemetry(m_stream, &telem)) {
            return "{}";
        }
        char buf[2048];
        oavx_telemetry_to_json(&telem, buf, sizeof(buf));
        return std::string(buf);
    }

    const char* get_web_control() const {
        return oavx_stream_get_web_control(m_stream);
    }

    void clear_connection_metadata() {
        oavx_stream_clear_connection_metadata(m_stream);
    }

    void add_connection_metadata(const oavx_metadata_frame_t& meta) {
        oavx_stream_add_connection_metadata(m_stream, &meta);
    }

    void set_failover(const oavx_endpoint_t& failover_endpoint) {
        oavx_stream_set_failover(m_stream, &failover_endpoint);
    }

    bool is_on_failover() const {
        return oavx_stream_is_on_failover(m_stream);
    }

    void set_failover_timeout(uint32_t timeout_ms) {
        oavx_stream_set_failover_timeout(m_stream, timeout_ms);
    }

    void set_auto_revert(bool auto_revert, uint32_t hold_time_ms = 0) {
        oavx_stream_set_auto_revert(m_stream, auto_revert, hold_time_ms);
    }

    oavx_frame_type_t capture_metadata(oavx_metadata_frame_t& meta, uint32_t timeout_ms = 0) {
        return oavx_stream_capture_metadata(m_stream, &meta, timeout_ms);
    }

    int get_fd() const {
        return oavx_stream_get_fd(m_stream);
    }

private:
    oavx_stream_t* m_stream;
};

/**
 * RAII Source Discovery Grid
 */
class Discovery {
public:
    Discovery() : m_disc(nullptr) {}

    Discovery(Context& ctx, const std::string& groups = "") {
        m_disc = oavx_discovery_create(ctx.get(), groups.empty() ? nullptr : groups.c_str());
    }

    ~Discovery() {
        if (m_disc) {
            oavx_discovery_destroy(m_disc);
            m_disc = nullptr;
        }
    }

    bool is_valid() const { return m_disc != nullptr; }
    explicit operator bool() const { return is_valid(); }
    oavx_discovery_t* get() const { return m_disc; }

    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    Discovery(Discovery&& other) noexcept : m_disc(other.m_disc) {
        other.m_disc = nullptr;
    }

    Discovery& operator=(Discovery&& other) noexcept {
        if (this != &other) {
            if (m_disc) {
                oavx_discovery_destroy(m_disc);
            }
            m_disc = other.m_disc;
            other.m_disc = nullptr;
        }
        return *this;
    }

    std::vector<oavx_endpoint_t> get_sources(size_t max_count = 64) const {
        std::vector<oavx_endpoint_t> vec(max_count);
        size_t count = oavx_discovery_get_sources(m_disc, vec.data(), max_count);
        vec.resize(count);
        return vec;
    }

    bool wait(uint32_t timeout_ms) {
        return oavx_discovery_wait(m_disc, timeout_ms);
    }

    int get_fd() const {
        return oavx_discovery_get_fd(m_disc);
    }

private:
    oavx_discovery_t* m_disc;
};

/**
 * RAII Dynamic Matrix Router
 */
class Router {
public:
    Router() : m_router(nullptr) {}

    Router(Context& ctx, const std::string& name, const std::string& groups = "") {
        m_router = oavx_router_create(ctx.get(), name.c_str(), groups.empty() ? nullptr : groups.c_str());
    }

    ~Router() {
        if (m_router) {
            oavx_router_destroy(m_router);
            m_router = nullptr;
        }
    }

    bool is_valid() const { return m_router != nullptr; }
    explicit operator bool() const { return is_valid(); }
    oavx_router_t* get() const { return m_router; }

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    Router(Router&& other) noexcept : m_router(other.m_router) {
        other.m_router = nullptr;
    }

    Router& operator=(Router&& other) noexcept {
        if (this != &other) {
            if (m_router) {
                oavx_router_destroy(m_router);
            }
            m_router = other.m_router;
            other.m_router = nullptr;
        }
        return *this;
    }

    /**
     * Switches routed source dynamically without disconnecting downstream clients.
     */
    bool route(const oavx_endpoint_t& source) {
        return oavx_router_route(m_router, &source);
    }

    bool clear() {
        return oavx_router_clear(m_router);
    }

    oavx_endpoint_t get_source() const {
        return oavx_router_get_source(m_router);
    }

private:
    oavx_router_t* m_router;
};

/**
 * RAII Frame Synchronizer & Clock Locker
 */
class FrameSync {
public:
    FrameSync() : m_fs(nullptr) {}

    explicit FrameSync(Stream& stream) {
        m_fs = oavx_framesync_create(stream.get());
    }

    ~FrameSync() {
        if (m_fs) {
            oavx_framesync_destroy(m_fs);
            m_fs = nullptr;
        }
    }

    bool is_valid() const { return m_fs != nullptr; }
    explicit operator bool() const { return is_valid(); }
    oavx_framesync_t* get() const { return m_fs; }

    FrameSync(const FrameSync&) = delete;
    FrameSync& operator=(const FrameSync&) = delete;

    FrameSync(FrameSync&& other) noexcept : m_fs(other.m_fs) {
        other.m_fs = nullptr;
    }

    FrameSync& operator=(FrameSync&& other) noexcept {
        if (this != &other) {
            if (m_fs) {
                oavx_framesync_destroy(m_fs);
            }
            m_fs = other.m_fs;
            other.m_fs = nullptr;
        }
        return *this;
    }

    /**
     * Pulls latest synchronized video frame. Must call free_video() when finished.
     */
    void capture_video(oavx_video_frame_t& frame, int field_type = 0) {
        oavx_framesync_capture_video(m_fs, &frame, field_type);
    }

    void free_video(oavx_video_frame_t& frame) {
        oavx_framesync_free_video(m_fs, &frame);
    }

    void capture_audio(oavx_audio_frame_t& frame, uint32_t sample_rate, uint16_t channels, uint32_t num_samples) {
        oavx_framesync_capture_audio(m_fs, &frame, sample_rate, channels, num_samples);
    }

    void free_audio(oavx_audio_frame_t& frame) {
        oavx_framesync_free_audio(m_fs, &frame);
    }

    int audio_queue_depth() const {
        return oavx_framesync_audio_queue_depth(m_fs);
    }

private:
    oavx_framesync_t* m_fs;
};

/* Unified Media Utilities */
inline void audio_to_interleaved_16s(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_16s_t* dst) {
    oavx_util_audio_to_interleaved_16s(src, dst);
}
inline void audio_from_interleaved_16s(const oavx_audio_frame_interleaved_16s_t* src, oavx_audio_frame_t* dst) {
    oavx_util_audio_from_interleaved_16s(src, dst);
}
inline void audio_to_interleaved_32f(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_32f_t* dst) {
    oavx_util_audio_to_interleaved_32f(src, dst);
}
inline void audio_from_interleaved_32f(const oavx_audio_frame_interleaved_32f_t* src, oavx_audio_frame_t* dst) {
    oavx_util_audio_from_interleaved_32f(src, dst);
}
inline void audio_to_interleaved_32s(const oavx_audio_frame_t* src, oavx_audio_frame_interleaved_32s_t* dst) {
    oavx_util_audio_to_interleaved_32s(src, dst);
}
inline void audio_from_interleaved_32s(const oavx_audio_frame_interleaved_32s_t* src, oavx_audio_frame_t* dst) {
    oavx_util_audio_from_interleaved_32s(src, dst);
}
inline void V210_to_P216(const oavx_video_frame_t* src, oavx_video_frame_t* dst) {
    oavx_util_V210_to_P216(src, dst);
}
inline void P216_to_V210(const oavx_video_frame_t* src, oavx_video_frame_t* dst) {
    oavx_util_P216_to_V210(src, dst);
}

/* SpeedHQ Dynamic Quantization & Bitrate Control */
inline void speedhq_set_quality(uint8_t quality) {
    oavx_speedhq_set_quality(quality);
}

inline uint8_t speedhq_get_quality() {
    return oavx_speedhq_get_quality();
}

} // namespace oavx

#endif /* OPENAVX_HPP */
