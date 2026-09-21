#include "spaces_library.h"
#ifdef __linux__
#include "multiseat_profile_network.h"
#include "src/utility.h"
#include "src/uuid.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <set>
#include <map>
#include <mutex>

namespace multiseat::spaces {
  namespace {
    bool token(std::string_view value) {
      return !value.empty() && value.size() <= 128 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string_view::npos;
    }
    constexpr std::size_t bound = 2 * 1024 * 1024;
  }
  library_reader_t make_library_reader(std::vector<container::profile_t> profiles,
    std::function<std::unique_ptr<container::host_t>()> factory) {
    struct cache_t {
      std::mutex mutex;
      std::map<std::string, std::pair<std::chrono::steady_clock::time_point, library_t>> entries;
    };
    auto cache = std::make_shared<cache_t>();
    return [profiles = std::move(profiles), factory = std::move(factory), cache](std::string_view id) -> library_t {
      const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const auto &p) { return p.profile_key == id; });
      if (profile == profiles.end() || !has_library(profile->runtime_profile)) return {};
      std::lock_guard lock(cache->mutex);
      const auto now = std::chrono::steady_clock::now();
      auto &entry = cache->entries[std::string(id)];
      if (now - entry.first < std::chrono::seconds(15)) return entry.second;
      auto host = factory ? factory() : nullptr;
      auto result = host ? read_profile_library(*host, *profile) : library_t{};
      entry = {std::chrono::steady_clock::now(), std::move(result)};
      return entry.second;
    };
  }
  std::string game_identity(std::string_view profile, std::string_view target) {
    return token(profile) && container::any_launcher_target(target) ?
      "space." + std::string(profile) + "." + std::string(target) : "";
  }
  std::optional<game_identity_t> parse_game_identity(std::string_view identity) {
    if (!identity.starts_with("space.")) return std::nullopt;
    identity.remove_prefix(6);
    const auto split = identity.find('.');
    if (split == std::string_view::npos) return std::nullopt;
    const auto profile = identity.substr(0, split), target = identity.substr(split + 1);
    if (!token(profile) || !container::any_launcher_target(target)) return std::nullopt;
    return game_identity_t{std::string(profile), std::string(target)};
  }
  std::optional<library_t> decode_library(std::string_view payload, runtime_profile_e family) {
    try {
      if (payload.size() > bound) return std::nullopt;
      std::vector<std::set<std::string>> keys;
      const auto value = nlohmann::json::parse(payload, [&](int depth, auto event, auto &item) {
        using event_t = nlohmann::json::parse_event_t;
        if (depth > 4) throw std::invalid_argument("library nesting");
        if (event == event_t::object_start) keys.emplace_back();
        if (event == event_t::key && !keys.back().insert(item.template get<std::string>()).second)
          throw std::invalid_argument("duplicate library field");
        if (event == event_t::object_end) keys.pop_back();
        return true;
      });
      if (!value.is_object() || value.size() != 2 || !value.at("schema").is_number_unsigned() || value.at("schema") != 1 ||
          !value.at("games").is_array() || value.at("games").size() > 4096) return std::nullopt;
      library_t result{true, {}};
      std::set<std::string> ids;
      for (const auto &game : value.at("games")) {
        if (!game.is_object() || game.size() != 2) return std::nullopt;
        const auto target = game.at("target").get<std::string>(), name = game.at("name").get<std::string>();
        // A title in this family's own grammar. The tile that opens the
        // launcher is offered by the host, never listed by a scanner.
        if (!container::valid_launcher_target(family, target) || target == container::launcher_sentinel(family) ||
            name.empty() || name.size() > 512 ||
            std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
            !ids.insert(target).second) return std::nullopt;
        result.games.push_back({target, name});
      }
      return result;
    } catch (...) { return std::nullopt; }
  }

  std::string_view steam_library_scanner() {
    return R"PY(import os, re, json, stat, signal
# This runs as PID 1 of its helper container, where a signal left at its default
# disposition is dropped, so an alarm with no handler never fires at all.
signal.signal(signal.SIGALRM, lambda *_: os._exit(3))
signal.alarm(8)
root = os.open('/profile', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
def directory(parts):
    fd = os.dup(root)
    try:
        for part in parts:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise
def read_file(fd, name):
    f = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
    try:
        s = os.fstat(f)
        if not stat.S_ISREG(s.st_mode) or s.st_size > 262144: raise ValueError('manifest size')
        with os.fdopen(os.dup(f), 'rb') as stream: data = stream.read(262145)
        if len(data) > 262144: raise ValueError('manifest grew')
        return data.decode('utf-8')
    finally: os.close(f)
def app_state(text):
    # Bounded Valve KeyValues, including escaped quotes and nested objects.
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|[{}]|//[^\n]*|[^\s{}"]+', text)
    tokens = [t for t in tokens if not t.startswith('//')]
    def string(t):
        if not t.startswith('"') or not t.endswith('"'): raise ValueError('token')
        return re.sub(r'\\([\\"])', r'\1', t[1:-1])
    i = 0
    def obj(depth):
        nonlocal i
        if depth > 8: raise ValueError('depth')
        result = {}
        while i < len(tokens) and tokens[i] != '}':
            key = string(tokens[i]).lower(); i += 1
            if key in result or i >= len(tokens): raise ValueError('duplicate or missing field')
            if tokens[i] == '{':
                i += 1; value = obj(depth + 1)
            else:
                value = string(tokens[i]); i += 1
            result[key] = value
        if depth:
            if i >= len(tokens) or tokens[i] != '}': raise ValueError('unclosed object')
            i += 1
        return result
    parsed = obj(0)
    if i != len(tokens) or set(parsed) != {'appstate'}: raise ValueError('root')
    return parsed['appstate']
class CatalogError(Exception): pass
games = {}
# Standard Steam roots within this Space. Never follow libraryfolders paths
# or symlinks to another home, host mount, or account configuration.
for parts in [('.local','share','Steam','steamapps'), ('.steam','steam','steamapps'), ('.steam','debian-installation','steamapps'), ('Steam','steamapps')]:
    try: fd = directory(parts)
    except (FileNotFoundError, NotADirectoryError, OSError): continue
    try:
        names = os.listdir(fd)
        if len(names) > 16384: raise ValueError('directory size')
        for name in sorted(names):
            match = re.fullmatch(r'appmanifest_([1-9][0-9]{0,9})\.acf', name)
            if not match: continue
            try:
                state = app_state(read_file(fd, name))
                appid, title = state.get('appid'), state.get('name')
                if appid != match[1] or int(appid) > 4294967295: continue
                if not isinstance(title, str) or not title or len(title.encode()) > 512: continue
                if any(ord(c) < 32 or ord(c) == 127 for c in title): continue
                if not int(state.get('stateflags', '0')) & 4: continue
                install = state.get('installdir', '')
                if not install or install in ('.','..') or '/' in install or '\\' in install: continue
                installed = directory(parts + ('common', install)); os.close(installed)
                # Steam compatibility tools are installation dependencies, not titles.
                if title.startswith(('Proton ', 'Steam Linux Runtime', 'Steamworks Common Redistributables')): continue
                if appid in games and games[appid] != title: raise CatalogError('conflicting manifest')
                games[appid] = title
                if len(games) > 4096: raise CatalogError('game count')
            except (OSError, ValueError, TypeError, KeyError): continue
    finally: os.close(fd)
print(json.dumps({'schema': 1, 'games': [{'target': k, 'name': v} for k,v in sorted(games.items(), key=lambda kv: kv[1].casefold())]}))
)PY";
  }

  std::string_view heroic_library_scanner() {
    return R"PY(import os, re, json, stat, signal
# This runs as PID 1 of its helper container, where a signal left at its default
# disposition is dropped, so an alarm with no handler never fires at all.
signal.signal(signal.SIGALRM, lambda *_: os._exit(3))
signal.alarm(8)
root = os.open('/profile', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
def directory(parts):
    fd = os.dup(root)
    try:
        for part in parts:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise
def read_json(fd, name):
    f = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
    try:
        s = os.fstat(f)
        if not stat.S_ISREG(s.st_mode) or s.st_size > 4194304: raise ValueError('store size')
        with os.fdopen(os.dup(f), 'rb') as stream: data = stream.read(4194305)
        if len(data) > 4194304: raise ValueError('store grew')
        return json.loads(data.decode('utf-8'))
    finally: os.close(f)
name_pattern = re.compile(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}')
games = {}
def remember(runner, app, title):
    if not isinstance(app, str) or not name_pattern.fullmatch(app): return
    if not isinstance(title, str) or not title or len(title.encode()) > 512: return
    if any(ord(c) < 32 or ord(c) == 127 for c in title): return
    games.setdefault(runner + '.' + app, title)
# Heroic keeps one installed store per runner under its own configuration.
# Read only those files: no path from them is followed, and an entry that is
# not a plain identifier and a plain title is skipped rather than repaired.
for parts, runner, key in [
        (('.config','heroic','legendaryConfig','legendary'), 'epic', 'app_name'),
        (('.config','heroic','gog_store'), 'gog', 'appName'),
        (('.config','heroic','nile_config','nile'), 'amazon', 'app_name')]:
    try: fd = directory(parts)
    except (FileNotFoundError, NotADirectoryError, OSError): continue
    try:
        document = read_json(fd, 'installed.json')
    except (OSError, ValueError, TypeError): 
        os.close(fd); continue
    os.close(fd)
    entries = document.get('installed', document) if isinstance(document, dict) else document
    if isinstance(entries, dict): entries = list(entries.values())
    if not isinstance(entries, list): continue
    if len(entries) > 4096: continue
    for entry in entries:
        if not isinstance(entry, dict): continue
        remember(runner, entry.get(key), entry.get('title') or entry.get('name') or entry.get(key))
        if len(games) > 4096: break
try:
    fd = directory(('.config','heroic','sideload_apps'))
    try: document = read_json(fd, 'library.json')
    finally: os.close(fd)
    entries = document.get('games', []) if isinstance(document, dict) else []
    if isinstance(entries, list) and len(entries) <= 4096:
        for entry in entries:
            if isinstance(entry, dict): remember('sideload', entry.get('app_name'), entry.get('title'))
except (FileNotFoundError, NotADirectoryError, OSError, ValueError, TypeError): pass
print(json.dumps({'schema': 1, 'games': [{'target': k, 'name': v} for k,v in sorted(games.items(), key=lambda kv: kv[1].casefold())]}))
)PY";
  }

  std::string_view lutris_library_scanner() {
    return R"PY(import os, json, stat, signal, sqlite3, time
# This runs as PID 1 of its helper container, where a signal left at its default
# disposition is dropped, so an alarm with no handler never fires at all.
signal.signal(signal.SIGALRM, lambda *_: os._exit(3))
signal.alarm(8)
root = os.open('/profile', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
def directory(parts):
    fd = os.dup(root)
    try:
        for part in parts:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise
# Lutris keeps its whole library in one SQLite database. SQLite opens a file by
# its name and wants a journal beside it, while this mount is read-only and
# nothing here trusts a name. So the file is read through a descriptor that
# followed no link, and the database is opened from that copy in memory. Asking
# Lutris itself would start the launcher inside a helper with 16 processes.
limit = 33554432
games = {}
try:
    fd = directory(('.local','share','lutris'))
    try: f = os.open('pga.db', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
    finally: os.close(fd)
    try:
        s = os.fstat(f)
        if not stat.S_ISREG(s.st_mode) or s.st_size > limit: raise ValueError('database size')
        with os.fdopen(os.dup(f), 'rb') as stream: data = stream.read(limit + 1)
        if len(data) > limit: raise ValueError('database grew')
    finally: os.close(f)
    # A database left in write-ahead mode will not open from memory. Its main
    # file is the last checkpointed state, which is all a reader that cannot see
    # the log would be given anyway, so it is read as a rollback database.
    if data[:16] == b'SQLite format 3\x00' and data[18:20] == b'\x02\x02':
        data = data[:18] + b'\x01\x01' + data[20:]
    connection = sqlite3.connect(':memory:')
    connection.deserialize(data)
    del data
    # The home is the player's and so is this database. SQLite runs the query
    # in C, where the alarm's handler cannot interrupt it, so the query carries
    # a deadline of its own. Text comes back as bytes: one title that is not
    # UTF-8 must cost that title, not the library.
    deadline = time.monotonic() + 4
    connection.set_progress_handler(lambda: time.monotonic() > deadline, 20000)
    connection.text_factory = bytes
    # A view called games can be any query at all, an endless one included.
    # Only plain tables are read.
    kinds = dict(connection.execute(
        "SELECT name, type FROM sqlite_master WHERE name IN ('games','categories','games_categories')").fetchall())
    if kinds.get(b'games') != b'table': raise ValueError('schema')
    categories = [kinds.get(b'categories'), kinds.get(b'games_categories')]
    if any(kind not in (None, b'table') for kind in categories): raise ValueError('schema')
    # Installed, and not in the category Lutris keeps its hidden games in. NOT
    # EXISTS rather than NOT IN: one NULL game_id would make NOT IN hide them all.
    hidden = (" AND NOT EXISTS (SELECT 1 FROM games_categories g JOIN categories c ON c.id = g.category_id "
              "WHERE g.game_id = games.id AND c.name = '.hidden')") if all(categories) else ""
    rows = connection.execute("SELECT id, name FROM games WHERE installed = 1" + hidden + " LIMIT 4097").fetchall()
    if len(rows) <= 4096:
        for number, title in rows:
            if type(number) is not int or not 0 < number <= 4294967295: continue
            if not isinstance(title, bytes) or not title or len(title) > 512: continue
            try: title = title.decode('utf-8')
            except UnicodeDecodeError: continue
            if any(ord(c) < 32 or ord(c) == 127 for c in title): continue
            games['id.' + str(number)] = title
# Whatever this database does, the answer is a library, empty if need be. An
# empty file alone raises MemoryError from deserialize.
except Exception: pass
print(json.dumps({'schema': 1, 'games': [{'target': k, 'name': v} for k,v in sorted(games.items(), key=lambda kv: kv[1].casefold())]}))
)PY";
  }

  namespace {
    /** What a family's own image labels itself. */
    std::string_view image_family(runtime_profile_e profile) {
      switch (profile) {
        case runtime_profile_e::steam: return "steam";
        case runtime_profile_e::heroic: return "heroic";
        case runtime_profile_e::lutris: return "lutris";
        default: return {};
      }
    }
  }  // namespace

  bool has_library(runtime_profile_e profile) {
    return !library_scanner(profile).empty();
  }

  std::string_view library_scanner(runtime_profile_e profile) {
    switch (profile) {
      case runtime_profile_e::steam: return steam_library_scanner();
      case runtime_profile_e::heroic: return heroic_library_scanner();
      case runtime_profile_e::lutris: return lutris_library_scanner();
      default: return {};
    }
  }

  library_t read_steam_library(container::host_t &host, const container::profile_t &profile) {
    return read_profile_library(host, profile);
  }

  library_t read_profile_library(container::host_t &host, const container::profile_t &profile) {
    try {
      if (!has_library(profile.runtime_profile) || !token(profile.profile_key) ||
          !token(profile.opaque_volume_name) || !profile.opaque_volume_name.starts_with("pv-") ||
          profile.image_reference.size() != 71 || !profile.image_reference.starts_with("sha256:") ||
          profile.image_reference.substr(7).find_first_not_of("0123456789abcdef") != std::string::npos ||
          host.effective_uid() != 1000 || host.effective_gid() != 1000 ||
          !host.trusted_runtime_file("/usr/bin/docker") || !host.trusted_runtime_file("/usr/bin/runc")) return {};
      auto run = [&](std::vector<std::string> args) {
        auto argv = container::command_prefix({});
        argv.insert(argv.end(), args.begin(), args.end());
        const auto result = host.run(argv, std::chrono::seconds(15), bound);
        if (result.exit_status || result.timed_out || result.output_truncated) throw std::runtime_error("library reader unavailable");
        return result.output;
      };
      const auto info = nlohmann::json::parse(run({"info", "--format={{json .}}"}));
      if (info.at("OSType") != "linux" || !info.at("SecurityOptions").is_array() ||
          (info.at("Runtimes").at("runc").at("path") != "runc" &&
           info.at("Runtimes").at("runc").at("path") != "/usr/bin/runc")) return {};
      for (const auto &option : info.at("SecurityOptions"))
        if (!option.is_string() || option.get<std::string>().starts_with("name=rootless")) return {};
      const auto volumes = nlohmann::json::parse(run({"volume", "inspect", profile.opaque_volume_name}));
      if (!volumes.is_array() || volumes.size() != 1) return {};
      const auto &volume = volumes[0];
      if (volume.at("Name") != profile.opaque_volume_name || volume.at("Driver") != "local" ||
          volume.at("Scope") != "local" || !volume.at("Options").empty() ||
          volume.at("Labels").at("io.polaris.multiseat.profile") != profile.profile_key) return {};
      const auto images = nlohmann::json::parse(run({"image", "inspect", profile.image_reference}));
      if (!images.is_array() || images.size() != 1 || images[0].at("Id") != profile.image_reference ||
          images[0].at("Os") != "linux" ||
          images[0].at("Config").at("Labels").at("io.polaris.multiseat.profile") !=
            image_family(profile.runtime_profile) ||
          (images[0].at("Config").contains("Volumes") && !images[0].at("Config").at("Volumes").empty())) return {};
      const auto scanned = run({"run", "--rm", "--pull=never", "--runtime=runc", "--network=none",
        "--name=polaris-library-" + uuid_util::uuid_t::generate().string(),
        "--label=io.polaris.spaces.library=" + profile.profile_key,
        "--userns=host", "--read-only", "--user=1000:1000", "--cap-drop=ALL",
        "--security-opt=no-new-privileges", "--pids-limit=16", "--memory=128m", "--cpus=0.5", "--no-healthcheck",
        "--mount=type=volume,src=" + profile.opaque_volume_name + ",dst=/profile,readonly,volume-nocopy",
        "--entrypoint=/usr/bin/python3", profile.image_reference, "-I", "-c",
        std::string(library_scanner(profile.runtime_profile))});
      return decode_library(scanned, profile.runtime_profile).value_or(library_t{});
    } catch (...) { return {}; }
  }
}
#endif
