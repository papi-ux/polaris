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
#include <random>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
  #include <aclapi.h>
  #include <sddl.h>
#else
  #include <fcntl.h>
  #include <sys/file.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

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

#if defined(_WIN32)
    class private_file_security_t {
    public:
      private_file_security_t() {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
              L"D:P(A;;FA;;;SY)(A;;FA;;;OW)",
              SDDL_REVISION_1,
              &descriptor_,
              nullptr
            )) {
          return;
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = descriptor_;
        attributes_.bInheritHandle = FALSE;
      }

      ~private_file_security_t() {
        if (descriptor_) {
          LocalFree(descriptor_);
        }
      }

      private_file_security_t(const private_file_security_t &) = delete;
      private_file_security_t &operator=(const private_file_security_t &) = delete;

      explicit operator bool() const {
        return descriptor_ != nullptr;
      }

      SECURITY_ATTRIBUTES *attributes() {
        return descriptor_ ? &attributes_ : nullptr;
      }

      bool apply_to_handle(HANDLE handle) const {
        PACL dacl = nullptr;
        BOOL dacl_present = FALSE;
        BOOL dacl_defaulted = FALSE;
        if (!descriptor_ ||
            !GetSecurityDescriptorDacl(descriptor_, &dacl_present, &dacl, &dacl_defaulted) ||
            !dacl_present || !dacl) {
          return false;
        }
        const auto result = SetSecurityInfo(
          handle,
          SE_FILE_OBJECT,
          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
          nullptr,
          nullptr,
          dacl,
          nullptr
        );
        if (result != ERROR_SUCCESS) {
          SetLastError(result);
          return false;
        }
        return true;
      }

      bool restrict_existing(const std::filesystem::path &path) const {
        auto handle = CreateFileW(
          path.c_str(),
          READ_CONTROL | WRITE_DAC,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
          nullptr
        );
        if (handle == INVALID_HANDLE_VALUE) {
          const auto error = GetLastError();
          return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        const auto secured = apply_to_handle(handle);
        const auto security_error = secured ? ERROR_SUCCESS : GetLastError();
        const auto closed = CloseHandle(handle);
        if (!secured) {
          const std::error_code ec(static_cast<int>(security_error), std::system_category());
          BOOST_LOG(error) << "Couldn't restrict existing private state file " << path << ": " << ec.message();
          return false;
        }
        if (!closed) {
          const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
          BOOST_LOG(error) << "Couldn't close restricted private state file " << path << ": " << ec.message();
          return false;
        }
        return true;
      }

    private:
      PSECURITY_DESCRIPTOR descriptor_ = nullptr;
      SECURITY_ATTRIBUTES attributes_ {};
    };
#endif

    class state_file_lock_t {
    public:
      explicit state_file_lock_t(const std::filesystem::path &target):
          path_ {lock_path_for(target)} {
#if defined(_WIN32)
        private_file_security_t security;
        if (!security) {
          return;
        }
        handle_ = CreateFileW(
          path_.c_str(),
          GENERIC_READ | GENERIC_WRITE | WRITE_DAC,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          security.attributes(),
          OPEN_ALWAYS,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
          nullptr
        );
        if (handle_ == INVALID_HANDLE_VALUE) {
          return;
        }
        FILE_ATTRIBUTE_TAG_INFO attributes {};
        if (!GetFileInformationByHandleEx(handle_, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            !security.apply_to_handle(handle_)) {
          CloseHandle(handle_);
          handle_ = INVALID_HANDLE_VALUE;
          return;
        }
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped_)) {
          CloseHandle(handle_);
          handle_ = INVALID_HANDLE_VALUE;
          return;
        }
#else
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
#endif
        locked_ = true;
      }

      state_file_lock_t(const state_file_lock_t &) = delete;
      state_file_lock_t &operator=(const state_file_lock_t &) = delete;

      ~state_file_lock_t() {
        if (!locked_) {
          return;
        }
#if defined(_WIN32)
        UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
        CloseHandle(handle_);
#else
        int result;
        do {
          result = ::flock(descriptor_, LOCK_UN);
        } while (result != 0 && errno == EINTR);
        ::close(descriptor_);
#endif
      }

      explicit operator bool() const {
        return locked_;
      }

    private:
      std::filesystem::path path_;
      bool locked_ = false;
