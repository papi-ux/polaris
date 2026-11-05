#include "epic_library.h"

#include <algorithm>
#include <map>
#include <set>
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
    std::optional<std::string> bounded_read(const std::filesystem::path &path, std::size_t limit) {
      // Only the named nonsecret metadata file is read. Never use .prev, account caches or a retry.
      fd_t root {::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
      struct stat root_before {}, root_after {}, named_root {}, before {}, after {}, named {};
      if (root.fd < 0 || ::fstat(root.fd, &root_before) || root_before.st_uid != ::getuid()) return {};
      fd_t file {::openat(root.fd, path.filename().c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
      if (file.fd < 0 || ::fstat(file.fd, &before) || !S_ISREG(before.st_mode) ||
          before.st_uid != ::getuid() || before.st_size <= 0 || static_cast<uint64_t>(before.st_size) > limit) return {};
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
#else
    std::optional<std::string> bounded_read(const std::filesystem::path &, std::size_t) { return {}; }
#endif
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

  snapshot_t scan(const std::vector<std::filesystem::path> &homes) {
    snapshot_t out;
    std::set<std::string> identities;
    for (const auto &home : homes) {
      const auto root = home / ".local/share/com.epicgames.eglv2";
      std::error_code ec;
      if (!std::filesystem::exists(root, ec)) continue;
      auto s = read_installations(root);
      if (!s.available) return s; // Do not silently trust a different root when one is unreadable.
      out.available = true;
      out.warnings.insert(out.warnings.end(), s.warnings.begin(), s.warnings.end());
      for (auto &game : s.games) {
        if (!identities.insert(key(game.identity)).second) return {false, {}, {"More than one Epic data root claims this title. Choose one launcher installation first."}};
        std::string name;
        const auto catalog = heroic_identity(game.identity.artifact_id, homes, &name);
        if (catalog && *catalog == game.identity && !name.empty()) game.name = name;
        out.games.push_back(std::move(game));
      }
    }
    return out;
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
