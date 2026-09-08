/**
 * @file tests/integration/test_multiseat_physical.cpp
 * @brief Opt-in two-seat input acceptance through the isolated controller.
 * Required: POLARIS_MULTISEAT_PHYSICAL=1, POLARIS_PHYSICAL_IMAGE (digest),
 * POLARIS_PHYSICAL_IPC_ROOT (private parent), POLARIS_PHYSICAL_VOLUME and
 * POLARIS_PHYSICAL_VOLUME_B (distinct pre-created volumes). PROFILE selects
 * gamescope/steam/heroic/lutris. POLARIS_PHYSICAL_GAME=1 additionally starts
 * the image-owned Gamescope input game. Production adapter selection stays off.
 */
#include "src/platform/linux/multiseat_controller_production.h"
#include "src/platform/linux/multiseat_podman_host.h"
#include "src/platform/linux/multiseat_moonlight_activation.h"
#include "src/rtsp.h"
#include "src/stream.h"

#ifdef __linux__
extern "C" {
  #include <moonlight-common-c/src/Input.h>
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <fstream>
#include <map>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
  using namespace multiseat;
  using namespace std::chrono_literals;
  using json = nlohmann::json;
  namespace input = multiseat::input;
  namespace podman = multiseat::podman;
  constexpr auto probe = "/usr/bin/polaris-seat-input-probe";

  std::string env_or(const char *name, const std::string &fallback = {}) {
    const auto *value = std::getenv(name);
    return value && *value ? value : fallback;
  }
  std::vector<std::filesystem::path> split_paths(const std::string &value) {
    std::vector<std::filesystem::path> paths;
    std::stringstream stream {value};
    std::string item;
    while (std::getline(stream, item, ',')) if (!item.empty()) paths.emplace_back(item);
    return paths;
  }
  std::string nonce() { return crypto::rand_alphabet(32, "0123456789abcdef"); }

  // Own one unique directory. Only remove it when it is still the same inode
  // and empty; controller reconciliation owns all authority-file cleanup.
  class private_root_t {
  public:
    explicit private_root_t(const std::filesystem::path &parent) {
      podman::local_host_t host;
      if (!host.private_read_write_directory(parent)) throw std::runtime_error {"private IPC parent is unavailable"};
      auto pattern = (parent / "physical-XXXXXX").string();
      const auto created = mkdtemp(pattern.data());
      if (!created) throw std::runtime_error {"private IPC root creation failed"};
      path = created;
      if (lstat(path.c_str(), &identity) != 0) throw std::runtime_error {"private root identity failed"};
    }
    bool remove_empty() {
      struct stat current {};
      if (lstat(path.c_str(), &current) != 0 || current.st_dev != identity.st_dev || current.st_ino != identity.st_ino) return false;
      if (rmdir(path.c_str()) != 0) return false;
      return lstat(path.c_str(), &current) != 0 && errno == ENOENT;
    }
    ~private_root_t() {
      struct stat current {};
      if (lstat(path.c_str(), &current) == 0 && current.st_dev == identity.st_dev && current.st_ino == identity.st_ino) {
        // rmdir cannot recursively erase residue or another run's state.
        (void) rmdir(path.c_str());
      }
    }
    std::filesystem::path path;
    struct stat identity {};
  };

  // Delegate every authority check to the real host, retaining only bounded
  // command failure output in private test evidence (never argv or auth files).
  class observed_host_t final : public podman::host_t {
  public:
    explicit observed_host_t(std::string &failure, std::string &diagnostics, bool game): failure_(failure), diagnostics_(diagnostics), game_(game) {}
    std::uint64_t effective_uid() const override { return host_.effective_uid(); }
    bool executable_file(const std::filesystem::path &path) const override { const auto result = host_.executable_file(path); if (!result) failure_ = "executable_file: " + path.filename().string(); return result; }
    bool trusted_runtime_file(const std::filesystem::path &path) const override { const auto result = host_.trusted_runtime_file(path); if (!result) failure_ = "trusted_runtime_file: " + path.filename().string(); return result; }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return host_.supplementary_groups(); }
    bool readable_directory(const std::filesystem::path &path) const override { const auto result = host_.readable_directory(path); if (!result) failure_ = "readable_directory: " + path.filename().string(); return result; }
    bool private_read_write_directory(const std::filesystem::path &path) const override { const auto result = host_.private_read_write_directory(path); if (!result) failure_ = "private_read_write_directory: " + path.filename().string(); return result; }
    bool private_readable_file(const std::filesystem::path &path) const override { const auto result = host_.private_readable_file(path); if (!result) failure_ = "private_readable_file: " + path.filename().string(); return result; }
    std::optional<podman::character_device_identity_t> read_write_character_device(const std::filesystem::path &path) const override { const auto result = host_.read_write_character_device(path); if (!result) failure_ = "device access: " + path.filename().string(); return result; }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &path, std::size_t maximum) const override { return host_.read_owned_regular_file(path, maximum); }
    podman::command_result_t run(const std::vector<std::string> &argv, std::chrono::milliseconds timeout, std::size_t maximum) override {
      auto admitted = argv;
      // Physical NVIDIA lane only: fixed reviewed domain, after the real backend
      // has classified every mount/device. No CDI or arbitrary security options.
      if (game_) {
        const auto run = std::find(admitted.begin(), admitted.end(), "run");
        if (run != admitted.end()) {
          admitted.insert(run + 1, "--security-opt=label=type:polaris_nvidia_worker_t");
          // Keep a failed worker's bounded log until exact-ID controller removal.
          // The harness still requires absence of running AND stopped workers.
          std::erase(admitted, "--rm");
        }
        if (std::find(admitted.begin(), admitted.end(), "rm") != admitted.end() && !admitted.empty()) {
          const auto inspected = host_.run({admitted.front(), "--remote=false", "inspect", "--format={{.LogPath}}", admitted.back()}, 2s, 4096);
          if (inspected.exit_status == 0) {
            auto path = inspected.output;
            while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) path.pop_back();
            if (std::filesystem::path {path}.is_absolute() && path.find('\n') == std::string::npos) {
              const auto log = host_.read_owned_regular_file(path, 65536);
              if (log && diagnostics_.size() + log->size() <= 65536) diagnostics_ += *log;
            }
          }
        }
      }
      auto result = host_.run(admitted, timeout, maximum);
      if (result.exit_status != 0 || result.timed_out) failure_ = result.output.substr(0,4096);
      return result;
    }
  private:
    podman::local_host_t host_;
    std::string &failure_;
    std::string &diagnostics_;
    bool game_;
  };

  using bytes = std::vector<std::uint8_t>;
  void le16(bytes &out, std::uint16_t value) { out.push_back(value); out.push_back(value >> 8); }
  void be16(bytes &out, std::uint16_t value) { out.push_back(value >> 8); out.push_back(value); }
  bytes packet(std::uint32_t magic, bytes body) {
    const auto length = static_cast<std::uint32_t>(body.size() + 4);
    bytes result {static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)};
    for (int byte = 0; byte < 4; ++byte) result.push_back(magic >> (8*byte));
    result.insert(result.end(), body.begin(), body.end());
    return result;
  }
  bytes keyboard(bool released) { return packet(released ? KEY_UP_EVENT_MAGIC : KEY_DOWN_EVENT_MAGIC, {0, 0x41, 0, 0, 0, 0}); }
  bytes gamepad(bool pressed) {
    bytes body;
    for (auto word : {MC_HEADER_B, 0, 1, MC_MID_B, pressed ? 0x1000 : 0}) le16(body, word);
    body.resize(20, 0);
    le16(body, MC_TAIL_A); le16(body, 0); le16(body, MC_TAIL_B);
    return packet(MULTI_CONTROLLER_MAGIC_GEN5, body);
  }
  void send_markers(stream::session_t &stream, int position) {
    EXPECT_TRUE(stream::session::route_multiseat_input_for_tests(stream, keyboard(false)));
    bytes relative; be16(relative, 7); be16(relative, 0);
    EXPECT_TRUE(stream::session::route_multiseat_input_for_tests(stream, packet(MOUSE_MOVE_REL_MAGIC_GEN5, relative)));
    bytes absolute; for (auto value : {position, 700, 0, 1920, 1080}) be16(absolute, value);
    EXPECT_TRUE(stream::session::route_multiseat_input_for_tests(stream, packet(MOUSE_MOVE_ABS_MAGIC, absolute)));
    EXPECT_TRUE(stream::session::route_multiseat_input_for_tests(stream, gamepad(true)));
    EXPECT_TRUE(stream::session::route_multiseat_input_for_tests(stream, keyboard(true)));
    EXPECT_TRUE(stream::session::route_multiseat_input_for_tests(stream, gamepad(false)));
  }

  bool container_id_valid(std::string_view id) {
    return id.size() == 64 && std::all_of(id.begin(),id.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
  }

  struct seat_t {
    seat_snapshot_t snapshot;
    input::allocation_t allocation;
    std::string container_id;
    std::shared_ptr<rtsp_stream::launch_session_t> launch;
    std::shared_ptr<stream::session_t> stream;
  };

  TEST(MultiseatPhysical, TwoWorkersReadOnlyTheirAllocatedInputAndStopIndependently) {
    if (env_or("POLARIS_MULTISEAT_PHYSICAL") != "1") GTEST_SKIP() << "opt-in physical harness";
    const auto image = env_or("POLARIS_PHYSICAL_IMAGE");
    const auto parent = env_or("POLARIS_PHYSICAL_IPC_ROOT");
    const std::array volumes {env_or("POLARIS_PHYSICAL_VOLUME"), env_or("POLARIS_PHYSICAL_VOLUME_B")};
    ASSERT_FALSE(image.empty()); ASSERT_FALSE(parent.empty());
    RecordProperty("worker_image", image);
    RecordProperty("access_policy", "crun-keep-groups-explicit-uid");
    ASSERT_FALSE(volumes[0].empty()); ASSERT_FALSE(volumes[1].empty()); ASSERT_NE(volumes[0], volumes[1]);
    const auto profile_name = env_or("POLARIS_PHYSICAL_PROFILE", "gamescope");
    const std::map<std::string, std::pair<runtime_profile_e, workload_kind_e>> profiles {
      {"gamescope", {runtime_profile_e::gamescope, workload_kind_e::gamescope}},
      {"steam", {runtime_profile_e::steam, workload_kind_e::steam}},
      {"heroic", {runtime_profile_e::heroic, workload_kind_e::heroic}},
      {"lutris", {runtime_profile_e::lutris, workload_kind_e::lutris}},
    };
    ASSERT_TRUE(profiles.contains(profile_name));
    RecordProperty("runtime_profile", profile_name);
    const bool game = env_or("POLARIS_PHYSICAL_GAME") == "1";
    ASSERT_TRUE(!game || profile_name == "gamescope");
    const std::string workload = game ? "input-pong-v1" : "physical-input-proof";
    RecordProperty("game_streaming", "false");
    RecordProperty("physical_game_requested", game ? "true" : "false");
    const auto [profile, kind] = profiles.at(profile_name);
    const auto render = env_or("POLARIS_PHYSICAL_RENDER_NODE", "/dev/dri/renderD128");
    const auto devices = split_paths(env_or("POLARIS_PHYSICAL_GPU_DEVICES", render));
    const auto executable = env_or("POLARIS_PHYSICAL_PODMAN", "/usr/bin/podman");
    const auto deployment = "physical-" + nonce();
    private_root_t root {parent};
    podman::local_host_t host;
    const auto command = [&](std::vector<std::string> arguments, std::chrono::milliseconds timeout = 5s) {
      arguments.insert(arguments.begin(), {executable, "--remote=false"});
      return host.run(arguments, timeout, 1024*1024);
    };
    production_controller_options_t options;
    options.enabled = true;
    options.gpus = {{.logical_gpu_id="physical-gpu", .render_node=render, .devices=devices, .max_seats=2, .max_encoder_sessions=2}};
    options.podman.executable = executable;
    options.podman.runtime_executable = env_or("POLARIS_PHYSICAL_CRUN", "/usr/bin/crun");
    options.podman.deployment_id = deployment;
    options.podman.ipc_root = root.path;
    for (int index = 0; index < 2; ++index) options.podman.profiles.push_back({
      .profile_key="physical-profile-"+std::to_string(index), .opaque_volume_name=volumes[index], .runtime_profile=profile, .image_reference=image});
    options.podman.workloads = {{.kind=kind, .target_id=workload}};
    ASSERT_TRUE(host.trusted_runtime_file(options.podman.runtime_executable));
    std::string command_failure, worker_diagnostics;
    production_controller_factories_t factories;
    factories.podman_host = [&] { return std::make_unique<observed_host_t>(command_failure, worker_diagnostics, game); };
    auto created = create_production_controller_runtime(std::move(options), std::move(factories));
    ASSERT_EQ(created.status, controller_runtime_create_status_e::ready_enabled);
    auto controller = std::move(created.runtime);
    ASSERT_TRUE(controller);
    std::array<seat_t, 2> seats;
    std::array<std::jthread, 2> games;
    std::array<podman::command_result_t, 2> game_results;
    std::array<std::atomic<bool>, 2> game_done {};
    const std::array game_tokens {nonce(), nonce()};
    constexpr auto game_probe = "/usr/bin/polaris-seat-worker";
    // Scoped failure recovery never scans another deployment or recursively
    // removes authority. A fallback is test failure, followed by exact-ID reap.
    auto cleanup = util::fail_guard([&] {
      for (auto &seat : seats) if (seat.stream) { stream::session::stop(*seat.stream); seat.stream.reset(); }
      for (int attempt = 0; attempt < 5 && !controller->closed(); ++attempt) {
        (void) controller->shutdown();
        if (!controller->closed()) std::this_thread::sleep_for(100ms);
      }
      if (!controller->closed()) {
        ADD_FAILURE() << "controller cleanup required exact-container fallback";
        for (const auto &seat : seats) if (container_id_valid(seat.container_id)) (void) command({"rm", "--force", "--", seat.container_id});
        (void) controller->reconcile();
        (void) controller->shutdown();
      }
      if (!worker_diagnostics.empty()) RecordProperty("worker_diagnostics", worker_diagnostics);
      // Destroying these exact workers interrupts failed probe execs. Join only
      // after that boundary, including on every assertion/exception path.
      for (int index = 0; index < 2; ++index) if (games[index].joinable()) {
        games[index].join();
        if (game_results[index].exit_status != 0) RecordProperty("game_failure_" + std::to_string(index), game_results[index].output);
      }
    });
    ASSERT_TRUE(controller->reconcile().ready()) << command_failure;
    for (int index = 0; index < 2; ++index) {
      seat_request_t request {
        .client_key="physical-client-"+std::to_string(index), .profile_key="physical-profile-"+std::to_string(index),
        .workload={.kind=kind, .target_id=workload}, .logical_gpu_id="physical-gpu", .runtime_profile=profile,
        .data_plane={.display_topology=display_topology_e::capture_host_with_nested_compositor, .media_pipeline=media_pipeline_e::worker_local_capture_encode},
        .display_mode={1920,1080,60000,false}, .requested_compositor=compositor_e::gamescope, .encoder_sessions=1};
      const auto admission = controller->admit(request);
      ASSERT_TRUE(admission.accepted()); ASSERT_TRUE(admission.seat);
      auto &seat = seats[index]; seat.snapshot = *admission.seat;
      ASSERT_LT((root.path / seat.snapshot.resources.runtime_namespace / "ipc/control.sock").native().size(), 108U) << "choose a shorter private IPC parent";
      ASSERT_EQ(controller->bind_runtime(seat.snapshot.handle, compositor_e::gamescope, "isolated input acceptance"), mutation_result_e::applied);
      const auto started = controller->start_seat(seat.snapshot.handle, {.gamepad_slots=1});
      ASSERT_TRUE(started.input.input.allocation);
      seat.allocation = *started.input.input.allocation;
      ASSERT_TRUE(started.started()) << "status=" << static_cast<int>(started.status) << " worker=" << (started.worker ? static_cast<int>(*started.worker) : -1) << " " << command_failure;
      const auto inspected = command({"inspect", "--format={{.Id}}", seat.snapshot.resources.worker_name});
      ASSERT_EQ(inspected.exit_status, 0) << inspected.output;
      auto candidate_id = inspected.output;
      while (!candidate_id.empty() && std::isspace(static_cast<unsigned char>(candidate_id.back()))) candidate_id.pop_back();
      ASSERT_TRUE(container_id_valid(candidate_id));
      seat.container_id = std::move(candidate_id);
    }
    ASSERT_NE(seats[0].container_id, seats[1].container_id);
    ASSERT_NE(seats[0].snapshot.resources.runtime_namespace, seats[1].snapshot.resources.runtime_namespace);
    ASSERT_NE(seats[0].snapshot.resources.audio_sink, seats[1].snapshot.resources.audio_sink);
    const auto deadline = std::chrono::steady_clock::now()+120s;
    bool selected = false;
    while (std::chrono::steady_clock::now()<deadline) {
      (void) controller->reconcile(); selected = true;
      for (int index=0; index<2; ++index) {
        auto &seat=seats[index];
        if (seat.stream) continue;
        if (!seat.launch) {
          seat.launch=std::make_shared<rtsp_stream::launch_session_t>();
          seat.launch->id=100+index; seat.launch->lifecycle_generation=200+index;
          seat.launch->unique_id="physical-client-"+std::to_string(index);
          seat.launch->device_name="isolated-input-proof"; seat.launch->session_token=nonce();
          seat.launch->gcm_key.resize(16); seat.launch->iv.resize(16);
          seat.launch->perm=static_cast<crypto::PERM>(static_cast<std::uint32_t>(crypto::PERM::input_kbd) | static_cast<std::uint32_t>(crypto::PERM::input_mouse) | static_cast<std::uint32_t>(crypto::PERM::input_controller)); seat.launch->watch_only=false;
        }
        if (!controller->select_authenticated_launch(seat.launch,seat.snapshot.handle).selected()) { selected=false; continue; }
        stream::config_t stream_config {};
        seat.stream=stream::session::alloc(stream_config,*seat.launch); ASSERT_TRUE(seat.stream);
        ASSERT_EQ(input::activate_registered_moonlight_session(*seat.stream),input::moonlight_session_activation_status_e::bound);
      }
      if (selected) break;
      std::this_thread::sleep_for(100ms);
    }
    ASSERT_TRUE(selected) << "workers did not reach authenticated input readiness";
    const auto game_state = [&](int index) -> std::optional<json> {
      auto result = command({"exec", seats[index].container_id, game_probe, "physical-game-probe", "state", game_tokens[index]}, 2s);
      if (result.exit_status != 0 || result.timed_out) return std::nullopt;
      auto state = json::parse(result.output, nullptr, false);
      if (!state.is_object() || state.size() != 5) return std::nullopt;
      for (const auto *key : {"keyboard", "pointer", "gamepad", "frames", "pid"})
        if (!state.contains(key) || !state[key].is_number_unsigned()) return std::nullopt;
      return state;
    };
    const auto finish_game = [&](int index) {
      EXPECT_EQ(command({"exec", seats[index].container_id, game_probe, "physical-game-probe", "finish", game_tokens[index]}, 2s).exit_status, 0);
      if (games[index].joinable()) games[index].join();
      EXPECT_FALSE(game_results[index].timed_out);
      EXPECT_EQ(game_results[index].exit_status, 0) << game_results[index].output;
    };
    if (game) {
      for (int index = 0; index < 2; ++index) games[index] = std::jthread([&, index] {
        game_results[index] = command({"exec", "--tty", seats[index].container_id, game_probe, "physical-game-probe", "start", game_tokens[index]}, 190s);
        game_done[index] = true;
      });
      bool ready = false;
      const auto ready_deadline = std::chrono::steady_clock::now() + 95s;
      while (std::chrono::steady_clock::now() < ready_deadline) {
        if (game_state(0) && game_state(1)) { ready = true; break; }
        if (game_done[0] || game_done[1]) break;
        std::this_thread::sleep_for(100ms);
      }
      ASSERT_TRUE(ready) << "two private games did not become observable";
      std::this_thread::sleep_for(200ms);
    }
    int observation_round = 0;
    const auto observe = [&](int target, bool both) {
      std::array<json, 2> game_before;
      if (game) for (int index = 0; index < 2; ++index) if (both || index == target) {
        auto state = game_state(index); ASSERT_TRUE(state); game_before[index] = *state;
      }
      std::array<podman::command_result_t,2> results;
      std::array<std::string,2> tokens {nonce(),nonce()};
      std::vector<std::jthread> readers;
      for (int index=0; index<2; ++index) {
        if (!both && index!=target) continue;
        json manifest {{"token",tokens[index]}, {"devices",json::array()}, {"absent",json::array()}};
        for (const auto &node: seats[index].allocation.nodes) manifest["devices"].push_back({{"path",node.worker_path.native()},{"major",node.character_major},{"minor",node.character_minor}});
        for (const auto &other: seats) for (const auto &node:other.allocation.nodes) manifest["absent"].push_back(node.host_path.native());
        readers.emplace_back([&,index,body=manifest.dump()] {
          results[index]=command({"exec","--tty",seats[index].container_id,probe,"observe",body},12s);
        });
      }
      bool ready=false;
      const auto deadline=std::chrono::steady_clock::now()+2s;
      while (std::chrono::steady_clock::now()<deadline) {
        ready=true;
        for (int index=0; index<2; ++index) if (both || index==target) {
          if (command({"exec",seats[index].container_id,probe,"ready",tokens[index]},1s).exit_status!=0) ready=false;
        }
        if (ready) break;
        std::this_thread::sleep_for(20ms);
      }
      EXPECT_TRUE(ready) << "input readers could not open every allocated node";
      if (ready) send_markers(*seats[target].stream, 1000 + 100 * observation_round++);
      // No observer can report success before this host signal, issued only
      // after every ready observer covered the injection boundary.
      for (int index=0; index<2; ++index) if (both || index==target) {
        EXPECT_EQ(command({"exec",seats[index].container_id,probe,"finish",tokens[index]},1s).exit_status,0);
      }
      readers.clear(); // join before parsing the completed bounded observation
      for (int index=0; index<2; ++index) if (both || index==target) {
        ASSERT_EQ(results[index].exit_status,0) << results[index].output;
        const auto observation=json::parse(results[index].output);
        RecordProperty("observation_" + std::to_string(observation_round) + "_seat_" + std::to_string(index), observation.dump());
        for (int device=0;device<4;++device) {
          if (index==target) EXPECT_GT(observation["markers"][device].get<int>(),0);
          else EXPECT_EQ(observation["events"][device].get<int>(),0);
        }
      }
      if (game) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        bool delivered = false;
        while (std::chrono::steady_clock::now() < deadline) {
          auto state = game_state(target);
          if (state && (*state)["keyboard"] > game_before[target]["keyboard"] &&
              (*state)["pointer"] > game_before[target]["pointer"] && (*state)["gamepad"] > game_before[target]["gamepad"]) { delivered = true; break; }
          std::this_thread::sleep_for(20ms);
        }
        EXPECT_TRUE(delivered) << "allocated events did not reach the private game";
        for (int index = 0; index < 2; ++index) if (both || index == target) {
          auto state = game_state(index); ASSERT_TRUE(state);
          EXPECT_EQ((*state)["pid"], game_before[index]["pid"]);
          EXPECT_GT((*state)["frames"], game_before[index]["frames"]);
          if (index != target) {
            for (const auto *key : {"keyboard", "pointer", "gamepad"}) { EXPECT_EQ((*state)[key], game_before[index][key]); }
          }
          RecordProperty("game_" + std::to_string(observation_round) + "_seat_" + std::to_string(index),
            json {{"before", game_before[index]}, {"after", *state}, {"target", index == target}}.dump());
        }
      }
    };
    observe(0,true);
    observe(1,true);
    if (game) finish_game(0);
    stream::session::stop(*seats[0].stream); seats[0].stream.reset();
    (void) controller->stop_seat(seats[0].snapshot.handle);
    const auto stop_deadline=std::chrono::steady_clock::now()+30s;
    while (controller->seats()>1 && std::chrono::steady_clock::now()<stop_deadline) {
      (void) controller->reconcile(); std::this_thread::sleep_for(100ms);
    }
    ASSERT_EQ(controller->seats(),1U);
    observe(1,false);
    if (game) finish_game(1);
    stream::session::stop(*seats[1].stream); seats[1].stream.reset();
    (void) controller->stop_seat(seats[1].snapshot.handle);
    for (int attempt=0;attempt<300 && controller->seats()!=0;++attempt) {
      (void) controller->reconcile(); std::this_thread::sleep_for(100ms);
    }
    EXPECT_EQ(controller->seats(),0U); EXPECT_EQ(controller->managed_workers(),0U);
    EXPECT_TRUE(controller->shutdown().closed()); EXPECT_EQ(controller->input_allocations(),0U);
    const auto listed=command({"ps","--all","--no-trunc","--filter=label=io.polaris.multiseat.deployment="+deployment,"--format={{.ID}}"});
    EXPECT_EQ(listed.exit_status,0); EXPECT_TRUE(listed.output.empty());
    EXPECT_TRUE(std::filesystem::is_empty(root.path));
    for (const auto &seat:seats) for (const auto &node:seat.allocation.nodes) {
      struct stat current {};
      if (lstat(node.host_path.c_str(),&current)==0) {
        EXPECT_FALSE(S_ISCHR(current.st_mode) && static_cast<std::uint64_t>(current.st_dev)==node.filesystem_device && static_cast<std::uint64_t>(current.st_ino)==node.inode);
      } else {
        EXPECT_EQ(errno, ENOENT);
      }
      const auto sysfs_name = std::filesystem::path {"/sys/dev/char"} /
        (std::to_string(node.character_major)+":"+std::to_string(node.character_minor)) / "device/name";
      std::error_code error;
      const auto exists = std::filesystem::exists(sysfs_name,error);
      EXPECT_FALSE(error);
      if (exists) {
        std::ifstream source {sysfs_name};
        std::string name;
        ASSERT_TRUE(static_cast<bool>(std::getline(source,name)));
        EXPECT_NE(name,node.kernel_name);
      }
    }
    EXPECT_TRUE(root.remove_empty()) << "private authority root was not removed";
  }
}
#endif
