/** @file tests/unit/test_video_lifecycle_hardware.cpp
 * Opt-in isolated private-display capture/encoder acceptance. No streaming server.
 */
#include "tests/tests_common.h"
#include "src/video.h"
#include "src/process.h"
#include "src/stream_stats.h"
#if defined(__linux__) && defined(POLARIS_BUILD_CUDA)
#include "src/platform/linux/cage_display_router.h"
#include "src/platform/linux/cuda.h"
#include <fstream>
#include <future>
#include <numeric>
#include <map>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace {
  long resident_kib() {
    std::ifstream statm("/proc/self/statm");
    long pages {}, resident {};
    statm >> pages >> resident;
    return resident * sysconf(_SC_PAGESIZE) / 1024;
  }
  std::size_t descriptor_count() {
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator {});
  }
  uint64_t packet_checksum(video::packet_raw_t &packet) {
    uint64_t value = 1469598103934665603ull;
    for (size_t i = 0; i < packet.data_size(); ++i) value = (value ^ packet.data()[i]) * 1099511628211ull;
    return value;
  }
}

TEST(VideoHardwareLifecycleTests, PrivateCompositorReconnectsRatesAndQueuedPacketTeardown) {
  if (!getenv("POLARIS_TEST_PRIVATE_VIDEO")) GTEST_SKIP() << "Requires the isolated physical video harness";
  const auto *runtime = getenv("XDG_RUNTIME_DIR");
  ASSERT_NE(runtime, nullptr);
  ASSERT_TRUE(std::string_view(runtime).find("polaris-video-proof-") != std::string_view::npos);
  const auto saved = config::video;
  const auto saved_generation = proc::proc.capture_generation;
  const auto *render_device = getenv("POLARIS_TEST_RENDER_DEVICE");
  ASSERT_NE(render_device, nullptr);
  auto restore = util::fail_guard([&] {
    video::reset_encoder_probe_state();
    cage_display_router::stop();
    config::video = saved;
    proc::proc.capture_generation = saved_generation;
  });
  config::video.encoder = "nvenc";
  config::video.capture = "wlr";
  config::video.adapter_name = render_device;
  config::video.output_name.clear();
  config::video.hevc_mode = 1;
  config::video.av1_mode = 1;
  config::video.linux_display.stream_mode.clear();
  config::video.linux_display.private_runtime = "labwc";
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.prefer_gpu_native_capture = true;
  config::video.linux_display.auto_manage_displays = false;
  config::video.linux_display.capture_profile = false;
  config::video.limit_framerate = false;
  ASSERT_TRUE(cage_display_router::start(1280, 720, 60, "/usr/bin/vkcube --wsi wayland --width 1280 --height 720 --present_mode 2", false, false, "offline-video-proof"));
  proc::proc.capture_generation = {
    .generation_id = 1,
    .exact_display_name = "HEADLESS-1",
    .capture_backend = "wlr",
    .private_runtime = "labwc",
    .private_wayland_socket = cage_display_router::get_wayland_socket(),
    .private_runtime_instance_id = cage_display_router::get_session_instance_id(),
    .adapter_name = config::video.adapter_name,
    .headless_mode = true,
    .use_cage_compositor = true,
  };
  const auto supervisor_pid = cage_display_router::get_pid();
  const auto socket = std::filesystem::path(runtime) / cage_display_router::get_wayland_socket();
  auto platform = platf::init();
  ASSERT_NE(platform, nullptr);
  ASSERT_EQ(cuda::init(), 0);
  video::packet_t retained_packet;
  uint64_t retained_checksum {};
  std::optional<std::map<std::string, int>> warm_render_descriptors;
  for (int cycle = 0; cycle < 9; ++cycle) {
    // Exercise the existing contained fallback and a later successful retry.
    if (cycle == 7) cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
    if (cycle == 8) cage_display_router::update_headless_extcopy_dmabuf_probe_result(true);
    const int requested_rate = cycle == 5 ? 59940 : 60;
    const int display_rate = cycle == 6 ? 120 : 60;
    ASSERT_TRUE(cage_display_router::ensure_output_refresh(display_rate, false));
    const auto probe_start = std::chrono::steady_clock::now();
    ASSERT_EQ(video::probe_encoders(true, false), 0);
    const double probe_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - probe_start).count();
    video::config_t cfg {1280, 720, 60, 10000, 1, 1, 2, 0, 0, 0};
    // Match RTSP's existing internal millihertz contract in both versions.
    cfg.encodingFramerate = requested_rate <= 4000 ? requested_rate * 1000 : requested_rate;
#if __has_include("src/video_rate.h")
    cfg.stream_rate = video::rate::from_wire(requested_rate, display_rate * 100);
    cfg.encode_rate = cfg.stream_rate;
#endif
    auto mailbox = std::make_shared<safe::mail_raw_t>();
    auto shutdown = mailbox->event<bool>(mail::shutdown);
    auto packets = mailbox->queue<video::packet_t>(mail::video_packets);
    stream_stats::update_capture_metadata({});
    if (cycle == 0) shutdown->raise(true);
    const auto start = std::chrono::steady_clock::now();
    auto captured = std::async(std::launch::async, [&] { video::capture(mailbox, cfg, nullptr, packets); });
    auto stop_capture = util::fail_guard([&] {
      shutdown->raise(true);
      if (captured.valid()) captured.wait();
    });
    int frames = 0;
    double latency_sum = 0;
    int latency_samples = 0;
    std::optional<std::chrono::steady_clock::time_point> first, last;
    while (cycle != 0 && frames < 121 && std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
      auto packet = packets->pop(std::chrono::milliseconds(100));
      if (!packet) {
        if (captured.wait_for(std::chrono::seconds(0)) == std::future_status::ready) break;
        continue;
      }
      ++frames;
      const auto now = std::chrono::steady_clock::now();
      if (!first) first = now;
      last = now;
      if (packet->frame_timestamp && packet->encode_done_timestamp) {
        latency_sum += std::chrono::duration<double, std::milli>(*packet->encode_done_timestamp - *packet->frame_timestamp).count();
        ++latency_samples;
      }
      if (!retained_packet) {
        retained_packet = std::move(packet);
        retained_checksum = packet_checksum(*retained_packet);
      }
    }
    shutdown->raise(true);
    const auto stop_start = std::chrono::steady_clock::now();
    EXPECT_EQ(captured.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    captured.get();
    stop_capture.disable();
    const double stop_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stop_start).count();
    EXPECT_EQ(frames, cycle == 0 ? 0 : 121);
    const double fps = first && last && *last > *first ? (frames - 1) / std::chrono::duration<double>(*last - *first).count() : 0;
    if (cycle != 0) {
      EXPECT_GT(fps, 45);
      EXPECT_LT(fps, 75); // A 120 Hz output must still encode the 60 FPS request.
    }
    const auto transport = stream_stats::get_current().capture_transport;
    if (cycle != 0) {
      EXPECT_EQ(transport, cycle == 7 ? platf::frame_transport_e::shm : platf::frame_transport_e::dmabuf);
    }
    std::map<std::string, int> device_descriptors;
    std::map<std::string, int> render_descriptors;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      std::error_code ec;
      const auto target = std::filesystem::read_symlink(entry.path(), ec).string();
      if (!ec && (target.starts_with("/dev/") || target.starts_with("anon_inode:"))) ++device_descriptors[target];
      if (!ec && target.starts_with("/dev/dri/renderD")) ++render_descriptors[target];
    }
    std::cout << "VIDEO_DEVICE_DESCRIPTORS cycle=" << cycle << ' ' << nlohmann::json(device_descriptors).dump() << '\n';
    EXPECT_FALSE(render_descriptors.empty());
    if (cycle == 1) warm_render_descriptors = render_descriptors;
    if (cycle > 1) {
      EXPECT_EQ(render_descriptors, *warm_render_descriptors)
        << "Repeated capture must release each GBM device's borrowed render descriptor";
    }
    std::cout << "VIDEO_LIFECYCLE_RESULT cycle=" << cycle << " requested=" << requested_rate
      << " display=" << display_rate << " frames=" << frames << " probe_ms=" << probe_ms
      << " transport=" << platf::from_frame_transport(transport)
      << " fps=" << fps << " capture_to_encode_ms=" << (latency_samples ? latency_sum / latency_samples : 0)
      << " stop_ms=" << stop_ms << " rss_kib=" << resident_kib() << " fds=" << descriptor_count() << '\n';
  }
  video::reset_encoder_probe_state();
  platform.reset();
  cage_display_router::stop();
  EXPECT_FALSE(cage_display_router::is_running());
  EXPECT_FALSE(std::filesystem::exists(socket));
  errno = 0;
  EXPECT_EQ(kill(supervisor_pid, 0), -1);
  EXPECT_EQ(errno, ESRCH);
  ASSERT_NE(retained_packet, nullptr);
  EXPECT_EQ(packet_checksum(*retained_packet), retained_checksum);
  retained_packet.reset();
}
#endif
