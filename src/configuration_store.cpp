#include "configuration_store.h"
#include "config_file_update.h"
#include "crypto.h"
#include "private_state_file.h"
#include "utility.h"
#include "logging.h"
#include <cstring>
#include <filesystem>
#include <format>
#include <unordered_map>
#include <unistd.h>

namespace configuration_store {
  namespace {
    constexpr std::size_t max_bytes = 4 * 1024 * 1024;
    std::recursive_mutex lock;
    std::unordered_map<std::string, std::string> revisions;
    // The last refusal logged for each path, so the log gets one line per change
    // rather than one per request.
    std::unordered_map<std::string, private_state_file::refusal_e> refusal_status;
    // The last refusal in full, for last_refusal(). Its own lock is a leaf: no
    // other lock is taken while it is held, so a reader outside the store's lock
    // order cannot deadlock against it.
    std::mutex published_lock;
    std::unordered_map<std::string, refusal_t> published;
    std::string digest(const std::string &contents) {
      return util::hex(crypto::hash(contents)).to_string();
    }

    // Quote a path for a command someone will paste, and only when it needs it.
    std::string shell_word(const std::string &value) {
      const bool plain = !value.empty() && value.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_./+:@%,=-") == std::string::npos;
      if (plain) return value;
      std::string quoted = "'";
      for (const char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
      }
      return quoted + "'";
    }

    std::string errno_suffix(int error_number) {
      return error_number == 0 ? std::string {} : std::string {": "} + std::strerror(error_number);
    }

    std::string mode_text(unsigned mode) {
      return std::format("{:04o}", mode & 07777U);
    }

    // Commands run in the operator's shell, not in the folder Polaris runs in,
    // so a relative path is spelled out from where Polaris resolved it.
    std::filesystem::path spelled_out(const std::filesystem::path &value) {
      if (value.empty() || value.is_absolute()) return value;
      std::error_code error;
      auto resolved = std::filesystem::absolute(value, error);
      if (error) return value;
      resolved = resolved.lexically_normal();
      // "." normalizes with a trailing slash, which reads as a typo.
      if (!resolved.has_filename() && resolved.has_relative_path()) resolved = resolved.parent_path();
      return resolved;
    }

    void note_refusal(const refusal_t &refusal) {
      {
        std::lock_guard guard(published_lock);
        if (refusal) published[refusal.path] = refusal;
        else published.erase(refusal.path);
      }
      auto &last = refusal_status[refusal.path];
      if (last == refusal.kind) return;
      last = refusal.kind;
      if (!refusal) {
        BOOST_LOG(info) << "The settings file [" << refusal.path << "] can be read again.";
        return;
      }
      BOOST_LOG(warning) << "Refusing to use the settings file [" << refusal.path << "]: "
                         << refusal.reason << ' ' << refusal.fix;
    }
  }
  std::recursive_mutex &mutex() { return lock; }

  std::optional<refusal_t> last_refusal(const std::string &path) {
    std::lock_guard guard(published_lock);
    const auto found = published.find(path);
    if (found == published.end()) return std::nullopt;
    return found->second;
  }

