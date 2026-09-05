#include <gtest/gtest.h>

#include "src/platform/linux/multiseat_worker_authority.h"

#ifdef __linux__

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  using namespace multiseat::worker_ipc;

  class temporary_root_t {
  public:
    temporary_root_t() {
      std::array<char, 48> pattern {};
      const std::string prefix = "/tmp/polaris-seat-authority-XXXXXX";
      std::copy(prefix.begin(), prefix.end(), pattern.begin());
      const auto *created = ::mkdtemp(pattern.data());
      if (!created) {
        throw std::runtime_error {"temporary authority root could not be created"};
      }
      path_ = created;
      if (::chmod(path_.c_str(), 0700) != 0) {
        throw std::runtime_error {"temporary authority root mode could not be set"};
      }
    }

    ~temporary_root_t() {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }

    temporary_root_t(const temporary_root_t &) = delete;
    temporary_root_t &operator=(const temporary_root_t &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };

  endpoint_identity_t identity_for(std::uint64_t generation = 7) {
    return {
      .controller_epoch = "controller-a1b2",
      .logical_gpu_id = "gpu-primary",
      .slot = 1,
      .generation = generation,
      .worker_name = "polaris-worker-controller-a1b2-7",
    };
  }

  provider_catalog_selection_t provider_selection_for(
    std::string target_id = "test-workload"
  ) {
    return {
      .compositor = multiseat::compositor_e::gamescope,
      .workload = {
        .kind = multiseat::workload_kind_e::heroic,
        .target_id = std::move(target_id),
      },
    };
  }

  capability_factory_t capability_filled_with(std::uint8_t value) {
    return [value](capability_t &capability) {
      capability.fill(value);
      return true;
    };
  }

  authority_handle_t take_authority(authority_create_result_t &result) {
    EXPECT_TRUE(result.created());
    return std::move(result.authority.value());
  }

  mode_t permissions_of(const std::filesystem::path &path) {
    struct stat metadata {};
    EXPECT_EQ(::lstat(path.c_str(), &metadata), 0);
    return metadata.st_mode & 07777;
  }

  std::string read_file(const std::filesystem::path &path) {
    std::ifstream stream {path, std::ios::binary};
    return {
      std::istreambuf_iterator<char> {stream},
      std::istreambuf_iterator<char> {},
    };
  }

  int bind_worker_socket(const std::filesystem::path &path, bool listen) {
    const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
      return -1;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto native = path.native();
    if (native.size() >= sizeof(address.sun_path)) {
      (void) ::close(descriptor);
      return -1;
    }
    std::copy(native.begin(), native.end(), address.sun_path);
    address.sun_path[native.size()] = '\0';
    if (::bind(
          descriptor,
          reinterpret_cast<const sockaddr *>(&address),
          static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + native.size() + 1)
        ) != 0 ||
        ::chmod(path.c_str(), 0600) != 0 ||
        (listen && ::listen(descriptor, 2) != 0)) {
      (void) ::close(descriptor);
      return -1;
    }
    return descriptor;
  }
}

TEST(MultiseatWorkerAuthority, ProviderCatalogMatchesTheDispatcherGoldenSchema) {
  const auto encoded = encode_provider_catalog(provider_selection_for());
  ASSERT_TRUE(encoded.has_value());
  const auto fixture = read_file(
    std::filesystem::path {POLARIS_SOURCE_DIR} /
    "multiseat_worker/internal/seatruntime/testdata/controller-catalog-v1.json"
  );
  ASSERT_FALSE(fixture.empty());
  EXPECT_EQ(
    std::string(encoded->begin(), encoded->end()),
    fixture
  );

  auto automatic = provider_selection_for();
  automatic.compositor = multiseat::compositor_e::automatic;
  EXPECT_FALSE(encode_provider_catalog(automatic).has_value());
  auto unknown_workload = provider_selection_for();
  unknown_workload.workload.kind = multiseat::workload_kind_e::unknown;
  EXPECT_FALSE(encode_provider_catalog(unknown_workload).has_value());
  auto hostile_target = provider_selection_for("../../bin/sh");
  EXPECT_FALSE(encode_provider_catalog(hostile_target).has_value());
}

