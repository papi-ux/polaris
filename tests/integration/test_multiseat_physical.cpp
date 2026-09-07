/**
 * @file tests/integration/test_multiseat_physical.cpp
 * @brief Opt-in physical smoke of the production multiseat composition.
 *
 * Skipped unless POLARIS_MULTISEAT_PHYSICAL=1. Everything else about the run
 * comes from the environment so no host detail is compiled in:
 *   POLARIS_PHYSICAL_IMAGE         digest-pinned worker image reference
 *   POLARIS_PHYSICAL_IPC_ROOT      pre-created mode-0700 runtime root
 *   POLARIS_PHYSICAL_RENDER_NODE   render node path (default /dev/dri/renderD128)
 *   POLARIS_PHYSICAL_GPU_DEVICES   comma-separated device paths, must include
 *                                  the render node (default: the render node)
 *   POLARIS_PHYSICAL_PODMAN        podman executable (default /usr/bin/podman)
 *   POLARIS_PHYSICAL_VOLUME        pre-created profile volume name
 *   POLARIS_PHYSICAL_READY_SECONDS seconds to wait for the worker (default 120)
 *
 * This drives the real controller with the real rootless Podman host, the real
 * inputtino backend, and the real worker transport. It creates virtual input
 * devices on the host and starts one container; it never touches the running
 * Polaris process.
 */
#include "src/platform/linux/multiseat_controller_production.h"

#ifdef __linux__

  #include <chrono>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <gtest/gtest.h>
  #include <iostream>
  #include <memory>
  #include <sstream>
  #include <string>
  #include <thread>
  #include <vector>

namespace {
  using namespace multiseat;
  namespace input = multiseat::input;
  namespace podman = multiseat::podman;

  std::string env_or(const char *name, const std::string &fallback) {
    const auto *value = std::getenv(name);
    return value && *value ? std::string {value} : fallback;
  }

  std::vector<std::filesystem::path> split_paths(const std::string &value) {
    std::vector<std::filesystem::path> paths;
    std::stringstream stream {value};
    std::string item;
    while (std::getline(stream, item, ',')) {
      if (!item.empty()) {
        paths.emplace_back(item);
      }
    }
    return paths;
  }

  std::string shell_output(const std::string &command) {
    std::string output;
    if (auto *pipe = ::popen(command.c_str(), "r")) {
      char buffer[4096];
      while (auto read = std::fread(buffer, 1, sizeof(buffer), pipe)) {
        output.append(buffer, read);
      }
      ::pclose(pipe);
    }
    return output;
  }

  void log(const std::string &line) {
    std::cout << "[physical] " << line << std::endl;
  }

  void log_podman(const std::string &podman, const std::string &deployment) {
    log("podman ps:\n" + shell_output(
      podman + " ps -a --filter label=io.polaris.multiseat.deployment=" +
      deployment + " --format '{{.ID}} {{.Names}} {{.Status}}' 2>&1"
    ));
  }

  void log_worker_inspect(const std::string &podman, const std::string &deployment) {
    const auto ids = shell_output(
      podman + " ps -a --filter label=io.polaris.multiseat.deployment=" +
      deployment + " --format '{{.ID}}' 2>/dev/null"
    );
    std::stringstream stream {ids};
    std::string id;
    while (std::getline(stream, id)) {
      if (!id.empty()) {
        log("inspect " + id + ": " + shell_output(
          podman + " container inspect " + id +
          " --format 'Name={{.Name}} Status={{.State.Status}} Health={{.State.Health.Status}} "
          "Devices={{json .HostConfig.Devices}} Mounts={{json .Mounts}}' 2>&1 | cut -c1-1200"
        ));
      }
    }
  }

