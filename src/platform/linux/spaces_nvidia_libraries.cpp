#include "spaces_nvidia_libraries.h"
#ifdef __linux__
#include "spaces_nvidia_contract.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace multiseat::spaces {
  namespace {
    using json = nlohmann::json;

    json strict_json(std::string_view text, std::size_t limit = 65536) {
      if (text.empty() || text.size() > limit) throw std::invalid_argument("invalid contract size");
      std::vector<std::set<std::string>> keys;
      return json::parse(text, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 8) throw std::invalid_argument("contract nesting");
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        if (event == json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate contract key");
        if (event == json::parse_event_t::object_end) keys.pop_back();
        return true;
      });
    }

    /** A SONAME is a bare file name: no separator, no traversal, nothing exotic. */
    bool soname_valid(std::string_view name) {
      return !name.empty() && name.size() <= 128 && name.front() != '.' &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-") ==
          std::string_view::npos;
    }

    bool absolute_normal(const std::filesystem::path &path) {
      return path.is_absolute() && path.lexically_normal() == path;
    }

    bool dotted_version(std::string_view value) {
      return value.size() >= 3 && value.size() <= 32 && value.front() != '.' && value.back() != '.' &&
        value.find_first_not_of("0123456789.") == std::string_view::npos &&
        value.find("..") == std::string_view::npos && value.find('.') != std::string_view::npos;
    }

    std::vector<unsigned long long> version_parts(std::string_view value) {
      std::vector<unsigned long long> parts;
      std::size_t start = 0;
      while (start <= value.size()) {
        const auto stop = value.find('.', start);
        const auto piece = value.substr(start, stop == std::string_view::npos ? std::string_view::npos : stop - start);
        unsigned long long number = 0;
        for (const char digit : piece) number = number * 10 + static_cast<unsigned>(digit - '0');
        parts.push_back(number);
        if (stop == std::string_view::npos) break;
        start = stop + 1;
      }
      return parts;
    }

    std::filesystem::path under(const std::filesystem::path &root, const std::filesystem::path &path) {
      if (root.empty() || root == "/") return path;
      return root / path.relative_path();
    }

    struct elf_identity_t {
      bool ok = false;
      bool thirty_two_bit = false;
      std::string soname;
    };

    template<typename T>
    bool read_at(std::ifstream &file, std::uint64_t offset, T &value) {
      file.clear();
      file.seekg(static_cast<std::streamoff>(offset));
      if (!file) return false;
      file.read(reinterpret_cast<char *>(&value), sizeof(T));
      return static_cast<bool>(file);
    }

    /**
     * Read the ELF class, machine and DT_SONAME without loading the file. A
     * driver library is hundreds of megabytes, so everything here seeks.
     */
    elf_identity_t read_elf_identity(const std::filesystem::path &path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) return {};
      std::array<unsigned char, 20> header {};
      if (!read_at(file, 0, header)) return {};
      if (std::memcmp(header.data(), "\x7f" "ELF", 4) != 0) return {};
      const bool thirty_two_bit = header[4] == 1;
      if (!thirty_two_bit && header[4] != 2) return {};
      if (header[5] != 1) return {};  // little endian only, like every x86 target
      const std::uint16_t type = static_cast<std::uint16_t>(header[16] | (header[17] << 8));
      const std::uint16_t machine = static_cast<std::uint16_t>(header[18] | (header[19] << 8));
      if (type != 3) return {};  // ET_DYN
      if (machine != (thirty_two_bit ? 3 : 62)) return {};  // EM_386 / EM_X86_64

      std::uint64_t program_offset = 0;
      std::uint16_t entry_size = 0, entry_count = 0;
      if (thirty_two_bit) {
        std::uint32_t offset32 = 0;
        if (!read_at(file, 28, offset32) || !read_at(file, 42, entry_size) || !read_at(file, 44, entry_count)) return {};
        program_offset = offset32;
      } else {
        if (!read_at(file, 32, program_offset) || !read_at(file, 54, entry_size) || !read_at(file, 56, entry_count)) return {};
      }
      if (entry_count == 0 || entry_count > 128 || entry_size < (thirty_two_bit ? 32 : 56)) return {};

      struct segment_t { std::uint64_t vaddr, offset, file_size; };
      std::vector<segment_t> loads;
      std::uint64_t dynamic_offset = 0, dynamic_size = 0;
      for (std::uint16_t index = 0; index < entry_count; ++index) {
        const std::uint64_t base = program_offset + static_cast<std::uint64_t>(index) * entry_size;
        std::uint32_t kind = 0;
        if (!read_at(file, base, kind)) return {};
        std::uint64_t offset = 0, vaddr = 0, file_size = 0;
        if (thirty_two_bit) {
          std::uint32_t offset32 = 0, vaddr32 = 0, size32 = 0;
          if (!read_at(file, base + 4, offset32) || !read_at(file, base + 8, vaddr32) ||
              !read_at(file, base + 16, size32)) return {};
          offset = offset32; vaddr = vaddr32; file_size = size32;
        } else {
          if (!read_at(file, base + 8, offset) || !read_at(file, base + 16, vaddr) ||
              !read_at(file, base + 32, file_size)) return {};
        }
        if (kind == 1) loads.push_back({vaddr, offset, file_size});
        if (kind == 2) { dynamic_offset = offset; dynamic_size = file_size; }
      }
      if (dynamic_offset == 0 || dynamic_size == 0 || dynamic_size > 1024 * 1024) return {};

      const auto file_offset = [&](std::uint64_t vaddr) -> std::optional<std::uint64_t> {
        for (const auto &load : loads)
          if (vaddr >= load.vaddr && vaddr - load.vaddr < load.file_size) return load.offset + (vaddr - load.vaddr);
        return std::nullopt;
      };

      const std::uint64_t pair_size = thirty_two_bit ? 8 : 16;
      std::uint64_t string_table = 0, soname_offset = 0;
      bool has_soname = false;
      for (std::uint64_t at = 0; at + pair_size <= dynamic_size; at += pair_size) {
        std::uint64_t tag = 0, value = 0;
        if (thirty_two_bit) {
          std::uint32_t tag32 = 0, value32 = 0;
          if (!read_at(file, dynamic_offset + at, tag32) || !read_at(file, dynamic_offset + at + 4, value32)) return {};
          tag = tag32; value = value32;
        } else {
          if (!read_at(file, dynamic_offset + at, tag) || !read_at(file, dynamic_offset + at + 8, value)) return {};
        }
        if (tag == 0) break;  // DT_NULL
        if (tag == 5) string_table = value;  // DT_STRTAB
        if (tag == 14) { soname_offset = value; has_soname = true; }  // DT_SONAME
      }
      if (!has_soname || string_table == 0) return {};
      const auto strings = file_offset(string_table);
      if (!strings) return {};

      std::string soname;
      file.clear();
      file.seekg(static_cast<std::streamoff>(*strings + soname_offset));
      if (!file) return {};
      std::getline(file, soname, '\0');
      if (!soname_valid(soname)) return {};
      return {true, thirty_two_bit, soname};
    }

    /** Rewrite every library_path to its bare file name, which the container resolves. */
    bool rewrite_library_paths(json &node, const std::set<std::string> &known, unsigned depth = 0) {
      if (depth > 8) return false;
      if (node.is_array()) {
        for (auto &child : node)
          if (!rewrite_library_paths(child, known, depth + 1)) return false;
        return true;
      }
      if (!node.is_object()) return true;
      for (auto &entry : node.items()) {
        if (entry.key() == "library_path") {
          if (!entry.value().is_string()) return false;
          const std::filesystem::path declared {entry.value().get<std::string>()};
          const auto name = declared.filename().string();
          if (!soname_valid(name) || !known.contains(name)) return false;
          entry.value() = name;
          continue;
        }
        if (!rewrite_library_paths(entry.value(), known, depth + 1)) return false;
      }
      return true;
    }
  }  // namespace

  std::optional<nvidia_contract_t> decode_nvidia_contract(std::string_view payload) {
    try {
      const auto document = strict_json(payload);
      if (!document.is_object() || document.size() != 6 || document.at("schema") != 1) return std::nullopt;
      nvidia_contract_t contract;
      contract.contract = document.at("contract").get<unsigned>();
      contract.minimum_driver = document.at("minimum_driver").get<std::string>();
      if (contract.contract == 0 || contract.contract > 16 || !dotted_version(contract.minimum_driver))
        return std::nullopt;
      for (const auto &prefix : document.at("library_prefixes")) {
        const auto value = prefix.get<std::string>();
        if (value.size() < 4 || value.size() > 64 || !value.starts_with("lib") || !value.ends_with(".so."))
          return std::nullopt;
        contract.library_prefixes.push_back(value);
      }
      if (contract.library_prefixes.empty() || contract.library_prefixes.size() > 64) return std::nullopt;
      std::set<std::string> names, destinations;
      for (const auto &entry : document.at("architectures")) {
        nvidia_architecture_t architecture;
        architecture.name = entry.at("name").get<std::string>();
        architecture.directory = entry.at("directory").get<std::string>();
        if ((architecture.name != "amd64" && architecture.name != "i386") ||
            !names.insert(architecture.name).second || !absolute_normal(architecture.directory))
          return std::nullopt;
        for (const auto &directory : entry.at("search")) {
          const std::filesystem::path path {directory.get<std::string>()};
          if (!absolute_normal(path)) return std::nullopt;
          architecture.search.push_back(path);
        }
        for (const auto &soname : entry.at("required")) {
          const auto value = soname.get<std::string>();
          if (!soname_valid(value)) return std::nullopt;
          architecture.required.push_back(value);
        }
        if (architecture.search.empty() || architecture.search.size() > 16 ||
            architecture.required.empty() || architecture.required.size() > 32) return std::nullopt;
        contract.architectures.push_back(std::move(architecture));
      }
      if (contract.architectures.size() != 2) return std::nullopt;
      for (const auto &entry : document.at("vendor_files")) {
        nvidia_vendor_file_t file;
        file.destination = entry.at("destination").get<std::string>();
        if (!absolute_normal(file.destination) || !destinations.insert(file.destination.string()).second ||
            !file.destination.string().starts_with("/usr/share/")) return std::nullopt;
        for (const auto &source : entry.at("sources")) {
          const std::filesystem::path path {source.get<std::string>()};
          if (!absolute_normal(path)) return std::nullopt;
          file.sources.push_back(path);
        }
        if (file.sources.empty() || file.sources.size() > 8) return std::nullopt;
        contract.vendor_files.push_back(std::move(file));
      }
      if (contract.vendor_files.empty() || contract.vendor_files.size() > 8) return std::nullopt;
      return contract;
    } catch (...) { return std::nullopt; }
  }

  const std::optional<nvidia_contract_t> &trusted_nvidia_contract() {
    static const auto contract = decode_nvidia_contract(nvidia_host_contract_json);
    return contract;
  }

  bool driver_at_least(std::string_view candidate, std::string_view minimum) {
    if (!dotted_version(candidate) || !dotted_version(minimum)) return false;
    const auto left = version_parts(candidate), right = version_parts(minimum);
    for (std::size_t index = 0; index < std::max(left.size(), right.size()); ++index) {
      const auto a = index < left.size() ? left[index] : 0;
      const auto b = index < right.size() ? right[index] : 0;
      if (a != b) return a > b;
    }
    return true;
  }

  host_driver_facts_t resolve_host_driver_libraries(
    const nvidia_contract_t &contract, const container::host_t &host,
    std::string_view loaded_driver, std::string_view minimum_driver,
    const std::filesystem::path &root) {
    host_driver_facts_t facts;
    facts.contract = contract.contract;
    facts.driver_version = std::string {loaded_driver};
    if (!dotted_version(loaded_driver)) { facts.code = "driver_libraries_missing"; return facts; }
    if (!driver_at_least(loaded_driver, minimum_driver.empty() ? contract.minimum_driver : minimum_driver)) {
      facts.code = "driver_below_minimum";
      return facts;
    }

    const std::string version_suffix = "." + facts.driver_version;
    std::set<std::string> amd64_sonames;
    for (const auto &architecture : contract.architectures) {
      const bool thirty_two_bit = architecture.name == "i386";
      const std::string missing_code = thirty_two_bit ? "driver_libraries_32bit_missing" : "driver_libraries_missing";
      std::map<std::string, std::filesystem::path> accepted;
      std::vector<std::string> missing;
      bool poisoned = false;

      for (const auto &directory : architecture.search) {
        const auto host_directory = under(root, directory);
        std::error_code error;
        if (!std::filesystem::is_directory(host_directory, error)) continue;
        std::map<std::string, std::filesystem::path> resolved;
        bool carries_loaded_driver = false;
        for (const auto &entry : std::filesystem::directory_iterator(host_directory, error)) {
          const auto name = entry.path().filename().string();
          if (std::none_of(contract.library_prefixes.begin(), contract.library_prefixes.end(),
                [&](const auto &prefix) { return name.starts_with(prefix); })) continue;
          const auto real = std::filesystem::weakly_canonical(entry.path(), error);
          if (error) { error.clear(); continue; }
          if (!host.trusted_system_file(real)) {
            // A driver file this machine will not vouch for poisons the whole
            // directory: taking the rest would hand a container a set that
            // someone else can still change.
            facts.code = "driver_libraries_untrusted";
            facts.missing.push_back(real.string());
            poisoned = true;
            break;
          }
          const auto identity = read_elf_identity(real);
          if (!identity.ok || identity.thirty_two_bit != thirty_two_bit) continue;
          const auto existing = resolved.find(identity.soname);
          if (existing != resolved.end() && existing->second != real) {
            facts.code = "driver_libraries_untrusted";
            facts.missing.push_back(identity.soname);
            poisoned = true;
            break;
          }
          if (name.ends_with(version_suffix) || real.filename().string().ends_with(version_suffix))
            carries_loaded_driver = true;
          resolved.insert({identity.soname, real});
        }
        if (poisoned) break;
        if (resolved.empty()) continue;
        if (!carries_loaded_driver) {
          // This directory belongs to another install. Mounting it would pair
          // userspace with a kernel module it was not built for.
          missing = {std::string {loaded_driver}};
          continue;
        }
        missing.clear();
        for (const auto &soname : architecture.required)
          if (!resolved.contains(soname)) missing.push_back(architecture.name + ":" + soname);
        if (missing.empty()) { accepted = std::move(resolved); break; }
      }

      if (poisoned) break;
      if (accepted.empty()) {
        if (facts.code.empty() || facts.code == "driver_libraries_32bit_missing") facts.code = missing_code;
        if (missing.empty()) missing.push_back(architecture.name + ":" + architecture.required.front());
        facts.missing.insert(facts.missing.end(), missing.begin(), missing.end());
        continue;
      }
      for (const auto &[soname, path] : accepted) {
        facts.libraries.push_back({architecture.name, soname, path});
        if (!thirty_two_bit) amd64_sonames.insert(soname);
      }
    }
    if (!facts.code.empty()) { facts.libraries.clear(); return facts; }

    for (const auto &file : contract.vendor_files) {
      bool published = false;
      for (const auto &source : file.sources) {
        const auto host_path = under(root, source);
        const auto contents = host.read_trusted_system_file(host_path, 65536);
        if (!contents) continue;
        try {
          auto document = strict_json(*contents);
          if (!rewrite_library_paths(document, amd64_sonames)) continue;
          facts.vendor_files.push_back({file.destination, document.dump(2) + "\n"});
          published = true;
          break;
        } catch (...) { continue; }
      }
      if (!published) {
        facts.code = "driver_configuration_missing";
        facts.missing.push_back(file.destination.string());
      }
    }
    if (!facts.code.empty()) {
      facts.libraries.clear();
      facts.vendor_files.clear();
    }
    return facts;
  }

  std::vector<container::host_driver_mount_t> host_driver_mounts(
    const nvidia_contract_t &contract, const host_driver_facts_t &facts,
    const std::filesystem::path &vendor_directory) {
    std::vector<container::host_driver_mount_t> mounts;
    if (!facts.ready() || !absolute_normal(vendor_directory)) return mounts;
    for (const auto &library : facts.libraries) {
      const auto architecture = std::find_if(contract.architectures.begin(), contract.architectures.end(),
        [&](const auto &entry) { return entry.name == library.architecture; });
      if (architecture == contract.architectures.end()) return {};
      mounts.push_back({(architecture->directory / library.soname).string(), library.host_path});
    }
    for (const auto &file : facts.vendor_files)
      mounts.push_back({file.destination.string(),
        vendor_directory / file.destination.filename()});
    return mounts;
  }

  bool publish_vendor_files(const host_driver_facts_t &facts, const std::filesystem::path &directory) {
    if (!facts.ready() || !absolute_normal(directory)) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace, error);
    if (error) return false;
    for (const auto &file : facts.vendor_files) {
      const auto path = directory / file.destination.filename();
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      out << file.contents;
      if (!out) return false;
      out.close();
      std::filesystem::permissions(path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::group_read | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, error);
      if (error) return false;
    }
    return true;
  }
}  // namespace multiseat::spaces
#endif
