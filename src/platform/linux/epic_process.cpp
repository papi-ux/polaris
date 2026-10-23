/**
 * @file src/platform/linux/epic_process.cpp
 * @brief Non-secret procfs observations and exact retained-pidfd Epic close grants.
 */
#include "epic_process.h"

#ifdef __linux__

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <fcntl.h>
#include <mutex>
#include <openssl/sha.h>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>

namespace platf::epic {
  namespace {
    namespace fs = std::filesystem;
    using testing::backend_t;
    using testing::exit_state_t;
    using testing::file_t;
    constexpr std::size_t small_limit = 64 * 1024;
    constexpr std::size_t maps_limit = 4 * 1024 * 1024;
    constexpr std::size_t mount_limit = 2 * 1024 * 1024;
    constexpr std::size_t process_limit = 8192;
    constexpr std::size_t snapshot_limit = 64 * 1024 * 1024;

    struct refusal_t: std::runtime_error {
      using std::runtime_error::runtime_error;
    };

    void require(bool condition, const char *reason) {
      if (!condition) {
        throw refusal_t(reason);
      }
    }

    template<class T>
    T needed(std::optional<T> value, const char *reason) {
      require(value.has_value(), reason);
      return std::move(*value);
    }

    std::uint64_t number(std::string_view text, int base = 10) {
      std::uint64_t result = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
      require(!text.empty() && parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size(), "malformed_proc_number");
      return result;
    }

    std::vector<std::string> words(const std::string &text) {
      std::istringstream stream(text);
      std::vector<std::string> result;
      std::string value;
      while (stream >> value) {
        result.push_back(std::move(value));
      }
      return result;
    }

    bool absolute(const fs::path &path) {
      const auto text = path.string();
      return path.is_absolute() && text.size() <= 4096 && text.find('\0') == std::string::npos &&
             std::none_of(path.begin(), path.end(), [](const auto &part) { return part == ".." || part == "."; });
    }

    bool inside(const fs::path &path, const fs::path &parent) {
      auto p = path.begin();
      for (auto q = parent.begin(); q != parent.end(); ++q, ++p) {
        if (p == path.end() || *p != *q) {
          return false;
        }
      }
      return true;
    }

    bool same_file(const file_t &a, const file_t &b) {
      return a.device == b.device && a.inode == b.inode && a.uid == b.uid && a.mode == b.mode;
    }

    bool same_image(const file_t &a, const file_t &b) {
      return same_file(a, b) && a.size == b.size && a.modified_ns == b.modified_ns && a.changed_ns == b.changed_ns;
    }

    bool safe_file(const file_t &file, std::uint64_t owner, bool executable) {
      return S_ISREG(file.mode) && file.inode != 0 && (file.uid == owner || file.uid == 0) &&
             (file.mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) == 0 && (!executable || (file.mode & 0111) != 0);
    }