#if defined(_WIN32)
      HANDLE handle_ = INVALID_HANDLE_VALUE;
      OVERLAPPED overlapped_ {};
#else
      int descriptor_ = -1;
#endif
    };

#if defined(_WIN32)
    std::wstring random_suffix() {
      constexpr wchar_t alphabet[] = L"0123456789abcdef";
      std::random_device random;
      std::wstring result(32, L'0');
      for (auto &character : result) {
        character = alphabet[random() & 0x0F];
      }
      return result;
    }
#endif
  }  // namespace

  read_result_t read_secure(const std::filesystem::path &target, std::size_t max_bytes) {
    state_file_lock_t lock {target};
    if (!lock) {
      BOOST_LOG(error) << "Couldn't acquire private state lock for " << target;
      return {.status = read_status_e::io_error};
    }

#if defined(_WIN32)
    private_file_security_t security;
    if (!security) {
      return {.status = read_status_e::io_error};
    }
    auto handle = CreateFileW(
      target.c_str(),
      GENERIC_READ | READ_CONTROL | WRITE_DAC,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return {.status = read_status_e::missing};
      }
      return {.status = read_status_e::io_error};
    }
    const auto close_handle = [&]() {
      if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
      }
    };

    FILE_ATTRIBUTE_TAG_INFO attributes {};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
      close_handle();
      return {.status = read_status_e::rejected};
    }
    if (!security.apply_to_handle(handle)) {
      close_handle();
      return {.status = read_status_e::io_error};
    }

    LARGE_INTEGER size {};
    if (!GetFileSizeEx(handle, &size)) {
      close_handle();
      return {.status = read_status_e::io_error};
    }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > max_bytes) {
      close_handle();
      return {.status = read_status_e::rejected};
    }

    std::string payload(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < payload.size()) {
      const auto remaining = payload.size() - offset;
      const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD));
      DWORD read = 0;
      if (!ReadFile(handle, payload.data() + offset, chunk, &read, nullptr) || read == 0) {
        close_handle();
        return {.status = read_status_e::io_error};
      }
      offset += read;
    }
    close_handle();
    return {.status = read_status_e::ok, .payload = std::move(payload)};
#else
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
#endif
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

#if defined(_WIN32)
    private_file_security_t security;
    if (!security || !security.restrict_existing(target)) {
      return false;
    }

    std::filesystem::path temporary;
    HANDLE descriptor = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32; ++attempt) {
      temporary = target;
      temporary += L".tmp." + random_suffix();
      descriptor = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE | WRITE_DAC,
        0,
        security.attributes(),
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr
      );
      if (descriptor != INVALID_HANDLE_VALUE) {
        break;
      }
      const auto error = GetLastError();
      if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
        return false;
      }
    }
    if (descriptor == INVALID_HANDLE_VALUE || !security.apply_to_handle(descriptor)) {
      if (descriptor != INVALID_HANDLE_VALUE) {
        CloseHandle(descriptor);
        DeleteFileW(temporary.c_str());
      }
      return false;
    }
    const auto cleanup = [&]() {
      if (descriptor != INVALID_HANDLE_VALUE) {
        CloseHandle(descriptor);
        descriptor = INVALID_HANDLE_VALUE;
      }
      DeleteFileW(temporary.c_str());
    };
    const auto write_bytes = [&](std::size_t count) {
      std::size_t offset = 0;
      while (offset < count) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(count - offset, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(descriptor, payload.data() + offset, chunk, &written, nullptr) || written == 0) {
          return false;
        }
        offset += written;
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
    const auto flushed = injected_fault != write_fault_e::sync && FlushFileBuffers(descriptor) != 0;
#else
    const auto flushed = FlushFileBuffers(descriptor) != 0;
#endif
    const auto closed = CloseHandle(descriptor) != 0;
    descriptor = INVALID_HANDLE_VALUE;
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
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      cleanup();
      return false;
    }
#ifdef POLARIS_TESTS
    if (injected_fault == write_fault_e::post_rename_durability) {
      return true;
    }
#endif
    return true;
#else
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
#endif
  }

#ifdef POLARIS_TESTS
  void set_write_fault_for_tests(write_fault_e fault) {
    write_fault.store(fault, std::memory_order_relaxed);
  }
#endif
}  // namespace private_state_file
