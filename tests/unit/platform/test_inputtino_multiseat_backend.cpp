/**
 * @file tests/unit/platform/test_inputtino_multiseat_backend.cpp
 * @brief Offline tests for trusted host multiseat input lifecycle.
 */
#include "src/platform/linux/input/inputtino_multiseat_backend.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
  using namespace multiseat::input;

  multiseat::seat_handle_t handle_for(
    std::uint32_t slot,
    std::uint64_t generation
  ) {
    return {
      .controller_epoch = "controller-a",
      .logical_gpu_id = "gpu-primary",
      .slot = slot,
      .generation = generation,
    };
  }

  expectation_t expectation_for(
    std::uint32_t slot,
    std::uint64_t generation,
    plan_t plan = {}
  ) {
    return {
      .handle = handle_for(slot, generation),
      .input_seat = "polaris-input-controller-a-" + std::to_string(generation),
      .plan = plan,
    };
  }

  class fake_kernel_io_t final : public kernel_node_io_t {
  public:
    input_node_read_t inspect_input_node(
      const std::filesystem::path &path
    ) override {
      const auto sequence = node_sequences.find(path);
      if (sequence != node_sequences.end() && !sequence->second.empty()) {
        auto result = sequence->second.front();
        sequence->second.erase(sequence->second.begin());
        return result;
      }
      const auto found = nodes.find(path);
      return found == nodes.end() ?
               input_node_read_t {.status = node_io_status_e::absent} :
               found->second;
    }

    trusted_text_read_t read_trusted_text(
      const std::filesystem::path &path,
      std::size_t maximum_bytes
    ) override {
      requested_text_limits.push_back(maximum_bytes);
      const auto found = text.find(path);
      return found == text.end() ?
               trusted_text_read_t {.status = node_io_status_e::absent} :
               found->second;
    }

    void install(
      const std::filesystem::path &path,
      std::uint32_t character_minor,
      std::string kernel_name,
      std::optional<std::string> phys = std::string {},
      std::optional<std::string> udev_seat = std::string {isolated_host_seat}
    ) {
      nodes[path] = {
        .status = node_io_status_e::ok,
        .metadata = input_node_metadata_t {
          .filesystem_device = 41,
          .inode = 10000 + character_minor,
          .character_major = 13,
          .character_minor = character_minor,
          .character_device = true,
        },
      };
      const auto filename = path.filename();
      const auto sysfs = std::filesystem::path {"/sys/class/input"} / filename;
      text[sysfs / "dev"] = {
        .status = node_io_status_e::ok,
        .text = "13:" + std::to_string(character_minor) + "\n",
      };
      text[sysfs / "device/name"] = {
        .status = node_io_status_e::ok,
        .text = std::move(kernel_name) + "\n",
      };
      if (phys) {
        text[sysfs / "device/phys"] = {
          .status = node_io_status_e::ok,
          .text = *phys + "\n",
        };
      }
      std::string udev = "E:OTHER=value\n";
      if (udev_seat) {
        udev += "E:ID_SEAT=" + *udev_seat + "\n";
      }
      text[std::filesystem::path {"/run/udev/data"} /
           ("c13:" + std::to_string(character_minor))] = {
        .status = node_io_status_e::ok,
        .text = std::move(udev),
      };
    }

    std::map<std::filesystem::path, input_node_read_t> nodes;
    std::map<std::filesystem::path, std::vector<input_node_read_t>> node_sequences;
    std::map<std::filesystem::path, trusted_text_read_t> text;
    std::vector<std::size_t> requested_text_limits;
  };

  TEST(LinuxKernelNodeProbe, DerivesExactEventAndJoystickIdentityFromInjectedIo) {
    fake_kernel_io_t io;
    io.install(
      "/dev/input/event0",
      64,
      "Polaris multiseat abc keyboard",
      "polaris/client-input-seat-isolated/a/keyboard"
    );
    io.install(
      "/dev/input/js3",
      3,
      "Polaris multiseat abc gamepad-0",
      std::nullopt
    );
    linux_kernel_node_probe_t probe {io};

    const auto event = probe.observe("/dev/input/event0");
    ASSERT_EQ(event.status, node_observation_status_e::observed);
    ASSERT_TRUE(event.snapshot.has_value());
    EXPECT_EQ(event.snapshot->character_major, 13U);
    EXPECT_EQ(event.snapshot->character_minor, 64U);
    EXPECT_EQ(event.snapshot->kernel_name, "Polaris multiseat abc keyboard");
    EXPECT_EQ(
      event.snapshot->phys,
      "polaris/client-input-seat-isolated/a/keyboard"
    );
    EXPECT_EQ(event.snapshot->host_seat, isolated_host_seat);

    const auto joystick = probe.observe("/dev/input/js3");
    ASSERT_EQ(joystick.status, node_observation_status_e::observed);
    ASSERT_TRUE(joystick.snapshot.has_value());
    EXPECT_EQ(joystick.snapshot->character_minor, 3U);
    EXPECT_TRUE(joystick.snapshot->phys.empty());
    EXPECT_TRUE(std::all_of(
      io.requested_text_limits.begin(),
      io.requested_text_limits.end(),
      [](auto limit) {
        return limit == maximum_kernel_metadata_bytes;
      }
    ));
  }

  TEST(LinuxKernelNodeProbe, AcceptsDynamicKernelMinorsAndRejectsTheStaticGap) {
    fake_kernel_io_t io;
    io.install("/dev/input/event256", 256, "Polaris multiseat abc keyboard");
    io.install("/dev/input/event31", 95, "Polaris multiseat abc mouse");
    io.install("/dev/input/event40", 104, "Polaris multiseat abc touch");
    io.install("/dev/input/js16", 16, "Polaris multiseat abc gamepad-0", std::nullopt);
    io.install("/dev/input/js256", 256, "Polaris multiseat abc gamepad-0", std::nullopt);
    linux_kernel_node_probe_t probe {io};

    const auto dynamic_event = probe.observe("/dev/input/event256");
    ASSERT_EQ(dynamic_event.status, node_observation_status_e::observed);
    ASSERT_TRUE(dynamic_event.snapshot.has_value());
    EXPECT_EQ(dynamic_event.snapshot->character_minor, 256U);

    const auto last_static = probe.observe("/dev/input/event31");
    ASSERT_EQ(last_static.status, node_observation_status_e::observed);
    EXPECT_EQ(last_static.snapshot->character_minor, 95U);

    EXPECT_EQ(
      probe.observe("/dev/input/event40").status,
      node_observation_status_e::unsafe
    );
    EXPECT_EQ(
      probe.observe("/dev/input/js16").status,
      node_observation_status_e::unsafe
    );
    const auto dynamic_joystick = probe.observe("/dev/input/js256");
    ASSERT_EQ(dynamic_joystick.status, node_observation_status_e::observed);
    EXPECT_EQ(dynamic_joystick.snapshot->character_minor, 256U);
  }

  TEST(LinuxKernelNodeProbe, RejectsNoncanonicalOrContradictoryKernelIdentity) {
    fake_kernel_io_t io;
    io.install("/dev/input/event2", 66, "Polaris multiseat abc keyboard");
    linux_kernel_node_probe_t probe {io};

    EXPECT_EQ(
      probe.observe("/dev/input/event02").status,
      node_observation_status_e::unsafe
    );
    EXPECT_EQ(
      probe.observe("/dev/uinput").status,
      node_observation_status_e::unsafe
    );

    io.nodes["/dev/input/event2"].metadata->character_minor = 67;
    auto result = probe.observe("/dev/input/event2");
    EXPECT_EQ(result.status, node_observation_status_e::unsafe);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_EQ(result.snapshot->character_minor, 67U);

    io.nodes["/dev/input/event2"].metadata->character_minor = 66;
    io.text["/sys/class/input/event2/dev"].text = "13:67\n";
    EXPECT_EQ(
      probe.observe("/dev/input/event2").status,
      node_observation_status_e::unsafe
    );

    io.text["/sys/class/input/event2/dev"].text = "13:66\n";
    io.text["/sys/class/input/event2/device/name"].status =
      node_io_status_e::unsafe;
    result = probe.observe("/dev/input/event2");
    EXPECT_EQ(result.status, node_observation_status_e::unsafe);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_EQ(result.snapshot->inode, 10066U);
  }

  TEST(LinuxKernelNodeProbe, TreatsUdevLagAsRetryableAndMissingSeatAsSeat0) {
    fake_kernel_io_t io;
    io.install(
      "/dev/input/event4",
      68,
      "Polaris multiseat abc keyboard",
      std::string {},
      std::nullopt
    );
    linux_kernel_node_probe_t probe {io};

    auto result = probe.observe("/dev/input/event4");
    ASSERT_EQ(result.status, node_observation_status_e::observed);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_EQ(result.snapshot->host_seat, "seat0");

    io.text.erase("/run/udev/data/c13:68");
    result = probe.observe("/dev/input/event4");
    EXPECT_EQ(result.status, node_observation_status_e::retryable);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_EQ(result.snapshot->filesystem_device, 41U);

    io.text["/run/udev/data/c13:68"] = {
      .status = node_io_status_e::ok,
      .text = "E:ID_SEAT=seat-polaris\nE:ID_SEAT=seat-polaris\n",
    };
    EXPECT_EQ(
      probe.observe("/dev/input/event4").status,
      node_observation_status_e::unsafe
    );
  }

  TEST(LinuxKernelNodeProbe, RejectsPathReplacementDuringMetadataReadback) {
    fake_kernel_io_t io;
    io.install("/dev/input/event6", 70, "Polaris multiseat abc keyboard");
    const auto initial = io.nodes["/dev/input/event6"];
    auto replacement = initial;
    ASSERT_TRUE(replacement.metadata.has_value());
    ++replacement.metadata->inode;
    io.node_sequences["/dev/input/event6"] = {initial, replacement};
    linux_kernel_node_probe_t probe {io};

    const auto result = probe.observe("/dev/input/event6");
    EXPECT_EQ(result.status, node_observation_status_e::unsafe);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_EQ(result.snapshot->inode, initial.metadata->inode);
  }

  TEST(PosixKernelNodeIo, RejectsUnsafeTextAndFinalSymlinkOffline) {
    auto pattern = (
      std::filesystem::temp_directory_path() /
      "polaris-kernel-node-io-XXXXXX"
    ).native();
    std::vector<char> writable_pattern {pattern.begin(), pattern.end()};
    writable_pattern.push_back('\0');
    const auto created = ::mkdtemp(writable_pattern.data());
    ASSERT_NE(created, nullptr);
    const auto root = std::filesystem::path {created};
    struct cleanup_t {
      std::filesystem::path root;
      ~cleanup_t() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
      }
    } cleanup {root};

    const auto writable = root / "writable";
    {
      std::ofstream output {writable};
      ASSERT_TRUE(output.good());
      output << "trusted-looking text";
    }
    std::error_code error;
    std::filesystem::permissions(
      writable,
      std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write |
        std::filesystem::perms::group_write,
      std::filesystem::perm_options::replace,
      error
    );
    ASSERT_FALSE(error);
    const auto symlink = root / "final-symlink";
    std::filesystem::create_symlink(writable, symlink, error);
    ASSERT_FALSE(error);

    posix_kernel_node_io_t io;
    EXPECT_EQ(
      io.inspect_input_node(root / "missing").status,
      node_io_status_e::absent
    );
    const auto regular = io.inspect_input_node(writable);
    ASSERT_EQ(regular.status, node_io_status_e::ok);
    ASSERT_TRUE(regular.metadata.has_value());
    EXPECT_FALSE(regular.metadata->character_device);
    EXPECT_EQ(
      io.read_trusted_text(writable, maximum_kernel_metadata_bytes).status,
      node_io_status_e::unsafe
    );
    EXPECT_EQ(
      io.read_trusted_text(symlink, maximum_kernel_metadata_bytes).status,
      node_io_status_e::unsafe
    );
    EXPECT_EQ(
      io.read_trusted_text(writable, 0).status,
      node_io_status_e::unsafe
    );
  }

  class fake_kernel_probe_t final : public kernel_node_probe_t {
  public:
    node_observation_t observe(const std::filesystem::path &path) override {
      const auto found = observations.find(path);
      return found == observations.end() ?
               node_observation_t {.status = node_observation_status_e::absent} :
               found->second;
    }

    void make_absent(const std::filesystem::path &path) {
      observations[path] = {.status = node_observation_status_e::absent};
    }

    void make_all_absent() {
      for (auto &[path, observation] : observations) {
        (void) path;
        observation = {.status = node_observation_status_e::absent};
      }
    }

    std::map<std::filesystem::path, node_observation_t> observations;
  };

  class fake_managed_device_t final : public managed_device_t {
  public:
    fake_managed_device_t(
      std::vector<std::filesystem::path> nodes,
      std::function<managed_device_apply_result_e(const input_event_t &)> on_apply,
      std::function<void()> on_destroy
    ):
        nodes_(std::move(nodes)),
        on_apply_(std::move(on_apply)),
        on_destroy_(std::move(on_destroy)) {
    }

    ~fake_managed_device_t() override {
      on_destroy_();
    }

    std::vector<std::filesystem::path> nodes() const override {
      if (throw_nodes) {
        throw std::runtime_error {"injected node enumeration failure"};
      }
      return nodes_;
    }

    managed_device_apply_result_e apply(const input_event_t &event) override {
      return on_apply_(event);
    }

    bool throw_nodes = false;

  private:
    std::vector<std::filesystem::path> nodes_;
    std::function<managed_device_apply_result_e(const input_event_t &)> on_apply_;
    std::function<void()> on_destroy_;
  };

  class fake_device_factory_t final : public device_factory_t {
  public:
    struct apply_call_t {
      device_spec_t spec;
      input_event_t event;

      bool operator==(const apply_call_t &) const = default;
    };

    explicit fake_device_factory_t(fake_kernel_probe_t &probe) :
        probe(probe) {
    }

    std::unique_ptr<managed_device_t> create(
      const device_spec_t &spec,
      managed_device_feedback_fn_t feedback
    ) override {
      const auto call = create_calls++;
      if (fail_at_call && call == *fail_at_call) {
        return {};
      }
      created_specs.push_back(spec);
      feedback_callbacks.push_back(std::move(feedback));
      std::vector<std::filesystem::path> paths;
      for (const auto &event : spec.event_nodes) {
        const auto index = next_event++;
        const auto path = std::filesystem::path {
          "/dev/input/event" + std::to_string(index)
        };
        install_snapshot(
          path,
          64 + index,
          name_override.value_or(event.kernel_name),
          spec
        );
        paths.push_back(path);
      }
      if (spec.permits_joystick_node && include_joystick_nodes) {
        const auto index = next_joystick++;
        const auto path = std::filesystem::path {
          "/dev/input/js" + std::to_string(index)
        };
        install_snapshot(
          path,
          index,
          name_override.value_or(spec.kernel_name),
          spec
        );
        paths.push_back(path);
      }
      const auto created_paths = paths;
      return std::make_unique<fake_managed_device_t>(
        std::move(paths),
        [this, spec](const input_event_t &event) {
          if (throw_apply) {
            throw std::runtime_error {"injected input apply failure"};
          }
          apply_calls.push_back({.spec = spec, .event = event});
          return next_apply;
        },
        [this, created_paths]() {
          if (!retain_nodes_on_destroy) {
            for (const auto &path : created_paths) {
              probe.make_absent(path);
            }
          }
        }
      );
    }

    void install_snapshot(
      const std::filesystem::path &path,
      std::uint32_t minor,
      std::string kernel_name,
      const device_spec_t &spec
    ) {
      probe.observations[path] = {
        .status = node_observation_status_e::observed,
        .snapshot = kernel_node_snapshot_t {
          .host_path = path,
          .filesystem_device = 41,
          .inode = next_inode++,
          .character_major = 13,
          .character_minor = minor,
          .kernel_name = std::move(kernel_name),
          .phys = phys_override.value_or(
            carry_phys ? spec.expected_phys : std::string {}
          ),
          .host_seat = seat_override.value_or(std::string {isolated_host_seat}),
        },
      };
    }

    fake_kernel_probe_t &probe;
    std::vector<device_spec_t> created_specs;
    std::vector<managed_device_feedback_fn_t> feedback_callbacks;
    std::vector<apply_call_t> apply_calls;
    std::optional<std::size_t> fail_at_call;
    std::optional<std::string> name_override;
    std::optional<std::string> phys_override;
    std::optional<std::string> seat_override;
    std::uint32_t next_event = 0;
    std::uint32_t next_joystick = 0;
    std::uint64_t next_inode = 10000;
    std::size_t create_calls = 0;
    managed_device_apply_result_e next_apply =
      managed_device_apply_result_e::applied;
    bool carry_phys = false;
    bool include_joystick_nodes = true;
    bool retain_nodes_on_destroy = false;
    bool throw_apply = false;
  };

  inputtino_host_backend_options_t fast_options() {
    return {
      .discovery_attempts = 2,
      .cleanup_attempts = 2,
      .stable_observations = 2,
      .retry_delay = std::chrono::milliseconds::zero(),
    };
  }

  TEST(InputtinoMultiseatBackend, BuildsBoundedOpaqueInputtinoPolicyForFullPlan) {
    const auto expectation = expectation_for(
      0,
      77,
      {.touch = true, .pen = true, .gamepad_slots = 2}
    );
    const auto specs = build_device_specs(expectation);
    ASSERT_TRUE(specs.has_value());
    ASSERT_EQ(specs->size(), 6U);
    EXPECT_EQ((*specs)[0].kind, managed_device_kind_e::keyboard);
    EXPECT_EQ((*specs)[1].kind, managed_device_kind_e::mouse);
    ASSERT_EQ((*specs)[1].event_nodes.size(), 2U);
    EXPECT_EQ(
      (*specs)[1].event_nodes[0].kind,
      device_kind_e::mouse_relative
    );
    EXPECT_EQ(
      (*specs)[1].event_nodes[1].kind,
      device_kind_e::mouse_absolute
    );
    EXPECT_EQ((*specs)[4].vendor_id, 0x045E);
    EXPECT_EQ((*specs)[4].product_id, 0x02EA);
    EXPECT_TRUE((*specs)[4].permits_joystick_node);
    for (const auto &spec : *specs) {
      EXPECT_LE(spec.kernel_name.size(), maximum_kernel_device_name_bytes);
      EXPECT_EQ(spec.kernel_name.find(multiseat_kernel_device_prefix), 0U);
      EXPECT_EQ(spec.kernel_name.find(expectation.input_seat), std::string::npos);
      EXPECT_FALSE(spec.expected_phys.empty());
    }

    auto invalid = expectation;
    invalid.plan.gamepad_slots = maximum_gamepad_slots + 1;
    EXPECT_FALSE(build_device_specs(invalid).has_value());
    invalid = expectation;
    invalid.input_seat = "../seat";
    EXPECT_FALSE(build_device_specs(invalid).has_value());
  }

  TEST(InputtinoMultiseatBackend, CreatesExactManifestWithoutExposingJoystickNode) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    std::size_t waits = 0;
    inputtino_host_backend_t backend {
      factory,
      probe,
      fast_options(),
      [&waits](auto) {
        ++waits;
      }
    };
    const auto expectation = expectation_for(0, 1);

    const auto created = backend.create(expectation);
    ASSERT_EQ(created.result, backend_result_e::applied);
    ASSERT_TRUE(created.allocation.has_value());
    EXPECT_TRUE(valid_allocation(*created.allocation, expectation));
    EXPECT_EQ(factory.created_specs.size(), 3U);
    ASSERT_EQ(created.allocation->nodes.size(), 4U);
    EXPECT_EQ(created.allocation->nodes[1].kind, device_kind_e::mouse_relative);
    EXPECT_EQ(created.allocation->nodes[2].kind, device_kind_e::mouse_absolute);
    for (const auto &node : created.allocation->nodes) {
      EXPECT_EQ(node.host_path.native().find("/dev/input/event"), 0U);
      EXPECT_TRUE(node.phys.empty());
      EXPECT_EQ(node.host_seat, isolated_host_seat);
    }
    EXPECT_GE(waits, 1U);
    EXPECT_EQ(backend.inventory(), std::vector<allocation_t> {*created.allocation});
    const input_event_t key_down {
      .payload = keyboard_key_event_t {
        .key_code = 0x41,
        .state = button_state_e::pressed,
      },
    };
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 1, key_down),
      backend_result_e::applied
    );
    EXPECT_EQ(
      backend.destroy(expectation.handle, expectation.input_seat),
      backend_result_e::applied
    );
    EXPECT_TRUE(backend.inventory().empty());
  }

  TEST(InputtinoMultiseatBackend, KeepsTwoSeatIdentityAndLifecycleDisjoint) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    const auto first = expectation_for(0, 1);
    const auto second = expectation_for(1, 2, {.gamepad_slots = 2});

    const auto first_result = backend.create(first);
    const auto second_result = backend.create(second);
    ASSERT_TRUE(first_result.allocation.has_value());
    ASSERT_TRUE(second_result.allocation.has_value());
    for (const auto &first_node : first_result.allocation->nodes) {
      for (const auto &second_node : second_result.allocation->nodes) {
        EXPECT_NE(first_node.host_path, second_node.host_path);
        EXPECT_NE(first_node.kernel_name, second_node.kernel_name);
        EXPECT_NE(first_node.inode, second_node.inode);
      }
    }
    EXPECT_EQ(backend.inventory().size(), 2U);
    EXPECT_EQ(
      backend.destroy(first.handle, first.input_seat),
      backend_result_e::applied
    );
    const auto inventory = backend.inventory();
    ASSERT_EQ(inventory.size(), 1U);
    EXPECT_EQ(inventory[0].handle, second.handle);
  }

  TEST(InputtinoMultiseatBackend, ConcurrentCreatesRemainSerializedAndDisjoint) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    std::vector<std::future<backend_create_result_t>> futures;
    for (std::uint32_t slot = 0; slot < 8; ++slot) {
      futures.push_back(std::async(std::launch::async, [&backend, slot]() {
        return backend.create(expectation_for(slot, slot + 1));
      }));
    }
    for (auto &future : futures) {
      const auto result = future.get();
      EXPECT_EQ(result.result, backend_result_e::applied);
      EXPECT_TRUE(result.allocation.has_value());
    }
    const auto inventory = backend.inventory();
    ASSERT_EQ(inventory.size(), 8U);
    for (std::size_t index = 0; index < inventory.size(); ++index) {
      for (std::size_t other = index + 1; other < inventory.size(); ++other) {
        for (const auto &node : inventory[index].nodes) {
          for (const auto &other_node : inventory[other].nodes) {
            EXPECT_NE(node.host_path, other_node.host_path);
            EXPECT_NE(node.inode, other_node.inode);
            EXPECT_NE(node.kernel_name, other_node.kernel_name);
          }
        }
      }
    }
  }

  TEST(InputtinoMultiseatBackend, RejectsUnisolatedOrContradictoryCreationAndCleans) {
    for (const auto mutation : {"seat", "name", "phys"}) {
      SCOPED_TRACE(mutation);
      fake_kernel_probe_t probe;
      fake_device_factory_t factory {probe};
      if (std::string_view {mutation} == "seat") {
        factory.seat_override = "seat0";
      } else if (std::string_view {mutation} == "name") {
        factory.name_override = "untrusted device";
      } else {
        factory.phys_override = "untrusted/phys";
      }
      inputtino_host_backend_t backend {factory, probe, fast_options()};
      const auto expectation = expectation_for(
        0,
        1,
        {.gamepad_slots = 0}
      );
      const auto result = backend.create(expectation);
      EXPECT_EQ(result.result, backend_result_e::rejected);
      EXPECT_FALSE(result.allocation.has_value());
      EXPECT_TRUE(backend.inventory().empty());
      EXPECT_TRUE(std::all_of(
        probe.observations.begin(),
        probe.observations.end(),
        [](const auto &entry) {
          return entry.second.status == node_observation_status_e::absent;
        }
      ));
    }
  }

  TEST(InputtinoMultiseatBackend, PartialFactoryFailureIsRolledBack) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    factory.fail_at_call = 2;
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    const auto expectation = expectation_for(0, 1);

    const auto result = backend.create(expectation);
    EXPECT_EQ(result.result, backend_result_e::rejected);
    EXPECT_FALSE(result.allocation.has_value());
    EXPECT_EQ(factory.create_calls, 3U);
    EXPECT_TRUE(backend.inventory().empty());
  }

  TEST(InputtinoMultiseatBackend, PersistentTeardownBecomesAReconciliationTombstone) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    factory.retain_nodes_on_destroy = true;
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    const auto expectation = expectation_for(0, 1);
    ASSERT_EQ(backend.create(expectation).result, backend_result_e::applied);

    EXPECT_EQ(
      backend.destroy(expectation.handle, expectation.input_seat),
      backend_result_e::indeterminate
    );
    EXPECT_THROW(backend.inventory(), std::runtime_error);
    EXPECT_EQ(
      backend.create(expectation_for(1, 2)).result,
      backend_result_e::indeterminate
    );

    probe.make_all_absent();
    EXPECT_EQ(
      backend.destroy(expectation.handle, expectation.input_seat),
      backend_result_e::already_applied
    );
    EXPECT_TRUE(backend.inventory().empty());
  }

  TEST(InputtinoMultiseatBackend, InventoryRejectsAReplacedKernelNode) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    const auto expectation = expectation_for(0, 1);
    const auto created = backend.create(expectation);
    ASSERT_TRUE(created.allocation.has_value());
    const auto path = created.allocation->nodes.front().host_path;
    ASSERT_TRUE(probe.observations[path].snapshot.has_value());
    ++probe.observations[path].snapshot->inode;

    EXPECT_THROW(backend.inventory(), std::runtime_error);
  }

  TEST(InputtinoMultiseatBackend, RoutesEveryTypedEventToOnlyItsManagedHandle) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    const auto expectation = expectation_for(
      0,
      40,
      {.touch = true, .pen = true, .gamepad_slots = 2}
    );
    ASSERT_EQ(backend.create(expectation).result, backend_result_e::applied);

    const std::vector<input_event_t> events {
      {
        .payload = keyboard_key_event_t {
          .key_code = 0x41,
          .state = button_state_e::pressed,
        },
      },
      {
        .payload = mouse_relative_event_t {.delta_x = 5, .delta_y = -7},
      },
      {
        .payload = mouse_absolute_event_t {
          .x = 1280,
          .y = 720,
          .width = 2560,
          .height = 1440,
        },
      },
      {
        .payload = mouse_button_event_t {
          .button = mouse_button_e::left,
          .state = button_state_e::pressed,
        },
      },
      {
        .payload = mouse_scroll_event_t {.vertical = 120, .horizontal = -120},
      },
      {
        .payload = touch_contact_event_t {
          .pointer_id = 9,
          .action = touch_action_e::down,
          .orientation_degrees = 30,
          .x = 100,
          .y = 200,
          .pressure = 300,
        },
      },
      {
        .payload = touch_contact_event_t {
          .pointer_id = 9,
          .action = touch_action_e::move,
          .orientation_degrees = 31,
          .x = 101,
          .y = 201,
          .pressure = 301,
        },
      },
      {
        .payload = touch_contact_event_t {
          .pointer_id = 9,
          .action = touch_action_e::release,
        },
      },
      {
        .payload = pen_tool_event_t {
          .proximity = pen_proximity_e::contact,
          .tool = pen_tool_e::pen,
          .buttons = 1,
          .x = 32000,
          .y = 16000,
          .pressure_or_distance = 8000,
          .tilt_x_degrees = -20,
          .tilt_y_degrees = 40,
        },
      },
      {
        .slot = 1,
        .payload = gamepad_state_event_t {
          .buttons = 0x1010,
          .left_trigger = 10,
          .right_trigger = 20,
          .left_stick_x = -30,
          .left_stick_y = 40,
          .right_stick_x = -50,
          .right_stick_y = 60,
        },
      },
    };

    for (std::size_t index = 0; index < events.size(); ++index) {
      EXPECT_EQ(
        backend.route(
          expectation.handle,
          expectation.input_seat,
          index + 1,
          events[index]
        ),
        backend_result_e::applied
      );
    }
    ASSERT_EQ(factory.apply_calls.size(), events.size());
    for (std::size_t index = 0; index < events.size(); ++index) {
      EXPECT_EQ(factory.apply_calls[index].event, events[index]);
    }
    EXPECT_EQ(factory.apply_calls[0].spec.kind, managed_device_kind_e::keyboard);
    EXPECT_EQ(factory.apply_calls[1].spec.kind, managed_device_kind_e::mouse);
    EXPECT_EQ(factory.apply_calls[5].spec.kind, managed_device_kind_e::touch);
    EXPECT_EQ(factory.apply_calls[8].spec.kind, managed_device_kind_e::pen);
    EXPECT_EQ(factory.apply_calls[9].spec.kind, managed_device_kind_e::gamepad);
    EXPECT_EQ(factory.apply_calls[9].spec.slot, 1U);
  }

  TEST(InputtinoMultiseatBackend, RouteFencesGenerationSeatSequenceAndState) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    const auto expectation = expectation_for(0, 50);
    ASSERT_EQ(backend.create(expectation).result, backend_result_e::applied);
    const input_event_t key_down {
      .payload = keyboard_key_event_t {
        .key_code = 0x41,
        .state = button_state_e::pressed,
      },
    };
    const input_event_t key_up {
      .payload = keyboard_key_event_t {
        .key_code = 0x41,
        .state = button_state_e::released,
      },
    };
    const input_event_t movement {
      .payload = mouse_relative_event_t {.delta_x = 1},
    };

    EXPECT_EQ(
      backend.route(handle_for(0, 49), expectation.input_seat, 1, key_down),
      backend_result_e::not_found
    );
    EXPECT_EQ(
      backend.route(expectation.handle, "wrong-seat", 1, key_down),
      backend_result_e::rejected
    );
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 2, key_down),
      backend_result_e::rejected
    );
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 1, key_down),
      backend_result_e::applied
    );
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 2, key_down),
      backend_result_e::rejected
    );
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 2, key_up),
      backend_result_e::applied
    );
    EXPECT_EQ(
      backend.route(
        expectation.handle,
        expectation.input_seat,
        3,
        input_event_t {
          .slot = 1,
          .payload = gamepad_state_event_t {},
        }
      ),
      backend_result_e::rejected
    );
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 3, movement),
      backend_result_e::applied
    );
    EXPECT_EQ(factory.apply_calls.size(), 3U);
  }

  TEST(InputtinoMultiseatBackend, BoundsTouchStateBeforeCallingInputtino) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    const auto expectation = expectation_for(
      0,
      60,
      {.touch = true, .gamepad_slots = 0}
    );
    ASSERT_EQ(backend.create(expectation).result, backend_result_e::applied);

    for (std::uint32_t pointer = 0; pointer < maximum_touch_contacts; ++pointer) {
      EXPECT_EQ(
        backend.route(
          expectation.handle,
          expectation.input_seat,
          pointer + 1,
          input_event_t {
            .payload = touch_contact_event_t {
              .pointer_id = pointer,
              .action = touch_action_e::down,
              .x = 100,
              .y = 200,
            },
          }
        ),
        backend_result_e::applied
      );
    }
    const auto next_sequence = maximum_touch_contacts + 1;
    EXPECT_EQ(
      backend.route(
        expectation.handle,
        expectation.input_seat,
        next_sequence,
        input_event_t {
          .payload = touch_contact_event_t {
            .pointer_id = 99,
            .action = touch_action_e::down,
          },
        }
      ),
      backend_result_e::rejected
    );
    EXPECT_EQ(
      backend.route(
        expectation.handle,
        expectation.input_seat,
        next_sequence,
        input_event_t {
          .payload = touch_contact_event_t {
            .pointer_id = 99,
            .action = touch_action_e::release,
          },
        }
      ),
      backend_result_e::rejected
    );
    EXPECT_EQ(
      backend.route(
        expectation.handle,
        expectation.input_seat,
        next_sequence,
        input_event_t {
          .payload = touch_contact_event_t {
            .pointer_id = 0,
            .action = touch_action_e::release,
          },
        }
      ),
      backend_result_e::applied
    );
    EXPECT_EQ(
      backend.route(
        expectation.handle,
        expectation.input_seat,
        next_sequence + 1,
        input_event_t {
          .payload = touch_contact_event_t {
            .pointer_id = 99,
            .action = touch_action_e::down,
          },
        }
      ),
      backend_result_e::applied
    );
    EXPECT_EQ(factory.apply_calls.size(), maximum_touch_contacts + 2U);
  }

  TEST(InputtinoMultiseatBackend, IndeterminateApplyPoisonsRoutingUntilTeardown) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    inputtino_host_backend_t backend {factory, probe, fast_options()};
    const auto expectation = expectation_for(0, 70);
    ASSERT_EQ(backend.create(expectation).result, backend_result_e::applied);
    const input_event_t movement {
      .payload = mouse_relative_event_t {.delta_x = 1},
    };

    factory.next_apply = managed_device_apply_result_e::rejected;
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 1, movement),
      backend_result_e::rejected
    );
    factory.next_apply = managed_device_apply_result_e::applied;
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 1, movement),
      backend_result_e::applied
    );
    factory.throw_apply = true;
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 2, movement),
      backend_result_e::indeterminate
    );
    factory.throw_apply = false;
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 2, movement),
      backend_result_e::indeterminate
    );
    EXPECT_THROW(backend.inventory(), std::runtime_error);
    EXPECT_EQ(
      backend.create(expectation).result,
      backend_result_e::indeterminate
    );
    EXPECT_EQ(
      backend.destroy(expectation.handle, expectation.input_seat),
      backend_result_e::applied
    );
  }

  TEST(InputtinoMultiseatBackend, FeedbackIsGenerationStampedAndOldCallbacksClose) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    std::vector<controller_feedback_t> feedback;
    inputtino_host_backend_t backend {
      factory,
      probe,
      fast_options(),
      {},
      [&feedback](const controller_feedback_t &packet) {
        feedback.push_back(packet);
      }
    };
    const auto first = expectation_for(0, 80, {.gamepad_slots = 2});
    ASSERT_EQ(backend.create(first).result, backend_result_e::applied);
    ASSERT_EQ(factory.feedback_callbacks.size(), factory.created_specs.size());

    std::size_t keyboard_callback = factory.feedback_callbacks.size();
    std::size_t gamepad_zero_callback = factory.feedback_callbacks.size();
    std::size_t gamepad_one_callback = factory.feedback_callbacks.size();
    for (std::size_t index = 0; index < factory.created_specs.size(); ++index) {
      const auto &spec = factory.created_specs[index];
      if (spec.kind == managed_device_kind_e::keyboard) {
        keyboard_callback = index;
      } else if (spec.kind == managed_device_kind_e::gamepad && spec.slot == 0) {
        gamepad_zero_callback = index;
      } else if (spec.kind == managed_device_kind_e::gamepad && spec.slot == 1) {
        gamepad_one_callback = index;
      }
    }
    ASSERT_LT(keyboard_callback, factory.feedback_callbacks.size());
    ASSERT_LT(gamepad_zero_callback, factory.feedback_callbacks.size());
    ASSERT_LT(gamepad_one_callback, factory.feedback_callbacks.size());

    factory.feedback_callbacks[gamepad_one_callback]({
      .kind = feedback_kind_e::rumble,
      .gamepad_slot = 1,
      .low_frequency = 10,
      .high_frequency = 20,
    });
    factory.feedback_callbacks[gamepad_zero_callback]({
      .kind = feedback_kind_e::rumble,
      .gamepad_slot = 0,
      .low_frequency = 30,
      .high_frequency = 40,
    });
    factory.feedback_callbacks[gamepad_zero_callback]({
      .kind = feedback_kind_e::rumble,
      .gamepad_slot = 1,
      .low_frequency = 50,
      .high_frequency = 60,
    });
    factory.feedback_callbacks[keyboard_callback]({
      .kind = feedback_kind_e::rumble,
      .gamepad_slot = 0,
      .low_frequency = 70,
      .high_frequency = 80,
    });
    ASSERT_EQ(feedback.size(), 2U);
    EXPECT_EQ(feedback[0].handle, first.handle);
    EXPECT_EQ(feedback[0].sequence, 1U);
    EXPECT_EQ(feedback[0].event.gamepad_slot, 1U);
    EXPECT_EQ(feedback[1].handle, first.handle);
    EXPECT_EQ(feedback[1].sequence, 2U);
    EXPECT_EQ(feedback[1].event.gamepad_slot, 0U);
    EXPECT_EQ(
      decode_feedback_event(encode_feedback_event(feedback[0].event)),
      feedback[0].event
    );

    const auto stale_callback = factory.feedback_callbacks[gamepad_zero_callback];
    ASSERT_EQ(
      backend.destroy(first.handle, first.input_seat),
      backend_result_e::applied
    );
    stale_callback({
      .kind = feedback_kind_e::rumble,
      .gamepad_slot = 0,
      .low_frequency = 90,
      .high_frequency = 100,
    });
    EXPECT_EQ(feedback.size(), 2U);

    const auto prior_callback_count = factory.feedback_callbacks.size();
    const auto second = expectation_for(0, 81, {.gamepad_slots = 1});
    ASSERT_EQ(backend.create(second).result, backend_result_e::applied);
    std::size_t second_gamepad_callback = factory.feedback_callbacks.size();
    for (std::size_t index = prior_callback_count;
         index < factory.created_specs.size();
         ++index) {
      if (factory.created_specs[index].kind == managed_device_kind_e::gamepad) {
        second_gamepad_callback = index;
      }
    }
    ASSERT_LT(second_gamepad_callback, factory.feedback_callbacks.size());
    stale_callback({
      .kind = feedback_kind_e::rumble,
      .gamepad_slot = 0,
      .low_frequency = 101,
      .high_frequency = 102,
    });
    factory.feedback_callbacks[second_gamepad_callback]({
      .kind = feedback_kind_e::rumble,
      .gamepad_slot = 0,
      .low_frequency = 103,
      .high_frequency = 104,
    });
    ASSERT_EQ(feedback.size(), 3U);
    EXPECT_EQ(feedback.back().handle, second.handle);
    EXPECT_EQ(feedback.back().sequence, 1U);
  }

  TEST(InputtinoMultiseatBackend, ConcurrentFeedbackDeliveryKeepsSequenceOrder) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    std::vector<controller_feedback_t> feedback;
    inputtino_host_backend_t backend {
      factory,
      probe,
      fast_options(),
      {},
      [&feedback](const controller_feedback_t &packet) {
        feedback.push_back(packet);
      }
    };
    const auto expectation = expectation_for(0, 90, {.gamepad_slots = 1});
    ASSERT_EQ(backend.create(expectation).result, backend_result_e::applied);
    const auto callback = std::find_if(
      factory.created_specs.begin(),
      factory.created_specs.end(),
      [](const auto &spec) {
        return spec.kind == managed_device_kind_e::gamepad;
      }
    );
    ASSERT_NE(callback, factory.created_specs.end());
    const auto index = static_cast<std::size_t>(
      std::distance(factory.created_specs.begin(), callback)
    );
    const auto publish = factory.feedback_callbacks[index];

    constexpr std::size_t callback_count = 64;
    std::vector<std::future<void>> futures;
    futures.reserve(callback_count);
    for (std::size_t call = 0; call < callback_count; ++call) {
      futures.push_back(std::async(std::launch::async, [publish, call]() {
        publish({
          .kind = feedback_kind_e::rumble,
          .gamepad_slot = 0,
          .low_frequency = static_cast<std::uint16_t>(call),
          .high_frequency = static_cast<std::uint16_t>(call + 1),
        });
      }));
    }
    for (auto &future : futures) {
      future.get();
    }

    ASSERT_EQ(feedback.size(), callback_count);
    for (std::size_t index = 0; index < feedback.size(); ++index) {
      EXPECT_EQ(feedback[index].handle, expectation.handle);
      EXPECT_EQ(feedback[index].sequence, index + 1);
      EXPECT_EQ(feedback[index].event.gamepad_slot, 0U);
    }
  }

  TEST(InputtinoMultiseatBackend, ConstructorRejectsUnboundedRetryPolicy) {
    fake_kernel_probe_t probe;
    fake_device_factory_t factory {probe};
    auto options = fast_options();
    options.discovery_attempts = 0;
    EXPECT_THROW(
      inputtino_host_backend_t(factory, probe, options),
      std::invalid_argument
    );
    options = fast_options();
    options.stable_observations = options.discovery_attempts + 1;
    EXPECT_THROW(
      inputtino_host_backend_t(factory, probe, options),
      std::invalid_argument
    );
  }
}  // namespace