TEST(MultiseatWorkerAuthority, RootMustBePrivateAbsoluteAndSymlinkFree) {
  authority_store_t relative {"relative/root"};
  EXPECT_EQ(relative.status(), authority_status_e::invalid_argument);

  temporary_root_t root;
  ASSERT_EQ(::chmod(root.path().c_str(), 0755), 0);
  authority_store_t permissive {root.path()};
  EXPECT_EQ(permissive.status(), authority_status_e::unsafe_root);
  ASSERT_EQ(::chmod(root.path().c_str(), 0700), 0);

  const auto link = root.path().string() + "-link";
  ASSERT_EQ(::symlink(root.path().c_str(), link.c_str()), 0);
  authority_store_t symlinked {link};
  EXPECT_EQ(symlinked.status(), authority_status_e::unsafe_root);
  EXPECT_EQ(::unlink(link.c_str()), 0);
}

TEST(MultiseatWorkerAuthority, CreatesExactPrivateHierarchyAndCanonicalCapability) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x2a)};
  ASSERT_EQ(store.status(), authority_status_e::applied);
  auto result = store.create(
    identity_for(),
    "polaris-runtime-controller-a1b2-7",
    provider_selection_for()
  );
  auto authority = take_authority(result);

  EXPECT_TRUE(authority.active());
  EXPECT_EQ(authority.identity(), identity_for());
  EXPECT_EQ(permissions_of(authority.paths().generation), 0700);
  EXPECT_EQ(permissions_of(authority.paths().ipc), 0700);
  EXPECT_EQ(permissions_of(authority.paths().auth), 0700);
  EXPECT_EQ(permissions_of(authority.paths().capability), 0600);
  EXPECT_EQ(permissions_of(authority.paths().record), 0600);
  EXPECT_EQ(permissions_of(authority.paths().provider_catalog), 0400);
  EXPECT_FALSE(std::filesystem::exists(authority.paths().control_socket));
  EXPECT_FALSE(std::filesystem::exists(authority.paths().media_socket));

  std::ifstream token {authority.paths().capability, std::ios::binary};
  const std::string encoded {
    std::istreambuf_iterator<char> {token},
    std::istreambuf_iterator<char> {},
  };
  std::string expected;
  for (std::size_t index = 0; index < capability_size; ++index) {
    expected += "2a";
  }
  expected += '\n';
  EXPECT_EQ(encoded, expected);
  EXPECT_EQ(
    read_file(authority.paths().provider_catalog),
    read_file(
      std::filesystem::path {POLARIS_SOURCE_DIR} /
      "multiseat_worker/internal/seatruntime/testdata/controller-catalog-v1.json"
    )
  );
  EXPECT_EQ(store.validate(authority), authority_status_e::applied);

  const auto generation_path = authority.paths().generation;
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
  EXPECT_FALSE(authority.active());
  EXPECT_FALSE(std::filesystem::exists(generation_path));
}

TEST(MultiseatWorkerAuthority, DuplicateGenerationCannotReplaceLiveAuthority) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x11)};
  auto first_result = store.create(identity_for(), "generation-7", provider_selection_for());
  auto first = take_authority(first_result);
  auto duplicate = store.create(identity_for(), "generation-7", provider_selection_for());

  EXPECT_EQ(duplicate.status, authority_status_e::already_exists);
  EXPECT_FALSE(duplicate.authority.has_value());
  EXPECT_EQ(store.validate(first), authority_status_e::applied);
  EXPECT_EQ(store.remove(first), authority_status_e::applied);
}

TEST(MultiseatWorkerAuthority, RandomFailureAndAllZeroCapabilityRollBackCompletely) {
  temporary_root_t root;
  authority_store_t failed {
    root.path(),
    [](capability_t &) { return false; },
  };
  auto failure = failed.create(
    identity_for(),
    "generation-failed",
    provider_selection_for()
  );
  EXPECT_EQ(failure.status, authority_status_e::random_failed);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "generation-failed"));

  authority_store_t zero {
    root.path(),
    [](capability_t &capability) {
      capability.fill(0);
      return true;
    },
  };
  auto zero_result = zero.create(identity_for(), "generation-zero", provider_selection_for());
  EXPECT_EQ(zero_result.status, authority_status_e::random_failed);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "generation-zero"));
}

