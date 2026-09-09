#include "configuration_store.h"
#include "config_file_update.h"
#include "crypto.h"
#include "private_state_file.h"
#include "utility.h"
#include "logging.h"
#include <unordered_map>

namespace configuration_store {
  namespace {
    std::recursive_mutex lock;
    std::unordered_map<std::string, std::string> revisions;
    std::string digest(const std::string &contents) {
      return util::hex(crypto::hash(contents)).to_string();
    }
  }
  std::recursive_mutex &mutex() { return lock; }
  std::optional<snapshot_t> read(const std::string &path) {
    std::lock_guard guard(lock);
    const auto value = private_state_file::read_secure(path, 4 * 1024 * 1024, true, false);
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
    std::string next_revision;
    const auto written = private_state_file::update_atomic(path, 4 * 1024 * 1024,
      [&](const private_state_file::read_result_t &read) -> std::optional<std::string> {
        const auto before = revisions[path] = read ? digest(read.payload) : std::string {};
        if (expected && (expected->empty() || *expected != before)) {
          conflict = true;
          return std::nullopt;
        }
        if (require_existing && !read) return std::nullopt;
        auto next = transform(read.payload);
        next_revision = digest(next);
        return next;
      }, true);
    if (conflict) return result::conflict;
    if (written.status == private_state_file::write_status_e::not_committed) return result::failed;
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
