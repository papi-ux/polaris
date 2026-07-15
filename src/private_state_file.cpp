/**
 * @file src/private_state_file.cpp
 * @brief Secure bounded persistence for authorization-adjacent private state.
 */
#include "private_state_file.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
  #error "Polaris is Linux-only; private_state_file has no Windows implementation"
#endif
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "logging.h"

namespace private_state_file {
  namespace {
#ifdef POLARIS_TESTS
    std::atomic<write_fault_e> write_fault {write_fault_e::none};
    std::atomic_size_t parent_component_fault_index {0};
    std::atomic_bool force_parent_eexist_race {false};
#endif

    int directory_open_flags() {
      int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_DIRECTORY
      flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
      flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
      flags |= O_NOFOLLOW;
#endif
      return flags;
    }

    bool secure_directory_descriptor(int descriptor, bool final_parent) {
      struct stat metadata {};
      if (::fstat(descriptor, &metadata) != 0 || !S_ISDIR(metadata.st_mode)) {
        return false;
      }

      const auto effective_user = ::geteuid();
      const auto writable_by_others = (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0;
      if (final_parent) {
        return metadata.st_uid == effective_user && !writable_by_others;
      }

      const auto trusted_owner = metadata.st_uid == 0 || metadata.st_uid == effective_user;
      const auto sticky_when_writable = !writable_by_others || (metadata.st_mode & S_ISVTX) != 0;
      return trusted_owner && sticky_when_writable;
    }

    class directory_handle_t {
    public:
      explicit directory_handle_t(const std::filesystem::path &target, bool create_missing = false):
          path_ {target.parent_path().empty() ? std::filesystem::path {"."} : target.parent_path()},
          target_name_ {target.filename().string()},
          lock_name_ {target_name_ + ".lock"} {
        if (target_name_.empty() || target_name_ == "." || target_name_ == "..") {
          return;
        }

        std::vector<std::string> components;
        for (const auto &component : path_.relative_path()) {
          const auto name = component.string();
          if (name.empty() || name == ".") {
            continue;
          }
          if (name == "..") {
            return;
          }
          components.push_back(name);
        }

        descriptor_ = ::open(path_.is_absolute() ? "/" : ".", directory_open_flags());
        if (descriptor_ < 0 || !secure_directory_descriptor(descriptor_, components.empty())) {
          (void) close();
          return;
        }

        std::size_t created_parent_index = 0;
        for (std::size_t index = 0; index < components.size(); ++index) {
          const auto &component = components[index];
          const bool final_parent = index + 1 == components.size();
#ifdef POLARIS_TESTS
          int next = -1;
          if (create_missing && final_parent && force_parent_eexist_race.load(std::memory_order_relaxed)) {
            errno = ENOENT;
          } else {
            next = ::openat(descriptor_, component.c_str(), directory_open_flags());
          }
#else
          int next = ::openat(descriptor_, component.c_str(), directory_open_flags());
#endif
          bool parent_entry_needs_sync = false;
          if (next < 0 && errno == ENOENT && create_missing) {
            if (::mkdirat(descriptor_, component.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
              (void) close();
              return;
            }
            parent_entry_needs_sync = true;
            next = ::openat(descriptor_, component.c_str(), directory_open_flags());
          }
          if (next < 0 || !secure_directory_descriptor(next, final_parent)) {
            if (next >= 0) {
              ::close(next);
            }
            (void) close();
            return;
          }
#ifdef POLARIS_TESTS
          const auto injected_fault = write_fault.load(std::memory_order_relaxed);
          const bool fault_this_parent = parent_entry_needs_sync &&
                                         created_parent_index == parent_component_fault_index.load(std::memory_order_relaxed);
#endif
          if (parent_entry_needs_sync) {
#ifdef POLARIS_TESTS
            const bool parent_synchronized =
              !(fault_this_parent && injected_fault == write_fault_e::parent_sync) &&
              ::fsync(descriptor_) == 0;
#else
            const bool parent_synchronized = ::fsync(descriptor_) == 0;
#endif
            if (!parent_synchronized) {
              ::close(next);
              (void) close();
              return;
            }
          }
          const bool parent_closed = ::close(descriptor_) == 0;
#ifdef POLARIS_TESTS
          if (fault_this_parent && injected_fault == write_fault_e::parent_close) {
            descriptor_ = -1;
            ::close(next);
            return;
          }
#endif
          if (!parent_closed) {
            descriptor_ = -1;
            ::close(next);
            return;
          }
          descriptor_ = next;
          if (parent_entry_needs_sync) {
            ++created_parent_index;
          }
        }
      }

      directory_handle_t(const directory_handle_t &) = delete;
      directory_handle_t &operator=(const directory_handle_t &) = delete;

      ~directory_handle_t() {
        if (descriptor_ >= 0) {
          ::close(descriptor_);
        }
      }

      explicit operator bool() const {
        return descriptor_ >= 0;
      }

      int descriptor() const {
        return descriptor_;
      }

      const std::string &target_name() const {
        return target_name_;
      }

      const std::string &lock_name() const {
        return lock_name_;
      }

      const std::filesystem::path &path() const {
        return path_;
      }

      bool sync() const {
        return descriptor_ >= 0 && ::fsync(descriptor_) == 0;
      }

      bool close() {
        if (descriptor_ < 0) {
          return true;
        }
        const int descriptor = std::exchange(descriptor_, -1);
        return ::close(descriptor) == 0;
      }

    private:
      std::filesystem::path path_;
      std::string target_name_;
      std::string lock_name_;
      int descriptor_ = -1;
    };

    class state_file_lock_t {
    public:
      explicit state_file_lock_t(const directory_handle_t &directory) {
        int flags = O_CREAT | O_RDWR | O_NONBLOCK;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        descriptor_ = ::openat(
          directory.descriptor(),
          directory.lock_name().c_str(),
          flags,
          S_IRUSR | S_IWUSR
        );
        if (descriptor_ < 0) {
          return;
        }
        struct stat metadata {};
        if (::fstat(descriptor_, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            metadata.st_uid != ::geteuid() || metadata.st_nlink != 1 ||
            ::fchmod(descriptor_, S_IRUSR | S_IWUSR) != 0) {
          ::close(descriptor_);
          descriptor_ = -1;
          return;
        }
        while (::flock(descriptor_, LOCK_EX) != 0) {
          if (errno == EINTR) {
            continue;
          }
          ::close(descriptor_);
          descriptor_ = -1;
          return;
        }
        locked_ = true;
      }

      state_file_lock_t(const state_file_lock_t &) = delete;
      state_file_lock_t &operator=(const state_file_lock_t &) = delete;

      ~state_file_lock_t() {
        if (!locked_) {
          return;
        }
        int result;
        do {
          result = ::flock(descriptor_, LOCK_UN);
        } while (result != 0 && errno == EINTR);
        ::close(descriptor_);
      }

      explicit operator bool() const {
        return locked_;
      }

    private:
      bool locked_ = false;
      int descriptor_ = -1;
    };

    std::string make_temporary_name(std::string_view target_name) {
      constexpr char hex[] = "0123456789abcdef";
      std::array<unsigned char, 16> random_bytes {};
      std::random_device source;
      for (auto &value : random_bytes) {
        value = static_cast<unsigned char>(source());
      }

      std::string result {target_name};
      result += ".tmp.";
      result.reserve(result.size() + random_bytes.size() * 2);
      for (const auto value : random_bytes) {
        result.push_back(hex[value >> 4U]);
        result.push_back(hex[value & 0x0FU]);
      }
      return result;
    }

    int create_temporary(const directory_handle_t &directory, std::string &temporary_name) {
      int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_CLOEXEC
      flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
      flags |= O_NOFOLLOW;
#endif
      for (int attempt = 0; attempt < 32; ++attempt) {
        temporary_name = make_temporary_name(directory.target_name());
        const int descriptor = ::openat(
          directory.descriptor(),
          temporary_name.c_str(),
          flags,
          S_IRUSR | S_IWUSR
        );
        if (descriptor >= 0) {
          return descriptor;
        }
        if (errno != EEXIST) {
          return -1;
        }
      }
      errno = EEXIST;
      return -1;
    }
  }  // namespace

  read_result_t read_secure(const std::filesystem::path &target, std::size_t max_bytes) {
    directory_handle_t directory {target};
    if (!directory) {
      BOOST_LOG(error) << "Rejected insecure or inaccessible private state directory for " << target;
      return {.status = read_status_e::io_error};
    }
    state_file_lock_t lock {directory};
    if (!lock) {
      BOOST_LOG(error) << "Couldn't acquire private state lock for " << target;
      return {.status = read_status_e::io_error};
    }

    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = ::openat(directory.descriptor(), directory.target_name().c_str(), flags);
    if (descriptor < 0) {
      if (errno == ENOENT) {
        return {.status = read_status_e::missing};
      }
#ifdef ELOOP
      if (errno == ELOOP) {
        return {.status = read_status_e::rejected};
      }
#endif
      return {.status = read_status_e::io_error};
    }
    const auto close_descriptor = [&]() {
      if (descriptor < 0) {
        return true;
      }
      const int closing = std::exchange(descriptor, -1);
      return ::close(closing) == 0;
    };

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) {
      (void) close_descriptor();
      return {.status = read_status_e::io_error};
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid() || metadata.st_nlink != 1 ||
        (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0 || metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) > max_bytes) {
      (void) close_descriptor();
      return {.status = read_status_e::rejected};
    }

    std::string payload(static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t offset = 0;
    while (offset < payload.size()) {
      const auto read_count = ::read(descriptor, payload.data() + offset, payload.size() - offset);
      if (read_count < 0) {
        if (errno == EINTR) {
          continue;
        }
        (void) close_descriptor();
        return {.status = read_status_e::io_error};
      }
      if (read_count == 0) {
        (void) close_descriptor();
        return {.status = read_status_e::io_error};
      }
      offset += static_cast<std::size_t>(read_count);
    }
    char extra = 0;
    ssize_t extra_count;
    do {
      extra_count = ::read(descriptor, &extra, 1);
    } while (extra_count < 0 && errno == EINTR);
    const bool closed = close_descriptor();
    if (extra_count < 0 || !closed) {
      return {.status = read_status_e::io_error};
    }
    if (extra_count > 0) {
      return {.status = read_status_e::rejected};
    }
    return {.status = read_status_e::ok, .payload = std::move(payload)};
  }

  write_result_t write_atomic(const std::filesystem::path &target, std::string_view payload) {
    directory_handle_t directory {target, true};
    if (!directory) {
      BOOST_LOG(error) << "Rejected insecure or inaccessible private state directory for " << target;
      return {write_status_e::not_committed};
    }
    state_file_lock_t lock {directory};
    if (!lock) {
      BOOST_LOG(error) << "Couldn't acquire private state lock for " << target;
      return {write_status_e::not_committed};
    }

#ifdef POLARIS_TESTS
    const auto injected_fault = write_fault.load(std::memory_order_relaxed);
    if (injected_fault == write_fault_e::open) {
      return {write_status_e::not_committed};
    }
#endif

    std::string temporary_name;
    int descriptor;
    try {
      descriptor = create_temporary(directory, temporary_name);
    } catch (const std::exception &exception) {
      BOOST_LOG(error) << "Couldn't create private state temporary file: " << exception.what();
      return {write_status_e::not_committed};
    }
    if (descriptor < 0) {
      return {write_status_e::not_committed};
    }
    bool temporary_exists = true;
    const auto cleanup = [&]() {
      if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
      }
      if (temporary_exists) {
        (void) ::unlinkat(directory.descriptor(), temporary_name.c_str(), 0);
        temporary_exists = false;
      }
    };
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
      cleanup();
      return {write_status_e::not_committed};
    }
    const auto write_bytes = [&](std::size_t count) {
      std::size_t offset = 0;
      while (offset < count) {
        const auto written = ::write(descriptor, payload.data() + offset, count - offset);
        if (written < 0) {
          if (errno == EINTR) {
            continue;
          }
          return false;
        }
        if (written == 0) {
          return false;
        }
        offset += static_cast<std::size_t>(written);
      }
      return true;
    };
#ifdef POLARIS_TESTS
    if (injected_fault == write_fault_e::short_write) {
      (void) write_bytes(payload.size() / 2);
      cleanup();
      return {write_status_e::not_committed};
    }
#endif
    if (!write_bytes(payload.size())) {
      cleanup();
      return {write_status_e::not_committed};
    }
#ifdef POLARIS_TESTS
    if (injected_fault == write_fault_e::flush) {
      cleanup();
      return {write_status_e::not_committed};
    }
#endif
#ifdef POLARIS_TESTS
    const auto flushed = injected_fault != write_fault_e::sync && ::fsync(descriptor) == 0;
#else
    const auto flushed = ::fsync(descriptor) == 0;
#endif
    const auto closed = ::close(descriptor) == 0;
    descriptor = -1;
    if (!flushed || !closed) {
      cleanup();
      return {write_status_e::not_committed};
    }
#ifdef POLARIS_TESTS
    if (injected_fault == write_fault_e::rename) {
      cleanup();
      return {write_status_e::not_committed};
    }
#endif
    if (::renameat(
          directory.descriptor(),
          temporary_name.c_str(),
          directory.descriptor(),
          directory.target_name().c_str()
        ) != 0) {
      cleanup();
      return {write_status_e::not_committed};
    }
    temporary_exists = false;
#ifdef POLARIS_TESTS
    const bool directory_synchronized =
      injected_fault != write_fault_e::post_rename_durability && directory.sync();
#else
    const bool directory_synchronized = directory.sync();
#endif
    const bool directory_closed = directory.close();
#ifdef POLARIS_TESTS
    const bool directory_close_succeeded =
      injected_fault != write_fault_e::directory_close && directory_closed;
#else
    const bool directory_close_succeeded = directory_closed;
#endif
    if (!directory_synchronized) {
      BOOST_LOG(error) << "Private state replaced, but directory durability failed for " << directory.path();
      return {write_status_e::durability_uncertain};
    }
    if (!directory_close_succeeded) {
      BOOST_LOG(error) << "Private state replaced, but directory close failed for " << directory.path();
      return {write_status_e::durability_uncertain};
    }
    return {write_status_e::committed};
  }

#ifdef POLARIS_TESTS
  void set_write_fault_for_tests(write_fault_e fault) {
    write_fault.store(fault, std::memory_order_relaxed);
  }

  void set_parent_component_fault_index_for_tests(std::size_t index) {
    parent_component_fault_index.store(index, std::memory_order_relaxed);
  }

  void set_parent_eexist_race_for_tests(bool enabled) {
    force_parent_eexist_race.store(enabled, std::memory_order_relaxed);
  }
#endif
}  // namespace private_state_file
