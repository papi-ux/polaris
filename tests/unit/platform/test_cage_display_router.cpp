/**
 * @file tests/unit/platform/test_cage_display_router.cpp
 * @brief Test Linux labwc runtime policy helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/config.h>
  #include <src/platform/linux/cage_display_router.h>
  #include <src/platform/linux/wayland.h>

  #include <cerrno>
  #include <csignal>
  #include <drm_fourcc.h>
  #include <filesystem>
  #include <fstream>
  #include <sstream>
  #include <sys/stat.h>
  #include <unistd.h>

namespace {
  bool wait_for_process_exit(pid_t pid, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (kill(pid, 0) != 0 && errno == ESRCH) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return kill(pid, 0) != 0 && errno == ESRCH;
  }

  pid_t wait_for_pid_file(const std::filesystem::path &path) {
    for (int attempt = 0; attempt < 80; ++attempt) {
      std::ifstream in(path);
      pid_t pid = -1;
      if (in >> pid && pid > 0) {
        return pid;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return -1;
  }

  bool wait_for_router_exit(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!cage_display_router::is_running()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return !cage_display_router::is_running();
  }
}

TEST(WaylandOutputRegistryStateTests, OutputHotplugMarksTopologyDirtyUntilCleared) {
  wl::output_registry_state_t state;

  state.add_output(10);
  EXPECT_TRUE(state.output_topology_dirty());

  state.clear_output_topology_dirty();
  EXPECT_FALSE(state.output_topology_dirty());

  state.remove_global(10);
  EXPECT_TRUE(state.output_topology_dirty());
  EXPECT_FALSE(state.has_output(10));
}

TEST(WaylandOutputRegistryStateTests, RemovingNonOutputGlobalDoesNotDirtyOutputTopology) {
  wl::output_registry_state_t state;

  state.add_output(10);
  state.clear_output_topology_dirty();

  state.remove_global(99);

  EXPECT_FALSE(state.output_topology_dirty());
  EXPECT_TRUE(state.has_output(10));
}

#ifdef POLARIS_TESTS
TEST(CageDisplayRouterLifecycleTests, ExternalExitDrainsPrivateChildrenAcrossRelaunch) {
  namespace fs = std::filesystem;

  struct cleanup_t {
    config::video_t::linux_display_t linux_display;
    std::string original_path;
    std::string original_home;
    std::string original_runtime_dir;
    std::string original_pid_file;
    std::string original_compositor_pid_file;
    std::string original_escape_child;
    bool had_path;
    bool had_home;
    bool had_runtime_dir;
    bool had_pid_file;
    bool had_compositor_pid_file;
    bool had_escape_child;
    fs::path root;
    std::vector<pid_t> cleanup_pids;

    ~cleanup_t() {
      cage_display_router::force_cage_pidfd_open_failure_for_tests(false);
      cage_display_router::stop();
      for (const auto process_pid : cleanup_pids) {
        if (process_pid > 0 && kill(process_pid, 0) == 0) {
          (void) kill(process_pid, SIGKILL);
        }
      }
      config::video.linux_display = linux_display;
      auto restore_env = [](const char *name, const std::string &value, bool had_value) {
        if (had_value) {
          (void) setenv(name, value.c_str(), 1);
        } else {
          (void) unsetenv(name);
        }
      };
      restore_env("PATH", original_path, had_path);
      restore_env("HOME", original_home, had_home);
      restore_env("XDG_RUNTIME_DIR", original_runtime_dir, had_runtime_dir);
      restore_env("FAKE_LABWC_CHILD_PID_FILE", original_pid_file, had_pid_file);
      restore_env("FAKE_LABWC_COMPOSITOR_PID_FILE", original_compositor_pid_file, had_compositor_pid_file);
      restore_env("FAKE_LABWC_ESCAPE_CHILD", original_escape_child, had_escape_child);
      std::error_code ec;
      fs::remove_all(root, ec);
    }
  };

  const auto *path_env = std::getenv("PATH");
  const auto *home_env = std::getenv("HOME");
  const auto *runtime_env = std::getenv("XDG_RUNTIME_DIR");
  const auto *pid_file_env = std::getenv("FAKE_LABWC_CHILD_PID_FILE");
  const auto *compositor_pid_file_env = std::getenv("FAKE_LABWC_COMPOSITOR_PID_FILE");
  const auto *escape_child_env = std::getenv("FAKE_LABWC_ESCAPE_CHILD");
  cleanup_t cleanup {
    .linux_display = config::video.linux_display,
    .original_path = path_env ? path_env : "",
    .original_home = home_env ? home_env : "",
    .original_runtime_dir = runtime_env ? runtime_env : "",
    .original_pid_file = pid_file_env ? pid_file_env : "",
    .original_compositor_pid_file = compositor_pid_file_env ? compositor_pid_file_env : "",
    .original_escape_child = escape_child_env ? escape_child_env : "",
    .had_path = path_env != nullptr,
    .had_home = home_env != nullptr,
    .had_runtime_dir = runtime_env != nullptr,
    .had_pid_file = pid_file_env != nullptr,
    .had_compositor_pid_file = compositor_pid_file_env != nullptr,
    .had_escape_child = escape_child_env != nullptr,
    .root = fs::temp_directory_path() / ("polaris-cage-external-exit-" + std::to_string(getpid())),
  };
  auto remove_cleanup_pid = [&](pid_t pid) {
    const auto position = std::find(cleanup.cleanup_pids.begin(), cleanup.cleanup_pids.end(), pid);
    if (position != cleanup.cleanup_pids.end()) {
      cleanup.cleanup_pids.erase(position);
    }
  };

  const auto bin_dir = cleanup.root / "bin";
  const auto runtime_dir = cleanup.root / "runtime";
  const auto pid_file = cleanup.root / "worker.pid";
  const auto compositor_pid_file = cleanup.root / "compositor.pid";
  ASSERT_TRUE(fs::create_directories(bin_dir));
  ASSERT_TRUE(fs::create_directories(runtime_dir));

  const auto fake_labwc = bin_dir / "labwc";
  {
    std::ofstream script(fake_labwc);
    ASSERT_TRUE(script.good());
    script << R"PY(#!/usr/bin/python3
import os
import signal
import socket
import subprocess
import sys

if len(sys.argv) > 1 and sys.argv[1] == "-V":
    print("fake labwc wlroots headless backend")
    raise SystemExit(0)

socket_path = os.path.join(os.environ["XDG_RUNTIME_DIR"], f"wayland-{(os.getpid() % 10000) + 100}")
wayland_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
wayland_socket.bind(socket_path)
escape_child = os.environ.get("FAKE_LABWC_ESCAPE_CHILD") == "1"
worker_command = ["sleep", "60"]
if escape_child:
    worker_command = [
        sys.executable,
        "-c",
        "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(60)",
    ]
worker = subprocess.Popen(worker_command, start_new_session=escape_child)
with open(os.environ["FAKE_LABWC_COMPOSITOR_PID_FILE"], "w", encoding="utf-8") as out:
    out.write(f"{os.getpid()}\n")
with open(os.environ["FAKE_LABWC_CHILD_PID_FILE"], "w", encoding="utf-8") as out:
    out.write(f"{worker.pid}\n")

def stop(_signum, _frame):
    raise SystemExit(0)

signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGHUP, stop)
while True:
    signal.pause()
)PY";
  }
  ASSERT_EQ(chmod(fake_labwc.c_str(), 0755), 0);

  const auto fake_wlr_randr = bin_dir / "wlr-randr";
  {
    std::ofstream script(fake_wlr_randr);
    ASSERT_TRUE(script.good());
    script << R"SH(#!/bin/sh
if [ "$#" -eq 0 ]; then
  cat <<'EOF'
HEADLESS-1 "Fake headless output"
  Enabled: yes
  Modes:
    1280x720 px, 60.000000 Hz (current)
WL-1 "Fake windowed output"
  Enabled: yes
  Modes:
    1280x720 px, 60.000000 Hz (current)
EOF
fi
exit 0
)SH";
  }
  ASSERT_EQ(chmod(fake_wlr_randr.c_str(), 0755), 0);

  config::video.linux_display.stream_mode.clear();
  config::video.linux_display.private_runtime = "labwc";
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.prefer_gpu_native_capture = false;
  config::video.linux_display.capture_profile = false;

  const auto test_path = bin_dir.string() + ":" + cleanup.original_path;
  ASSERT_EQ(setenv("PATH", test_path.c_str(), 1), 0);
  ASSERT_EQ(setenv("HOME", cleanup.root.c_str(), 1), 0);
  ASSERT_EQ(setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1), 0);
  ASSERT_EQ(setenv("FAKE_LABWC_CHILD_PID_FILE", pid_file.c_str(), 1), 0);
  ASSERT_EQ(setenv("FAKE_LABWC_COMPOSITOR_PID_FILE", compositor_pid_file.c_str(), 1), 0);

  for (int cycle = 0; cycle < 2; ++cycle) {
    std::error_code ec;
    fs::remove(pid_file, ec);
    fs::remove(compositor_pid_file, ec);
    ASSERT_TRUE(cage_display_router::start(
      1280,
      720,
      60,
      "",
      false,
      false,
      "external-exit-cycle-" + std::to_string(cycle)
    ));

    const auto runtime_state = cage_display_router::runtime_state();
    EXPECT_TRUE(runtime_state.requested_headless);
    EXPECT_TRUE(runtime_state.effective_headless);
    EXPECT_FALSE(runtime_state.gpu_native_override_active);
    EXPECT_EQ(runtime_state.backend_name, "labwc");
    EXPECT_EQ(runtime_state.path_id, "headless_stream");

    const auto router_pid = cage_display_router::get_pid();
    ASSERT_GT(router_pid, 0);
    const auto compositor_pid = wait_for_pid_file(compositor_pid_file);
    ASSERT_GT(compositor_pid, 0);
    EXPECT_NE(router_pid, compositor_pid);
    EXPECT_EQ(getpgid(router_pid), router_pid);
    EXPECT_EQ(getpgid(compositor_pid), router_pid);
    EXPECT_EQ(getsid(compositor_pid), router_pid);
    EXPECT_NE(getpgid(compositor_pid), getpgrp());
    const auto worker = wait_for_pid_file(pid_file);
    ASSERT_GT(worker, 0);
    cleanup.cleanup_pids.push_back(worker);

    ASSERT_EQ(kill(compositor_pid, SIGTERM), 0);
    ASSERT_TRUE(wait_for_router_exit(std::chrono::seconds(3)));
    cage_display_router::stop();

    const bool worker_exited = wait_for_process_exit(worker, std::chrono::seconds(2));
    EXPECT_TRUE(worker_exited)
      << "cycle " << cycle << " leaked private worker pid " << worker;
    if (worker_exited) {
      remove_cleanup_pid(worker);
    }
  }

  // labwc may launch its startup client into a separate session. The
  // supervisor is a child subreaper, so that client becomes its direct child
  // when labwc exits even though it is outside the anchored process group.
  // Teardown must drain that exact adopted descendant before the supervisor
  // exits; otherwise the client is reparented to systemd and survives.
  ASSERT_EQ(setenv("FAKE_LABWC_ESCAPE_CHILD", "1", 1), 0);
  std::error_code escaped_ec;
  fs::remove(pid_file, escaped_ec);
  fs::remove(compositor_pid_file, escaped_ec);
  ASSERT_TRUE(cage_display_router::start(
    1280,
    720,
    60,
    "",
    false,
    false,
    "escaped-startup-client"
  ));
  const auto escaped_supervisor_pid = cage_display_router::get_pid();
  ASSERT_GT(escaped_supervisor_pid, 0);
  const auto escaped_compositor_pid = wait_for_pid_file(compositor_pid_file);
  ASSERT_GT(escaped_compositor_pid, 0);
  const auto escaped_worker_pid = wait_for_pid_file(pid_file);
  ASSERT_GT(escaped_worker_pid, 0);
  cleanup.cleanup_pids.push_back(escaped_worker_pid);
  EXPECT_EQ(getpgid(escaped_worker_pid), escaped_worker_pid);
  EXPECT_EQ(getsid(escaped_worker_pid), escaped_worker_pid);
  EXPECT_NE(getpgid(escaped_worker_pid), escaped_supervisor_pid);

  ASSERT_EQ(kill(escaped_compositor_pid, SIGTERM), 0);
  ASSERT_TRUE(wait_for_router_exit(std::chrono::seconds(3)));
  cage_display_router::stop();
  const bool escaped_worker_exited = wait_for_process_exit(escaped_worker_pid, std::chrono::seconds(2));
  EXPECT_TRUE(escaped_worker_exited)
    << "external labwc exit leaked separate-session startup client pid " << escaped_worker_pid;
  if (escaped_worker_exited) {
    remove_cleanup_pid(escaped_worker_pid);
  }
  ASSERT_EQ(unsetenv("FAKE_LABWC_ESCAPE_CHILD"), 0);

  // A compositor that cannot honor SIGTERM must still be drained by the
  // supervisor's bounded escalation, not abandoned when stop() gives up.
  std::error_code ec;
  fs::remove(pid_file, ec);
  fs::remove(compositor_pid_file, ec);
  ASSERT_TRUE(cage_display_router::start(
    1280,
    720,
    60,
    "",
    true,
    false,
    "explicit-stop-escalation"
  ));
  const auto forced_windowed_runtime_state = cage_display_router::runtime_state();
  EXPECT_TRUE(forced_windowed_runtime_state.requested_headless);
  EXPECT_FALSE(forced_windowed_runtime_state.effective_headless);
  EXPECT_TRUE(forced_windowed_runtime_state.gpu_native_override_active);
  EXPECT_EQ(forced_windowed_runtime_state.backend_name, "labwc");
  EXPECT_EQ(forced_windowed_runtime_state.path_id, "windowed_stream");
  const auto stopped_compositor_pid = wait_for_pid_file(compositor_pid_file);
  ASSERT_GT(stopped_compositor_pid, 0);
  const auto stopped_worker_pid = wait_for_pid_file(pid_file);
  ASSERT_GT(stopped_worker_pid, 0);
  cleanup.cleanup_pids.push_back(stopped_worker_pid);

  ASSERT_EQ(kill(stopped_compositor_pid, SIGSTOP), 0);
  cage_display_router::stop();
  const bool stopped_worker_exited = wait_for_process_exit(stopped_worker_pid, std::chrono::seconds(2));
  EXPECT_TRUE(stopped_worker_exited)
    << "explicit stop leaked private worker pid " << stopped_worker_pid;
  if (stopped_worker_exited) {
    remove_cleanup_pid(stopped_worker_pid);
  }

  // If the supervisor itself is wedged, the parent fallback must still own an
  // immutable handle to the separate runtime group. Killing only the frozen
  // supervisor would strand both labwc and its placeholder.
  fs::remove(pid_file, ec);
  fs::remove(compositor_pid_file, ec);
  ASSERT_TRUE(cage_display_router::start(
    1280,
    720,
    60,
    "",
    false,
    false,
    "supervisor-hard-stop"
  ));
  const auto frozen_supervisor_pid = cage_display_router::get_pid();
  ASSERT_GT(frozen_supervisor_pid, 0);
  const auto frozen_compositor_pid = wait_for_pid_file(compositor_pid_file);
  ASSERT_GT(frozen_compositor_pid, 0);
  const auto frozen_worker_pid = wait_for_pid_file(pid_file);
  ASSERT_GT(frozen_worker_pid, 0);
  cleanup.cleanup_pids.push_back(frozen_compositor_pid);
  cleanup.cleanup_pids.push_back(frozen_worker_pid);

  ASSERT_EQ(kill(frozen_supervisor_pid, SIGSTOP), 0);
  cage_display_router::stop();
  const bool frozen_supervisor_exited = wait_for_process_exit(frozen_supervisor_pid, std::chrono::seconds(2));
  const bool frozen_worker_exited = wait_for_process_exit(frozen_worker_pid, std::chrono::seconds(2));
  const bool frozen_compositor_exited = wait_for_process_exit(frozen_compositor_pid, std::chrono::seconds(2));
  EXPECT_TRUE(frozen_supervisor_exited)
    << "hard supervisor stop did not reap supervisor pid " << frozen_supervisor_pid;
  EXPECT_TRUE(frozen_worker_exited)
    << "hard supervisor stop leaked private worker pid " << frozen_worker_pid;
  EXPECT_TRUE(frozen_compositor_exited)
    << "hard supervisor stop leaked compositor pid " << frozen_compositor_pid;
  if (frozen_worker_exited) {
    remove_cleanup_pid(frozen_worker_pid);
  }
  if (frozen_compositor_exited) {
    remove_cleanup_pid(frozen_compositor_pid);
  }

  // Exercise the older-kernel fallback where pidfd_open() is unavailable. The
  // supervisor exits first and is then reaped by stop(); after that successful
  // waitpid(), no raw liveness or group signal may consult its numeric PID.
  cage_display_router::force_cage_pidfd_open_failure_for_tests(true);
  fs::remove(pid_file, ec);
  fs::remove(compositor_pid_file, ec);
  ASSERT_TRUE(cage_display_router::start(
    1280,
    720,
    60,
    "",
    false,
    false,
    "pidfd-unavailable-external-exit"
  ));
  EXPECT_FALSE(cage_display_router::cage_pidfd_available_for_tests());
  const auto no_pidfd_supervisor_pid = cage_display_router::get_pid();
  ASSERT_GT(no_pidfd_supervisor_pid, 0);
  const auto no_pidfd_compositor_pid = wait_for_pid_file(compositor_pid_file);
  ASSERT_GT(no_pidfd_compositor_pid, 0);
  const auto no_pidfd_worker_pid = wait_for_pid_file(pid_file);
  ASSERT_GT(no_pidfd_worker_pid, 0);
  cleanup.cleanup_pids.push_back(no_pidfd_compositor_pid);
  cleanup.cleanup_pids.push_back(no_pidfd_worker_pid);

  ASSERT_EQ(kill(no_pidfd_compositor_pid, SIGTERM), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(750));
  cage_display_router::stop();
  cage_display_router::force_cage_pidfd_open_failure_for_tests(false);

  const bool no_pidfd_supervisor_exited = wait_for_process_exit(no_pidfd_supervisor_pid, std::chrono::seconds(2));
  const bool no_pidfd_compositor_exited = wait_for_process_exit(no_pidfd_compositor_pid, std::chrono::seconds(2));
  const bool no_pidfd_worker_exited = wait_for_process_exit(no_pidfd_worker_pid, std::chrono::seconds(2));
  EXPECT_TRUE(no_pidfd_supervisor_exited)
    << "pidfd-unavailable stop did not reap supervisor pid " << no_pidfd_supervisor_pid;
  EXPECT_TRUE(no_pidfd_compositor_exited)
    << "pidfd-unavailable stop leaked compositor pid " << no_pidfd_compositor_pid;
  EXPECT_TRUE(no_pidfd_worker_exited)
    << "pidfd-unavailable stop leaked worker pid " << no_pidfd_worker_pid;
  if (no_pidfd_compositor_exited) {
    remove_cleanup_pid(no_pidfd_compositor_pid);
  }
  if (no_pidfd_worker_exited) {
    remove_cleanup_pid(no_pidfd_worker_pid);
  }
}

TEST(WaylandInterfaceTests, RemovedOutputMarksDirtyButKeepsMonitorStorageUntilReinit) {
  wl::interface_t interface;

  interface.add_monitor_for_tests(57);
  EXPECT_TRUE(interface.consume_output_topology_dirty());
  ASSERT_EQ(interface.monitors.size(), 1u);

  interface.remove_global_for_tests(57);

  EXPECT_TRUE(interface.consume_output_topology_dirty());
  EXPECT_EQ(interface.monitors.size(), 1u);
}
#endif

// Trivial AND/negation helpers (windowed_gpu_native_probe, gpu_native_cage_capture,
// windowed_ram_fallback, headless_extcopy_probe_succeeded) live only under
// stream_runtime::labwc as one-liners — no dual cage export / matrix tests.

TEST(CageDisplayRouterPolicyTests, EffectiveHeadlessVaapiSkipsExtcopyDmabuf) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::vaapi
  ));
}

TEST(CageDisplayRouterPolicyTests, EffectiveHeadlessCudaCanAttemptExtcopyDmabuf) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::cuda
  ));
}

TEST(CageDisplayRouterPolicyTests, EffectiveHeadlessVulkanCanAttemptExtcopyDmabuf) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::vulkan
  ));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideDoesNotAttemptHeadlessExtcopyDmabuf) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::cuda
  ));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideRefusesGpuNativeCaptureForVaapi) {
  // The crash in #212 and #279: the override put VAAPI on the DMA-BUF path the
  // headless branch was already refusing for the same encoder, and the
  // conversion segfaulted at stream start.
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_gpu_native_cage_capture(
    runtime_state,
    platf::mem_type_e::vaapi
  ));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideAttemptsGpuNativeCaptureForCuda) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_attempt_gpu_native_cage_capture(
    runtime_state,
    platf::mem_type_e::cuda
  ));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideAttemptsGpuNativeCaptureForVulkan) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_attempt_gpu_native_cage_capture(
    runtime_state,
    platf::mem_type_e::vulkan
  ));
}

TEST(CageDisplayRouterPolicyTests, GpuNativeCaptureNeedsTheOverrideRegardlessOfEncoder) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_gpu_native_cage_capture(
    runtime_state,
    platf::mem_type_e::cuda
  ));
}

TEST(CageDisplayRouterPolicyTests, LinearHeadlessVaapiRemainsUnsafeAfterFieldCrash) {
  using route_e = wlgrab_capture_policy::gpu_native_capture_route_e;
  constexpr std::uint64_t tiled_modifier = 0x0100000000000002ULL;

  EXPECT_FALSE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::headless_extcopy,
    DRM_FORMAT_MOD_LINEAR
  ));
  EXPECT_FALSE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::headless_extcopy,
    tiled_modifier
  ));
  EXPECT_FALSE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::headless_extcopy,
    std::nullopt
  ));
  EXPECT_FALSE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::windowed_nested,
    DRM_FORMAT_MOD_LINEAR
  ));
  EXPECT_FALSE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::direct_wayland,
    DRM_FORMAT_MOD_LINEAR
  ));
}

TEST(CageDisplayRouterPolicyTests, CudaRemainsSafeWithoutAModifier) {
  using route_e = wlgrab_capture_policy::gpu_native_capture_route_e;

  EXPECT_TRUE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::cuda,
    route_e::headless_extcopy,
    std::nullopt
  ));
  EXPECT_TRUE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::cuda,
    route_e::windowed_nested,
    std::nullopt
  ));
  EXPECT_TRUE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::cuda,
    route_e::direct_wayland,
    std::nullopt
  ));
}

TEST(CageDisplayRouterPolicyTests, VulkanRemainsSafeWithoutAModifier) {
  using route_e = wlgrab_capture_policy::gpu_native_capture_route_e;

  EXPECT_TRUE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vulkan,
    route_e::headless_extcopy,
    std::nullopt
  ));
  EXPECT_TRUE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vulkan,
    route_e::windowed_nested,
    std::nullopt
  ));
  EXPECT_TRUE(cage_display_router::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vulkan,
    route_e::direct_wayland,
    std::nullopt
  ));
}

TEST(CageDisplayRouterPolicyTests, HeadlessDmabufGpuConversionFailureDisablesExtcopyFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };
  const platf::frame_metadata_t metadata {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
    .format = platf::frame_format_e::bgra8,
  };

  EXPECT_TRUE(cage_display_router::should_disable_headless_extcopy_after_conversion_failure(
    runtime_state,
    metadata
  ));
}

TEST(CageDisplayRouterPolicyTests, InitialImageConversionFailureRetiresASelectedHeadlessDmabufRoute) {
  // #409: the initial image is a dummy allocation, so it carries no capture
  // metadata and the steady-state predicate cannot see it. A GPU-native route
  // that cannot convert it delivers no frames at all.
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_disable_headless_extcopy_after_initial_conversion_failure(
    runtime_state,
    std::optional<bool> {true}
  ));
}

TEST(CageDisplayRouterPolicyTests, InitialImageConversionFailureDoesNotRetireARouteThatWasNeverSelected) {
  // This is also what keeps the retirement from repeating: once the route is
  // retired the cached result reads false, so the reinitialized SHM attempt
  // cannot retire anything a second time and reinit cannot loop.
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_disable_headless_extcopy_after_initial_conversion_failure(
    runtime_state,
    std::optional<bool> {false}
  ));
  EXPECT_FALSE(cage_display_router::should_disable_headless_extcopy_after_initial_conversion_failure(
    runtime_state,
    std::nullopt
  ));
}

TEST(CageDisplayRouterPolicyTests, InitialImageConversionFailureLeavesTheWindowedOverrideAlone) {
  // The windowed GPU-native override owns its own probe and its own fallback.
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_disable_headless_extcopy_after_initial_conversion_failure(
    runtime_state,
    std::optional<bool> {true}
  ));
}

TEST(CageDisplayRouterPolicyTests, NonHeadlessDmabufConversionFailureDoesNotDisableExtcopyFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };
  const platf::frame_metadata_t metadata {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
    .format = platf::frame_format_e::bgra8,
  };

  EXPECT_FALSE(cage_display_router::should_disable_headless_extcopy_after_conversion_failure(
    runtime_state,
    metadata
  ));
}

TEST(CageDisplayRouterPolicyTests, WindowedDmabufGpuConversionFailureDisablesGpuNativeOverride) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };
  const platf::frame_metadata_t metadata {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
    .format = platf::frame_format_e::bgra8,
  };

  EXPECT_TRUE(cage_display_router::should_disable_windowed_gpu_native_after_conversion_failure(
    runtime_state,
    metadata
  ));
}

TEST(CageDisplayRouterPolicyTests, CpuFrameDoesNotDisableWindowedGpuNativeOverride) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };
  const platf::frame_metadata_t metadata {
    .transport = platf::frame_transport_e::shm,
    .residency = platf::frame_residency_e::cpu,
    .format = platf::frame_format_e::bgra8,
  };

  EXPECT_FALSE(cage_display_router::should_disable_windowed_gpu_native_after_conversion_failure(
    runtime_state,
    metadata
  ));
}

TEST(CageDisplayRouterPolicyTests, CachedWindowedProbeFailureSuppressesGpuNativeCapture) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  // cuda, so the memory-type policy passes and the cached probe result is what
  // decides — that is the behaviour under test here.
  EXPECT_TRUE(cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state, platf::mem_type_e::cuda));
  cage_display_router::update_windowed_gpu_native_probe_result(false);
  EXPECT_FALSE(cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state, platf::mem_type_e::cuda));
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();
}

TEST(CageDisplayRouterPolicyTests, TheTwoGpuNativeRefusalsAreIndependent) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  // An unsafe memory type is refused even with no probe result recorded, and a
  // failed probe refuses a safe one. Collapsing either into the other would let
  // a host-specific failure look like an encoder policy, or the reverse.
  EXPECT_FALSE(cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state, platf::mem_type_e::vaapi));
  EXPECT_TRUE(cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state, platf::mem_type_e::cuda));

  cage_display_router::update_windowed_gpu_native_probe_result(false);
  EXPECT_FALSE(cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state, platf::mem_type_e::cuda));
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();
}

TEST(CageDisplayRouterPolicyTests, CpuFrameConversionFailureDoesNotDisableExtcopyFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };
  const platf::frame_metadata_t metadata {
    .transport = platf::frame_transport_e::shm,
    .residency = platf::frame_residency_e::cpu,
    .format = platf::frame_format_e::bgra8,
  };

  EXPECT_FALSE(cage_display_router::should_disable_headless_extcopy_after_conversion_failure(
    runtime_state,
    metadata
  ));
}

TEST(CageDisplayRouterPolicyTests, RequestedHeadlessWithoutOverrideStillReportsHeadlessFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_report_headless_ram_capture_fallback(runtime_state));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideDoesNotReportHeadlessFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_report_headless_ram_capture_fallback(runtime_state));
}

#ifdef POLARIS_TESTS
TEST(WaylandDrmDeviceTests, InvalidDeviceDoesNotFabricateRenderNode) {
  EXPECT_TRUE(wl::render_node_from_drm_device_for_tests(dev_t {}).empty());
}

TEST(CageDisplayRouterPolicyTests, WindowedRamCaptureFallbackWarningLogsOnlyOnce) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_TRUE(cage_display_router::should_log_windowed_ram_capture_warning());
  EXPECT_FALSE(cage_display_router::should_log_windowed_ram_capture_warning());
}

TEST(CageDisplayRouterPolicyTests, HeadlessRamCaptureFallbackWarningLogsOnlyOnce) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_TRUE(cage_display_router::should_log_headless_ram_capture_warning());
  EXPECT_FALSE(cage_display_router::should_log_headless_ram_capture_warning());
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultDefaultsToUnknown) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_FALSE(cage_display_router::cached_windowed_gpu_native_probe_result().has_value());
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultCachesFailure) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_windowed_gpu_native_probe_result(false);

  EXPECT_EQ(cage_display_router::cached_windowed_gpu_native_probe_result(), std::optional<bool> {false});
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultCachesSuccess) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_windowed_gpu_native_probe_result(true);

  EXPECT_EQ(cage_display_router::cached_windowed_gpu_native_probe_result(), std::optional<bool> {true});
}

TEST(CageDisplayRouterPolicyTests, HeadlessExtcopyDmabufProbeResultDefaultsToUnknown) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_FALSE(cage_display_router::cached_headless_extcopy_dmabuf_probe_result().has_value());
}

TEST(CageDisplayRouterPolicyTests, HeadlessExtcopyDmabufProbeResultCachesFailure) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);

  EXPECT_EQ(cage_display_router::cached_headless_extcopy_dmabuf_probe_result(), std::optional<bool> {false});
}

TEST(CageDisplayRouterPolicyTests, HeadlessExtcopyDmabufProbeResultCachesSuccess) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_headless_extcopy_dmabuf_probe_result(true);

  EXPECT_EQ(cage_display_router::cached_headless_extcopy_dmabuf_probe_result(), std::optional<bool> {true});
}

TEST(CageDisplayRouterPolicyTests, HeadlessExtcopyDmabufProbeFailureSuppressesRetry) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::cuda
  ));

  cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);

  EXPECT_FALSE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::cuda
  ));
}

TEST(CageDisplayRouterPolicyTests, MangoHudPrefixIsSuppressedForSteamBigPicture) {
  EXPECT_TRUE(cage_display_router::mangohud_prefix_for_command_for_tests(
    "steam -gamepadui",
    true,
    "1",
    "fps_limit=60"
  ).empty());

  EXPECT_TRUE(cage_display_router::mangohud_prefix_for_command_for_tests(
    "setsid steam steam://open/bigpicture",
    true,
    "1",
    "fps_limit=60"
  ).empty());
}

TEST(CageDisplayRouterPolicyTests, MangoHudPrefixStillAppliesToRegularGames) {
  EXPECT_EQ(
    cage_display_router::mangohud_prefix_for_command_for_tests(
      "steam steam://rungameid/12345",
      true,
      "1",
      "fps_limit=60"
    ),
    "MANGOHUD=1 MANGOHUD_DLSYM=1 MANGOHUD_CONFIG=fps_limit=60 "
  );
}

TEST(CageDisplayRouterPolicyTests, LabwcProcessEnvironmentDisablesHardwareCursors) {
  EXPECT_EQ(
    cage_display_router::labwc_process_environment_value_for_tests(
      true,
      "WLR_NO_HARDWARE_CURSORS"
    ),
    "1"
  );
  EXPECT_EQ(
    cage_display_router::labwc_process_environment_value_for_tests(
      false,
      "WLR_NO_HARDWARE_CURSORS"
    ),
    "1"
  );
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRecognizesRequestedCurrentMode) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1280x720 px, 60.000000 Hz
    1920x1080 px, 60.000000 Hz (current)
)";

  EXPECT_TRUE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080
  ));
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRecognizesRequestedCurrentRefresh) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1920x1080 px, 60.000000 Hz
    1920x1080 px, 120.000000 Hz (current)
)";

  EXPECT_TRUE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080,
    120
  ));
  const auto reported_refresh = cage_display_router::output_current_refresh_hz_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080
  );
  ASSERT_TRUE(reported_refresh);
  EXPECT_DOUBLE_EQ(*reported_refresh, 120.0);
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRecognizesLocalizedDecimalComma) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1920x1080 px, 59,940000 Hz (current)
)";

  const auto reported_refresh = cage_display_router::output_current_refresh_hz_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080
  );
  ASSERT_TRUE(reported_refresh);
  EXPECT_DOUBLE_EQ(*reported_refresh, 59.94);
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRejectsWrongCurrentRefresh) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1920x1080 px, 60.000000 Hz (current)
    1920x1080 px, 120.000000 Hz
)";

  EXPECT_FALSE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080,
    120
  ));
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRejectsNonCurrentRequestedMode) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1280x720 px, 60.000000 Hz (current)
    1920x1080 px, 60.000000 Hz
)";

  EXPECT_FALSE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080
  ));
}

TEST(CageDisplayRouterPolicyTests, FormatWlrCustomModeIncludesRefresh) {
  EXPECT_EQ(
    cage_display_router::format_wlr_custom_mode_for_tests(1920, 1080, 120),
    "1920x1080@120Hz"
  );
}

TEST(CageDisplayRouterPolicyTests, WaylandSocketNameParserAcceptsNumberedSocketsOnly) {
  EXPECT_TRUE(cage_display_router::is_wayland_socket_name_for_tests("wayland-0"));
  EXPECT_TRUE(cage_display_router::is_wayland_socket_name_for_tests("wayland-50"));
  EXPECT_TRUE(cage_display_router::is_wayland_socket_name_for_tests("wayland-123"));

  EXPECT_FALSE(cage_display_router::is_wayland_socket_name_for_tests("wayland-"));
  EXPECT_FALSE(cage_display_router::is_wayland_socket_name_for_tests("wayland-1.lock"));
  EXPECT_FALSE(cage_display_router::is_wayland_socket_name_for_tests("pipewire-0"));
  EXPECT_FALSE(cage_display_router::is_wayland_socket_name_for_tests("nested-wayland-1"));
}
#endif

TEST(CageDisplayRouterResumeRefreshTests, NormalizesMillihertzSessionFps) {
  EXPECT_EQ(cage_display_router::normalize_session_refresh_hz(60), 60);
  EXPECT_EQ(cage_display_router::normalize_session_refresh_hz(120), 120);
  // Clients requesting fractional rates send millihertz (59.94 Hz → 59940).
  EXPECT_EQ(cage_display_router::normalize_session_refresh_hz(59940), 60);
  EXPECT_EQ(cage_display_router::normalize_session_refresh_hz(120000), 120);
  EXPECT_EQ(cage_display_router::normalize_session_refresh_hz(0), 0);
}

TEST(CageDisplayRouterResumeRefreshTests, EnsureOutputRefreshRefusesWithoutRunningCage) {
  // No cage is running in the test environment; the resume-path re-apply must
  // refuse rather than shell out to wlr-randr against a stale socket.
  EXPECT_FALSE(cage_display_router::ensure_output_refresh(120));
  EXPECT_FALSE(cage_display_router::ensure_output_refresh(0));
}

TEST(CageDisplayRouterResumeRefreshTests, ResumeRefreshHonorsLaunchClamp) {
  // A launch that deliberately ran below the client's request (optimizer or
  // runtime policy) records the effective rate as a ceiling; a resume with
  // the raw request must not out-vote that decision.
  EXPECT_EQ(cage_display_router::resolve_resume_refresh_hz(120, 60), 60);
  EXPECT_EQ(cage_display_router::resolve_resume_refresh_hz(120000, 60), 60);
  // Unclamped launches leave no ceiling, and lower requests always pass.
  EXPECT_EQ(cage_display_router::resolve_resume_refresh_hz(120, 0), 120);
  EXPECT_EQ(cage_display_router::resolve_resume_refresh_hz(60, 120), 60);
  EXPECT_EQ(cage_display_router::resolve_resume_refresh_hz(0, 60), 0);
}

TEST(CageDisplayRouterResumeRefreshTests, ExactResolvedRefreshBypassesLegacyClamp) {
  // The deterministic resolver already chose the exact target. Reusing the
  // prior generation's raw-request ceiling would falsely acknowledge the new
  // profile while leaving the private output at the old cadence.
  EXPECT_EQ(cage_display_router::resolve_resume_refresh_hz(120, 60, false), 120);
  EXPECT_EQ(cage_display_router::resolve_resume_refresh_hz(120000, 60, false), 120);
  EXPECT_EQ(cage_display_router::resolve_resume_refresh_hz(60, 120, false), 60);
}

TEST(CageDisplayRouterResumeRefreshTests, ResumePathReappliesRefreshInNvhttp) {
  // Source pin, same rationale as the render-device guards: the fix is only
  // real if the resume handler actually calls it. A cage that outlives its
  // launch otherwise keeps the old refresh for the whole session (issue #367).
  const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/nvhttp.cpp";
  std::ifstream file {path};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const auto source = buffer.str();
  ASSERT_FALSE(source.empty()) << "could not read nvhttp.cpp via POLARIS_SOURCE_DIR";

  const auto resume_pos = source.find("void resume(");
  ASSERT_NE(resume_pos, std::string::npos);
  const auto validation = source.find("validate_resolved_profile_for_running_app", resume_pos);
  const auto refresh = source.find("stream_runtime::labwc::ensure_output_refresh", resume_pos);
  const auto raise = source.find("raise_session_for_admitted_launch", refresh);
  ASSERT_NE(validation, std::string::npos);
  ASSERT_NE(refresh, std::string::npos);
  ASSERT_NE(raise, std::string::npos);
  EXPECT_LT(validation, refresh)
    << "a rejected exact resume must not mutate the surviving cage generation";
  EXPECT_LT(refresh, raise);
  EXPECT_NE(
    source.find("!cage_refresh_applied && launch_session->resolved_profile_from_client", refresh),
    std::string::npos
  )
    << "an exact resume must fail closed when the resolved cage mode does not settle";
  EXPECT_NE(source.find("prior_cage_refresh_hz * 1000", refresh), std::string::npos)
    << "a failed pending launch must restore the prior settled cage mode";
  EXPECT_NE(
    source.find("!launch_session->resolved_profile_from_client", refresh),
    std::string::npos
  ) << "only legacy resumes may reuse the prior generation's request ceiling";
  EXPECT_NE(refresh, std::string::npos)
    << "the resume handler must re-apply the resuming client's refresh to a running cage";
}
#else
TEST(CageDisplayRouterPolicyTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only runtime policy tests";
}
#endif