TEST(MultiseatWorkerAuthority, InvalidIdentityNamespaceAndLongSocketPathCreateNothing) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x33)};
  auto invalid_identity = identity_for();
  invalid_identity.generation = 0;
  EXPECT_EQ(
    store.create(invalid_identity, "generation-invalid", provider_selection_for()).status,
    authority_status_e::invalid_argument
  );
  EXPECT_EQ(
    store.create(identity_for(), "../generation", provider_selection_for()).status,
    authority_status_e::invalid_argument
  );

  const std::string long_namespace(100, 'a');
  EXPECT_EQ(
    store.create(identity_for(), long_namespace, provider_selection_for()).status,
    authority_status_e::invalid_argument
  );
  auto invalid_provider_selection = provider_selection_for();
  invalid_provider_selection.compositor = multiseat::compositor_e::automatic;
  EXPECT_EQ(
    store.create(
      identity_for(),
      "generation-invalid-catalog",
      invalid_provider_selection
    ).status,
    authority_status_e::invalid_argument
  );
  EXPECT_TRUE(std::filesystem::is_empty(root.path()));
}

TEST(MultiseatWorkerAuthority, TokenMutationAndUnexpectedFilesFailClosed) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x44)};
  auto token_result = store.create(
    identity_for(),
    "generation-token",
    provider_selection_for()
  );
  auto token_authority = take_authority(token_result);
  {
    std::fstream token {
      token_authority.paths().capability,
      std::ios::in | std::ios::out | std::ios::binary,
    };
    ASSERT_TRUE(token.good());
    token.put('0');
  }
  EXPECT_EQ(store.validate(token_authority), authority_status_e::integrity_violation);
  EXPECT_EQ(store.remove(token_authority), authority_status_e::integrity_violation);
  EXPECT_TRUE(std::filesystem::exists(token_authority.paths().generation));

  auto other_result = store.create(
    identity_for(8),
    "generation-extra",
    provider_selection_for()
  );
  auto other = take_authority(other_result);
  {
    std::ofstream unexpected {other.paths().generation / "unexpected"};
    unexpected << "do not delete";
  }
  EXPECT_EQ(store.validate(other), authority_status_e::integrity_violation);
  EXPECT_EQ(store.remove(other), authority_status_e::integrity_violation);
  EXPECT_TRUE(std::filesystem::exists(other.paths().generation / "unexpected"));
}

TEST(MultiseatWorkerAuthority, ProviderCatalogModeBytesAndInodeAreFenced) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x45)};
  auto mutated_result = store.create(
    identity_for(),
    "generation-catalog-mutated",
    provider_selection_for()
  );
  auto mutated = take_authority(mutated_result);
  EXPECT_FALSE(std::filesystem::exists(
    mutated.paths().auth / ".runtime-providers.json.tmp"
  ));
  ASSERT_EQ(::chmod(mutated.paths().provider_catalog.c_str(), 0600), 0);
  EXPECT_EQ(store.validate(mutated), authority_status_e::integrity_violation);
  {
    std::fstream catalog {
      mutated.paths().provider_catalog,
      std::ios::in | std::ios::out | std::ios::binary,
    };
    ASSERT_TRUE(catalog.good());
    catalog.put('X');
  }
  ASSERT_EQ(::chmod(mutated.paths().provider_catalog.c_str(), 0400), 0);
  EXPECT_EQ(store.validate(mutated), authority_status_e::integrity_violation);
  EXPECT_EQ(store.remove(mutated), authority_status_e::integrity_violation);

  auto replaced_result = store.create(
    identity_for(8),
    "generation-catalog-replaced",
    provider_selection_for()
  );
  auto replaced = take_authority(replaced_result);
  const auto exact_bytes = read_file(replaced.paths().provider_catalog);
  const auto displaced = replaced.paths().auth / "catalog-displaced";
  ASSERT_EQ(
    ::rename(replaced.paths().provider_catalog.c_str(), displaced.c_str()),
    0
  );
  {
    std::ofstream replacement {replaced.paths().provider_catalog, std::ios::binary};
    replacement << exact_bytes;
  }
  ASSERT_EQ(::chmod(replaced.paths().provider_catalog.c_str(), 0400), 0);
  EXPECT_EQ(store.validate(replaced), authority_status_e::integrity_violation);
  EXPECT_EQ(store.remove(replaced), authority_status_e::integrity_violation);
  EXPECT_TRUE(std::filesystem::exists(displaced));
}

