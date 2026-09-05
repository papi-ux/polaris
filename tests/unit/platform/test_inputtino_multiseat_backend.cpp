/**
 * @file tests/unit/platform/test_inputtino_multiseat_backend.cpp
 * @brief Offline tests for trusted host multiseat input lifecycle.
 */
#include "src/platform/linux/input/inputtino_multiseat_backend.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <future>
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
      std::function<void()> on_destroy
    ) :
        nodes_(std::move(nodes)),
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

    bool throw_nodes = false;

  private:
    std::vector<std::filesystem::path> nodes_;
    std::function<void()> on_destroy_;
  };

  class fake_device_factory_t final : public device_factory_t {
  public:
    explicit fake_device_factory_t(fake_kernel_probe_t &probe) :
        probe(probe) {
    }

    std::unique_ptr<managed_device_t> create(
      const device_spec_t &spec
    ) override {
      const auto call = create_calls++;
      if (fail_at_call && call == *fail_at_call) {
        return {};
      }
      created_specs.push_back(spec);
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
    std::optional<std::size_t> fail_at_call;
    std::optional<std::string> name_override;
    std::optional<std::string> phys_override;
    std::optional<std::string> seat_override;
    std::uint32_t next_event = 0;
    std::uint32_t next_joystick = 0;
    std::uint64_t next_inode = 10000;
    std::size_t create_calls = 0;
    bool carry_phys = false;
    bool include_joystick_nodes = true;
    bool retain_nodes_on_destroy = false;
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
    const std::array<std::uint8_t, 1> payload {1};
    EXPECT_EQ(
      backend.route(expectation.handle, expectation.input_seat, 1, payload),
      backend_result_e::rejected
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
