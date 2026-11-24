/*
 * SPDX-License-Identifier: AGPL-3.0-or-later WITH Classpath-exception-2.0
 * Copyright (c) 2026 Saif Mukhtar
 *
 * OpenAVX - Open Audio-Video eXchange Library
 * test/openavx_cpp_test.cpp - Modern C++11 RAII Verification Suite (namespace oavx)
 */

#include "openavx.hpp"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>
#include <chrono>
#include <thread>

static void test_context() {
    std::cout << "[*] 1. Testing oavx::Context RAII and version info..." << std::endl;
    const char* ver = oavx::version();
    assert(ver != nullptr);
    assert(std::strlen(ver) > 0);
    std::cout << "    Version: " << ver << std::endl;

    assert(oavx::is_supported_cpu());
    std::cout << "    CPU Supported: true" << std::endl;

    {
        oavx::Context ctx;
        assert(ctx.is_valid());
        assert((bool)ctx);

        // Move semantics
        oavx::Context moved_ctx = std::move(ctx);
        assert(!ctx.is_valid());
        assert(moved_ctx.is_valid());

        // Wire profile configuration
        oavx_wire_profile_t prof = {};
        moved_ctx.get_wire_profile(prof);
        assert(prof.base_port != 0);
        prof.base_port = 7980;
        moved_ctx.set_wire_profile(prof);
        oavx_wire_profile_t updated_prof = {};
        moved_ctx.get_wire_profile(updated_prof);
        assert(updated_prof.base_port == 7980);
    }
    std::cout << "    [+] Context RAII passed." << std::endl;
}

