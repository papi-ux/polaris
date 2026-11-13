#include "epic_library.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

#ifndef _WIN32
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace epic_library {
  namespace {
    using json = nlohmann::json;
    constexpr std::size_t max_bytes = 16 * 1024 * 1024;
    constexpr std::size_t max_line = 64 * 1024;
    constexpr std::size_t max_catalog = 1024 * 1024;
    constexpr const char *flatpak_app = "com.epicgames.launcher";
    bool text(const std::string &s, std::size_t limit = 4096) {
      return !s.empty() && s.size() <= limit &&
        std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
    }
    std::string value(const json &j, const char *key) {
      const auto it = j.find(key);
      return it != j.end() && it->is_string() ? it->get<std::string>() : std::string {};
    }
    bool id(const std::string &s) {
      return text(s, 256) && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-';
      });
    }
    bool valid_absolute_path(const std::filesystem::path &p) {
      const auto s = p.string();
      return text(s) && p.is_absolute() && p != p.root_path() && !s.starts_with("//") &&
        std::none_of(p.begin(), p.end(), [](const auto &c) { return c == ".."; });
    }
    json strict_json(std::string_view bytes) {
      std::vector<std::set<std::string>> objects;
      return json::parse(bytes, [&](int depth, json::parse_event_t event, json &parsed) {
        if (depth > 32) throw std::runtime_error("excessive JSON nesting");
        if (event == json::parse_event_t::object_start) objects.emplace_back();
        if (event == json::parse_event_t::key && !objects.back().insert(parsed.get<std::string>()).second)
          throw std::runtime_error("duplicate JSON key");
        if (event == json::parse_event_t::object_end) objects.pop_back();
        return true;
      });
    }
    std::string key(const identity_t &i) { return i.namespace_id + ':' + i.item_id + ':' + i.artifact_id; }
    identity_t identity(const json &j) { return {value(j, "namespace"), value(j, "itemId"), value(j, "artifactId")}; }
    std::string quote(const std::string &s) {
      std::string out = "'";
      for (char c : s) out += c == '\'' ? "'\\''" : std::string(1, c);
      return out + "'";
    }
    bool component(const std::string &s) {
      return id(s) && s.front() != '-';
    }
    bool commit_id(const std::string &s) {
      return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }
    bool beneath(const std::filesystem::path &path, const std::filesystem::path &parent) {
      auto p = path.begin();
      for (const auto &part : parent) if (p == path.end() || *p++ != part) return false;
      return p != path.end();
    }
    using ini_t = std::map<std::string, std::string>;
    ini_t flatpak_ini(std::string_view raw) {
      if (raw.empty() || raw.size() > max_catalog) throw std::runtime_error("unavailable Flatpak metadata");
      ini_t out;
      std::set<std::string> groups;
      std::string group, line;
      std::istringstream stream {std::string(raw)};
      while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
          group = line.substr(1, line.size() - 2);
          if (!text(group) || !groups.insert(group).second) throw std::runtime_error("ambiguous Flatpak group");
        } else {
          const auto equals = line.find('=');
          if (group.empty() || equals == std::string::npos || !equals ||
              !out.emplace(group + '/' + line.substr(0, equals), line.substr(equals + 1)).second)
            throw std::runtime_error("ambiguous Flatpak key");
        }
      }
      return out;
    }