TEST(MultiseatWorkerAuthority, ProviderCatalogSymlinkFailsClosed) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x46)};
  auto result = store.create(
    identity_for(),
    "generation-catalog-symlink",
    provider_selection_for()
  );
  auto authority = take_authority(result);
  const auto displaced = authority.paths().auth / "catalog-displaced";
  ASSERT_EQ(
    ::rename(authority.paths().provider_catalog.c_str(), displaced.c_str()),
    0
  );
  ASSERT_EQ(
    ::symlink(displaced.c_str(), authority.paths().provider_catalog.c_str()),
    0
  );

  EXPECT_EQ(store.validate(authority), authority_status_e::integrity_violation);
  EXPECT_EQ(store.remove(authority), authority_status_e::integrity_violation);
  EXPECT_TRUE(std::filesystem::is_symlink(
    std::filesystem::symlink_status(authority.paths().provider_catalog)
  ));
  EXPECT_TRUE(std::filesystem::exists(displaced));
}

TEST(MultiseatWorkerAuthority, RenamedGenerationCannotAuthorizeReplacementPath) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x55)};
  auto result = store.create(identity_for(), "generation-reused", provider_selection_for());
  auto authority = take_authority(result);
  const auto original = authority.paths().generation;
  const auto displaced = root.path() / "generation-displaced";
  ASSERT_EQ(::rename(original.c_str(), displaced.c_str()), 0);
  ASSERT_TRUE(std::filesystem::create_directory(original));
  ASSERT_EQ(::chmod(original.c_str(), 0700), 0);

  EXPECT_EQ(store.validate(authority), authority_status_e::integrity_violation);
  EXPECT_EQ(store.remove(authority), authority_status_e::integrity_violation);
  EXPECT_TRUE(std::filesystem::exists(original));
  EXPECT_TRUE(std::filesystem::exists(displaced));
}

TEST(MultiseatWorkerAuthority, ReplacedRootCannotRedirectCreationOrValidation) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x56)};
  auto result = store.create(
    identity_for(),
    "generation-before-root-swap",
    provider_selection_for()
  );
  auto authority = take_authority(result);
  const auto displaced = root.path().string() + "-displaced";
  ASSERT_EQ(::rename(root.path().c_str(), displaced.c_str()), 0);
  ASSERT_TRUE(std::filesystem::create_directory(root.path()));
  ASSERT_EQ(::chmod(root.path().c_str(), 0700), 0);

  EXPECT_EQ(store.validate(authority), authority_status_e::integrity_violation);
  EXPECT_EQ(
    store.create(identity_for(8), "generation-after-root-swap", provider_selection_for()).status,
    authority_status_e::unsafe_root
  );
  EXPECT_FALSE(std::filesystem::exists(
    root.path() / "generation-after-root-swap"
  ));

  std::error_code ignored;
  std::filesystem::remove_all(displaced, ignored);
}

TEST(MultiseatWorkerAuthority, RemovesOnlyInactiveAllowlistedSocketNodes) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x66)};
  auto result = store.create(
    identity_for(),
    "generation-stale-socket",
    provider_selection_for()
  );
  auto authority = take_authority(result);
  const auto socket_path = authority.paths().control_socket;
  const auto descriptor = bind_worker_socket(socket_path, false);
  ASSERT_GE(descriptor, 0);
  ASSERT_EQ(::close(descriptor), 0);
  ASSERT_EQ(store.validate(authority), authority_status_e::applied);

  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}

TEST(MultiseatWorkerAuthority, RefusesToRemoveAnActivelyListeningWorkerSocket) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x77)};
  auto result = store.create(
    identity_for(),
    "generation-active-socket",
    provider_selection_for()
  );
  auto authority = take_authority(result);
  const auto descriptor = bind_worker_socket(authority.paths().control_socket, true);
  ASSERT_GE(descriptor, 0);
  ASSERT_EQ(store.validate(authority), authority_status_e::applied);

  EXPECT_EQ(store.remove(authority), authority_status_e::integrity_violation);
  EXPECT_TRUE(authority.active());
  EXPECT_TRUE(std::filesystem::exists(authority.paths().control_socket));
  EXPECT_EQ(::close(descriptor), 0);
}

