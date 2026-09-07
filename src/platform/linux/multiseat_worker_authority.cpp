/**
 * @file src/platform/linux/multiseat_worker_authority.cpp
 * @brief Private per-generation filesystem authority for multiseat workers.
 */
#include "multiseat_worker_authority.h"

#ifdef __linux__

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace multiseat::worker_ipc {
  namespace {
    constexpr std::size_t max_authority_entries = 256;
    constexpr std::size_t max_authority_record_size = 1024;
    constexpr std::size_t max_provider_catalog_size = 1024 * 1024;
    constexpr std::array<std::uint8_t, 8> authority_record_magic {
      'P', 'W', 'A', 'U', 'T', 'H', '0', '2'
    };
    constexpr std::string_view provider_catalog_temporary_file_name =
      ".runtime-providers.json.tmp";
    constexpr std::string_view provider_root = "/usr/libexec/polaris-seat/";

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

    bool immutable_private_regular_file(
      const struct stat &metadata,
      std::uint32_t owner_uid
    ) {
      return S_ISREG(metadata.st_mode) &&
             static_cast<std::uint32_t>(metadata.st_uid) == owner_uid &&
             (metadata.st_mode & 07777) == 0400;
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

    bool write_all(int descriptor, std::span<const std::uint8_t> payload) {
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

    bool create_private_file(
      int parent,
      std::string_view name,
      std::span<const std::uint8_t> payload,
      std::uint32_t owner_uid,
      struct stat &metadata
    ) {
      const std::string owned_name {name};
      auto descriptor = ::openat(
        parent,
        owned_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600
      );
      if (descriptor < 0) {
        return false;
      }
      const auto written = ::fchmod(descriptor, 0600) == 0 &&
                           write_all(descriptor, payload) &&
                           ::fsync(descriptor) == 0 &&
                           ::fstat(descriptor, &metadata) == 0 &&
                           private_regular_file(metadata, owner_uid) &&
                           metadata.st_size == static_cast<off_t>(payload.size());
      if (!written) {
        struct stat descriptor_metadata {};
        struct stat entry_metadata {};
        if (::fstat(descriptor, &descriptor_metadata) == 0 &&
            ::fstatat(
              parent,
              owned_name.c_str(),
              &entry_metadata,
              AT_SYMLINK_NOFOLLOW
            ) == 0 &&
            identity_of(descriptor_metadata) == identity_of(entry_metadata)) {
          (void) ::unlinkat(parent, owned_name.c_str(), 0);
        }
      }
      close_descriptor(descriptor);
      return written;
    }

    bool create_atomic_immutable_file(
      int parent,
      std::string_view name,
      std::span<const std::uint8_t> payload,
      std::uint32_t owner_uid,
      struct stat &metadata
    ) {
      const std::string owned_name {name};
      const std::string temporary_name {provider_catalog_temporary_file_name};
      auto descriptor = ::openat(
        parent,
        temporary_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600
      );
      if (descriptor < 0) {
        return false;
      }
      bool final_linked = false;
      const auto cleanup = [&]() {
        struct stat descriptor_metadata {};
        if (::fstat(descriptor, &descriptor_metadata) == 0) {
          for (const auto &candidate : {owned_name, temporary_name}) {
            struct stat entry_metadata {};
            if (::fstatat(
                  parent,
                  candidate.c_str(),
                  &entry_metadata,
                  AT_SYMLINK_NOFOLLOW
                ) == 0 &&
                identity_of(descriptor_metadata) == identity_of(entry_metadata)) {
              (void) ::unlinkat(parent, candidate.c_str(), 0);
            }
          }
        }
      };
      const auto prepared = write_all(descriptor, payload) &&
                            ::fsync(descriptor) == 0 &&
                            ::fchmod(descriptor, 0400) == 0 &&
                            ::fsync(descriptor) == 0 &&
                            ::fstat(descriptor, &metadata) == 0 &&
                            immutable_private_regular_file(metadata, owner_uid) &&
                            metadata.st_size == static_cast<off_t>(payload.size());
      if (prepared &&
          ::linkat(
            parent,
            temporary_name.c_str(),
            parent,
            owned_name.c_str(),
            0
          ) == 0) {
        final_linked = true;
      }
      struct stat published_metadata {};
      auto published = final_linked &&
                       ::fstatat(
                         parent,
                         owned_name.c_str(),
                         &published_metadata,
                         AT_SYMLINK_NOFOLLOW
                       ) == 0 &&
                       identity_of(published_metadata) == identity_of(metadata) &&
                       immutable_private_regular_file(published_metadata, owner_uid) &&
                       published_metadata.st_size ==
                         static_cast<off_t>(payload.size());
      if (published) {
        published = ::unlinkat(parent, temporary_name.c_str(), 0) == 0 &&
                    ::fsync(parent) == 0;
      }
      struct stat stable_metadata {};
      if (published) {
        published = ::fstatat(
                      parent,
                      owned_name.c_str(),
                      &stable_metadata,
                      AT_SYMLINK_NOFOLLOW
                    ) == 0 &&
                    identity_of(stable_metadata) == identity_of(metadata) &&
                    immutable_private_regular_file(stable_metadata, owner_uid) &&
                    stable_metadata.st_size == static_cast<off_t>(payload.size());
      }
      if (!published) {
        cleanup();
      } else {
        metadata = stable_metadata;
      }
      close_descriptor(descriptor);
      return published;
    }

    struct private_file_payload_t {
      file_identity_t identity;
      std::vector<std::uint8_t> payload;
    };

    std::optional<private_file_payload_t> read_private_file(
      int parent,
      std::string_view name,
      std::uint32_t owner_uid,
      std::size_t max_size,
      mode_t expected_mode = 0600
    ) {
      const std::string owned_name {name};
      auto descriptor = ::openat(
        parent,
        owned_name.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW
      );
      if (descriptor < 0) {
        return std::nullopt;
      }
      struct stat before {};
      if (::fstat(descriptor, &before) != 0 ||
          !(expected_mode == 0600 ?
              private_regular_file(before, owner_uid) :
              expected_mode == 0400 &&
                immutable_private_regular_file(before, owner_uid)) ||
          before.st_size < 0 ||
          static_cast<std::uint64_t>(before.st_size) > max_size) {
        close_descriptor(descriptor);
        return std::nullopt;
      }
      std::vector<std::uint8_t> payload(static_cast<std::size_t>(before.st_size));
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
      std::uint8_t trailing = 0;
      ssize_t trailing_result;
      do {
        trailing_result = ::read(descriptor, &trailing, 1);
      } while (trailing_result < 0 && errno == EINTR);
      struct stat after {};
      const auto stable = ::fstat(descriptor, &after) == 0 &&
                          (expected_mode == 0600 ?
                             private_regular_file(after, owner_uid) :
                             expected_mode == 0400 &&
                               immutable_private_regular_file(after, owner_uid)) &&
                          identity_of(before) == identity_of(after) &&
                          before.st_size == after.st_size;
      close_descriptor(descriptor);
      if (!stable || offset != payload.size() || trailing_result != 0) {
        if (!payload.empty()) {
          OPENSSL_cleanse(payload.data(), payload.size());
        }
        return std::nullopt;
      }
      return private_file_payload_t {
        .identity = identity_of(after),
        .payload = std::move(payload),
      };
    }

    struct capability_file_t {
      capability_t capability {};
      file_identity_t identity;
    };

    std::optional<capability_file_t> read_capability_file(
      int auth_fd,
      std::uint32_t owner_uid
    ) {
      auto file = read_private_file(
        auth_fd,
        authority_capability_file_name,
        owner_uid,
        capability_size * 2 + 1
      );
      if (!file || file->payload.size() != capability_size * 2 + 1 ||
          file->payload.back() != '\n') {
        if (file && !file->payload.empty()) {
          OPENSSL_cleanse(file->payload.data(), file->payload.size());
        }
        return std::nullopt;
      }
      const std::string_view encoded {
        reinterpret_cast<const char *>(file->payload.data()),
        capability_size * 2,
      };
      auto capability = parse_capability_hex(encoded);
      const auto nonzero = capability && std::any_of(
        capability->begin(),
        capability->end(),
        [](std::uint8_t byte) {
          return byte != 0;
        }
      );
      OPENSSL_cleanse(file->payload.data(), file->payload.size());
      if (!nonzero) {
        if (capability) {
          OPENSSL_cleanse(capability->data(), capability->size());
        }
        return std::nullopt;
      }
      capability_file_t result {
        .capability = *capability,
        .identity = file->identity,
      };
      OPENSSL_cleanse(capability->data(), capability->size());
      return result;
    }

    void append_u16(std::vector<std::uint8_t> &payload, std::uint16_t value) {
      payload.push_back(static_cast<std::uint8_t>(value >> 8U));
      payload.push_back(static_cast<std::uint8_t>(value));
    }

    void append_u32(std::vector<std::uint8_t> &payload, std::uint32_t value) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        payload.push_back(static_cast<std::uint8_t>(value >> shift));
      }
    }

    void append_u64(std::vector<std::uint8_t> &payload, std::uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8) {
        payload.push_back(static_cast<std::uint8_t>(value >> shift));
      }
    }

    void append_string(std::vector<std::uint8_t> &payload, std::string_view value) {
      append_u16(payload, static_cast<std::uint16_t>(value.size()));
      payload.insert(payload.end(), value.begin(), value.end());
    }

    std::optional<proof_t> sha256_digest(std::span<const std::uint8_t> payload) {
      proof_t digest {};
      unsigned int length = 0;
      if (EVP_Digest(
            payload.data(),
            payload.size(),
            digest.data(),
            &length,
            EVP_sha256(),
            nullptr
          ) != 1 ||
          length != digest.size()) {
        return std::nullopt;
      }
      return digest;
    }

    std::string_view compositor_name(compositor_e compositor) {
      switch (compositor) {
        case compositor_e::gamescope:
          return "gamescope";
        case compositor_e::sway:
          return "sway";
        case compositor_e::labwc:
          return "labwc";
        case compositor_e::automatic:
          break;
      }
      return {};
    }

    std::string_view workload_kind_name(workload_kind_e kind) {
      switch (kind) {
        case workload_kind_e::gamescope:
          return "gamescope";
        case workload_kind_e::steam:
          return "steam";
        case workload_kind_e::heroic:
          return "heroic";
        case workload_kind_e::lutris:
          return "lutris";
        case workload_kind_e::unknown:
          break;
      }
      return {};
    }

    std::optional<proof_t> authority_record_hmac(
      const capability_t &capability,
      std::span<const std::uint8_t> payload
    ) {
      proof_t proof {};
      unsigned int length = 0;
      const auto *result = HMAC(
        EVP_sha256(),
        capability.data(),
        static_cast<int>(capability.size()),
        payload.data(),
        payload.size(),
        proof.data(),
        &length
      );
      if (!result || length != proof.size()) {
        OPENSSL_cleanse(proof.data(), proof.size());
        return std::nullopt;
      }
      return proof;
    }

    std::optional<std::vector<std::uint8_t>> encode_authority_record(
      const endpoint_identity_t &identity,
      std::string_view runtime_namespace,
      const proof_t &provider_catalog_digest,
      const capability_t &capability
    ) {
      if (!valid_identity(identity) || !opaque_name_token(runtime_namespace)) {
        return std::nullopt;
      }
      std::vector<std::uint8_t> payload;
      payload.reserve(
        authority_record_magic.size() + 4 + 8 + 8 +
        identity.controller_epoch.size() + identity.logical_gpu_id.size() +
        identity.worker_name.size() + runtime_namespace.size() +
        provider_catalog_digest.size() + proof_size
      );
      payload.insert(
        payload.end(),
        authority_record_magic.begin(),
        authority_record_magic.end()
      );
      append_u32(payload, identity.slot);
      append_u64(payload, identity.generation);
      append_string(payload, identity.controller_epoch);
      append_string(payload, identity.logical_gpu_id);
      append_string(payload, identity.worker_name);
      append_string(payload, runtime_namespace);
      payload.insert(
        payload.end(),
        provider_catalog_digest.begin(),
        provider_catalog_digest.end()
      );
      auto proof = authority_record_hmac(capability, payload);
      if (!proof) {
        return std::nullopt;
      }
      payload.insert(payload.end(), proof->begin(), proof->end());
      OPENSSL_cleanse(proof->data(), proof->size());
      return payload;
    }

    bool read_u16(
      std::span<const std::uint8_t> payload,
      std::size_t &offset,
      std::uint16_t &value
    ) {
      if (offset > payload.size() || payload.size() - offset < 2) {
        return false;
      }
      value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(payload[offset]) << 8U) |
        payload[offset + 1]
      );
      offset += 2;
      return true;
    }

    bool read_u32(
      std::span<const std::uint8_t> payload,
      std::size_t &offset,
      std::uint32_t &value
    ) {
      if (offset > payload.size() || payload.size() - offset < 4) {
        return false;
      }
      value = 0;
      for (int index = 0; index < 4; ++index) {
        value = (value << 8U) | payload[offset++];
      }
      return true;
    }

    bool read_u64(
      std::span<const std::uint8_t> payload,
      std::size_t &offset,
      std::uint64_t &value
    ) {
      if (offset > payload.size() || payload.size() - offset < 8) {
        return false;
      }
      value = 0;
      for (int index = 0; index < 8; ++index) {
        value = (value << 8U) | payload[offset++];
      }
      return true;
    }

    bool read_string(
      std::span<const std::uint8_t> payload,
      std::size_t &offset,
      std::string &value
    ) {
      std::uint16_t size = 0;
      if (!read_u16(payload, offset, size) ||
          offset > payload.size() ||
          payload.size() - offset < size) {
        return false;
      }
      value.assign(
        reinterpret_cast<const char *>(payload.data() + offset),
        size
      );
      offset += size;
      return true;
    }

    struct authority_record_t {
      endpoint_identity_t identity;
      std::string runtime_namespace;
      proof_t provider_catalog_digest {};
    };

    std::optional<authority_record_t> parse_authority_record(
      std::span<const std::uint8_t> encoded,
      const capability_t &capability
    ) {
      if (encoded.size() <
            authority_record_magic.size() + 4 + 8 + 8 + proof_size * 2 ||
          encoded.size() > max_authority_record_size) {
        return std::nullopt;
      }
      const auto transcript = encoded.first(encoded.size() - proof_size);
      const auto presented = encoded.last(proof_size);
      auto expected = authority_record_hmac(capability, transcript);
      if (!expected || CRYPTO_memcmp(
                         expected->data(),
                         presented.data(),
                         proof_size
                       ) != 0) {
        if (expected) {
          OPENSSL_cleanse(expected->data(), expected->size());
        }
        return std::nullopt;
      }
      OPENSSL_cleanse(expected->data(), expected->size());
      if (!std::equal(
            authority_record_magic.begin(),
            authority_record_magic.end(),
            transcript.begin()
          )) {
        return std::nullopt;
      }
      std::size_t offset = authority_record_magic.size();
      authority_record_t record;
      if (!read_u32(transcript, offset, record.identity.slot) ||
          !read_u64(transcript, offset, record.identity.generation) ||
          !read_string(transcript, offset, record.identity.controller_epoch) ||
          !read_string(transcript, offset, record.identity.logical_gpu_id) ||
          !read_string(transcript, offset, record.identity.worker_name) ||
          !read_string(transcript, offset, record.runtime_namespace) ||
          offset > transcript.size() ||
          transcript.size() - offset < record.provider_catalog_digest.size()) {
        return std::nullopt;
      }
      std::copy_n(
        transcript.begin() + static_cast<std::ptrdiff_t>(offset),
        record.provider_catalog_digest.size(),
        record.provider_catalog_digest.begin()
      );
      offset += record.provider_catalog_digest.size();
      if (offset != transcript.size() ||
          !valid_identity(record.identity) ||
          !opaque_name_token(record.runtime_namespace)) {
        return std::nullopt;
      }
      return record;
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

    std::optional<std::vector<std::string>> bounded_directory_entries(
      int descriptor,
      std::size_t maximum
    ) {
      const auto independent = ::openat(
        descriptor,
        ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      );
      if (independent < 0) {
        return std::nullopt;
      }
      auto *directory = ::fdopendir(independent);
      if (!directory) {
        (void) ::close(independent);
        return std::nullopt;
      }
      std::vector<std::string> entries;
      bool valid = true;
      errno = 0;
      while (const auto *entry = ::readdir(directory)) {
        const std::string_view name {entry->d_name};
        if (name != "." && name != "..") {
          if (!opaque_name_token(name) || entries.size() >= maximum) {
            valid = false;
            break;
          }
          entries.emplace_back(name);
        }
        errno = 0;
      }
      const auto read_error = errno;
      (void) ::closedir(directory);
      if (!valid || read_error != 0) {
        return std::nullopt;
      }
      std::sort(entries.begin(), entries.end());
      return entries;
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
      std::uint32_t owner_uid,
      std::uint64_t expected_device,
      std::uint64_t expected_inode,
      const capability_t &capability
    ) {
      auto file = read_capability_file(auth_fd, owner_uid);
      if (!file) {
        return false;
      }
      const auto matches = file->identity == file_identity_t {
        expected_device,
        expected_inode,
      } && CRYPTO_memcmp(
        file->capability.data(),
        capability.data(),
        capability.size()
      ) == 0;
      OPENSSL_cleanse(file->capability.data(), file->capability.size());
      return matches;
    }

    bool authority_record_matches(
      int auth_fd,
      std::uint32_t owner_uid,
      std::uint64_t expected_device,
      std::uint64_t expected_inode,
      const endpoint_identity_t &identity,
      std::string_view runtime_namespace,
      const proof_t &provider_catalog_digest,
      const capability_t &capability
    ) {
      auto file = read_private_file(
        auth_fd,
        authority_record_file_name,
        owner_uid,
        max_authority_record_size
      );
      if (!file || file->identity != file_identity_t {
            expected_device,
            expected_inode,
          }) {
        return false;
      }
      const auto record = parse_authority_record(file->payload, capability);
      return record &&
             record->identity == identity &&
             record->runtime_namespace == runtime_namespace &&
             CRYPTO_memcmp(
               record->provider_catalog_digest.data(),
               provider_catalog_digest.data(),
               provider_catalog_digest.size()
             ) == 0;
    }

    bool provider_catalog_file_matches(
      int auth_fd,
      std::uint32_t owner_uid,
      std::uint64_t expected_device,
      std::uint64_t expected_inode,
      const proof_t &expected_digest
    ) {
      auto file = read_private_file(
        auth_fd,
        authority_provider_catalog_file_name,
        owner_uid,
        max_provider_catalog_size,
        0400
      );
      if (!file || file->payload.empty() || file->identity != file_identity_t {
            expected_device,
            expected_inode,
          }) {
        return false;
      }
      const auto digest = sha256_digest(file->payload);
      return digest && CRYPTO_memcmp(
                         digest->data(),
                         expected_digest.data(),
                         expected_digest.size()
                       ) == 0;
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
      paths.record = paths.auth / authority_record_file_name;
      paths.provider_catalog = paths.auth / authority_provider_catalog_file_name;
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

  std::optional<std::vector<std::uint8_t>> encode_provider_catalog(
    const provider_catalog_selection_t &selection
  ) {
    const auto compositor = compositor_name(selection.compositor);
    const auto workload_kind = workload_kind_name(selection.workload.kind);
    if (compositor.empty() || workload_kind.empty() ||
        !valid_workload_plan(selection.workload)) {
      return std::nullopt;
    }
    try {
      using json = nlohmann::json;
      const auto provider = [](
                              std::string_view stage,
                              std::string_view executable,
                              std::string_view selector = {},
                              std::string_view target_id = {}
                            ) {
        json entry {
          {"stage", stage},
          {"executable", executable},
          {"arguments", json::array()},
        };
        if (!selector.empty()) {
          entry["selector"] = selector;
        }
        if (!target_id.empty()) {
          entry["target_id"] = target_id;
        }
        return entry;
      };
      const auto executable = [](std::string_view name) {
        return std::string {provider_root} + std::string {name};
      };
      json catalog {
        {"schema", 1},
        {"providers", json::array({
          provider("session-bus", executable("session-bus")),
          provider("audio", executable("audio")),
          provider("display-capture", executable("display-capture")),
          provider(
            "nested-compositor",
            executable("nested-compositor"),
            compositor
          ),
          provider("virtual-input", executable("virtual-input")),
          provider("encoder", executable("encoder")),
          provider(
            "launcher-process-tree",
            executable("launcher"),
            workload_kind,
            selection.workload.target_id
          ),
        })},
      };
      auto encoded = catalog.dump();
      encoded.push_back('\n');
      if (encoded.size() > max_provider_catalog_size) {
        return std::nullopt;
      }
      return std::vector<std::uint8_t> {encoded.begin(), encoded.end()};
    } catch (...) {
      return std::nullopt;
    }
  }

  authority_handle_t::authority_handle_t(
    endpoint_identity_t identity,
    authority_paths_t paths,
    const capability_t &capability,
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
    std::uint64_t record_device,
    std::uint64_t record_inode,
    std::uint64_t provider_catalog_device,
    std::uint64_t provider_catalog_inode,
    const proof_t &provider_catalog_digest,
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
      record_device_(record_device),
      record_inode_(record_inode),
      provider_catalog_device_(provider_catalog_device),
      provider_catalog_inode_(provider_catalog_inode),
      provider_catalog_digest_(provider_catalog_digest),
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
      record_device_(other.record_device_),
      record_inode_(other.record_inode_),
      provider_catalog_device_(other.provider_catalog_device_),
      provider_catalog_inode_(other.provider_catalog_inode_),
      provider_catalog_digest_(other.provider_catalog_digest_),
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
    record_device_ = other.record_device_;
    record_inode_ = other.record_inode_;
    provider_catalog_device_ = other.provider_catalog_device_;
    provider_catalog_inode_ = other.provider_catalog_inode_;
    provider_catalog_digest_ = other.provider_catalog_digest_;
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
    std::string runtime_namespace,
    const provider_catalog_selection_t &provider_selection
  ) {
    if (status_ != authority_status_e::applied) {
      return {.status = status_};
    }
    const auto encoded_provider_catalog = encode_provider_catalog(provider_selection);
    const auto provider_catalog_digest = encoded_provider_catalog ?
                                           sha256_digest(*encoded_provider_catalog) :
                                           std::nullopt;
    const auto paths = paths_for(root_, runtime_namespace);
    if (!valid_identity(identity) ||
        !opaque_name_token(runtime_namespace) ||
        !encoded_provider_catalog ||
        !provider_catalog_digest ||
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
    bool provider_catalog_created = false;
    bool record_created = false;
    const auto rollback = [&]() {
      if (auth_fd >= 0 && record_created) {
        (void) ::unlinkat(
          auth_fd,
          std::string {authority_record_file_name}.c_str(),
          0
        );
      }
      if (auth_fd >= 0 && provider_catalog_created) {
        (void) ::unlinkat(
          auth_fd,
          std::string {authority_provider_catalog_file_name}.c_str(),
          0
        );
      }
      if (auth_fd >= 0) {
        (void) ::unlinkat(
          auth_fd,
          std::string {provider_catalog_temporary_file_name}.c_str(),
          0
        );
      }
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

    auto encoded_capability = capability_hex(capability) + "\n";
    struct stat capability_metadata {};
    const auto capability_written = create_private_file(
      auth_fd,
      authority_capability_file_name,
      std::span<const std::uint8_t> {
        reinterpret_cast<const std::uint8_t *>(encoded_capability.data()),
        encoded_capability.size(),
      },
      owner_uid_,
      capability_metadata
    );
    token_created = capability_written;
    OPENSSL_cleanse(encoded_capability.data(), encoded_capability.size());
    if (!capability_written) {
      OPENSSL_cleanse(capability.data(), capability.size());
      rollback();
      return {.status = authority_status_e::io_error};
    }

    struct stat provider_catalog_metadata {};
    const auto provider_catalog_written = create_atomic_immutable_file(
      auth_fd,
      authority_provider_catalog_file_name,
      *encoded_provider_catalog,
      owner_uid_,
      provider_catalog_metadata
    );
    provider_catalog_created = provider_catalog_written;
    if (!provider_catalog_written) {
      OPENSSL_cleanse(capability.data(), capability.size());
      rollback();
      return {.status = authority_status_e::io_error};
    }

    auto encoded_record = encode_authority_record(
      identity,
      runtime_namespace,
      *provider_catalog_digest,
      capability
    );
    struct stat record_metadata {};
    const auto record_written = encoded_record && create_private_file(
      auth_fd,
      authority_record_file_name,
      *encoded_record,
      owner_uid_,
      record_metadata
    );
    record_created = record_written;
    if (encoded_record && !encoded_record->empty()) {
      OPENSSL_cleanse(encoded_record->data(), encoded_record->size());
    }
    if (!record_written ||
        ::fsync(auth_fd) != 0 ||
        ::fsync(owned_generation_fd) != 0 ||
        ::fsync(root_fd_) != 0) {
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
    const auto record_identity = identity_of(record_metadata);
    const auto provider_catalog_identity = identity_of(provider_catalog_metadata);
    const auto complete = entry_is_directory(
                            owned_generation_fd,
                            authority_ipc_directory_name,
                            owner_uid_,
                            ipc_identity.device,
                            ipc_identity.inode
                          ) &&
                          entry_is_directory(
                            owned_generation_fd,
                            authority_auth_directory_name,
                            owner_uid_,
                            auth_identity.device,
                            auth_identity.inode
                          ) &&
                          capability_file_matches(
                            auth_fd,
                            owner_uid_,
                            token_identity.device,
                            token_identity.inode,
                            capability
                          ) &&
                          provider_catalog_file_matches(
                            auth_fd,
                            owner_uid_,
                            provider_catalog_identity.device,
                            provider_catalog_identity.inode,
                            *provider_catalog_digest
                          ) &&
                          authority_record_matches(
                            auth_fd,
                            owner_uid_,
                            record_identity.device,
                            record_identity.inode,
                            identity,
                            runtime_namespace,
                            *provider_catalog_digest,
                            capability
                          ) &&
                          allowed_directory_entries(
                            owned_generation_fd,
                            {
                              std::string {authority_ipc_directory_name},
                              std::string {authority_auth_directory_name},
                            },
                            {}
                          ) &&
                          allowed_directory_entries(
                            auth_fd,
                            {
                              std::string {authority_capability_file_name},
                              std::string {authority_record_file_name},
                              std::string {authority_provider_catalog_file_name},
                            },
                            {}
                          ) &&
                          allowed_directory_entries(
                            ipc_fd,
                            {},
                            {
                              std::string {authority_control_socket_name},
                              std::string {authority_media_socket_name},
                            }
                          );
    if (!complete) {
      OPENSSL_cleanse(capability.data(), capability.size());
      rollback();
      return {.status = authority_status_e::integrity_violation};
    }
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
      record_identity.device,
      record_identity.inode,
      provider_catalog_identity.device,
      provider_catalog_identity.inode,
      *provider_catalog_digest,
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

  authority_recovery_result_t authority_store_t::recover_inactive(
    std::span<const endpoint_identity_t> active_identities
  ) {
    authority_recovery_result_t result;
    const auto fail = [&result](authority_status_e status) {
      result.inactive.clear();
      result.active_identities.clear();
      result.active = 0;
      result.status = status;
      return std::move(result);
    };
    if (status_ != authority_status_e::applied) {
      return fail(status_);
    }
    if (active_identities.size() > max_authority_entries) {
      return fail(authority_status_e::invalid_argument);
    }
    std::vector<endpoint_identity_t> unique_active;
    unique_active.reserve(active_identities.size());
    for (const auto &identity : active_identities) {
      if (!valid_identity(identity) ||
          std::find(unique_active.begin(), unique_active.end(), identity) !=
            unique_active.end()) {
        return fail(authority_status_e::invalid_argument);
      }
      unique_active.push_back(identity);
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
      return fail(authority_status_e::unsafe_root);
    }
    const auto entries = bounded_directory_entries(root_fd_, max_authority_entries);
    if (!entries) {
      return fail(authority_status_e::integrity_violation);
    }
    result.observed = entries->size();
    std::vector<endpoint_identity_t> observed_identities;
    observed_identities.reserve(entries->size());

    for (const auto &runtime_namespace : *entries) {
      const auto paths = paths_for(root_, runtime_namespace);
      if (!socket_path_fits(paths.control_socket) ||
          !socket_path_fits(paths.media_socket)) {
        return fail(authority_status_e::integrity_violation);
      }
      auto generation_fd = ::openat(
        root_fd_,
        runtime_namespace.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      );
      auto ipc_fd = -1;
      auto auth_fd = -1;
      const auto close_all = [&]() {
        close_descriptor(auth_fd);
        close_descriptor(ipc_fd);
        close_descriptor(generation_fd);
      };
      struct stat generation_metadata {};
      if (generation_fd < 0 ||
          ::fstat(generation_fd, &generation_metadata) != 0 ||
          !private_directory(generation_metadata, owner_uid_)) {
        close_all();
        return fail(authority_status_e::integrity_violation);
      }
      ipc_fd = ::openat(
        generation_fd,
        std::string {authority_ipc_directory_name}.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      );
      auth_fd = ::openat(
        generation_fd,
        std::string {authority_auth_directory_name}.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
      );
      struct stat ipc_metadata {};
      struct stat auth_metadata {};
      if (ipc_fd < 0 || auth_fd < 0 ||
          ::fstat(ipc_fd, &ipc_metadata) != 0 ||
          ::fstat(auth_fd, &auth_metadata) != 0 ||
          !private_directory(ipc_metadata, owner_uid_) ||
          !private_directory(auth_metadata, owner_uid_) ||
          !allowed_directory_entries(
            generation_fd,
            {
              std::string {authority_ipc_directory_name},
              std::string {authority_auth_directory_name},
            },
            {}
          ) ||
          !allowed_directory_entries(
            auth_fd,
            {
              std::string {authority_capability_file_name},
              std::string {authority_record_file_name},
              std::string {authority_provider_catalog_file_name},
            },
            {}
          ) ||
          !allowed_directory_entries(
            ipc_fd,
            {},
            {
              std::string {authority_control_socket_name},
              std::string {authority_media_socket_name},
            }
          )) {
        close_all();
        return fail(authority_status_e::integrity_violation);
      }

      auto capability_file = read_capability_file(auth_fd, owner_uid_);
      auto record_file = read_private_file(
        auth_fd,
        authority_record_file_name,
        owner_uid_,
        max_authority_record_size
      );
      auto provider_catalog_file = read_private_file(
        auth_fd,
        authority_provider_catalog_file_name,
        owner_uid_,
        max_provider_catalog_size,
        0400
      );
      const auto provider_catalog_digest = provider_catalog_file &&
                                               !provider_catalog_file->payload.empty() ?
                                             sha256_digest(provider_catalog_file->payload) :
                                             std::nullopt;
      auto record = capability_file && record_file ?
                      parse_authority_record(
                        record_file->payload,
                        capability_file->capability
                      ) :
                      std::nullopt;
      if (!capability_file || !record_file || !provider_catalog_file ||
          !provider_catalog_digest || !record ||
          record->runtime_namespace != runtime_namespace ||
          CRYPTO_memcmp(
            record->provider_catalog_digest.data(),
            provider_catalog_digest->data(),
            provider_catalog_digest->size()
          ) != 0 ||
          std::find(
            observed_identities.begin(),
            observed_identities.end(),
            record->identity
          ) != observed_identities.end()) {
        if (capability_file) {
          OPENSSL_cleanse(
            capability_file->capability.data(),
            capability_file->capability.size()
          );
        }
        close_all();
        return fail(authority_status_e::integrity_violation);
      }
      observed_identities.push_back(record->identity);

      const auto generation_identity = identity_of(generation_metadata);
      const auto ipc_identity = identity_of(ipc_metadata);
      const auto auth_identity = identity_of(auth_metadata);
      struct stat token_entry {};
      struct stat record_entry {};
      struct stat provider_catalog_entry {};
      const auto stable = entry_is_directory(
                            root_fd_,
                            runtime_namespace,
                            owner_uid_,
                            generation_identity.device,
                            generation_identity.inode
                          ) &&
                          entry_is_directory(
                            generation_fd,
                            authority_ipc_directory_name,
                            owner_uid_,
                            ipc_identity.device,
                            ipc_identity.inode
                          ) &&
                          entry_is_directory(
                            generation_fd,
                            authority_auth_directory_name,
                            owner_uid_,
                            auth_identity.device,
                            auth_identity.inode
                          ) &&
                          ::fstatat(
                            auth_fd,
                            std::string {authority_capability_file_name}.c_str(),
                            &token_entry,
                            AT_SYMLINK_NOFOLLOW
                          ) == 0 &&
                          private_regular_file(token_entry, owner_uid_) &&
                          identity_of(token_entry) == capability_file->identity &&
                          ::fstatat(
                            auth_fd,
                            std::string {authority_record_file_name}.c_str(),
                            &record_entry,
                            AT_SYMLINK_NOFOLLOW
                          ) == 0 &&
                          private_regular_file(record_entry, owner_uid_) &&
                          identity_of(record_entry) == record_file->identity &&
                          ::fstatat(
                            auth_fd,
                            std::string {authority_provider_catalog_file_name}.c_str(),
                            &provider_catalog_entry,
                            AT_SYMLINK_NOFOLLOW
                          ) == 0 &&
                          immutable_private_regular_file(
                            provider_catalog_entry,
                            owner_uid_
                          ) &&
                          identity_of(provider_catalog_entry) ==
                            provider_catalog_file->identity;
      if (!stable) {
        OPENSSL_cleanse(
          capability_file->capability.data(),
          capability_file->capability.size()
        );
        close_all();
        return fail(authority_status_e::integrity_violation);
      }
      for (const auto socket_name : {
             authority_control_socket_name,
             authority_media_socket_name,
           }) {
        struct stat socket_metadata {};
        const std::string owned_name {socket_name};
        if (::fstatat(
              ipc_fd,
              owned_name.c_str(),
              &socket_metadata,
              AT_SYMLINK_NOFOLLOW
            ) == 0) {
          if (!private_socket(socket_metadata, owner_uid_)) {
            OPENSSL_cleanse(
              capability_file->capability.data(),
              capability_file->capability.size()
            );
            close_all();
            return fail(authority_status_e::integrity_violation);
          }
        } else if (errno != ENOENT) {
          OPENSSL_cleanse(
            capability_file->capability.data(),
            capability_file->capability.size()
          );
          close_all();
          return fail(authority_status_e::integrity_violation);
        }
      }

      if (std::find(
            unique_active.begin(),
            unique_active.end(),
            record->identity
          ) != unique_active.end()) {
        ++result.active;
        result.active_identities.push_back(record->identity);
        OPENSSL_cleanse(
          capability_file->capability.data(),
          capability_file->capability.size()
        );
        close_all();
        continue;
      }

      result.inactive.emplace_back(authority_handle_t {
        record->identity,
        paths,
        capability_file->capability,
        owner_uid_,
        root_device_,
        root_inode_,
        generation_identity.device,
        generation_identity.inode,
        ipc_identity.device,
        ipc_identity.inode,
        auth_identity.device,
        auth_identity.inode,
        capability_file->identity.device,
        capability_file->identity.inode,
        record_file->identity.device,
        record_file->identity.inode,
        provider_catalog_file->identity.device,
        provider_catalog_file->identity.inode,
        *provider_catalog_digest,
        std::exchange(generation_fd, -1),
        std::exchange(ipc_fd, -1),
        std::exchange(auth_fd, -1)
      });
      OPENSSL_cleanse(
        capability_file->capability.data(),
        capability_file->capability.size()
      );
    }

    current_root = open_absolute_directory_without_symlinks(root_);
    const auto final_entries = bounded_directory_entries(root_fd_, max_authority_entries);
    const auto stable_root = descriptor_is_directory(
      current_root,
      owner_uid_,
      root_device_,
      root_inode_
    ) && descriptor_is_directory(
      root_fd_,
      owner_uid_,
      root_device_,
      root_inode_
    );
    close_descriptor(current_root);
    if (!stable_root || !final_entries || *final_entries != *entries) {
      return fail(authority_status_e::integrity_violation);
    }
    result.status = authority_status_e::applied;
    return result;
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
    struct stat record_metadata {};
    struct stat provider_catalog_metadata {};
    const auto capability_name = std::string {authority_capability_file_name};
    const auto record_name = std::string {authority_record_file_name};
    const auto provider_catalog_name =
      std::string {authority_provider_catalog_file_name};
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
        ::fstatat(
          authority.auth_fd_,
          record_name.c_str(),
          &record_metadata,
          AT_SYMLINK_NOFOLLOW
        ) != 0 ||
        !private_regular_file(record_metadata, owner_uid_) ||
        !exact_identity(
          record_metadata,
          authority.record_device_,
          authority.record_inode_
        ) ||
        ::fstatat(
          authority.auth_fd_,
          provider_catalog_name.c_str(),
          &provider_catalog_metadata,
          AT_SYMLINK_NOFOLLOW
        ) != 0 ||
        !immutable_private_regular_file(provider_catalog_metadata, owner_uid_) ||
        !exact_identity(
          provider_catalog_metadata,
          authority.provider_catalog_device_,
          authority.provider_catalog_inode_
        ) ||
        !capability_file_matches(
          authority.auth_fd_,
          owner_uid_,
          authority.capability_device_,
          authority.capability_inode_,
          authority.capability_
        ) ||
        !authority_record_matches(
          authority.auth_fd_,
          owner_uid_,
          authority.record_device_,
          authority.record_inode_,
          authority.identity_,
          generation_name,
          authority.provider_catalog_digest_,
          authority.capability_
        ) ||
        !provider_catalog_file_matches(
          authority.auth_fd_,
          owner_uid_,
          authority.provider_catalog_device_,
          authority.provider_catalog_inode_,
          authority.provider_catalog_digest_
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
          {
            std::string {authority_capability_file_name},
            std::string {authority_record_file_name},
            std::string {authority_provider_catalog_file_name},
          },
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
    const auto record_name = std::string {authority_record_file_name};
    const auto provider_catalog_name =
      std::string {authority_provider_catalog_file_name};
    struct stat capability_metadata {};
    struct stat record_metadata {};
    struct stat provider_catalog_metadata {};
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
        ::fstatat(
          authority.auth_fd_,
          record_name.c_str(),
          &record_metadata,
          AT_SYMLINK_NOFOLLOW
        ) != 0 ||
        !private_regular_file(record_metadata, owner_uid_) ||
        !exact_identity(
          record_metadata,
          authority.record_device_,
          authority.record_inode_
        ) ||
        ::fstatat(
          authority.auth_fd_,
          provider_catalog_name.c_str(),
          &provider_catalog_metadata,
          AT_SYMLINK_NOFOLLOW
        ) != 0 ||
        !immutable_private_regular_file(provider_catalog_metadata, owner_uid_) ||
        !exact_identity(
          provider_catalog_metadata,
          authority.provider_catalog_device_,
          authority.provider_catalog_inode_
        ) ||
        !capability_file_matches(
          authority.auth_fd_,
          owner_uid_,
          authority.capability_device_,
          authority.capability_inode_,
          authority.capability_
        ) ||
        !authority_record_matches(
          authority.auth_fd_,
          owner_uid_,
          authority.record_device_,
          authority.record_inode_,
          authority.identity_,
          authority.paths_.generation.filename().native(),
          authority.provider_catalog_digest_,
          authority.capability_
        ) ||
        !provider_catalog_file_matches(
          authority.auth_fd_,
          owner_uid_,
          authority.provider_catalog_device_,
          authority.provider_catalog_inode_,
          authority.provider_catalog_digest_
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
        )) {
      return authority_status_e::integrity_violation;
    }
    if (::unlinkat(authority.auth_fd_, record_name.c_str(), 0) != 0 ||
        ::unlinkat(authority.auth_fd_, provider_catalog_name.c_str(), 0) != 0 ||
        ::unlinkat(authority.auth_fd_, capability_name.c_str(), 0) != 0 ||
        ::fsync(authority.auth_fd_) != 0) {
      return authority_status_e::io_error;
    }
    if (::unlinkat(
          authority.generation_fd_,
          std::string {authority_auth_directory_name}.c_str(),
          AT_REMOVEDIR
        ) != 0 ||
        ::unlinkat(
          authority.generation_fd_,
          std::string {authority_ipc_directory_name}.c_str(),
          AT_REMOVEDIR
        ) != 0 ||
        ::fsync(authority.generation_fd_) != 0) {
      return authority_status_e::io_error;
    }
    if (::unlinkat(
          root_fd_,
          authority.paths_.generation.filename().c_str(),
          AT_REMOVEDIR
        ) != 0 || ::fsync(root_fd_) != 0) {
      return authority_status_e::io_error;
    }

    authority.release_resources();
    return authority_status_e::applied;
  }

}  // namespace multiseat::worker_ipc

#endif