#ifndef _WIN32
    struct fd_t {
      int fd = -1;
      ~fd_t() { if (fd >= 0) ::close(fd); }
    };
    bool same(const struct stat &a, const struct stat &b) {
      return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_uid == b.st_uid &&
        a.st_size == b.st_size && a.st_mtime == b.st_mtime && a.st_ctime == b.st_ctime
#ifdef __linux__
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec
#elif defined(__APPLE__)
        && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec
#endif
        ;
    }
    std::optional<std::string> bounded_read(const std::filesystem::path &path, std::size_t limit, bool deployment = false) {
      // Only the named nonsecret metadata file is read. Never use .prev, account caches or a retry.
      fd_t root {::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
      struct stat root_before {}, root_after {}, named_root {}, before {}, after {}, named {};
      const auto owner = [deployment](const struct stat &s) {
        return (s.st_uid == ::getuid() || (deployment && s.st_uid == 0)) &&
          (!deployment || (s.st_mode & (S_IWGRP | S_IWOTH)) == 0);
      };
      if (root.fd < 0 || ::fstat(root.fd, &root_before) || !owner(root_before)) return {};
      fd_t file {::openat(root.fd, path.filename().c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
      if (file.fd < 0 || ::fstat(file.fd, &before) || !S_ISREG(before.st_mode) ||
          !owner(before) || (deployment && !(before.st_mode & S_IRUSR)) || before.st_size <= 0 || static_cast<uint64_t>(before.st_size) > limit) return {};
      std::string raw(static_cast<std::size_t>(before.st_size) + 1, '\0');
      std::size_t count = 0;
      while (count < raw.size()) {
        const auto n = ::read(file.fd, raw.data() + count, raw.size() - count);
        if (n < 0) return {};
        if (!n) break;
        count += static_cast<std::size_t>(n);
      }
      if (count != static_cast<std::size_t>(before.st_size) || ::fstat(file.fd, &after) || !same(before, after) ||
          ::fstatat(root.fd, path.filename().c_str(), &named, AT_SYMLINK_NOFOLLOW) || !same(before, named) ||
          ::fstat(root.fd, &root_after) || !same(root_before, root_after) ||
          ::lstat(path.parent_path().c_str(), &named_root) || !same(root_before, named_root)) return {};
      raw.resize(count);
      return raw;
    }
    std::string image_stamp(const std::filesystem::path &path) {
      fd_t file {::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
      struct stat before {}, after {};
      char header[4] {};
      if (file.fd < 0 || ::fstat(file.fd, &before) || !S_ISREG(before.st_mode) ||
          (before.st_uid != 0 && before.st_uid != ::getuid()) ||
          (before.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) || !(before.st_mode & 0111) ||
          ::read(file.fd, header, 4) != 4 || std::string(header, 4) != std::string("\177ELF", 4) ||
          ::fstat(file.fd, &after) || !same(before, after)) return {};
#ifdef __APPLE__
      const auto modified = before.st_mtimespec, changed = before.st_ctimespec;
#else
      const auto modified = before.st_mtim, changed = before.st_ctim;
#endif
      return std::to_string(before.st_dev) + ':' + std::to_string(before.st_ino) + ':' + std::to_string(before.st_uid) + ':' +
        std::to_string(before.st_mode) + ':' + std::to_string(before.st_size) + ':' +
        std::to_string(static_cast<std::int64_t>(modified.tv_sec) * 1000000000 + modified.tv_nsec) + ':' +
        std::to_string(static_cast<std::int64_t>(changed.tv_sec) * 1000000000 + changed.tv_nsec);
    }
#else
    std::optional<std::string> bounded_read(const std::filesystem::path &, std::size_t, bool = false) { return {}; }
    std::string image_stamp(const std::filesystem::path &) { return {}; }
#endif
    flatpak_deployment_t deployment(const std::filesystem::path &root, const std::filesystem::path &app_home, const std::string &scope) {
      namespace fs = std::filesystem;
      if (!valid_absolute_path(root) || !valid_absolute_path(app_home) || (scope != "user" && scope != "system"))
        throw std::runtime_error("invalid Flatpak installation");
      std::vector<flatpak_deployment_t> found;
      const auto app = root / "app" / flatpak_app;
      std::size_t entries = 0;
      for (const auto &arch : fs::directory_iterator(app)) {
        if (++entries > 64) throw std::runtime_error("too many Flatpak refs");
        // current is Flatpak's own convenience symlink, never an implicit ref choice.
        if (arch.path().filename() == "current") continue;
        if (!arch.is_directory() || arch.is_symlink()) throw std::runtime_error("invalid Flatpak architecture");
        for (const auto &branch : fs::directory_iterator(arch.path())) {
          if (++entries > 64) throw std::runtime_error("too many Flatpak refs");
          if (!branch.is_directory() || branch.is_symlink()) throw std::runtime_error("invalid Flatpak branch");
          const auto active = branch.path() / "active";
          std::error_code ec;
          if (fs::symlink_status(active, ec).type() == fs::file_type::not_found) continue;
          const auto commit = fs::read_symlink(active).string();
          if (arch.path().filename() != "x86_64" || !component(branch.path().filename().string()) || !commit_id(commit))
            throw std::runtime_error("unsupported Flatpak ref");
          const auto dir = branch.path() / commit;
          if (fs::canonical(active) != dir || fs::canonical(root) != root || fs::canonical(dir) != dir)
            throw std::runtime_error("redirected Flatpak deployment");
          const auto raw = bounded_read(dir / "metadata", max_catalog, true);
          if (!raw) throw std::runtime_error("unreadable Flatpak deployment");
          auto ini = flatpak_ini(*raw);
          if (ini["Application/name"] != flatpak_app || ini["Application/command"] != "EpicGamesLauncher" ||
              !ini["Application/runtime"].starts_with("org.gnome.Platform/x86_64/"))
            throw std::runtime_error("mismatched Flatpak metadata");
          const auto stamp = image_stamp(dir / "files/bin/EpicGamesLauncher");
          if (stamp.empty() || fs::canonical(active) != dir || bounded_read(dir / "metadata", max_catalog, true) != raw)
            throw std::runtime_error("changed Flatpak deployment");
          found.push_back({scope, scope == "system" ? "default" : "", "x86_64", branch.path().filename().string(), commit,
            root, dir, app_home, *raw, stamp});
        }
      }
      if (found.size() != 1) throw std::runtime_error("missing or ambiguous Flatpak deployment");
      return found.front();
    }
  }

  bool valid_identity(const identity_t &i) { return id(i.namespace_id) && id(i.item_id) && id(i.artifact_id); }

  std::optional<control_request_t> parse_control_request(std::string_view bytes) {
    try {
      if (bytes.empty() || bytes.size() > 2048) return {};
      const auto j = strict_json(bytes);
      if (!j.is_object()) return {};
      control_request_t request {value(j, "action"), value(j, "session_id"), value(j, "revision"), false};
      if (!text(request.session_id, 256) || !text(request.revision, 512) ||
          (request.action != "adopt" && request.action != "release")) return {};
      for (auto it = j.begin(); it != j.end(); ++it)
        if (it.key() != "action" && it.key() != "session_id" && it.key() != "revision" &&
            !(request.action == "adopt" && it.key() == "confirm")) return {};
      request.confirmed = j.contains("confirm") && j["confirm"].is_boolean() && j["confirm"].get<bool>();
      if (request.action == "adopt" && !request.confirmed) return {};
      return request;
    } catch (const std::exception &) { return {}; }
  }

  snapshot_t parse_installations(std::string_view raw, const std::filesystem::path &root) {
    snapshot_t result;
    try {
      if (raw.empty() || raw.size() > max_bytes || !valid_absolute_path(root)) throw std::runtime_error("unavailable installed-state snapshot");
      std::map<std::string, json> current;
      std::size_t offset = 0, lines = 0;
      while (offset < raw.size()) {
        auto end = raw.find('\n', offset);
        if (end == std::string_view::npos) end = raw.size();
        if (end == offset || end - offset > max_line || ++lines > 50001) throw std::runtime_error("invalid installed-state framing");
        auto row = strict_json(raw.substr(offset, end - offset));
        offset = end + 1;
        if (!row.is_object()) throw std::runtime_error("installed-state row is not an object");
        if (lines == 1) {
          if (!row.contains("version") || !row["version"].is_number_integer() || row["version"] != 1 ||
              !row.contains("entries") || !row["entries"].is_number_integer() || row["entries"] < 0)
            throw std::runtime_error("unsupported installed-state header");
          continue;
        }
        const auto installation_id = value(row, "installationId");
        if (!text(installation_id, 256) || (row.contains("uid") && row["uid"] != installation_id) ||
            (row.contains("deleted") && !row["deleted"].is_boolean())) throw std::runtime_error("unkeyable installed-state row");
        if (row.value("deleted", false)) {
          if (!row.contains("writtenAt") || row["writtenAt"].is_null()) throw std::runtime_error("ambiguous installed-state tombstone");
          current.erase(installation_id);
        } else current[installation_id] = std::move(row); // Last RAW row wins, even when not launchable.
      }
      std::map<std::string, unsigned> ids, directories;
      for (const auto &[iid, row] : current) {
        const auto i = identity(row);
        if (valid_identity(i)) ++ids[key(i)];
        const std::filesystem::path directory = value(row, "dir");
        if (valid_absolute_path(directory)) ++directories[directory.lexically_normal().string()];
      }
      for (const auto &[iid, row] : current) {
        const auto i = identity(row);
        const std::filesystem::path directory = value(row, "dir");
        const std::filesystem::path executable = value(row, "launchExecutable");
        const bool safe_exe = text(executable.string()) && executable.string().find('\\') == std::string::npos && !executable.is_absolute() &&
          std::none_of(executable.begin(), executable.end(), [](const auto &c) { return c == ".." || c == "."; });
        const auto platform = value(row, "platform");
        if (!valid_identity(i) || !valid_absolute_path(directory) || !safe_exe ||
            platform != "Windows" || !row.contains("complete") || !row["complete"].is_boolean() || !row["complete"].get<bool>() ||
            (row.contains("pendingVersion") && !row["pendingVersion"].is_null()) || !text(value(row, "version")) ||
            (row.contains("launchCommand") && !row["launchCommand"].is_null() && row["launchCommand"] != "") ||
            ids[key(i)] != 1 || directories[directory.lexically_normal().string()] != 1) {
          result.warnings.push_back("An Epic installation is incomplete, unsupported or ambiguous; it was not imported.");
          continue;
        }
        result.games.push_back({i, iid, directory.filename().string(), executable.string(), value(row, "version"), directory, root, platform});
      }
      result.available = true;
    } catch (const std::exception &) {
      result.games.clear();
      result.warnings = {"Epic installed-state metadata is unavailable or changed. Open the launcher and try again."};
    }
    return result;
  }

  snapshot_t read_installations(const std::filesystem::path &root) {
    if (auto raw = bounded_read(root / "installations.jsonl", max_bytes)) return parse_installations(*raw, root);
    return {false, {}, {"Epic installed-state metadata is unavailable. Open the desktop launcher first."}};
  }

  bool matches_heroic_catalog(const identity_t &i, std::string_view bytes, std::string *name) {
    try {
      if (!valid_identity(i) || bytes.empty() || bytes.size() > max_catalog) return false;
      const auto j = strict_json(bytes);
      if (!j.is_object() || value(j, "app_name") != i.artifact_id || !j.contains("metadata") || !j["metadata"].is_object()) return false;
      const auto &metadata = j["metadata"];
      if (value(metadata, "namespace") != i.namespace_id || value(metadata, "id") != i.item_id) return false;
      if (name && text(value(metadata, "title"))) *name = value(metadata, "title");
      return true;
    } catch (const std::exception &) { return false; }
  }

  std::optional<identity_t> heroic_identity(const std::string &artifact, const std::vector<std::filesystem::path> &homes, std::string *name) {
    if (!id(artifact)) return {};
    std::optional<identity_t> found;
    std::string found_name;
    for (const auto &home : homes) for (const auto &config : {home / ".config/heroic", home / ".var/app/com.heroicgameslauncher.hgl/config/heroic"}) {
      const auto raw = bounded_read(config / "legendaryConfig/legendary/metadata" / (artifact + ".json"), max_catalog);
      if (!raw) continue;
      try {
        const auto j = strict_json(*raw);
        if (!j.is_object() || !j.contains("metadata") || !j["metadata"].is_object()) continue;
        const identity_t i {value(j["metadata"], "namespace"), value(j["metadata"], "id"), value(j, "app_name")};
        std::string title;
        if (i.artifact_id != artifact || !matches_heroic_catalog(i, *raw, &title)) continue;
        if (found && *found != i) return {};
        found = i;
        found_name = title;
      } catch (const std::exception &) { return {}; }
    }
    if (found && name) *name = found_name;
    return found;
  }

  bool verify_flatpak_deployment(const flatpak_deployment_t &retained) {
    try { return deployment(retained.root, retained.app_home, retained.scope) == retained; }
    catch (const std::exception &) { return false; }
  }

  std::optional<std::string> flatpak_instance_id(std::string_view raw, const flatpak_deployment_t &d) {
    try {
      auto info = flatpak_ini(raw);
      const auto instance = info["Instance/instance-id"];
      if (!component(instance) || info["Application/name"] != flatpak_app || info["Instance/arch"] != d.arch ||
          info["Instance/branch"] != d.branch || info["Instance/app-commit"] != d.commit ||
          info["Instance/app-path"] != (d.directory / "files").string() ||
          info["Instance/instance-path"] != d.app_home.string() || info.contains("Instance/original-app-path")) return {};
      for (const auto *flag : {"Instance/build", "Instance/devel", "Instance/sandbox"})
        if (info.contains(flag) && info[flag] != "false") return {};
      return instance;
    } catch (const std::exception &) { return {}; }
  }

  snapshot_t scan(const std::vector<std::filesystem::path> &homes) { return scan(homes, "/var/lib/flatpak"); }

  snapshot_t scan(const std::vector<std::filesystem::path> &homes, const std::filesystem::path &system_root) {
    snapshot_t out;
    bool native_metadata = false;
    std::set<std::string> identities;
    const auto append = [&](snapshot_t s) {
      if (!s.available) { out = std::move(s); return false; }
      out.available = true;
      out.warnings.insert(out.warnings.end(), s.warnings.begin(), s.warnings.end());
      for (auto &game : s.games) {
        if (!identities.insert(key(game.identity)).second) {
          out = {false, {}, {"More than one Epic data root claims this title. Choose one launcher installation first."}};
          return false;
        }
        std::string name;
        const auto catalog = heroic_identity(game.identity.artifact_id, homes, &name);
        if (catalog && *catalog == game.identity && !name.empty()) game.name = name;
        out.games.push_back(std::move(game));
      }
      return true;
    };
    try {
      for (const auto &home : homes) {
        const auto root = home / ".local/share/com.epicgames.eglv2";
        if (std::filesystem::exists(root)) {
          native_metadata = true;
          out.flatpak_only = false;
          if (!append(read_installations(root))) return out;
        }
        const auto app_home = home / ".var/app" / flatpak_app;
        const auto data = app_home / "data/com.epicgames.eglv2";
        if (!std::filesystem::exists(data)) continue;
        std::vector<flatpak_deployment_t> deployments;
        for (const auto &[location, scope] : std::vector<std::pair<std::filesystem::path, std::string>> {
            {home / ".local/share/flatpak", "user"}, {system_root, "system"}})
          if (std::filesystem::exists(location / "app" / flatpak_app)) deployments.push_back(deployment(location, app_home, scope));
        if (deployments.size() != 1 || std::filesystem::canonical(app_home) != app_home || std::filesystem::canonical(data) != data)
          throw std::runtime_error("missing, redirected or ambiguous Flatpak installation");
        auto s = read_installations(data);
        if (!s.available) return s;
        s.games.erase(std::remove_if(s.games.begin(), s.games.end(), [&](auto &game) {
          // Only an identical app-private host path is qualified. No /app, portal,
          // home-directory or mount/drive mapping is guessed from a sandbox path.
          std::error_code ec;
          if (!beneath(game.install_dir, app_home) || std::filesystem::canonical(game.install_dir, ec) != game.install_dir || ec) {
            s.warnings.push_back("An Epic Flatpak game path has no verified app-private host mapping; it was not imported.");
            return true;
          }
          game.flatpak = deployments.front();
          return false;
        }), s.games.end());
        if (!verify_flatpak_deployment(deployments.front())) throw std::runtime_error("changed Flatpak deployment");
        s.warnings.push_back("Epic Flatpak supports Mirror Desktop only. End ends the stream and leaves the desktop game running.");
        if (!append(std::move(s))) return out;
        out.flatpak_only = !native_metadata;
      }
    } catch (const std::exception &) {
      return {false, {}, {"Epic Flatpak deployment or metadata is missing, unreadable, changed or ambiguous. Finish its Flatpak update and keep only one installation before rescanning."}};
    }
    return out;
  }

  bool same_snapshot(const snapshot_t &a, const snapshot_t &b) {
    if (!a.available || !b.available || a.games.size() != b.games.size() || a.warnings != b.warnings || a.flatpak_only != b.flatpak_only) return false;
    auto left = a.games, right = b.games;
    const auto order = [](const installation_t &x, const installation_t &y) { return key(x.identity) < key(y.identity); };
    std::sort(left.begin(), left.end(), order);
    std::sort(right.begin(), right.end(), order);
    return left == right;
  }

  std::optional<installation_t> find(const snapshot_t &s, const identity_t &i) {
    if (!s.available || !valid_identity(i)) return {};
    const auto it = std::find_if(s.games.begin(), s.games.end(), [&](const auto &g) { return g.identity == i; });
    return it == s.games.end() ? std::nullopt : std::optional<installation_t>(*it);
  }

  std::string launch_uri(const identity_t &i) {
    if (!valid_identity(i)) throw std::invalid_argument("Invalid Epic title identity");
    return "com.epicgames.launcher://apps/" + i.namespace_id + "%3A" + i.item_id + "%3A" + i.artifact_id + "?action=launch&silent=true";
  }
  std::string launch_command(const std::filesystem::path &launcher, const identity_t &i) {
    if (!valid_absolute_path(launcher)) throw std::invalid_argument("Invalid Epic launcher path");
    return quote(launcher.string()) + ' ' + quote(launch_uri(i));
  }
  std::string flatpak_launch_command(const flatpak_deployment_t &d, const identity_t &i) {
    const auto home = d.app_home.parent_path().parent_path().parent_path();
    if ((d.scope != "user" && d.scope != "system") || d.installation != (d.scope == "system" ? "default" : "") ||
        d.arch != "x86_64" || !component(d.branch) || !commit_id(d.commit) || !valid_absolute_path(d.root) ||
        !valid_absolute_path(home) || d.app_home != home / ".var/app" / flatpak_app ||
        d.directory != d.root / "app" / flatpak_app / d.arch / d.branch / d.commit)
      throw std::invalid_argument("Invalid Epic Flatpak deployment");
    // Bind this child's app-private home and installation lookup even if the service
    // inherited another directory. This changes no host setting or sandbox permission.
    return "env HOME=" + quote(home.string()) + ' ' + std::string(d.scope == "user" ? "FLATPAK_USER_DIR=" : "FLATPAK_SYSTEM_DIR=") + quote(d.root.string()) +
      " flatpak run " + (d.scope == "user" ? "--user" : "--installation=default") + " --arch=x86_64 " +
      quote("--branch=" + d.branch) + ' ' + quote("--commit=" + d.commit) + ' ' + quote(flatpak_app) + ' ' + quote(launch_uri(i));
  }
  std::string appimage_stage_command(const std::filesystem::path &image, const std::filesystem::path &install_dir,
    const identity_t &i, const std::function<std::string(const std::string &)> &isolate) {
    if (!valid_absolute_path(image) || !valid_absolute_path(install_dir)) throw std::invalid_argument("Invalid Epic AppImage or installation path");
    const auto runtime = quote(image.string());
    const auto to_install = "cd -- " + quote(install_dir.string());
    // Fd 9: 3, 4 and 5 are the startup diagnostics and the identity channel. Ten seconds, since the
    // game's birth window is 30 from before this starts. The runtime exports these same variables
    // when it runs AppRun itself, which it does not get to do here.
    return to_install + " || exit 127\n"
      "exec 9< <(exec " + runtime + " --appimage-mount)\n"
      "holder=$!\n"
      "if ! IFS= read -r -t 10 APPDIR <&9 || [ ! -x \"$APPDIR/AppRun\" ]; then\n"
      "  echo 'polaris: the Epic AppImage did not mount' >&2\n"
      "  kill \"$holder\" 2>/dev/null\n"
      "  exit 127\n"
      "fi\n"
      "exec 9<&-\n"
      "export APPDIR APPIMAGE=" + runtime + " ARGV0=" + runtime + " OWD=\"$PWD\"\n" +
      isolate(to_install + " && exec \"$APPDIR/AppRun\" " + quote(launch_uri(i))) + "\n"
      "status=$?\n"
      "echo \"polaris: Epic launcher returned status=$status\" >&2\n"
      "wait \"$holder\"\n"
      "exit \"$status\"\n";
  }
}