TEST(MultiseatWorkerAuthority, LivenessValidationPrecedesEverySocketRemoval) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x78)};
  auto result = store.create(
    identity_for(),
    "generation-mixed-sockets",
    provider_selection_for()
  );
  auto authority = take_authority(result);
  const auto stale = bind_worker_socket(authority.paths().control_socket, false);
  const auto active = bind_worker_socket(authority.paths().media_socket, true);
  ASSERT_GE(stale, 0);
  ASSERT_GE(active, 0);
  ASSERT_EQ(::close(stale), 0);

  EXPECT_EQ(store.remove(authority), authority_status_e::integrity_violation);
  EXPECT_TRUE(std::filesystem::exists(authority.paths().control_socket));
  EXPECT_TRUE(std::filesystem::exists(authority.paths().media_socket));
  EXPECT_EQ(::close(active), 0);
}

TEST(MultiseatWorkerAuthority, MoveInvalidatesTheOldHandleAndPreservesCleanupFence) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x7f)};
  auto result = store.create(identity_for(), "generation-moved", provider_selection_for());
  auto authority = take_authority(result);
  authority_handle_t moved {std::move(authority)};

  EXPECT_FALSE(authority.active());
  EXPECT_EQ(store.remove(authority), authority_status_e::inactive);
  EXPECT_TRUE(moved.active());
  EXPECT_EQ(store.validate(moved), authority_status_e::applied);
  EXPECT_EQ(store.remove(moved), authority_status_e::applied);
}

TEST(MultiseatWorkerAuthority, SignedRecoveryReturnsOnlyInventoryAbsentAuthorities) {
  temporary_root_t root;
  const auto first_identity = identity_for(41);
  const auto second_identity = identity_for(42);
  std::filesystem::path first_path;
  std::filesystem::path second_path;
  {
    authority_store_t original {root.path(), capability_filled_with(0x81)};
    auto first_result = original.create(
      first_identity,
      "recovery-first",
      provider_selection_for("first-workload")
    );
    auto first = take_authority(first_result);
    auto second_result = original.create(
      second_identity,
      "recovery-second",
      provider_selection_for("second-workload")
    );
    auto second = take_authority(second_result);
    first_path = first.paths().generation;
    second_path = second.paths().generation;
  }
  ASSERT_TRUE(std::filesystem::exists(first_path));
  ASSERT_TRUE(std::filesystem::exists(second_path));

  authority_store_t replacement {root.path(), capability_filled_with(0x82)};
  const std::array active {second_identity};
  auto first_pass = replacement.recover_inactive(active);
  ASSERT_TRUE(first_pass.inspected());
  EXPECT_EQ(first_pass.observed, std::size_t {2});
  EXPECT_EQ(first_pass.active, std::size_t {1});
  ASSERT_EQ(first_pass.inactive.size(), std::size_t {1});
  EXPECT_EQ(first_pass.inactive.front().identity(), first_identity);
  EXPECT_EQ(
    replacement.remove(first_pass.inactive.front()),
    authority_status_e::applied
  );
  EXPECT_FALSE(std::filesystem::exists(first_path));
  EXPECT_TRUE(std::filesystem::exists(second_path));

  auto second_pass = replacement.recover_inactive(
    std::span<const endpoint_identity_t> {}
  );
  ASSERT_TRUE(second_pass.all_inactive());
  ASSERT_EQ(second_pass.inactive.size(), std::size_t {1});
  EXPECT_EQ(second_pass.inactive.front().identity(), second_identity);
  EXPECT_EQ(
    replacement.remove(second_pass.inactive.front()),
    authority_status_e::applied
  );
  EXPECT_TRUE(std::filesystem::is_empty(root.path()));
}

TEST(MultiseatWorkerAuthority, RecoveryRejectsTamperedRecordsAndDuplicateInventory) {
  temporary_root_t root;
  const auto identity = identity_for(51);
  std::filesystem::path generation;
  std::filesystem::path record;
  {
    authority_store_t original {root.path(), capability_filled_with(0x91)};
    auto result = original.create(identity, "recovery-tampered", provider_selection_for());
    auto authority = take_authority(result);
    generation = authority.paths().generation;
    record = authority.paths().record;
  }

  authority_store_t replacement {root.path(), capability_filled_with(0x92)};
  const std::array duplicate {identity, identity};
  const auto duplicate_result = replacement.recover_inactive(duplicate);
  EXPECT_EQ(duplicate_result.status, authority_status_e::invalid_argument);
  EXPECT_TRUE(duplicate_result.inactive.empty());
  EXPECT_TRUE(std::filesystem::exists(generation));

  {
    std::fstream stream {record, std::ios::in | std::ios::out | std::ios::binary};
    ASSERT_TRUE(stream.good());
    stream.put('X');
  }
  const auto tampered = replacement.recover_inactive(
    std::span<const endpoint_identity_t> {}
  );
  EXPECT_EQ(tampered.status, authority_status_e::integrity_violation);
  EXPECT_TRUE(tampered.inactive.empty());
  EXPECT_TRUE(std::filesystem::exists(generation));
}