    // Only kernel path escapes are decoded. A literal backslash is encoded as \134.
    std::string unescape(std::string_view text) {
      std::string result;
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') {
          result += text[i];
          continue;
        }
        require(i + 3 < text.size(), "malformed_proc_path");
        const auto code = text.substr(i + 1, 3);
        require(code == "040" || code == "011" || code == "012" || code == "134", "malformed_proc_path");
        result += static_cast<char>(number(code, 8));
        i += 3;
      }
      require(result.find('\0') == std::string::npos, "malformed_proc_path");
      return result;
    }

    struct stat_t {
      std::uint64_t pid, parent, start;
      char state;
    };

    stat_t parse_stat(const std::string &text) {
      const auto left = text.find('(');
      const auto right = text.rfind(')');
      require(left != std::string::npos && right != std::string::npos && left > 1 && right > left && text[left - 1] == ' ', "malformed_process_stat");
      const auto fields = words(text.substr(right + 1));
      require(fields.size() >= 20 && fields[0].size() == 1, "incomplete_process_stat");
      stat_t result {number(std::string_view(text).substr(0, left - 1)), number(fields[1]), number(fields[19]), fields[0][0]};
      require(result.pid > 1 && result.pid <= INT_MAX && result.start > 0, "invalid_process_identity");
      return result;
    }

    std::string cgroup(const std::string &text) {
      std::optional<std::string> result;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.compare(0, 3, "0::") == 0) {
          require(!result, "ambiguous_process_cgroup");
          result = line.substr(3);
        }
      }
      require(result && absolute(*result), "missing_unified_cgroup");
      return *result;
    }

    bool service_cgroup(const std::string &path, const std::string &self) {
      for (const auto &part : fs::path(path)) {
        const auto name = part.string();
        if (name == "polaris.service" || (name.compare(0, 8, "polaris@") == 0 && name.size() >= 8 && name.find(".service") != std::string::npos)) {
          return true;
        }
      }
      return inside(path, self);
    }

    bool check_uid(const std::string &text, std::uint64_t owner) {
      bool found = false;
      bool owned = false;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.compare(0, 4, "Uid:") == 0) {
          require(!found, "ambiguous_process_uid");
          found = true;
          const auto ids = words(line.substr(4));
          require(ids.size() == 4, "incomplete_process_uid");
          const auto matching = std::count_if(ids.begin(), ids.end(), [owner](const auto &id) { return number(id) == owner; });
          require(matching == 0 || matching == 4, "foreign_process_uid");
          owned = matching == 4;
        }
      }
      require(found, "missing_process_uid");
      return owned;
    }

    struct map_t {
      std::string range;
      std::uint64_t major, minor, inode;
    };

    std::vector<map_t> image_maps(const std::string &text, const fs::path &image) {
      std::vector<map_t> result;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string range, perms, offset, device, inode, path;
        require(bool(row >> range >> perms >> offset >> device >> inode), "malformed_process_maps");
        const auto dash = range.find('-');
        const auto colon = device.find(':');
        require(dash != std::string::npos && colon != std::string::npos && perms.size() == 4, "malformed_process_maps");
        require((perms[0] == 'r' || perms[0] == '-') && (perms[1] == 'w' || perms[1] == '-') &&
                  (perms[2] == 'x' || perms[2] == '-') && (perms[3] == 'p' || perms[3] == 's'), "malformed_map_permissions");
        require(number(std::string_view(range).substr(0, dash), 16) < number(std::string_view(range).substr(dash + 1), 16), "malformed_map_range");
        (void) number(offset, 16);
        const auto major = number(std::string_view(device).substr(0, colon), 16);
        const auto minor = number(std::string_view(device).substr(colon + 1), 16);
        const auto ino = number(inode);
        std::getline(row, path);
        const auto first = path.find_first_not_of(' ');
        path = first == std::string::npos ? "" : unescape(std::string_view(path).substr(first));
        require(path != image.string() + " (deleted)", "deleted_game_image");
        if (path == image) {
          require(ino > 0, "missing_mapped_image_inode");
          result.push_back({range, major, minor, ino});
        }
      }
      return result;
    }

    struct mount_t {
      std::string identity, type, source;
      fs::path point;
      std::uint64_t major, minor;
    };

    mount_t image_mount(const std::string &text, const fs::path &image) {
      std::optional<mount_t> result;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        const auto fields = words(line);
        const auto sep = std::find(fields.begin(), fields.end(), "-");
        require(fields.size() >= 10 && sep != fields.end() && sep - fields.begin() >= 6 && fields.end() - sep >= 4, "malformed_mountinfo");
        (void) number(fields[0]);
        (void) number(fields[1]);
        const auto colon = fields[2].find(':');
        require(colon != std::string::npos, "malformed_mount_device");
        const auto major = number(std::string_view(fields[2]).substr(0, colon));
        const auto minor = number(std::string_view(fields[2]).substr(colon + 1));
        const fs::path point = unescape(fields[4]);
        require(absolute(point) && absolute(unescape(fields[3])), "malformed_mount_path");
        if (!inside(image, point)) {
          continue;
        }
        require(!result || point != result->point, "ambiguous_image_mount");
        if (!result || point.string().size() > result->point.string().size()) {
          // Include mount ID, root, source, type and mount flags, not just the device pair.
          result = mount_t {line, *(sep + 1), unescape(*(sep + 2)), point, major, minor};
        }
      }
      require(result.has_value(), "missing_image_mount");
      return *result;
    }

    struct process_t {
      stat_t stat;
      fs::path exe, cwd;
      file_t image;
      std::string group;
      std::vector<map_t> maps;
    };

    struct reader_t {
      backend_t &backend;
      std::size_t bytes {0};
      fs::path group_namespace;

      std::string read(const fs::path &path, std::size_t limit = small_limit) {
        auto value = needed(backend.read(path, limit), "incomplete_process_observation");
        bytes += value.size();
        require(bytes <= snapshot_limit, "process_snapshot_too_large");
        return value;
      }

      fs::path link(const fs::path &path) {
        auto value = needed(backend.link(path), "unreadable_process_link");
        require(absolute(value), "unresolved_process_link");
        return value;
      }

      file_t stat(const fs::path &path) {
        return needed(backend.stat(path), "unreadable_file_identity");
      }

      fs::path canonical(const fs::path &path) {
        auto value = needed(backend.canonical(path), "unresolved_title_path");
        require(absolute(value), "unresolved_title_path");
        return value;
      }

      std::string boot() {
        auto value = read("/proc/sys/kernel/random/boot_id", 64);
        if (!value.empty() && value.back() == '\n') {
          value.pop_back();
        }
        require(value.size() == 36, "missing_boot_identity");
        for (std::size_t i = 0; i < value.size(); ++i) {
          require((i == 8 || i == 13 || i == 18 || i == 23) ? value[i] == '-' :
                    ((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')), "invalid_boot_identity");
        }
        return value;
      }

      bool unchanged(const process_t &process) {
        const auto path = fs::path("/proc") / std::to_string(process.stat.pid);
        const auto current = parse_stat(read(path / "stat"));
        return current.pid == process.stat.pid && current.start == process.stat.start &&
               current.state != 'Z' && current.state != 'X' && current.state != 'x' &&
               check_uid(read(path / "status"), backend.owner_uid()) && stat(path).uid == backend.owner_uid() &&
               link(path / "exe") == process.exe && link(path / "cwd") == process.cwd &&
               cgroup(read(path / "cgroup")) == process.group && backend.link(path / "ns/cgroup") == group_namespace &&
               same_image(process.image, stat(path / "exe")) && same_image(process.image, stat(process.exe)) &&
               same_image(process.image, stat(path / "root" / process.exe.relative_path()));
      }

      std::vector<process_t> table(const fs::path &game_image = {}) {
        const auto owner = backend.owner_uid();
        require(owner != 0, "host_owner_uid_required");
        group_namespace = needed(backend.link("/proc/self/ns/cgroup"), "unknown_cgroup_namespace");
        const auto ns = group_namespace.string();
        require(ns.size() > 9 && ns.compare(0, 8, "cgroup:[") == 0 && ns.back() == ']' &&
                  number(std::string_view(ns).substr(8, ns.size() - 9)) > 0, "unknown_cgroup_namespace");
        const auto names = needed(backend.list("/proc", process_limit), "incomplete_process_table");
        std::set<std::uint64_t> seen;
        std::vector<process_t> result;
        for (const auto &name : names) {
          if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
          }
          const auto pid = number(name);
          require(pid > 0 && pid <= INT_MAX && seen.insert(pid).second, "ambiguous_process_table");
          if (pid == 1) {
            continue;
          }
          const auto path = fs::path("/proc") / name;
          // A same-UID non-dumpable process can have a root-owned proc directory.
          // Read non-secret status first, so it cannot silently disappear from an owned scan.
          if (!check_uid(read(path / "status"), owner)) {
            continue;
          }
          require(stat(path).uid == owner, "unreadable_owner_process");
          const auto first = parse_stat(read(path / "stat"));
          require(first.pid == pid, "process_pid_changed");
          if (first.state == 'Z' || first.state == 'X' || first.state == 'x') {
            continue;
          }
          process_t row {first, link(path / "exe"), link(path / "cwd"), stat(path / "exe"), cgroup(read(path / "cgroup")), {}};
          require(backend.link(path / "ns/cgroup") == group_namespace, "foreign_cgroup_namespace");
          require(same_image(row.image, stat(row.exe)) && same_image(row.image, stat(path / "root" / row.exe.relative_path())), "process_executable_view_changed");
          if (!game_image.empty()) {
            row.maps = image_maps(read(path / "maps", maps_limit), game_image);
          }
          require(unchanged(row), "process_changed_during_read");
          result.push_back(std::move(row));
        }
        return result;
      }
    };

    bool token(const std::string &value) {
      return !value.empty() && value.size() <= 256 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
      });
    }

    struct digest_t {
      std::string bytes;

      void add(const std::string &value) {
        bytes += std::to_string(value.size()) + ":" + value;
      }

      void add(std::uint64_t value) {
        add(std::to_string(value));
      }

      void file(const file_t &value, bool directory = false) {
        add(value.device);
        add(value.inode);
        add(value.uid);
        add(value.mode);
        if (!directory) {
          add(value.size);
          add(static_cast<std::uint64_t>(value.modified_ns));
          add(static_cast<std::uint64_t>(value.changed_ns));
        }
      }

      void process(const process_t &value) {
        add(value.stat.pid);
        add(value.stat.start);
        add(value.exe.string());
        add(value.cwd.string());
        add(value.group);
        file(value.image);
      }

      std::string finish() const {
        std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
        SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest.data());
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        for (const auto byte : digest) {
          result += hex[byte >> 4];
          result += hex[byte & 15];
        }
        return result;
      }
    };

    std::optional<fs::path> runtime(const fs::path &exe, const fs::path &base, const char *binary) {
      if (!inside(exe, base)) {
        return std::nullopt;
      }
      const auto relative = exe.lexically_relative(base);
      std::vector<fs::path> parts(relative.begin(), relative.end());
      if (parts.size() != 4 || parts[1] != "files" || parts[2] != "bin" || parts[3] != binary || !token(parts[0].string())) {
        return std::nullopt;
      }
      return base / parts[0];
    }

    observation_t snapshot(const title_t &title, backend_t &backend) {
      reader_t read {backend};
      require(token(title.namespace_id) && token(title.item_id) && token(title.artifact_id), "invalid_title_identity");
      require(title.platform == "Windows" || title.platform == "Linux", "unknown_title_runtime");
      require(absolute(title.install_dir) && absolute(title.data_root), "invalid_title_path");
      const fs::path relative = title.launch_executable;
      require(!relative.empty() && !relative.is_absolute() && absolute(fs::path("/") / relative) &&
                title.launch_executable.find('\\') == std::string::npos, "invalid_title_executable");
      const auto install = read.canonical(title.install_dir);
      const auto data = read.canonical(title.data_root);
      const auto image = read.canonical(install / relative);
      require(inside(image, install) && image != install, "title_image_outside_installation");
      const auto image_file = read.stat(image);
      const auto install_file = read.stat(install);
      const auto data_file = read.stat(data);
      require(safe_file(image_file, backend.owner_uid(), title.platform == "Linux"), "unsafe_title_image");
      const auto magic = needed(backend.prefix(image, 4), "unreadable_title_image");
      const bool native = title.platform == "Linux";
      require(native ? magic == std::string("\177ELF", 4) : magic.size() == 4 && magic.substr(0, 2) == "MZ", "unsupported_title_image");
      const auto boot = read.boot();
      const auto own_group = cgroup(read.read("/proc/self/cgroup"));
      require(own_group != "/", "unknown_polaris_cgroup");
      const auto processes = read.table(image);
      const process_t *game = nullptr;
      for (const auto &process : processes) {
        if (!process.maps.empty() || process.exe == image) {
          require(!game, "ambiguous_game_process");
          game = &process;
        }
      }
      require(game != nullptr, "game_not_observed");
      require(!game->maps.empty() && game->cwd == install && !service_cgroup(game->group, own_group), "game_identity_incomplete");
      require(safe_file(game->image, backend.owner_uid(), true), "unsafe_game_executable");
      const auto process_path = fs::path("/proc") / std::to_string(game->stat.pid);
      require(same_image(image_file, read.stat(process_path / "root" / image.relative_path())), "game_process_root_mismatch");
      const auto mount = image_mount(read.read(process_path / "mountinfo", mount_limit), image);
      digest_t digest;
      digest.add("epic-owner-consent-v1");
      digest.add(title.namespace_id);
      digest.add(title.item_id);
      digest.add(title.artifact_id);
      digest.add(install.string());
      digest.add(data.string());
      digest.add(image.string());
      digest.add(title.platform);
      digest.add(backend.owner_uid());
      digest.add(boot);
      digest.process(*game);
      digest.file(image_file);
      digest.file(install_file, true);
      digest.file(data_file, true);
      digest.add(mount.identity);
      for (const auto &map : game->maps) {
        require(map.inode == image_file.inode && map.major == mount.major && map.minor == mount.minor, "mapped_game_identity_mismatch");
        // Btrfs subvolumes have a different stat st_dev from their VMA superblock device.
        require(mount.type == "btrfs" || (map.major == major(image_file.device) && map.minor == minor(image_file.device)), "mapped_game_device_mismatch");
        require(read.link(process_path / "map_files" / map.range) == image, "mapped_game_link_mismatch");
        digest.add(map.range);
        digest.add(map.major);
        digest.add(map.minor);
        digest.add(map.inode);
      }
      const process_t *server = nullptr;
      std::vector<std::pair<fs::path, file_t>> directories;
      if (native) {
        require(game->exe == image && same_image(game->image, image_file), "native_executable_mismatch");
        digest.add("native");
      }
      else {
        const auto base = read.canonical(data / "compat/runtime");
        require(base == data / "compat/runtime", "redirected_runtime_root");
        const auto game_runtime = runtime(game->exe, base, "wine64-preloader");
        require(game_runtime.has_value(), "unknown_game_runtime");
        require(read.canonical(game->exe) == game->exe, "redirected_game_runtime");
        const auto prefix_path = data / "compat/compatdata" / title.artifact_id / "pfx";
        require(read.canonical(prefix_path) == prefix_path, "redirected_title_prefix");
        const auto prefix = read.stat(prefix_path);
        require(S_ISDIR(prefix.mode) && prefix.uid == backend.owner_uid() && prefix.inode > 0, "unsafe_title_prefix");
        std::ostringstream server_name;
        server_name << "server-" << std::hex << prefix.device << '-' << prefix.inode;
        const auto server_dir = fs::path("/tmp") / (".wine-" + std::to_string(backend.owner_uid())) / server_name.str();
        for (const auto &process : processes) {
          if (process.cwd == server_dir) {
            require(!server, "ambiguous_prefix_server");
            require(runtime(process.exe, base, "wineserver") == game_runtime && !service_cgroup(process.group, own_group), "prefix_runtime_mismatch");
            require(safe_file(process.image, backend.owner_uid(), true) && read.canonical(process.exe) == process.exe, "unsafe_prefix_runtime");
            server = &process;
          }
        }
        require(server != nullptr, "prefix_server_not_observed");
        digest.add("proton");
        digest.add(game_runtime->string());
        const auto runtime_file = read.stat(*game_runtime);
        require(S_ISDIR(runtime_file.mode) && (runtime_file.uid == backend.owner_uid() || runtime_file.uid == 0), "unsafe_runtime_directory");
        digest.file(runtime_file, true);
        digest.add(prefix_path.string());
        digest.file(prefix, true);
        digest.process(*server);
        directories.emplace_back(*game_runtime, runtime_file);
        directories.emplace_back(prefix_path, prefix);
      }
      // Bookend process and file identity after maps/root/runtime reads as well as inside table().
      for (const auto &[path, identity] : directories) {
        require(read.canonical(path) == path && same_file(identity, read.stat(path)), "runtime_or_prefix_changed");
      }
      require(!server || read.unchanged(*server), "prefix_server_changed");
      const auto last_maps = image_maps(read.read(process_path / "maps", maps_limit), image);
      require(last_maps.size() == game->maps.size(), "game_mappings_changed");
      for (std::size_t i = 0; i < last_maps.size(); ++i) {
        const auto &a = last_maps[i];
        const auto &b = game->maps[i];
        require(a.range == b.range && a.major == b.major && a.minor == b.minor && a.inode == b.inode &&
                  read.link(process_path / "map_files" / a.range) == image, "game_mappings_changed");
      }
      require(read.unchanged(*game) && same_image(image_file, read.stat(image)) &&
                same_image(image_file, read.stat(process_path / "root" / image.relative_path())) &&
                image_mount(read.read(process_path / "mountinfo", mount_limit), image).identity == mount.identity &&
                read.canonical(title.install_dir) == install && read.canonical(title.data_root) == data &&
                same_file(install_file, read.stat(install)) && same_file(data_file, read.stat(data)) && boot == read.boot(), "game_changed_during_observation");
      return {true, "owner_confirmation_required", digest.finish(), native ? "native" : "proton", game->stat.pid, game->stat.start};
    }

    observation_t safe_snapshot(const title_t &title, backend_t &backend) {
      try {
        return snapshot(title, backend);
      }
      catch (const refusal_t &error) {
        return {false, error.what(), "", "unknown", 0, 0};
      }
      catch (const std::exception &) {
        return {false, "process_observation_failed", "", "unknown", 0, 0};
      }
    }

    bool same_observation(const observation_t &a, const observation_t &b) {
      return a.available && b.available && !a.revision.empty() && a.revision == b.revision &&
             a.pid == b.pid && a.start_ticks == b.start_ticks && a.runtime == b.runtime;
    }

    std::optional<std::string> read_file(const fs::path &path, std::size_t limit, bool prefix) {
      if (limit == 0 || limit > maps_limit) {
        return std::nullopt;
      }
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
      if (fd < 0) {
        return std::nullopt;
      }
      struct fd_guard_t {
        int fd;
        ~fd_guard_t() { ::close(fd); }
      } guard {fd};
      struct stat info {};
      if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::nullopt;
      }
      // Procfs reports st_size == 0 for maps/status/stat. Always read to EOF or the cap.
      std::string result;
      std::array<char, 4096> buffer;
      const auto cap = prefix ? limit : limit + 1;
      while (result.size() < cap) {
        const auto count = ::read(fd, buffer.data(), std::min(buffer.size(), cap - result.size()));
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count < 0) {
          return std::nullopt;
        }
        if (count == 0) {
          break;
        }
        result.append(buffer.data(), static_cast<std::size_t>(count));
      }
      return result.size() > limit || (prefix && result.size() != limit) ? std::nullopt : std::make_optional(std::move(result));
    }

    struct linux_backend_t final: backend_t {
      std::uint64_t owner_uid() const override { return ::getuid(); }

      std::optional<std::string> read(const fs::path &path, std::size_t limit) override {
        return read_file(path, limit, false);
      }

      std::optional<std::string> prefix(const fs::path &path, std::size_t length) override {
        return read_file(path, length, true);
      }

      std::optional<fs::path> link(const fs::path &path) override {
        std::array<char, 4097> buffer;
        const auto count = ::readlink(path.c_str(), buffer.data(), buffer.size());
        if (count <= 0 || static_cast<std::size_t>(count) == buffer.size()) {
          return std::nullopt;
        }
        return fs::path(std::string(buffer.data(), static_cast<std::size_t>(count)));
      }

      std::optional<fs::path> canonical(const fs::path &path) override {
        std::error_code error;
        auto result = fs::canonical(path, error);
        return error ? std::nullopt : std::make_optional(result);
      }

      std::optional<file_t> stat(const fs::path &path) override {
        struct stat value {};
        if (::stat(path.c_str(), &value) != 0) {
          return std::nullopt;
        }
        return file_t {value.st_dev, value.st_ino, value.st_uid, value.st_mode, static_cast<std::uint64_t>(value.st_size),
                       value.st_mtim.tv_sec * 1000000000LL + value.st_mtim.tv_nsec, value.st_ctim.tv_sec * 1000000000LL + value.st_ctim.tv_nsec};
      }

      std::optional<std::vector<std::string>> list(const fs::path &path, std::size_t limit) override {
        std::vector<std::string> result;
        std::error_code error;
        fs::directory_iterator it(path, error), end;
        while (!error && it != end) {
          if (result.size() == limit) {
            return std::nullopt;
          }
          result.push_back(it->path().filename().string());
          it.increment(error);
        }
        return error ? std::nullopt : std::make_optional(result);
      }

      int open_pidfd(std::uint64_t pid) override {
#ifdef SYS_pidfd_open
        return static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
#else
        return -1;
#endif
      }

      std::optional<std::uint64_t> pidfd_pid(int fd) override {
        const auto info = read(fs::path("/proc/self/fdinfo") / std::to_string(fd), small_limit);
        if (!info) {
          return std::nullopt;
        }
        std::istringstream stream(*info);
        std::string line;
        std::optional<std::uint64_t> pid;
        try {
          while (std::getline(stream, line)) {
            if (line.compare(0, 4, "Pid:") == 0) {
              const auto fields = words(line.substr(4));
              if (pid || fields.size() != 1) {
                return std::nullopt;
              }
              pid = number(fields[0]);
            }
          }
        }
        catch (const refusal_t &) {
          return std::nullopt;
        }
        return pid;
      }

      exit_state_t wait(int fd, std::chrono::milliseconds timeout) override {
        const auto bounded = std::clamp(timeout, std::chrono::milliseconds::zero(), std::chrono::milliseconds(10000));
        const auto deadline = std::chrono::steady_clock::now() + bounded;
        for (;;) {
          pollfd process {fd, POLLIN, 0};
          const auto remaining = std::max(std::chrono::milliseconds::zero(), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
          const int result = ::poll(&process, 1, static_cast<int>(remaining.count()));
          if (result < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline) {
            continue;
          }
          if (result < 0 || (process.revents & (POLLERR | POLLNVAL | POLLHUP))) {
            return exit_state_t::unavailable;
          }
          return (process.revents & POLLIN) ? exit_state_t::exited : exit_state_t::alive;
        }
      }

      bool term(int fd) override {
#ifdef SYS_pidfd_send_signal
        return ::syscall(SYS_pidfd_send_signal, fd, SIGTERM, nullptr, 0) == 0;
#else
        return false;
#endif
      }

      void close(int fd) override { ::close(fd); }
    };

    std::shared_ptr<backend_t> production_backend() {
      return std::make_shared<linux_backend_t>();
    }

    fs::path launcher_path(backend_t &backend) {
      reader_t read {backend};
      const auto boot = read.boot();
      const auto own_group = cgroup(read.read("/proc/self/cgroup"));
      require(own_group != "/", "unknown_polaris_cgroup");
      const auto table = read.table();
      const process_t *launcher = nullptr;
      for (const auto &process : table) {
        if (process.exe.filename() == "EpicGamesLauncher") {
          require(!launcher, "ambiguous_desktop_launcher");
          launcher = &process;
        }
      }
      require(launcher != nullptr, "open_epic_on_host_desktop");
      require(!service_cgroup(launcher->group, own_group), "launcher_in_polaris_cgroup");
      require(safe_file(launcher->image, backend.owner_uid(), true) && read.canonical(launcher->exe) == launcher->exe, "unsafe_desktop_launcher");
      require(needed(backend.prefix(launcher->exe, 4), "unreadable_desktop_launcher") == std::string("\177ELF", 4), "unknown_launcher_executable");
      const auto proc = fs::path("/proc") / std::to_string(launcher->stat.pid);
      const auto mount = image_mount(read.read(proc / "mountinfo", mount_limit), launcher->exe);
      auto executable = launcher->exe;
      auto executable_file = launcher->image;
      std::optional<fs::path> backing_source;
      if (mount.point.filename().string().compare(0, 7, ".mount_") == 0 || mount.type.compare(0, 4, "fuse") == 0 || mount.type == "squashfs") {
        // Do not guess Desktop/Downloads paths or use an unpacked AppImage binary without its runtime.
        // Only an absolute kernel mount source can prove the backing AppImage in this first slice.
        const fs::path source = mount.source;
        require(absolute(source), "launcher_appimage_path_unproven");
        executable = read.canonical(source);
        backing_source = source;
        executable_file = read.stat(executable);
        require(executable_file.uid == backend.owner_uid() && safe_file(executable_file, backend.owner_uid(), true), "unsafe_launcher_appimage");
        const auto header = needed(backend.prefix(executable, 11), "unreadable_launcher_appimage");
        require(header.substr(0, 4) == std::string("\177ELF", 4) && header.substr(8, 3) == std::string("AI\002", 3), "unknown_launcher_appimage");
      }
      require(read.unchanged(*launcher) && boot == read.boot() &&
                (!backing_source || read.canonical(*backing_source) == executable) &&
                same_image(executable_file, read.stat(executable)) &&
                image_mount(read.read(proc / "mountinfo", mount_limit), launcher->exe).identity == mount.identity, "desktop_launcher_changed");
      return executable;
    }
  }  // namespace

  struct adopted_game_t::state_t {
    title_t title;
    observation_t observation;
    std::shared_ptr<backend_t> backend;
    int fd {-1};
    mutable std::mutex mutex;
    mutable bool revoked {false};
    bool signalled {false};

    ~state_t() {
      if (fd >= 0) {
        backend->close(fd);
      }
    }

    observation_t inspect() const {
      if (revoked) {
        return {false, "adoption_revoked", "", "unknown", 0, 0};
      }
      const auto status = backend->wait(fd, std::chrono::milliseconds::zero());
      if (status != exit_state_t::alive || backend->pidfd_pid(fd) != observation.pid) {
        revoked = true;
        return {false, status == exit_state_t::exited ? "adopted_game_exited" : "retained_pidfd_unavailable", "", "unknown", 0, 0};
      }
      auto current = safe_snapshot(title, *backend);
      if (!same_observation(observation, current) || backend->wait(fd, std::chrono::milliseconds::zero()) != exit_state_t::alive || backend->pidfd_pid(fd) != observation.pid) {
        revoked = true;
        return {false, current.available ? "adopted_game_identity_changed" : current.reason, "", "unknown", 0, 0};
      }
      current.reason = "managed";
      return current;
    }
  };

  namespace detail {
    struct engine_t {
      static std::unique_ptr<adopted_game_t> adopt(const title_t &title, const observation_t &observation, std::string &error, std::shared_ptr<backend_t> backend) {
        error.clear();
        auto current = safe_snapshot(title, *backend);
        if (!same_observation(current, observation)) {
          error = current.available ? "stale_observation" : current.reason;
          return nullptr;
        }
        auto state = std::make_unique<adopted_game_t::state_t>();
        state->title = title;
        state->observation = current;
        state->backend = std::move(backend);
        state->fd = state->backend->open_pidfd(current.pid);
        if (state->fd < 0) {
          error = "pidfd_open_failed";
          return nullptr;
        }
        // The descriptor stays owned across every subsequent check and every refusal path.
        const auto verified = state->inspect();
        if (!verified.available) {
          error = verified.reason;
          return nullptr;
        }
        return std::unique_ptr<adopted_game_t>(new adopted_game_t(std::move(state)));
      }
    };
  }

  adopted_game_t::adopted_game_t(std::unique_ptr<state_t> state): state_(std::move(state)) {}
  adopted_game_t::~adopted_game_t() = default;
  adopted_game_t::adopted_game_t(adopted_game_t &&) noexcept = default;
  adopted_game_t &adopted_game_t::operator=(adopted_game_t &&) noexcept = default;

  observation_t observe(const title_t &title) {
    return safe_snapshot(title, *production_backend());
  }

  std::unique_ptr<adopted_game_t> adopt(const title_t &title, const observation_t &observation, std::string &error) {
    return detail::engine_t::adopt(title, observation, error, production_backend());
  }

  observation_t inspect(const adopted_game_t &game) {
    if (!game.state_) {
      return {false, "no_adoption", "", "unknown", 0, 0};
    }
    const std::lock_guard<std::mutex> lock(game.state_->mutex);
    return game.state_->inspect();
  }

  close_result_t ask_to_close(adopted_game_t &game, std::chrono::milliseconds timeout) {
    if (!game.state_) {
      return {false, false, "no_adoption"};
    }
    auto &state = *game.state_;
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.inspect().available) {
      const auto exited = state.backend->wait(state.fd, std::chrono::milliseconds::zero()) == exit_state_t::exited;
      return {exited, state.signalled, exited ? "adopted_game_exited" : "close_refused_identity_changed"};
    }
    if (!state.signalled) {
      if (!state.backend->term(state.fd)) {
        state.revoked = true;
        const auto exited = state.backend->wait(state.fd, std::chrono::milliseconds::zero()) == exit_state_t::exited;
        return {exited, false, exited ? "adopted_game_exited" : "pidfd_sigterm_failed"};
      }
      state.signalled = true;
    }
    const auto status = state.backend->wait(state.fd, std::clamp(timeout, std::chrono::milliseconds::zero(), std::chrono::milliseconds(10000)));
    if (status == exit_state_t::exited) {
      state.revoked = true;
      return {true, true, "adopted_game_exited"};
    }
    if (status == exit_state_t::unavailable) {
      state.revoked = true;
    }
    return {false, true, status == exit_state_t::alive ? "close_timeout" : "retained_pidfd_unavailable"};
  }

  std::optional<fs::path> desktop_launcher_executable(std::string &reason) {
    return testing::desktop_launcher_executable(reason, production_backend());
  }

  bool desktop_launcher_running(std::string &reason) {
    return desktop_launcher_executable(reason).has_value();
  }

  namespace testing {
    observation_t observe(const title_t &title, std::shared_ptr<backend_t> backend) {
      return backend ? safe_snapshot(title, *backend) : observation_t {false, "missing_process_reader", "", "unknown", 0, 0};
    }

    std::unique_ptr<adopted_game_t> adopt(const title_t &title, const observation_t &observation, std::string &error, std::shared_ptr<backend_t> backend) {
      if (!backend) {
        error = "missing_process_reader";
        return nullptr;
      }
      return detail::engine_t::adopt(title, observation, error, std::move(backend));
    }

    std::optional<fs::path> desktop_launcher_executable(std::string &reason, std::shared_ptr<backend_t> backend) {
      reason.clear();
      try {
        require(bool(backend), "missing_process_reader");
        return launcher_path(*backend);
      }
      catch (const refusal_t &error) {
        reason = error.what();
      }
      catch (const std::exception &) {
        reason = "desktop_launcher_observation_failed";
      }
      return std::nullopt;
    }

    std::optional<std::string> read_bounded(const fs::path &path, std::size_t limit) {
      return read_file(path, limit, false);
    }
  }  // namespace testing
}  // namespace platf::epic

#endif