  /** Force-remove anything the controller left behind so the host stays clean. */
  void cleanup_leftovers(
    const std::string &podman,
    const std::string &deployment,
    const std::string &ipc_root
  ) {
    const auto ids = shell_output(
      podman + " ps -a --filter label=io.polaris.multiseat.deployment=" +
      deployment + " --format '{{.ID}}' 2>/dev/null"
    );
    std::stringstream stream {ids};
    std::string id;
    while (std::getline(stream, id)) {
      if (!id.empty()) {
        log("LEFTOVER container force-removed: " + id + " " +
            shell_output(podman + " rm -f " + id + " 2>&1"));
      }
    }
    std::error_code ignored;
    for (const auto &entry : std::filesystem::directory_iterator {ipc_root, ignored}) {
      if (entry.path().filename().string().starts_with("polaris-runtime-")) {
        log("LEFTOVER authority directory removed: " + entry.path().string());
        std::filesystem::remove_all(entry.path(), ignored);
      }
    }
  }

  void log_worker_logs(const std::string &podman, const std::string &deployment) {
    const auto ids = shell_output(
      podman + " ps -a --filter label=io.polaris.multiseat.deployment=" +
      deployment + " --format '{{.ID}}' 2>/dev/null"
    );
    std::stringstream stream {ids};
    std::string id;
    while (std::getline(stream, id)) {
      if (!id.empty()) {
        log("podman logs " + id + ":\n" + shell_output(podman + " logs " + id + " 2>&1 | tail -40"));
        log("podman inspect state " + id + ": " + shell_output(
          podman + " inspect --format '{{.State.Status}} health={{.State.Health.Status}} exit={{.State.ExitCode}}' " + id + " 2>&1"
        ));
      }
    }
  }

  const char *input_status_name(input::status_e status) {
    switch (status) {
      case input::status_e::applied: return "applied";
      case input::status_e::already_applied: return "already_applied";
      case input::status_e::reconciliation_required: return "reconciliation_required";
      case input::status_e::invalid_request: return "invalid_request";
      case input::status_e::not_found: return "not_found";
      case input::status_e::stale_authority: return "stale_authority";
      case input::status_e::backend_rejected: return "backend_rejected";
      case input::status_e::backend_indeterminate: return "backend_indeterminate";
      case input::status_e::backend_protocol_error: return "backend_protocol_error";
    }
    return "?";
  }

  const char *reconcile_name(controller_reconcile_status_e status) {
    switch (status) {
      case controller_reconcile_status_e::ready: return "ready";
      case controller_reconcile_status_e::controller_shutting_down: return "controller_shutting_down";
      case controller_reconcile_status_e::invalid_input_expectations: return "invalid_input_expectations";
      case controller_reconcile_status_e::worker_reconciliation_required: return "worker_reconciliation_required";
      case controller_reconcile_status_e::input_reconciliation_required: return "input_reconciliation_required";
    }
    return "?";
  }

  std::string describe(const controller_reconcile_result_t &result) {
    std::ostringstream text;
    const auto &b = result.worker.broker;
    text << "status=" << reconcile_name(result.status)
         << " ready=" << (result.ready() ? "yes" : "no")
         << " worker.admission_ready=" << result.worker.admission_ready
         << " startup_recovery=" << result.worker.startup_recovery_complete
         << " authority_blocked=" << result.worker.authority_blocked
         << " broker{obs=" << b.observations << " current=" << b.current_workers
         << " orphan=" << b.orphan_workers << " ready_transitions=" << b.ready_transitions
         << " missing=" << b.missing_workers << " released=" << b.released_seats
         << " graceful=" << b.graceful_stop_requests << " force=" << b.force_stop_requests
         << " stuck=" << b.stuck_workers << " protocol_errors=" << b.protocol_errors
         << " readiness_rejections=" << b.readiness_rejections
         << " observation_failed=" << b.backend_observation_failed
         << " authoritative=" << b.inventory_authoritative << "}"
         << " endpoints{connect=" << result.worker.endpoint_connections
         << " fail=" << result.worker.endpoint_failures
         << " heartbeats=" << result.worker.endpoint_heartbeats << "}";
    if (result.input) {
      text << " input{status=" << static_cast<int>(result.input->status)
           << " admission_ready=" << result.input->report.admission_ready << "}";
    }
    return text.str();
  }