TEST(MultiseatWorkerAuthority, RecoveryRejectsCatalogBytesNotBoundToTheSignedRecord) {
  temporary_root_t root;
  std::filesystem::path generation;
  std::filesystem::path catalog;
  {
    authority_store_t original {root.path(), capability_filled_with(0x93)};
    auto result = original.create(
      identity_for(52),
      "recovery-catalog-tampered",
      provider_selection_for()
    );
    auto authority = take_authority(result);
    generation = authority.paths().generation;
    catalog = authority.paths().provider_catalog;
  }
  ASSERT_EQ(::chmod(catalog.c_str(), 0600), 0);
  {
    std::fstream stream {catalog, std::ios::in | std::ios::out | std::ios::binary};
    ASSERT_TRUE(stream.good());
    stream.put('X');
  }
  ASSERT_EQ(::chmod(catalog.c_str(), 0400), 0);

  authority_store_t replacement {root.path(), capability_filled_with(0x94)};
  const auto recovered = replacement.recover_inactive(
    std::span<const endpoint_identity_t> {}
  );
  EXPECT_EQ(recovered.status, authority_status_e::integrity_violation);
  EXPECT_TRUE(recovered.inactive.empty());
  EXPECT_TRUE(std::filesystem::exists(generation));
}

TEST(MultiseatWorkerAuthority, RecoveryNeverConvertsALiveSocketIntoCleanupAuthority) {
  temporary_root_t root;
  std::filesystem::path generation;
  std::filesystem::path socket_path;
  {
    authority_store_t original {root.path(), capability_filled_with(0xa1)};
    auto result = original.create(
      identity_for(61),
      "recovery-live-socket",
      provider_selection_for()
    );
    auto authority = take_authority(result);
    generation = authority.paths().generation;
    socket_path = authority.paths().control_socket;
  }
  const auto listener = bind_worker_socket(socket_path, true);
  ASSERT_GE(listener, 0);

  authority_store_t replacement {root.path(), capability_filled_with(0xa2)};
  auto recovered = replacement.recover_inactive(
    std::span<const endpoint_identity_t> {}
  );
  ASSERT_TRUE(recovered.all_inactive());
  ASSERT_EQ(recovered.inactive.size(), std::size_t {1});
  EXPECT_EQ(
    replacement.remove(recovered.inactive.front()),
    authority_status_e::integrity_violation
  );
  EXPECT_TRUE(std::filesystem::exists(generation));
  EXPECT_EQ(::close(listener), 0);
}

TEST(MultiseatWorkerAuthority, RecoveryRejectsUnexpectedRootEntriesWithoutPartialCleanup) {
  temporary_root_t root;
  std::filesystem::path generation;
  {
    authority_store_t original {root.path(), capability_filled_with(0xb1)};
    auto result = original.create(
      identity_for(71),
      "recovery-valid",
      provider_selection_for()
    );
    auto authority = take_authority(result);
    generation = authority.paths().generation;
  }
  {
    std::ofstream unexpected {root.path() / "unexpected-file"};
    unexpected << "not an authority";
  }

  authority_store_t replacement {root.path(), capability_filled_with(0xb2)};
  const auto recovered = replacement.recover_inactive(
    std::span<const endpoint_identity_t> {}
  );
  EXPECT_EQ(recovered.status, authority_status_e::integrity_violation);
  EXPECT_TRUE(recovered.inactive.empty());
  EXPECT_TRUE(std::filesystem::exists(generation));
  EXPECT_TRUE(std::filesystem::exists(root.path() / "unexpected-file"));
}

