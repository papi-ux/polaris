/**
 * @file src/platform/linux/multiseat_worker_authority.cpp
 * @brief Private per-generation filesystem authority for multiseat workers.
 */
#include "multiseat_worker_authority.h"

#ifdef __linux__

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace multiseat::worker_ipc {
  namespace {
    struct file_identity_t {
      std::uint64_t device = 0;
      std::uint64_t inode = 0;

      bool operator==(const file_identity_t &) const = default;
    };

    bool ascii_alphanumeric(char value) {
      return (value >= 'a' && value <= 'z') ||
             (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9');
    }

    bool opaque_name_token(std::string_view value, std::size_t max_size = 128) {
      return !value.empty() &&
             value.size() <= max_size &&
             ascii_alphanumeric(value.front()) &&
             std::all_of(
               value.begin(),
               value.end(),
               [](char character) {
                 return ascii_alphanumeric(character) ||
                        character == '-' ||
                        character == '_' ||
                        character == '.';
               }
             );
    }

    bool clean_absolute_path(const std::filesystem::path &path) {
      const auto &native = path.native();
      return path.is_absolute() &&
             path.lexically_normal() == path &&
             native.find('\0') == std::string::npos;
    }

    file_identity_t identity_of(const struct stat &metadata) {
      return {
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
      };
    }

    bool exact_identity(
      const struct stat &metadata,
      std::uint64_t device,
      std::uint64_t inode
    ) {
      return identity_of(metadata) == file_identity_t {device, inode};
    }

    bool private_directory(const struct stat &metadata, std::uint32_t owner_uid) {
      return S_ISDIR(metadata.st_mode) &&
             static_cast<std::uint32_t>(metadata.st_uid) == owner_uid &&
             (metadata.st_mode & 07777) == 0700;
    }

    bool private_regular_file(const struct stat &metadata, std::uint32_t owner_uid) {
      return S_ISREG(metadata.st_mode) &&
             static_cast<std::uint32_t>(metadata.st_uid) == owner_uid &&
             (metadata.st_mode & 07777) == 0600;
    }

    bool private_socket(const struct stat &metadata, std::uint32_t owner_uid) {
      return S_ISSOCK(metadata.st_mode) &&
             static_cast<std::uint32_t>(metadata.st_uid) == owner_uid &&
             (metadata.st_mode & 07777) == 0600;
    }

    void close_descriptor(int &descriptor) noexcept {
      if (descriptor >= 0) {
        (void) ::close(descriptor);
        descriptor = -1;
      }
    }

    int open_absolute_directory_without_symlinks(const std::filesystem::path &path) {
      if (!clean_absolute_path(path)) {
        return -1;
      }
      auto descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (descriptor < 0) {
        return -1;
      }
      for (const auto &component_path : path.relative_path()) {
        const auto component = component_path.native();
        if (component.empty() || component == "." || component == ".." ||
            component.find('/') != std::string::npos ||
            component.find('\0') != std::string::npos) {
          close_descriptor(descriptor);
          return -1;
        }
        const auto next = ::openat(
          descriptor,
          component.c_str(),
          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        );
        close_descriptor(descriptor);
        descriptor = next;
        if (descriptor < 0) {
          return -1;
        }
      }
      return descriptor;
    }

    bool descriptor_is_directory(
      int descriptor,
      std::uint32_t owner_uid,
      std::uint64_t device,
      std::uint64_t inode
    ) {
      struct stat metadata {};
      return descriptor >= 0 &&
             ::fstat(descriptor, &metadata) == 0 &&
             private_directory(metadata, owner_uid) &&
             exact_identity(metadata, device, inode);
    }

    bool entry_is_directory(
      int parent,
      std::string_view name,
      std::uint32_t owner_uid,
      std::uint64_t device,
      std::uint64_t inode
    ) {
      struct stat metadata {};
      const std::string owned_name {name};
      return ::fstatat(parent, owned_name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
             private_directory(metadata, owner_uid) &&
             exact_identity(metadata, device, inode);
    }

    int create_private_directory(
      int parent,
      std::string_view name,
      bool &already_exists
    ) {
      already_exists = false;
      const std::string owned_name {name};
      if (::mkdirat(parent, owned_name.c_str(), 0700) != 0) {
        already_exists = errno == EEXIST;
        return -1;
      }
      const auto descriptor = ::openat(
        parent,
        owned_name.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      );
      if (descriptor < 0 || ::fchmod(descriptor, 0700) != 0) {
        if (descriptor >= 0) {
          (void) ::close(descriptor);
        }
        (void) ::unlinkat(parent, owned_name.c_str(), AT_REMOVEDIR);
        return -1;
      }
      return descriptor;
    }

    bool write_all(int descriptor, std::string_view payload) {
      std::size_t offset = 0;
      while (offset < payload.size()) {
        const auto result = ::write(
          descriptor,
          payload.data() + offset,
          payload.size() - offset
        );
        if (result > 0) {
          offset += static_cast<std::size_t>(result);
          continue;
        }
        if (result < 0 && errno == EINTR) {
          continue;
        }
        return false;
      }
      return true;
    }

    bool allowed_directory_entries(
      int descriptor,
      const std::set<std::string> &required,
      const std::set<std::string> &optional
    ) {
      const auto independent = ::openat(
        descriptor,
        ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      );
      if (independent < 0) {
        return false;
      }
      auto *directory = ::fdopendir(independent);
      if (!directory) {
        (void) ::close(independent);
        return false;
      }
      std::set<std::string> observed;
      errno = 0;
      while (const auto *entry = ::readdir(directory)) {
        const std::string_view name {entry->d_name};
        if (name != "." && name != "..") {
          observed.emplace(name);
        }
        errno = 0;
      }
      const auto read_error = errno;
      (void) ::closedir(directory);
      if (read_error != 0) {
        return false;
      }
      for (const auto &name : required) {
        if (!observed.contains(name)) {
          return false;
        }
      }
      for (const auto &name : observed) {
        if (!required.contains(name) && !optional.contains(name)) {
          return false;
        }
      }
      return true;
    }

    bool socket_path_fits(const std::filesystem::path &path) {
      const auto &native = path.native();
      sockaddr_un address {};
      return !native.empty() &&
             native.size() < sizeof(address.sun_path) &&
             native.find('\0') == std::string::npos;
    }

    bool capability_file_matches(
      int auth_fd,
      std::string_view name,
      std::uint32_t owner_uid,
      std::uint64_t expected_device,
      std::uint64_t expected_inode,
      const capability_t &capability
    ) {
      const std::string owned_name {name};
      auto descriptor = ::openat(
        auth_fd,
        owned_name.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW
      );
      if (descriptor < 0) {
        return false;
      }
      struct stat metadata {};
      std::array<char, capability_size * 2 + 1> payload {};
      std::size_t offset = 0;
      while (offset < payload.size()) {
        const auto result = ::read(
          descriptor,
          payload.data() + offset,
          payload.size() - offset
        );
        if (result > 0) {
          offset += static_cast<std::size_t>(result);
          continue;
        }
        if (result < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      char trailing = 0;
      const auto trailing_result = ::read(descriptor, &trailing, 1);
      const auto metadata_ready = ::fstat(descriptor, &metadata) == 0;
      close_descriptor(descriptor);
      auto expected = capability_hex(capability);
      const auto matches = metadata_ready &&
                           private_regular_file(metadata, owner_uid) &&
                           exact_identity(metadata, expected_device, expected_inode) &&
                           metadata.st_size == static_cast<off_t>(payload.size()) &&
                           offset == payload.size() &&
                           trailing_result == 0 &&
                           payload.back() == '\n' &&
                           CRYPTO_memcmp(
                             payload.data(),
                             expected.data(),
                             expected.size()
                           ) == 0;
      OPENSSL_cleanse(expected.data(), expected.size());
      OPENSSL_cleanse(payload.data(), payload.size());
      return matches;
    }

    bool inactive_socket(const std::filesystem::path &path) {
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      const auto &native = path.native();
      if (native.empty() || native.size() >= sizeof(address.sun_path) ||
          native.find('\0') != std::string::npos) {
        return false;
      }
      std::memcpy(address.sun_path, native.c_str(), native.size() + 1);
      auto descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      if (descriptor < 0) {
        return false;
      }
      const auto result = ::connect(
        descriptor,
        reinterpret_cast<const sockaddr *>(&address),
        static_cast<socklen_t>(
          offsetof(sockaddr_un, sun_path) + native.size() + 1
        )
      );
      const auto connect_error = errno;
      close_descriptor(descriptor);
      return result < 0 && connect_error == ECONNREFUSED;
    }

    authority_paths_t paths_for(
      const std::filesystem::path &root,
      std::string_view runtime_namespace
    ) {
      authority_paths_t paths;
      paths.root = root;
      paths.generation = root / runtime_namespace;
      paths.ipc = paths.generation / authority_ipc_directory_name;
      paths.auth = paths.generation / authority_auth_directory_name;
      paths.capability = paths.auth / authority_capability_file_name;
      paths.control_socket = paths.ipc / authority_control_socket_name;
      paths.media_socket = paths.ipc / authority_media_socket_name;
      return paths;
    }

    bool default_capability_factory(capability_t &capability) {
      return RAND_priv_bytes(
               capability.data(),
               static_cast<int>(capability.size())
             ) == 1;
    }
  }  // namespace

  authority_handle_t::authority_handle_t(
    endpoint_identity_t identity,
    authority_paths_t paths,
    capability_t capability,
    std::uint32_t owner_uid,
    std::uint64_t root_device,
    std::uint64_t root_inode,
    std::uint64_t generation_device,
    std::uint64_t generation_inode,
    std::uint64_t ipc_device,
    std::uint64_t ipc_inode,
    std::uint64_t auth_device,
    std::uint64_t auth_inode,
    std::uint64_t capability_device,
    std::uint64_t capability_inode,
    int generation_fd,
    int ipc_fd,
    int auth_fd
  ) :
      identity_(std::move(identity)),
      paths_(std::move(paths)),
      capability_(capability),
      owner_uid_(owner_uid),
      root_device_(root_device),
      root_inode_(root_inode),
      generation_device_(generation_device),
      generation_inode_(generation_inode),
      ipc_device_(ipc_device),
      ipc_inode_(ipc_inode),
      auth_device_(auth_device),
      auth_inode_(auth_inode),
      capability_device_(capability_device),
      capability_inode_(capability_inode),
      generation_fd_(generation_fd),
      ipc_fd_(ipc_fd),
      auth_fd_(auth_fd),
      active_(true) {
  }

  authority_handle_t::~authority_handle_t() {
    release_resources();
  }

  authority_handle_t::authority_handle_t(authority_handle_t &&other) noexcept :
      identity_(std::move(other.identity_)),
      paths_(std::move(other.paths_)),
      capability_(other.capability_),
      owner_uid_(other.owner_uid_),
      root_device_(other.root_device_),
      root_inode_(other.root_inode_),
      generation_device_(other.generation_device_),
      generation_inode_(other.generation_inode_),
      ipc_device_(other.ipc_device_),
      ipc_inode_(other.ipc_inode_),
      auth_device_(other.auth_device_),
      auth_inode_(other.auth_inode_),
      capability_device_(other.capability_device_),
      capability_inode_(other.capability_inode_),
      generation_fd_(std::exchange(other.generation_fd_, -1)),
      ipc_fd_(std::exchange(other.ipc_fd_, -1)),
      auth_fd_(std::exchange(other.auth_fd_, -1)),
      active_(std::exchange(other.active_, false)) {
    OPENSSL_cleanse(other.capability_.data(), other.capability_.size());
  }

  authority_handle_t &authority_handle_t::operator=(authority_handle_t &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    release_resources();
    identity_ = std::move(other.identity_);
    paths_ = std::move(other.paths_);
    capability_ = other.capability_;
    owner_uid_ = other.owner_uid_;
    root_device_ = other.root_device_;
    root_inode_ = other.root_inode_;
    generation_device_ = other.generation_device_;
    generation_inode_ = other.generation_inode_;
    ipc_device_ = other.ipc_device_;
    ipc_inode_ = other.ipc_inode_;
    auth_device_ = other.auth_device_;
    auth_inode_ = other.auth_inode_;
    capability_device_ = other.capability_device_;
    capability_inode_ = other.capability_inode_;
    generation_fd_ = std::exchange(other.generation_fd_, -1);
    ipc_fd_ = std::exchange(other.ipc_fd_, -1);
    auth_fd_ = std::exchange(other.auth_fd_, -1);
    active_ = std::exchange(other.active_, false);
    OPENSSL_cleanse(other.capability_.data(), other.capability_.size());
    return *this;
  }

  bool authority_handle_t::active() const {
    return active_;
  }

  const endpoint_identity_t &authority_handle_t::identity() const {
    return identity_;
  }

  const authority_paths_t &authority_handle_t::paths() const {
    return paths_;
  }

  std::span<const std::uint8_t, capability_size> authority_handle_t::capability() const {
    return capability_;
  }

  std::uint32_t authority_handle_t::owner_uid() const {
    return owner_uid_;
  }

  void authority_handle_t::release_resources() noexcept {
    close_descriptor(auth_fd_);
    close_descriptor(ipc_fd_);
    close_descriptor(generation_fd_);
    OPENSSL_cleanse(capability_.data(), capability_.size());
    active_ = false;
  }

  authority_store_t::authority_store_t(
    std::filesystem::path root,
    capability_factory_t capability_factory
  ) :
      root_(std::move(root)),
      capability_factory_(std::move(capability_factory)),
      owner_uid_(static_cast<std::uint32_t>(::geteuid())) {
    if (!clean_absolute_path(root_) || owner_uid_ == 0) {
      status_ = authority_status_e::invalid_argument;
      return;
    }
    root_fd_ = open_absolute_directory_without_symlinks(root_);
    struct stat metadata {};
    if (root_fd_ < 0 ||
        ::fstat(root_fd_, &metadata) != 0 ||
        !private_directory(metadata, owner_uid_)) {
      close_descriptor(root_fd_);
      status_ = authority_status_e::unsafe_root;
      return;
    }
    const auto identity = identity_of(metadata);
    root_device_ = identity.device;
    root_inode_ = identity.inode;
    if (!capability_factory_) {
      capability_factory_ = default_capability_factory;
    }
    status_ = authority_status_e::applied;
  }

  authority_store_t::~authority_store_t() {
    close_descriptor(root_fd_);
  }

  authority_status_e authority_store_t::status() const {
    return status_;
  }

  authority_create_result_t authority_store_t::create(
    const endpoint_identity_t &identity,
    std::string runtime_namespace
  ) {
    if (status_ != authority_status_e::applied) {
      return {.status = status_};
    }
    const auto paths = paths_for(root_, runtime_namespace);
    if (!valid_identity(identity) ||
        !opaque_name_token(runtime_namespace) ||
        !socket_path_fits(paths.control_socket) ||
        !socket_path_fits(paths.media_socket)) {
      return {.status = authority_status_e::invalid_argument};
    }

    auto current_root = open_absolute_directory_without_symlinks(root_);
    const auto root_matches = descriptor_is_directory(
      current_root,
      owner_uid_,
      root_device_,
      root_inode_
    );
    close_descriptor(current_root);
    if (!root_matches ||
        !descriptor_is_directory(root_fd_, owner_uid_, root_device_, root_inode_)) {
      return {.status = authority_status_e::unsafe_root};
    }

    bool generation_exists = false;
    const auto generation_fd = create_private_directory(
      root_fd_,
      runtime_namespace,
      generation_exists
    );
    if (generation_fd < 0) {
      return {
        .status = generation_exists ?
                    authority_status_e::already_exists :
                    authority_status_e::io_error,
      };
    }
    auto owned_generation_fd = generation_fd;
    auto ipc_fd = -1;
    auto auth_fd = -1;
    bool token_created = false;
    const auto rollback = [&]() {
      if (auth_fd >= 0 && token_created) {
        (void) ::unlinkat(
          auth_fd,
          std::string {authority_capability_file_name}.c_str(),
          0
        );
      }
      close_descriptor(auth_fd);
      close_descriptor(ipc_fd);
      if (owned_generation_fd >= 0) {
        (void) ::unlinkat(
          owned_generation_fd,
          std::string {authority_auth_directory_name}.c_str(),
          AT_REMOVEDIR
        );
        (void) ::unlinkat(
          owned_generation_fd,
          std::string {authority_ipc_directory_name}.c_str(),
          AT_REMOVEDIR
        );
      }
      close_descriptor(owned_generation_fd);
      (void) ::unlinkat(root_fd_, runtime_namespace.c_str(), AT_REMOVEDIR);
    };

    struct stat generation_metadata {};
    if (::fstat(owned_generation_fd, &generation_metadata) != 0 ||
        !private_directory(generation_metadata, owner_uid_)) {
      rollback();
      return {.status = authority_status_e::io_error};
    }
    bool child_exists = false;
    ipc_fd = create_private_directory(
      owned_generation_fd,
      authority_ipc_directory_name,
      child_exists
    );
    auth_fd = create_private_directory(
      owned_generation_fd,
      authority_auth_directory_name,
      child_exists
    );
    if (ipc_fd < 0 || auth_fd < 0) {
      rollback();
      return {.status = authority_status_e::io_error};
    }

    capability_t capability {};
    bool random_ready = false;
    try {
      random_ready = capability_factory_(capability);
    } catch (...) {
      random_ready = false;
    }
    if (!random_ready ||
        std::none_of(
          capability.begin(),
          capability.end(),
          [](std::uint8_t byte) { return byte != 0; }
        )) {
      OPENSSL_cleanse(capability.data(), capability.size());
      rollback();
      return {.status = authority_status_e::random_failed};
    }

    const auto capability_name = std::string {authority_capability_file_name};
    auto capability_fd = ::openat(
      auth_fd,
      capability_name.c_str(),
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      0600
    );
    if (capability_fd < 0) {
      OPENSSL_cleanse(capability.data(), capability.size());
      rollback();
      return {.status = authority_status_e::io_error};
    }
    token_created = true;
    auto encoded_capability = capability_hex(capability) + "\n";
    struct stat capability_metadata {};
    const auto capability_written =
      ::fchmod(capability_fd, 0600) == 0 &&
      write_all(capability_fd, encoded_capability) &&
      ::fsync(capability_fd) == 0 &&
      ::fstat(capability_fd, &capability_metadata) == 0 &&
      private_regular_file(capability_metadata, owner_uid_) &&
      capability_metadata.st_size == static_cast<off_t>(encoded_capability.size());
    close_descriptor(capability_fd);
    OPENSSL_cleanse(encoded_capability.data(), encoded_capability.size());
    if (!capability_written) {
      OPENSSL_cleanse(capability.data(), capability.size());
      rollback();
      return {.status = authority_status_e::io_error};
    }

    struct stat ipc_metadata {};
    struct stat auth_metadata {};
    if (::fstat(ipc_fd, &ipc_metadata) != 0 ||
        ::fstat(auth_fd, &auth_metadata) != 0 ||
        !private_directory(ipc_metadata, owner_uid_) ||
        !private_directory(auth_metadata, owner_uid_)) {
      OPENSSL_cleanse(capability.data(), capability.size());
      rollback();
      return {.status = authority_status_e::io_error};
    }

    current_root = open_absolute_directory_without_symlinks(root_);
    const auto still_matches = descriptor_is_directory(
      current_root,
      owner_uid_,
      root_device_,
      root_inode_
    );
    close_descriptor(current_root);
    if (!still_matches ||
        !entry_is_directory(
          root_fd_,
          runtime_namespace,
          owner_uid_,
          identity_of(generation_metadata).device,
          identity_of(generation_metadata).inode
        )) {
      OPENSSL_cleanse(capability.data(), capability.size());
      rollback();
      return {.status = authority_status_e::integrity_violation};
    }

    const auto generation_identity = identity_of(generation_metadata);
    const auto ipc_identity = identity_of(ipc_metadata);
    const auto auth_identity = identity_of(auth_metadata);
    const auto token_identity = identity_of(capability_metadata);
    authority_handle_t handle {
      identity,
      paths,
      capability,
      owner_uid_,
      root_device_,
      root_inode_,
      generation_identity.device,
      generation_identity.inode,
      ipc_identity.device,
      ipc_identity.inode,
      auth_identity.device,
      auth_identity.inode,
      token_identity.device,
      token_identity.inode,
      std::exchange(owned_generation_fd, -1),
      std::exchange(ipc_fd, -1),
      std::exchange(auth_fd, -1)
    };
    OPENSSL_cleanse(capability.data(), capability.size());
    return {
      .status = authority_status_e::applied,
      .authority = std::move(handle),
    };
  }

  authority_status_e authority_store_t::validate(
    const authority_handle_t &authority
  ) const {
    if (status_ != authority_status_e::applied || !authority.active_) {
      return authority_status_e::inactive;
    }
    if (authority.owner_uid_ != owner_uid_ ||
        authority.root_device_ != root_device_ ||
        authority.root_inode_ != root_inode_ ||
        authority.paths_.root != root_ ||
        !descriptor_is_directory(
          root_fd_,
          owner_uid_,
          root_device_,
          root_inode_
        )) {
      return authority_status_e::integrity_violation;
    }
    auto current_root = open_absolute_directory_without_symlinks(root_);
    const auto root_matches = descriptor_is_directory(
      current_root,
      owner_uid_,
      root_device_,
      root_inode_
    );
    close_descriptor(current_root);
    if (!root_matches ||
        !descriptor_is_directory(
          authority.generation_fd_,
          owner_uid_,
          authority.generation_device_,
          authority.generation_inode_
        ) ||
        !descriptor_is_directory(
          authority.ipc_fd_,
          owner_uid_,
          authority.ipc_device_,
          authority.ipc_inode_
        ) ||
        !descriptor_is_directory(
          authority.auth_fd_,
          owner_uid_,
          authority.auth_device_,
          authority.auth_inode_
        )) {
      return authority_status_e::integrity_violation;
    }

    const auto generation_name = authority.paths_.generation.filename().native();
    if (!entry_is_directory(
          root_fd_,
          generation_name,
          owner_uid_,
          authority.generation_device_,
          authority.generation_inode_
        ) ||
        !entry_is_directory(
          authority.generation_fd_,
          authority_ipc_directory_name,
          owner_uid_,
          authority.ipc_device_,
          authority.ipc_inode_
        ) ||
        !entry_is_directory(
          authority.generation_fd_,
          authority_auth_directory_name,
          owner_uid_,
          authority.auth_device_,
          authority.auth_inode_
        )) {
      return authority_status_e::integrity_violation;
    }

    struct stat capability_metadata {};
    const auto capability_name = std::string {authority_capability_file_name};
    if (::fstatat(
          authority.auth_fd_,
          capability_name.c_str(),
          &capability_metadata,
          AT_SYMLINK_NOFOLLOW
        ) != 0 ||
        !private_regular_file(capability_metadata, owner_uid_) ||
        !exact_identity(
          capability_metadata,
          authority.capability_device_,
          authority.capability_inode_
        ) ||
        capability_metadata.st_size != static_cast<off_t>(capability_size * 2 + 1) ||
        !capability_file_matches(
          authority.auth_fd_,
          authority_capability_file_name,
          owner_uid_,
          authority.capability_device_,
          authority.capability_inode_,
          authority.capability_
        ) ||
        !allowed_directory_entries(
          authority.generation_fd_,
          {
            std::string {authority_ipc_directory_name},
            std::string {authority_auth_directory_name},
          },
          {}
        ) ||
        !allowed_directory_entries(
          authority.auth_fd_,
          {std::string {authority_capability_file_name}},
          {}
        ) ||
        !allowed_directory_entries(
          authority.ipc_fd_,
          {},
          {
            std::string {authority_control_socket_name},
            std::string {authority_media_socket_name},
          }
        )) {
      return authority_status_e::integrity_violation;
    }

    for (const auto socket_name : {
           authority_control_socket_name,
           authority_media_socket_name,
         }) {
      struct stat socket_metadata {};
      const std::string owned_name {socket_name};
      if (::fstatat(
            authority.ipc_fd_,
            owned_name.c_str(),
            &socket_metadata,
            AT_SYMLINK_NOFOLLOW
          ) == 0) {
        if (!private_socket(socket_metadata, owner_uid_)) {
          return authority_status_e::integrity_violation;
        }
      } else if (errno != ENOENT) {
        return authority_status_e::integrity_violation;
      }
    }
    return authority_status_e::applied;
  }

  authority_status_e authority_store_t::remove(authority_handle_t &authority) {
    if (!authority.active_) {
      return authority_status_e::inactive;
    }
    if (validate(authority) != authority_status_e::applied) {
      return authority_status_e::integrity_violation;
    }

    const std::array socket_entries {
      std::pair {authority_control_socket_name, authority.paths_.control_socket},
      std::pair {authority_media_socket_name, authority.paths_.media_socket},
    };
    std::array<bool, 2> socket_exists {};
    std::array<file_identity_t, 2> socket_identities {};
    for (std::size_t index = 0; index < socket_entries.size(); ++index) {
      const auto &[socket_name, socket_path] = socket_entries[index];
      const std::string owned_name {socket_name};
      struct stat metadata {};
      if (::fstatat(
            authority.ipc_fd_,
            owned_name.c_str(),
            &metadata,
            AT_SYMLINK_NOFOLLOW
          ) == 0) {
        socket_exists[index] = true;
        socket_identities[index] = identity_of(metadata);
        if (!private_socket(metadata, owner_uid_) || !inactive_socket(socket_path)) {
          return authority_status_e::integrity_violation;
        }
      } else if (errno != ENOENT) {
        return authority_status_e::io_error;
      }
    }
    for (std::size_t index = 0; index < socket_entries.size(); ++index) {
      if (!socket_exists[index]) {
        continue;
      }
      const auto socket_name = std::string {socket_entries[index].first};
      struct stat metadata {};
      if (::fstatat(
            authority.ipc_fd_,
            socket_name.c_str(),
            &metadata,
            AT_SYMLINK_NOFOLLOW
          ) != 0 ||
          !private_socket(metadata, owner_uid_) ||
          identity_of(metadata) != socket_identities[index] ||
          !inactive_socket(socket_entries[index].second) ||
          ::unlinkat(authority.ipc_fd_, socket_name.c_str(), 0) != 0) {
        return authority_status_e::integrity_violation;
      }
    }

    const auto capability_name = std::string {authority_capability_file_name};
    struct stat capability_metadata {};
    if (::fstatat(
          authority.auth_fd_,
          capability_name.c_str(),
          &capability_metadata,
          AT_SYMLINK_NOFOLLOW
        ) != 0 ||
        !private_regular_file(capability_metadata, owner_uid_) ||
        !exact_identity(
          capability_metadata,
          authority.capability_device_,
          authority.capability_inode_
        ) ||
        !entry_is_directory(
          authority.generation_fd_,
          authority_auth_directory_name,
          owner_uid_,
          authority.auth_device_,
          authority.auth_inode_
        ) ||
        !entry_is_directory(
          authority.generation_fd_,
          authority_ipc_directory_name,
          owner_uid_,
          authority.ipc_device_,
          authority.ipc_inode_
        ) ||
        ::unlinkat(authority.auth_fd_, capability_name.c_str(), 0) != 0 ||
        ::unlinkat(
          authority.generation_fd_,
          std::string {authority_auth_directory_name}.c_str(),
          AT_REMOVEDIR
        ) != 0 ||
        ::unlinkat(
          authority.generation_fd_,
          std::string {authority_ipc_directory_name}.c_str(),
          AT_REMOVEDIR
        ) != 0 ||
        ::unlinkat(
          root_fd_,
          authority.paths_.generation.filename().c_str(),
          AT_REMOVEDIR
        ) != 0) {
      return authority_status_e::io_error;
    }

    authority.release_resources();
    return authority_status_e::applied;
  }

}  // namespace multiseat::worker_ipc

#endif