  TEST(MultiseatPhysical, OneRealSupervisorWorkerStartsReportsReadyAndTearsDown) {
    if (env_or("POLARIS_MULTISEAT_PHYSICAL", "") != "1") {
      GTEST_SKIP() << "set POLARIS_MULTISEAT_PHYSICAL=1 to run against real Podman";
    }
    const auto image = env_or("POLARIS_PHYSICAL_IMAGE", "");
    const auto ipc_root = env_or("POLARIS_PHYSICAL_IPC_ROOT", "");
    const auto render_node = env_or("POLARIS_PHYSICAL_RENDER_NODE", "/dev/dri/renderD128");
    const auto devices = split_paths(env_or("POLARIS_PHYSICAL_GPU_DEVICES", render_node));
    const auto podman_path = env_or("POLARIS_PHYSICAL_PODMAN", "/usr/bin/podman");
    const auto volume = env_or("POLARIS_PHYSICAL_VOLUME", "pv-physical-a1b2");
    const auto ready_seconds = std::stoul(env_or("POLARIS_PHYSICAL_READY_SECONDS", "120"));
    ASSERT_FALSE(image.empty()) << "POLARIS_PHYSICAL_IMAGE is required";
    ASSERT_FALSE(ipc_root.empty()) << "POLARIS_PHYSICAL_IPC_ROOT is required";
    const std::string deployment = "physical-smoke";

    production_controller_options_t options;
    options.enabled = true;
    options.gpus = {{
      .logical_gpu_id = "gpu-physical",
      .render_node = render_node,
      .devices = devices,
      .max_seats = 2,
      .max_encoder_sessions = 2,
    }};
    options.podman.executable = podman_path;
    options.podman.deployment_id = deployment;
    options.podman.worker_entrypoint = "/usr/bin/polaris-seat-worker";
    options.podman.ipc_root = ipc_root;
    options.podman.profiles = {{
      .profile_key = "physical-gamescope",
      .opaque_volume_name = volume,
      .runtime_profile = runtime_profile_e::gamescope,
      .image_reference = image,
    }};
    options.podman.workloads = {{
      .kind = workload_kind_e::gamescope,
      .target_id = "physical-smoke",
    }};

    log("image=" + image + " ipc_root=" + ipc_root + " render=" + render_node +
        " devices=" + std::to_string(devices.size()) + " podman=" + podman_path);
    auto created = create_production_controller_runtime(std::move(options));
    log("create status=" + std::to_string(static_cast<int>(created.status)));
    ASSERT_EQ(created.status, controller_runtime_create_status_e::ready_enabled);
    ASSERT_TRUE(created.runtime);
    auto controller = std::move(created.runtime);

    const auto initial = controller->reconcile();
    log("initial reconcile: " + describe(initial));
    log_podman(podman_path, deployment);
    ASSERT_TRUE(initial.ready()) << describe(initial);

    seat_request_t request {
      .client_key = "physical-client",
      .profile_key = "physical-gamescope",
      .workload = {.kind = workload_kind_e::gamescope, .target_id = "physical-smoke"},
      .logical_gpu_id = "gpu-physical",
      .runtime_profile = runtime_profile_e::gamescope,
      .data_plane = {
        .display_topology = display_topology_e::capture_host_with_nested_compositor,
        .media_pipeline = media_pipeline_e::worker_local_capture_encode,
      },
      .display_mode = {1920, 1080, 60000, false},
      .requested_compositor = compositor_e::gamescope,
      .encoder_sessions = 1,
    };
    const auto admitted = controller->admit(request);
    log("admit rejection=" + std::to_string(static_cast<int>(admitted.rejection)));
    ASSERT_TRUE(admitted.accepted());
    ASSERT_TRUE(admitted.seat);
    const auto handle = admitted.seat->handle;
    log("seat epoch=" + handle.controller_epoch + " gpu=" + handle.logical_gpu_id +
        " slot=" + std::to_string(handle.slot) + " gen=" + std::to_string(handle.generation) +
        " worker=" + admitted.seat->resources.worker_name +
        " ns=" + admitted.seat->resources.runtime_namespace +
        " input_seat=" + admitted.seat->resources.input_seat);
    ASSERT_EQ(
      controller->bind_runtime(handle, compositor_e::gamescope, "physical smoke"),
      mutation_result_e::applied
    );

    const auto started = controller->start_seat(handle, input::plan_t {.gamepad_slots = 1});
    log("start status=" + std::to_string(static_cast<int>(started.status)) +
        " worker=" + (started.worker ? std::to_string(static_cast<int>(*started.worker)) : "none") +
        " input.status=" + std::to_string(static_cast<int>(started.input.status)) +
        " authority=" + input_status_name(started.input.input.status) +
        " nodes=" + (started.input.input.allocation ? std::to_string(started.input.input.allocation->nodes.size()) : std::string {"none"}) +
        " input.prepared=" + (started.input.input.prepared() ? "yes" : "no"));
    if (started.input.input.allocation) {
      for (const auto &node : started.input.input.allocation->nodes) {
        log("  node " + node.host_path.string() + " -> " + node.worker_path.string() +
            " kernel_name='" + node.kernel_name + "' seat='" + node.host_seat + "' phys='" + node.phys + "'");
      }
    }
    log_podman(podman_path, deployment);
    if (!started.started()) {
      log_worker_logs(podman_path, deployment);
      const auto report = controller->shutdown();
      log("shutdown after failed start: status=" + std::to_string(static_cast<int>(report.status)));
      cleanup_leftovers(podman_path, deployment, ipc_root);
      FAIL() << "worker did not start";
    }
    EXPECT_EQ(controller->seats(), 1U);
    EXPECT_EQ(controller->managed_workers(), 1U);
    EXPECT_EQ(controller->input_allocations(), 1U);

    // Poll until the worker is observed ready and its endpoint is connected.
    bool ready = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {ready_seconds};
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::seconds {2});
      const auto result = controller->reconcile();
      log("reconcile: " + describe(result));
      if (result.worker.broker.ready_transitions != 0 ||
          result.worker.endpoint_connections != 0) {
        ready = true;
        break;
      }
      if (controller->seats() == 0) {
        log("seat vanished during startup");
        break;
      }
    }
    log_podman(podman_path, deployment);
    log_worker_inspect(podman_path, deployment);
    log_worker_logs(podman_path, deployment);
    EXPECT_TRUE(ready) << "worker never became ready within " << ready_seconds << "s";

    // Heartbeat the endpoint a few times if it connected.
    for (int i = 0; i < 3 && ready; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds {2});
      const auto result = controller->reconcile();
      log("steady reconcile: " + describe(result));
    }

    const auto stopped = controller->stop_seat(handle);
    log("stop status=" + std::to_string(static_cast<int>(stopped.status)) +
        " broker=" + std::to_string(static_cast<int>(stopped.worker.broker)));
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {90};
    while (std::chrono::steady_clock::now() < stop_deadline &&
           (controller->seats() != 0 || controller->managed_workers() != 0)) {
      std::this_thread::sleep_for(std::chrono::seconds {2});
      const auto result = controller->reconcile();
      log("teardown reconcile: " + describe(result) +
          " seats=" + std::to_string(controller->seats()) +
          " managed=" + std::to_string(controller->managed_workers()));
    }
    log_podman(podman_path, deployment);
    EXPECT_EQ(controller->seats(), 0U);
    EXPECT_EQ(controller->managed_workers(), 0U);

    controller_shutdown_report_t report;
    for (int attempt = 0; attempt < 5; ++attempt) {
      report = controller->shutdown();
      log("shutdown attempt " + std::to_string(attempt) + ": status=" +
          std::to_string(static_cast<int>(report.status)));
      if (report.closed()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::seconds {3});
    }
    EXPECT_TRUE(report.closed());
    EXPECT_EQ(controller->input_allocations(), 0U);
    log_podman(podman_path, deployment);
    cleanup_leftovers(podman_path, deployment, ipc_root);
  }
}  // namespace

#endif