TEST(MultiseatWorkerAuthority, RecoveryRejectsMoreThanTheBoundedAuthorityLimit) {
  temporary_root_t root;
  std::filesystem::path generation;
  {
    authority_store_t original {root.path(), capability_filled_with(0xc1)};
    auto result = original.create(
      identity_for(81),
      "recovery-within-bound",
      provider_selection_for()
    );
    auto authority = take_authority(result);
    generation = authority.paths().generation;
  }
  for (std::size_t index = 0; index < 256; ++index) {
    ASSERT_TRUE(std::filesystem::create_directory(
      root.path() / ("unexpected-" + std::to_string(index))
    ));
  }

  authority_store_t replacement {root.path(), capability_filled_with(0xc2)};
  const auto recovered = replacement.recover_inactive(
    std::span<const endpoint_identity_t> {}
  );
  EXPECT_EQ(recovered.status, authority_status_e::integrity_violation);
  EXPECT_EQ(recovered.observed, std::size_t {0});
  EXPECT_TRUE(recovered.inactive.empty());
  EXPECT_TRUE(std::filesystem::exists(generation));
}

TEST(MultiseatWorkerAuthority, SignedRecordsCannotMoveBetweenRuntimeNamespaces) {
  temporary_root_t root;
  std::filesystem::path first_generation;
  std::filesystem::path second_generation;
  std::filesystem::path first_record;
  std::filesystem::path second_record;
  {
    authority_store_t original {root.path(), capability_filled_with(0xd1)};
    auto first_result = original.create(
      identity_for(91),
      "recovery-swap-first",
      provider_selection_for("first-workload")
    );
    auto first = take_authority(first_result);
    auto second_result = original.create(
      identity_for(92),
      "recovery-swap-second",
      provider_selection_for("second-workload")
    );
    auto second = take_authority(second_result);
    first_generation = first.paths().generation;
    second_generation = second.paths().generation;
    first_record = first.paths().record;
    second_record = second.paths().record;
  }
  const auto displaced = root.path() / "record-swap-temporary";
  ASSERT_EQ(::rename(first_record.c_str(), displaced.c_str()), 0);
  ASSERT_EQ(::rename(second_record.c_str(), first_record.c_str()), 0);
  ASSERT_EQ(::rename(displaced.c_str(), second_record.c_str()), 0);

  authority_store_t replacement {root.path(), capability_filled_with(0xd2)};
  const auto recovered = replacement.recover_inactive(
    std::span<const endpoint_identity_t> {}
  );
  EXPECT_EQ(recovered.status, authority_status_e::integrity_violation);
  EXPECT_TRUE(recovered.inactive.empty());
  EXPECT_TRUE(std::filesystem::exists(first_generation));
  EXPECT_TRUE(std::filesystem::exists(second_generation));
}

TEST(MultiseatWorkerAuthority, SignedRecordsBindEachExactProviderCatalog) {
  temporary_root_t root;
  std::filesystem::path first_generation;
  std::filesystem::path second_generation;
  std::filesystem::path first_catalog;
  std::filesystem::path second_catalog;
  {
    authority_store_t original {root.path(), capability_filled_with(0xd3)};
    auto first_result = original.create(
      identity_for(93),
      "catalog-swap-first",
      provider_selection_for("first-workload")
    );
    auto first = take_authority(first_result);
    auto second_result = original.create(
      identity_for(94),
      "catalog-swap-second",
      provider_selection_for("second-workload")
    );
    auto second = take_authority(second_result);
    first_generation = first.paths().generation;
    second_generation = second.paths().generation;
    first_catalog = first.paths().provider_catalog;
    second_catalog = second.paths().provider_catalog;
  }
  const auto displaced = root.path() / "catalog-swap-temporary";
  ASSERT_EQ(::rename(first_catalog.c_str(), displaced.c_str()), 0);
  ASSERT_EQ(::rename(second_catalog.c_str(), first_catalog.c_str()), 0);
  ASSERT_EQ(::rename(displaced.c_str(), second_catalog.c_str()), 0);

  authority_store_t replacement {root.path(), capability_filled_with(0xd4)};
  const auto recovered = replacement.recover_inactive(
    std::span<const endpoint_identity_t> {}
  );
  EXPECT_EQ(recovered.status, authority_status_e::integrity_violation);
  EXPECT_TRUE(recovered.inactive.empty());
  EXPECT_TRUE(std::filesystem::exists(first_generation));
  EXPECT_TRUE(std::filesystem::exists(second_generation));
}

#endif