static void test_stream_tx_rx() {
    std::cout << "[*] 2. Testing oavx::Stream RAII (Transmit & Receive)..." << std::endl;
    oavx::Context ctx;
    assert(ctx.is_valid());

    // 1. Transmit Stream
    oavx::Stream tx = oavx::Stream::create_transmitter(ctx, "CPP_STREAM_FEED");
    assert(tx.is_valid());

    // Move semantics on stream
    oavx::Stream moved_tx = std::move(tx);
    assert(!tx.is_valid());
    assert(moved_tx.is_valid());

    // 1b. Audio Compression Control
    bool comp_ok = moved_tx.set_audio_compression(OAVX_AUDIO_COMPRESSION_MANUAL, 128);
    assert(comp_ok);
    assert(moved_tx.set_audio_compression(OAVX_AUDIO_COMPRESSION_ADAPTIVE, 96));
    assert(moved_tx.set_audio_compression(OAVX_AUDIO_COMPRESSION_OFF, 0));

    // 1c. Embedded HTTP/fMP4 Web Preview
    bool web_ok = moved_tx.enable_web_preview(0);
    assert(web_ok);
    uint16_t web_port = moved_tx.get_web_preview_port();
    assert(web_port > 0);
    std::string web_url = moved_tx.get_web_preview_url();
    assert(web_url.find("http://") != std::string::npos);
    assert(moved_tx.get_web_client_count() == 0);
    moved_tx.disable_web_preview();
    assert(moved_tx.get_web_preview_port() == 0);

    // 1d. Native RTSP 1.0 Server
    bool rtsp_ok = moved_tx.enable_rtsp_server(0, "/live");
    assert(rtsp_ok);
    uint16_t rtsp_port = moved_tx.get_rtsp_port();
    assert(rtsp_port > 0);
    assert(moved_tx.get_rtsp_client_count() == 0);
    moved_tx.disable_rtsp_server();
    assert(moved_tx.get_rtsp_port() == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 2. Receive Stream
    oavx::Stream rx = oavx::Stream::create_receiver(ctx, "CPP_STREAM_FEED", "127.0.0.1:7960");
    assert(rx.is_valid());

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 3. Send video frame
    const uint32_t width = 640;
    const uint32_t height = 360;
    std::vector<uint8_t> vbuf(width * height * 2, 128); // UYVY
    oavx_video_frame_t vf = {};
    vf.width = width;
    vf.height = height;
    vf.fourcc = OAVX_FOURCC_VIDEO_TYPE_UYVY;
    vf.framerate.num = 60000;
    vf.framerate.den = 1000;
    vf.aspect_ratio.num = 16;
    vf.aspect_ratio.den = 9;
    vf.scan_type = OAVX_SCAN_PROGRESSIVE;
    vf.planes[0] = vbuf.data();
    vf.strides[0] = width * 2;

    int v_sent = moved_tx.send_video(vf);
    assert(v_sent >= 0);

    // 4. Send audio frame
    const uint32_t sample_rate = 48000;
    const uint16_t channels = 2;
    const uint32_t samples = 1024;
    std::vector<float> abuf(channels * samples, 0.5f);
    oavx_audio_frame_t af = {};
    af.sample_rate = sample_rate;
    af.channels = channels;
    af.frames = samples;
    af.format = OAVX_AUDIO_FMT_FLT_PLANAR;
    af.planes[0] = abuf.data();
    af.planes[1] = abuf.data() + samples;
    af.stride_bytes = samples * sizeof(float);

    int a_sent = moved_tx.send_audio(af);
    assert(a_sent >= 0);

    // 5. Send metadata frame
    oavx_metadata_frame_t mf = {};
    char meta_str[] = "<oavx_test>stream_payload</oavx_test>";
    mf.data = meta_str;
    mf.length = (int)std::strlen(meta_str);

    int m_sent = moved_tx.send_metadata(mf);
    assert(m_sent >= 0);

    // 6. Receive frames
    oavx_video_frame_t rx_v = {};
    oavx_audio_frame_t rx_a = {};
    oavx_metadata_frame_t rx_m = {};
    oavx_frame_type_t ft = rx.receive(&rx_v, &rx_a, &rx_m, 200);
    if (ft == OAVX_FRAME_TYPE_VIDEO) {
        rx.release_video(rx_v);
    } else if (ft == OAVX_FRAME_TYPE_AUDIO) {
        rx.release_audio(rx_a);
    } else if (ft == OAVX_FRAME_TYPE_METADATA) {
        rx.release_metadata(rx_m);
    }

    // 7. Test Tally
    oavx_tally_t tally = { OAVX_TALLY_PROGRAM };
    rx.set_tally(tally);

    oavx_tally_t tx_tally = {};
    moved_tx.get_tally(tx_tally, 10);

    // 8. Test PTZ controls
    oavx_ptz_state_t ptz = { 0.5f, -0.2f, 1.5f, 0.8f };
    rx.set_ptz(ptz);

    // 9. Test Record triggers
    rx.record_trigger("start", "test_rec.mp4");
    rx.record_set_audio_level(-6.0f);
    rx.record_trigger("stop");

    // 10. Performance & Queue inspection
    oavx_performance_t perf_total = {};
    oavx_performance_t perf_dropped = {};
    rx.get_performance(perf_total, perf_dropped);

    oavx_queue_t q = {};
    rx.get_queue(q);

    // 11. Full Stream Observability Telemetry inspection
    oavx::Telemetry telem = {};
    bool telem_ok = rx.get_telemetry(telem);
    assert(telem_ok);
    std::string json = rx.get_telemetry_json();
    assert(!json.empty());
    assert(json.find("\"connected\":") != std::string::npos);

    // 12. Failover and Metadata capture controls
    oavx_endpoint_t fo_ep = {};
    fo_ep.name = "BACKUP_FEED";
    fo_ep.address = "127.0.0.1:7961";
    rx.set_failover(fo_ep);
    assert(!rx.is_on_failover());
    rx.set_failover_timeout(1000);
    rx.set_auto_revert(true, 500);

    oavx_metadata_frame_t cap_meta = {};
    rx.capture_metadata(cap_meta, 10);

    std::cout << "    [+] Stream transmit, receive and telemetry passed." << std::endl;
}

static void test_discovery() {
    std::cout << "[*] 3. Testing oavx::Discovery RAII..." << std::endl;
    oavx::Context ctx;
    assert(ctx.is_valid());

    {
        oavx::Discovery disc(ctx, "Public");
        assert(disc.is_valid());

        // Move semantics
        oavx::Discovery moved_disc = std::move(disc);
        assert(!disc.is_valid());
        assert(moved_disc.is_valid());

        moved_disc.wait(50);
        std::vector<oavx_endpoint_t> sources = moved_disc.get_sources(32);
        std::cout << "    Discovered sources count: " << sources.size() << std::endl;
    }
    std::cout << "    [+] Discovery RAII passed." << std::endl;
}

static void test_router() {
    std::cout << "[*] 4. Testing oavx::Router RAII..." << std::endl;
    oavx::Context ctx;
    assert(ctx.is_valid());

    {
        oavx::Router router(ctx, "OAVX_MATRIX_ROUTER");
        assert(router.is_valid());

        // Move semantics
        oavx::Router moved_router = std::move(router);
        assert(!router.is_valid());
        assert(moved_router.is_valid());

        oavx_endpoint_t target = {};
        target.name = "STUDIO_CAM_1";
        target.address = "127.0.0.1:7960";
        bool routed = moved_router.route(target);
        assert(routed);

        oavx_endpoint_t cur = moved_router.get_source();
        assert(cur.name != nullptr);

        bool cleared = moved_router.clear();
        assert(cleared);
    }
    std::cout << "    [+] Router RAII passed." << std::endl;
}

static void test_framesync() {
    std::cout << "[*] 5. Testing oavx::FrameSync RAII..." << std::endl;
    oavx::Context ctx;
    assert(ctx.is_valid());

    oavx::Stream rx = oavx::Stream::create_receiver(ctx, "FS_TEST_FEED", "127.0.0.1:7960");
    assert(rx.is_valid());

    {
        oavx::FrameSync fs(rx);
        assert(fs.is_valid());

        // Move semantics
        oavx::FrameSync moved_fs = std::move(fs);
        assert(!fs.is_valid());
        assert(moved_fs.is_valid());

        oavx_video_frame_t vf = {};
        moved_fs.capture_video(vf, 0);
        moved_fs.free_video(vf);

        oavx_audio_frame_t af = {};
        moved_fs.capture_audio(af, 48000, 2, 1024);
        moved_fs.free_audio(af);

        int q_depth = moved_fs.audio_queue_depth();
        assert(q_depth >= 0);
    }
    std::cout << "    [+] FrameSync RAII passed." << std::endl;
}

static void test_utilities() {
    std::cout << "[*] 6. Testing oavx utilities..." << std::endl;

    const uint32_t samples = 256;
    const uint16_t channels = 2;
    std::vector<float> planar(channels * samples, 0.25f);
    oavx_audio_frame_t src_audio = {};
    src_audio.sample_rate = 48000;
    src_audio.channels = channels;
    src_audio.frames = samples;
    src_audio.planes[0] = planar.data();
    src_audio.planes[1] = planar.data() + samples;

    std::vector<int16_t> interleaved_16(channels * samples, 0);
    oavx_audio_frame_interleaved_16s_t dst_16 = {};
    dst_16.data = interleaved_16.data();
    dst_16.reference_level = 0;

    oavx::audio_to_interleaved_16s(&src_audio, &dst_16);
    assert(dst_16.channels == channels);
    assert(dst_16.frames == samples);

    oavx_audio_frame_t recovered_audio = {};
    std::vector<float> rec_buf(channels * samples, 0.0f);
    recovered_audio.planes[0] = rec_buf.data();
    recovered_audio.planes[1] = rec_buf.data() + samples;

    oavx::audio_from_interleaved_16s(&dst_16, &recovered_audio);
    assert(recovered_audio.channels == channels);
    assert(recovered_audio.frames == samples);

    std::cout << "    [+] Audio interleaved utilities passed." << std::endl;

    // SpeedHQ quality configuration (clamped [20, 98])
    oavx::speedhq_set_quality(80);
    assert(oavx::speedhq_get_quality() == 80);
    oavx::speedhq_set_quality(98);
    assert(oavx::speedhq_get_quality() == 98);
    oavx::speedhq_set_quality(100);
    assert(oavx::speedhq_get_quality() == 98); // clamped to 98
    oavx::speedhq_set_quality(10);
    assert(oavx::speedhq_get_quality() == 20); // clamped to 20
    std::cout << "    [+] SpeedHQ dynamic quality passed." << std::endl;
}

int main() {
    std::cout << "=== OpenAVX C++11 Wrapper Tests ===\n\n";

    test_context();
    test_stream_tx_rx();
    test_discovery();
    test_router();
    test_framesync();
    test_utilities();

    std::cout << "\n[PASS] openavx_cpp_test\n";
    return 0;
}
