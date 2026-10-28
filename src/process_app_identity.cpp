/**
 * @file src/process_app_identity.cpp
 * @brief App image validation and stable app identity helpers.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#ifndef BOOST_PROCESS_VERSION
 #define BOOST_PROCESS_VERSION 1
#endif

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "crypto.h"
#include "logging.h"
#include "process.h"

#ifdef _WIN32
  #include <Windows.h>
#elif __linux__
  #include <unistd.h>
#elif __APPLE__
  #include <mach-o/dyld.h>
#endif

#define DEFAULT_APP_IMAGE_PATH POLARIS_ASSETS_DIR "/box.png"

namespace proc {
  using namespace std::literals;

  namespace {
    std::optional<std::filesystem::path> executable_dir() {
#ifdef _WIN32
      wchar_t path[MAX_PATH];
      const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
      if (len == 0 || len == MAX_PATH) {
        return std::nullopt;
      }
      return std::filesystem::path(path).parent_path();
#elif __linux__
      std::array<char, 4096> path {};
      const auto len = readlink("/proc/self/exe", path.data(), path.size() - 1);
      if (len <= 0) {
        return std::nullopt;
      }
      path[len] = '\0';
      return std::filesystem::path(path.data()).parent_path();
#elif __APPLE__
      uint32_t size = 0;
      _NSGetExecutablePath(nullptr, &size);
      std::string buffer(size, '\0');
      if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
      }
      return std::filesystem::path(buffer.c_str()).parent_path();
#else
      return std::nullopt;
#endif
    }

    std::string resolve_bundled_asset(const std::string &asset_name) {
      const auto installed_path = std::filesystem::path(POLARIS_ASSETS_DIR) / asset_name;
      if (std::filesystem::exists(installed_path)) {
        return installed_path.string();
      }

      const auto exe_dir = executable_dir();
      if (exe_dir) {
        const auto local_build_path = *exe_dir / "assets" / asset_name;
        if (std::filesystem::exists(local_build_path)) {
          return local_build_path.string();
        }
      }

      return installed_path.string();
    }

    bool has_supported_image_signature(const std::filesystem::path &path, const std::string &extension) {
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        return false;
      }

      std::array<unsigned char, 16> header {};
      input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
      const auto bytes_read = static_cast<size_t>(input.gcount());

      if (extension == ".png") {
        static constexpr std::array<unsigned char, 8> png_signature {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        return bytes_read >= png_signature.size() &&
               std::equal(png_signature.begin(), png_signature.end(), header.begin());
      }

      if (extension == ".jpg" || extension == ".jpeg") {
        return bytes_read >= 3 &&
               header[0] == 0xFF &&
               header[1] == 0xD8 &&
               header[2] == 0xFF;
      }

      if (extension == ".webp") {
        static constexpr std::array<unsigned char, 4> riff_signature {'R', 'I', 'F', 'F'};
        static constexpr std::array<unsigned char, 4> webp_signature {'W', 'E', 'B', 'P'};
        return bytes_read >= 12 &&
               std::equal(riff_signature.begin(), riff_signature.end(), header.begin()) &&
               std::equal(webp_signature.begin(), webp_signature.end(), header.begin() + 8);
      }

      return false;
    }

    bool should_log_invalid_app_image_once(const std::string &path) {
      static std::mutex mutex;
      static std::unordered_set<std::string> logged_paths;

      std::lock_guard<std::mutex> lock(mutex);
      return logged_paths.emplace(path).second;
    }

  }  // namespace

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return resolve_bundled_asset("box.png");
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if extension is not one we can safely serve
    if (image_extension != ".png" &&
        image_extension != ".jpg" &&
        image_extension != ".jpeg" &&
        image_extension != ".webp") {
      return resolve_bundled_asset("box.png");
    }

    if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return resolve_bundled_asset("steam.png");
    }

    // check if image is in assets directory
    if (!std::filesystem::path(app_image_path).is_absolute()) {
      const auto bundled_asset = resolve_bundled_asset(app_image_path);
      if (std::filesystem::exists(bundled_asset)) {
        return bundled_asset;
      }
    }

    // check if specified image exists
    std::error_code code;
    if (!std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      if (should_log_invalid_app_image_once(app_image_path)) {
        BOOST_LOG(info) << "Couldn't find app image at path ["sv << app_image_path << "]; using default box art"sv;
      }
      return resolve_bundled_asset("box.png");
    }

    if (!has_supported_image_signature(app_image_path, image_extension)) {
      if (should_log_invalid_app_image_once(app_image_path)) {
        BOOST_LOG(warning) << "App image at path ["sv << app_image_path << "] failed signature validation";
      }
      return resolve_bundled_asset("box.png");
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

}  // namespace proc
