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
  auto result = store.create(identity_for(), "polaris-runtime-controller-a1b2-7");
  auto authority = take_authority(result);

  EXPECT_TRUE(authority.active());
  EXPECT_EQ(authority.identity(), identity_for());
  EXPECT_EQ(permissions_of(authority.paths().generation), 0700);
  EXPECT_EQ(permissions_of(authority.paths().ipc), 0700);
  EXPECT_EQ(permissions_of(authority.paths().auth), 0700);
  EXPECT_EQ(permissions_of(authority.paths().capability), 0600);
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
  EXPECT_EQ(store.validate(authority), authority_status_e::applied);

  const auto generation_path = authority.paths().generation;
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
  EXPECT_FALSE(authority.active());
  EXPECT_FALSE(std::filesystem::exists(generation_path));
}

TEST(MultiseatWorkerAuthority, DuplicateGenerationCannotReplaceLiveAuthority) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x11)};
  auto first_result = store.create(identity_for(), "generation-7");
  auto first = take_authority(first_result);
  auto duplicate = store.create(identity_for(), "generation-7");

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
  auto failure = failed.create(identity_for(), "generation-failed");
  EXPECT_EQ(failure.status, authority_status_e::random_failed);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "generation-failed"));

  authority_store_t zero {
    root.path(),
    [](capability_t &capability) {
      capability.fill(0);
      return true;
    },
  };
  auto zero_result = zero.create(identity_for(), "generation-zero");
  EXPECT_EQ(zero_result.status, authority_status_e::random_failed);
  EXPECT_FALSE(std::filesystem::exists(root.path() / "generation-zero"));
}

TEST(MultiseatWorkerAuthority, InvalidIdentityNamespaceAndLongSocketPathCreateNothing) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x33)};
  auto invalid_identity = identity_for();
  invalid_identity.generation = 0;
  EXPECT_EQ(
    store.create(invalid_identity, "generation-invalid").status,
    authority_status_e::invalid_argument
  );
  EXPECT_EQ(
    store.create(identity_for(), "../generation").status,
    authority_status_e::invalid_argument
  );

  const std::string long_namespace(100, 'a');
  EXPECT_EQ(
    store.create(identity_for(), long_namespace).status,
    authority_status_e::invalid_argument
  );
  EXPECT_TRUE(std::filesystem::is_empty(root.path()));
}

TEST(MultiseatWorkerAuthority, TokenMutationAndUnexpectedFilesFailClosed) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x44)};
  auto token_result = store.create(identity_for(), "generation-token");
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

  auto other_result = store.create(identity_for(8), "generation-extra");
  auto other = take_authority(other_result);
  {
    std::ofstream unexpected {other.paths().generation / "unexpected"};
    unexpected << "do not delete";
  }
  EXPECT_EQ(store.validate(other), authority_status_e::integrity_violation);
  EXPECT_EQ(store.remove(other), authority_status_e::integrity_violation);
  EXPECT_TRUE(std::filesystem::exists(other.paths().generation / "unexpected"));
}

TEST(MultiseatWorkerAuthority, RenamedGenerationCannotAuthorizeReplacementPath) {
  temporary_root_t root;
  authority_store_t store {root.path(), capability_filled_with(0x55)};
  auto result = store.create(identity_for(), "generation-reused");
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
  auto result = store.create(identity_for(), "generation-before-root-swap");
  auto authority = take_authority(result);
  const auto displaced = root.path().string() + "-displaced";
  ASSERT_EQ(::rename(root.path().c_str(), displaced.c_str()), 0);
  ASSERT_TRUE(std::filesystem::create_directory(root.path()));
  ASSERT_EQ(::chmod(root.path().c_str(), 0700), 0);

  EXPECT_EQ(store.validate(authority), authority_status_e::integrity_violation);
  EXPECT_EQ(
    store.create(identity_for(8), "generation-after-root-swap").status,
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
  auto result = store.create(identity_for(), "generation-stale-socket");
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
  auto result = store.create(identity_for(), "generation-active-socket");
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
  auto result = store.create(identity_for(), "generation-mixed-sockets");
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
  auto result = store.create(identity_for(), "generation-moved");
  auto authority = take_authority(result);
  authority_handle_t moved {std::move(authority)};

  EXPECT_FALSE(authority.active());
  EXPECT_EQ(store.remove(authority), authority_status_e::inactive);
  EXPECT_TRUE(moved.active());
  EXPECT_EQ(store.validate(moved), authority_status_e::applied);
  EXPECT_EQ(store.remove(moved), authority_status_e::applied);
}

#endif