  refusal_t describe_refusal(const std::string &path, const private_state_file::refusal_t &refusal) {
    using enum private_state_file::refusal_e;
    refusal_t described {.kind = refusal.kind, .path = path};
    const auto target = spelled_out(path).string();
    const auto file = shell_word(target);
    const auto lock_file = shell_word(target + ".lock");
    const auto directory_path = spelled_out(refusal.directory);
    const auto directory = directory_path.string();
    const auto folder = shell_word(directory);
    // The walk refuses folders above the file as well. Calling one of those the
    // folder that holds it sends people to the wrong folder.
    const auto folder_named = std::string {refusal.holds_file ? "the folder that holds it, " : "a folder on the way to it, "} +
                              directory;
    const auto where = std::string {refusal.holds_file ? "The folder that holds it, " : "A folder on the way to it, "} +
                       directory;
    // A file directly under / is refused because root owns /. Handing / to the
    // user or taking its permissions away would break the machine, so the fix
    // is to keep the file somewhere else.
    const bool filesystem_root = directory_path.is_absolute() && !directory_path.has_relative_path();
    const auto elsewhere = "Keep the settings file in a folder your user owns, such as ~/.config/polaris, "
                           "and start Polaris with that path instead of " + target + ".";
    const auto take_back = [](const std::string &word) {
      return "Take it back with \"sudo chown \"$USER\": " + word +
             "\" and start Polaris without sudo; one \"sudo polaris\" is enough to leave it owned by root.";
    };
    switch (refusal.kind) {
      case none:
        described.reason = "Polaris could not read it.";
        described.fix = "Check the Polaris log for the cause, then try again.";
        break;
      case missing:
        described.reason = "The settings file does not exist.";
        described.fix = "Restart Polaris to write a new one with the defaults, or put your copy back at " + target + ".";
        break;
      case directory_missing:
        described.reason = where + ", does not exist.";
        described.fix = "Restart Polaris to create it with a new settings file, or put your copy back at " + target + ".";
        break;
      case directory_foreign_owner:
        described.reason = where + ", is owned by " +
                           (refusal.inspected ? "uid " + std::to_string(refusal.owner) + ", and Polaris runs as uid " +
                                                  std::to_string(::geteuid())
                                              : std::string {"another user"}) +
                           ", so Polaris will not keep private state there.";
        described.fix = filesystem_root ? elsewhere :
                                          "Take it back with \"sudo chown -R \"$USER\": " + folder +
                                            "\" and start Polaris without sudo; one \"sudo polaris\" is enough to leave it owned by root.";
        break;
      case directory_writable:
        described.reason = where + ", is writable by group or other users" +
                           (refusal.inspected ? " (mode " + mode_text(refusal.mode) + ")" : std::string {}) +
                           ", so Polaris will not keep private state there.";
        described.fix = filesystem_root ? elsewhere :
                                          "Restrict it with \"chmod 700 " + folder + "\"; a umask of 002 is enough to leave it group writable.";
        break;
      case directory_unusable:
        described.reason = "Polaris could not open " + folder_named + errno_suffix(refusal.error_number) + ".";
        described.fix = "Check it with \"ls -ld " + folder + "\": it must be a folder your user owns and can open.";
        break;
      case directory_symlink:
        described.reason = where + ", is a symbolic link, and Polaris only walks real folders to private state.";
        described.fix = "Replace the link with the folder it points to, which \"readlink -f " + folder + "\" prints.";
        break;
      case lock_unavailable:
        described.reason = "Polaris could not open or lock its lock file, " + target + ".lock" +
                           errno_suffix(refusal.error_number) + ".";
        described.fix = "Take it back with \"sudo chown \"$USER\": " + lock_file +
                        "\" if another user owns it, and make sure your user can write to the folder that holds it.";
        break;
      case lock_unsafe:
        described.reason = "Its lock file, " + target + ".lock, is not a regular file that your user owns with a single link.";
        described.fix = "Move it out of the way with \"mv " + lock_file + ' ' + shell_word(target + ".lock.old") +
                        "\"; Polaris creates a new one.";
        break;
      case lock_busy:
        described.reason = "Another process is holding its lock file, " + target + ".lock.";
        described.fix = "Wait a moment and try again. If it stays locked, look for a second Polaris with "
                        "\"pgrep -a polaris\" and stop it.";
        break;
      case permission_denied:
        described.reason = "Your user is not allowed to open it" + errno_suffix(refusal.error_number) + ".";
        described.fix = "Take it back with \"sudo chown \"$USER\": " + file + "\" and \"chmod 600 " + file +
                        "\", then start Polaris without sudo.";
        break;
      case symlink:
        described.reason = "It is a symbolic link, and the settings store only reads a real file.";
        described.fix = "Replace the link with a copy of what it points to: \"cp --remove-destination \"$(readlink -f " +
                        file + ")\" " + file + "\".";
        break;
      case not_regular:
        described.reason = "It is not a regular file.";
        described.fix = "Move it out of the way with \"mv " + file + ' ' + shell_word(target + ".old") +
                        "\" and restart Polaris to write a new settings file.";
        break;
      case foreign_owner:
        described.reason = "It is owned by uid " + std::to_string(refusal.owner) + ", and Polaris runs as uid " +
                           std::to_string(::geteuid()) + ".";
        described.fix = take_back(file);
        break;
      case hard_linked:
        described.reason = "It has " + std::to_string(refusal.links) +
                           " hard links. The settings store only reads a file with one, since any other name for it can change it too.";
        described.fix = "Give it a single link with \"cp -p " + file + ' ' + shell_word(target + ".new") + " && mv " +
                        shell_word(target + ".new") + ' ' + file + "\".";
        break;
      case group_writable:
        described.reason = "It is writable by its group (mode " + mode_text(refusal.mode) +
                           "), and the settings store refuses a file another user can change.";
        described.fix = "Restrict it with \"chmod go-w " + file + "\".";
        break;
      case other_writable:
        described.reason = "It is writable by every user on this machine (mode " + mode_text(refusal.mode) +
                           "), and the settings store refuses a file another user can change.";
        described.fix = "Restrict it with \"chmod go-w " + file + "\".";
        break;
      case not_private:
        described.reason = "Its group or other users can read it (mode " + mode_text(refusal.mode) +
                           "), and this file has to stay private.";
        described.fix = "Restrict it with \"chmod 600 " + file + "\".";
        break;
      case oversize:
        described.reason = "It is " + std::to_string(refusal.size) + " bytes, more than the " +
                           std::to_string(max_bytes / (1024 * 1024)) + " MiB the settings store reads.";
        described.fix = "Move it aside with \"mv " + file + ' ' + shell_word(target + ".old") +
                        "\", restart Polaris to write a new one, then copy back the settings you need.";
        break;
      case size_changed:
        described.reason = "It changed size while Polaris was reading it, so something else is writing to it.";
        described.fix = "Close whatever else is editing or syncing " + target + ", then try again.";
        break;
      case read_failed:
        described.reason = "Reading it failed" + errno_suffix(refusal.error_number) + ".";
        described.fix = "Check the file with \"ls -l " + file + "\" and the disk that holds it, then try again.";
        break;
    }
    return described;
  }

