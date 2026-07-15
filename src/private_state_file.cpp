/**
 * @file src/private_state_file.cpp
 * @brief Secure bounded persistence for authorization-adjacent private state.
 */
#include "private_state_file.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
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
#endif

    std::filesystem::path lock_path_for(const std::filesystem::path &target) {
      auto result = target;
      result += ".lock";
      return result;
    }


    class state_file_lock_t {
    public:
      explicit state_file_lock_t(const std::filesystem::path &target):
          path_ {lock_path_for(target)} {
        int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        descriptor_ = ::open(path_.c_str(), flags, S_IRUSR | S_IWUSR);
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
      std::filesystem::path path_;
      bool locked_ = false;
      int descriptor_ = -1;
    };

  }  // namespace

  read_result_t read_secure(const std::filesystem::path &target, std::size_t max_bytes) {
    state_file_lock_t lock {target};
    if (!lock) {
      BOOST_LOG(error) << "Couldn't acquire private state lock for " << target;
      return {.status = read_status_e::io_error};
    }

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = ::open(target.c_str(), flags);
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
      if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
      }
    };

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) {
      close_descriptor();
      return {.status = read_status_e::io_error};
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid() || metadata.st_nlink != 1 ||
        (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0 || metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) > max_bytes) {
      close_descriptor();
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
        close_descriptor();
        return {.status = read_status_e::io_error};
      }
      if (read_count == 0) {
        close_descriptor();
        return {.status = read_status_e::io_error};
      }
      offset += static_cast<std::size_t>(read_count);
    }
    char extra = 0;
    ssize_t extra_count;
    do {
      extra_count = ::read(descriptor, &extra, 1);
    } while (extra_count < 0 && errno == EINTR);
    close_descriptor();
    if (extra_count < 0) {
      return {.status = read_status_e::io_error};
    }
    if (extra_count > 0) {
      return {.status = read_status_e::rejected};
    }
    return {.status = read_status_e::ok, .payload = std::move(payload)};
  }

  bool write_atomic(const std::filesystem::path &target, std::string_view payload) {
    state_file_lock_t lock {target};
    if (!lock) {
      BOOST_LOG(error) << "Couldn't acquire private state lock for " << target;
      return false;
    }

#ifdef POLARIS_TESTS
    const auto injected_fault = write_fault.load(std::memory_order_relaxed);
    if (injected_fault == write_fault_e::open) {
      return false;
    }
#endif

    auto temporary_template = target.string() + ".tmp.XXXXXX";
    std::vector<char> temporary_buffer(temporary_template.begin(), temporary_template.end());
    temporary_buffer.push_back('\0');
#if defined(__linux__) && defined(O_CLOEXEC)
    int descriptor = ::mkostemp(temporary_buffer.data(), O_CLOEXEC);
#else
    int descriptor = ::mkstemp(temporary_buffer.data());
#endif
    if (descriptor < 0) {
      return false;
    }
    const std::filesystem::path temporary {temporary_buffer.data()};
    const auto cleanup = [&]() {
      if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
      }
      ::unlink(temporary.c_str());
    };
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
      cleanup();
      return false;
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
      return false;
    }
#endif
    if (!write_bytes(payload.size())) {
      cleanup();
      return false;
    }
#ifdef POLARIS_TESTS
    if (injected_fault == write_fault_e::flush) {
      cleanup();
      return false;
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
      return false;
    }
#ifdef POLARIS_TESTS
    if (injected_fault == write_fault_e::rename) {
      cleanup();
      return false;
    }
#endif
    std::error_code rename_error;
    std::filesystem::rename(temporary, target, rename_error);
    if (rename_error) {
      cleanup();
      return false;
    }
#ifdef POLARIS_TESTS
    if (injected_fault == write_fault_e::post_rename_durability) {
      return true;
    }
#endif

    auto directory = target.parent_path();
    if (directory.empty()) {
      directory = ".";
    }
    int directory_flags = O_RDONLY;
#ifdef O_DIRECTORY
    directory_flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    directory_flags |= O_CLOEXEC;
#endif
    const int directory_descriptor = ::open(directory.c_str(), directory_flags);
    if (directory_descriptor < 0) {
      BOOST_LOG(warning) << "Private state replaced, but directory durability open failed for " << directory;
      return true;
    }
    const int flush_result = ::fsync(directory_descriptor);
    const int close_result = ::close(directory_descriptor);
    if (flush_result != 0 || close_result != 0) {
      BOOST_LOG(warning) << "Private state replaced, but directory durability flush failed for " << directory;
    }
    return true;
  }

#ifdef POLARIS_TESTS
  void set_write_fault_for_tests(write_fault_e fault) {
    write_fault.store(fault, std::memory_order_relaxed);
  }
#endif
}  // namespace private_state_file