  std::optional<snapshot_t> read(const std::string &path, refusal_t *refusal) {
    std::lock_guard guard(lock);
    // The store explains a refused folder itself, once per change, so the walk stays quiet.
    const auto value = private_state_file::read_secure(path, max_bytes, true, false,
                                                       private_state_file::refusal_log_e::caller);
    auto status = value ? private_state_file::refusal_t {} : value.refusal;
    if (!value && !status) status.kind = private_state_file::refusal_e::read_failed;
    auto described = value ? refusal_t {.path = path} : describe_refusal(path, status);
    note_refusal(described);
    if (refusal) *refusal = std::move(described);
    if (!value) return std::nullopt;
    auto revision = revisions[path] = digest(value.payload);
    return snapshot_t {value.payload, std::move(revision)};
  }
  std::string revision(const std::string &path, bool refresh) {
    std::lock_guard guard(lock);
    if (!refresh && revisions.contains(path)) return revisions.at(path);
    const auto observed = read(path);
    return observed ? observed->revision : std::string {};
  }
  static result update(const std::string &path, const std::optional<std::string> &expected,
                       const std::function<std::string(const std::string &)> &transform,
                       bool require_existing) {
    std::lock_guard guard(lock);
    bool conflict = false;
    // Whether the transaction got as far as the file, and what it found there:
    // nothing wrong, or a missing file, the only refusal it goes on past.
    bool looked = false;
    private_state_file::refusal_t observed;
    std::string next_revision;
    const auto written = private_state_file::update_atomic(path, max_bytes,
      [&](const private_state_file::read_result_t &read) -> std::optional<std::string> {
        looked = true;
        observed = read ? private_state_file::refusal_t {} : read.refusal;
        const auto before = revisions[path] = read ? digest(read.payload) : std::string {};
        if (expected && (expected->empty() || *expected != before)) {
          conflict = true;
          return std::nullopt;
        }
        if (require_existing && !read) return std::nullopt;
        auto next = transform(read.payload);
        next_revision = digest(next);
        return next;
      }, true, private_state_file::refusal_log_e::caller);
    // What the transaction found at the file is its status now, even when the
    // save stopped there. A conflict on a missing file is not a file that reads.
    const auto note_observed = [&] {
      note_refusal(observed ? describe_refusal(path, observed) : refusal_t {.path = path});
    };
    if (conflict) {
      note_observed();
      return result::conflict;
    }
    if (written.status == private_state_file::write_status_e::not_committed) {
      // The same words the read side logs and serves. A failure after the
      // read, such as a short write, leaves the status the read found.
      if (written.refusal) note_refusal(describe_refusal(path, written.refusal));
      else if (looked) note_observed();
      return result::failed;
    }
    note_refusal({.path = path});
    if (written.status == private_state_file::write_status_e::durability_uncertain) {
      BOOST_LOG(warning) << "Configuration committed with uncertain directory durability";
    }
    revisions[path] = next_revision;
    return result::committed;
  }
  result replace(const std::string &path, const std::string &contents,
                 const std::optional<std::string> &expected) {
    return update(path, expected, [&](const auto &) { return contents; }, false);
  }
  result patch(const std::string &path,
               const std::unordered_map<std::string, std::string> &updates,
               const std::optional<std::string> &expected) {
    return update(path, expected, [&](const auto &current) {
      return config_file_update::apply(current, updates).content;
    }, true);
  }
}
