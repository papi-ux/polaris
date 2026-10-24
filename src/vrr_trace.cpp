/** @file src/vrr_trace.cpp
 * Version 2 private host trace writer. All JSON and filesystem work lives off the media threads.
 */
#include "vrr_trace.h"
#include "vrr_source_pacing.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#ifdef __linux__
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <time.h>
  #include <unistd.h>
#endif

namespace vrr_trace {
  using json = nlohmann::ordered_json;
  namespace {
    field_t *append(event_t &event, const char *key, field_t::type_e type) noexcept {
      if (event.size == event.fields.size()) {
        event.overflow = true;
        return nullptr;
      }
      auto &field = event.fields[event.size++];
      field.key = key;
      field.type = type;
      return &field;
    }

    [[maybe_unused]] json encode(const event_t &event) {
      json row {{"type", event.kind}, {"order", event.order}, {"observed_steady_ns", event.observed_steady_ns}};
      for (std::size_t i = 0; i < event.size; ++i) {
        const auto &f = event.fields[i];
        switch (f.type) {
          case field_t::type_e::null: row[f.key] = nullptr; break;
          case field_t::type_e::signed_integer: row[f.key] = f.i; break;
          case field_t::type_e::unsigned_integer: row[f.key] = f.u; break;
          case field_t::type_e::text: row[f.key] = f.text; break;
          case field_t::type_e::real: row[f.key] = f.d; break;
        }
      }
      return row;
    }

#ifdef __linux__
    std::int64_t read_clock(clockid_t clock) noexcept {
      timespec t {};
      if (clock_gettime(clock, &t) || t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1'000'000'000 ||
          t.tv_sec > (INT64_MAX - t.tv_nsec) / 1'000'000'000) return -1;
      return t.tv_sec * 1'000'000'000LL + t.tv_nsec;
    }

    // Bounded /proc reads. These are writer-only; truncation/refusal is explicit, never guessed.
    json read_text(const std::string &path) {
      char bytes[4097];
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
      if (fd < 0) return {{"value", nullptr}, {"errno", errno}};
      const auto count = ::read(fd, bytes, sizeof(bytes));
      const int error = count < 0 ? errno : 0;
      ::close(fd);
      if (count < 0 || count == sizeof(bytes)) return {{"value", nullptr}, {"errno", error}, {"truncated", count == sizeof(bytes)}};
      return {{"value", std::string(bytes, static_cast<std::size_t>(count))}, {"errno", 0}};
    }

    json process_identity(std::int64_t pid) {
      const auto path = pid == 0 ? std::string("/proc/self") : "/proc/" + std::to_string(pid);
      json out {{"pid", pid == 0 ? ::getpid() : pid}};
      // Keep raw stat bytes, not a process-name identity claim. starttime is field 22 after last ')'.
      out["stat_before"] = read_text(path + "/stat");
      for (const auto *pass : {"first", "second"}) {
        const int fd = ::open((path + "/ns/time").c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st {};
        const bool ok = fd >= 0 && ::fstat(fd, &st) == 0;
        const int error = ok ? 0 : errno;
        if (fd >= 0) ::close(fd);
        out[pass] = ok ? json {{"dev", st.st_dev}, {"ino", st.st_ino}} : json {{"unavailable_errno", error}};
        out[std::string("timens_offsets_") + pass] = read_text(path + "/timens_offsets");
      }
      out["stat_after"] = read_text(path + "/stat");
      return out;
    }

    json identity() {
      const auto before = read_clock(CLOCK_MONOTONIC);
      const auto boot = read_clock(CLOCK_BOOTTIME);
      const auto after = read_clock(CLOCK_MONOTONIC);
      return {{"mono_before_ns", before}, {"boottime_ns", boot}, {"mono_after_ns", after},
              {"boot_id", read_text("/proc/sys/kernel/random/boot_id")},
              {"self", process_identity(0)}, {"uid", ::getuid()},
              {"clocksource", read_text("/sys/devices/system/clocksource/clocksource0/current_clocksource")}};
    }

    // Refuse symlinks in every output path component, traversal, existing leaves and nonregular files.
    int exclusive_file(std::string_view path) {
      if (path.empty() || path.front() != '/' || path.size() > 4096) { errno = EINVAL; return -1; }
      int parent = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (parent < 0) return -1;
      std::size_t pos = 1;
      while (pos < path.size()) {
        const auto slash = path.find('/', pos);
        const auto part = path.substr(pos, slash == path.npos ? path.size() - pos : slash - pos);
        if (part.empty() || part == "." || part == "..") { ::close(parent); errno = EINVAL; return -1; }
        const auto name = std::string(part);
        const int flags = slash == path.npos ? O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK :
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        const int next = ::openat(parent, name.c_str(), flags, 0600);
        const int error = errno;
        ::close(parent);
        if (next < 0) { errno = error; return -1; }
        if (slash == path.npos) {
          struct stat st {};
          if (::fstat(next, &st) || !S_ISREG(st.st_mode) || ::fchmod(next, 0600)) {
            const int failure = errno ? errno : EINVAL;
            ::close(next); errno = failure; return -1;
          }
          return next;
        }
        parent = next;
        pos = slash + 1;
      }
      ::close(parent); errno = EINVAL; return -1;
    }

    // Finite calls, even under EINTR or a broken filesystem. A partial line invalidates framing.
    bool write_line(int fd, const std::string &line, std::uint64_t &written, int &error, EVP_MD_CTX *hash = nullptr) noexcept {
      std::size_t offset = 0;
      for (unsigned attempt = 0; offset < line.size() && attempt < 8; ++attempt) {
        const auto count = ::write(fd, line.data() + offset, line.size() - offset);
        if (count > 0) {
          written += count;
          if (hash && EVP_DigestUpdate(hash, line.data() + offset, static_cast<std::size_t>(count)) != 1) { error = EIO; return false; }
          offset += count;
        }
        else if (count < 0 && errno == EINTR) continue;
        else { error = count < 0 ? errno : EIO; return false; }
      }
      if (offset != line.size()) { error = EINTR; return false; }
      return true;
    }

    std::string digest(EVP_MD_CTX *hash) {
      std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> copy {EVP_MD_CTX_new(), EVP_MD_CTX_free};
      unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned size = 0;
      if (!hash || !copy || EVP_MD_CTX_copy_ex(copy.get(), hash) != 1 || EVP_DigestFinal_ex(copy.get(), bytes, &size) != 1 || size != 32) {
        throw 0;
      }
      const char *hex = "0123456789abcdef";
      std::string result; result.reserve(size * 2);
      for (unsigned i = 0; i < size; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
      return result;
    }
#endif
  }  // namespace

  event_t &event_t::u(const char *key, std::uint64_t value) noexcept {
    if (auto *f = append(*this, key, field_t::type_e::unsigned_integer)) f->u = value;
    return *this;
  }
  event_t &event_t::i(const char *key, std::int64_t value) noexcept {
    if (auto *f = append(*this, key, field_t::type_e::signed_integer)) f->i = value;
    return *this;
  }
  event_t &event_t::s(const char *key, std::string_view value) noexcept {
    if (value.size() > 128) { overflow = true; return *this; }
    if (auto *f = append(*this, key, field_t::type_e::text)) f->text = value;
    return *this;
  }
  event_t &event_t::d(const char *key, double value) noexcept {
    if (!std::isfinite(value)) { overflow = true; return *this; }
    if (auto *f = append(*this, key, field_t::type_e::real)) f->d = value;
    return *this;
  }
  event_t &event_t::n(const char *key) noexcept { append(*this, key, field_t::type_e::null); return *this; }
  event_t &event_t::o(const char *key, std::optional<std::int64_t> value) noexcept { return value ? i(key, *value) : n(key); }

  struct recorder_t::impl_t {
    struct slot_t {
      std::atomic<std::uint64_t> sequence {0};
      event_t event;
    };
    impl_t() {
      for (std::size_t i = 0; i < queue.size(); ++i) queue[i].sequence.store(i, std::memory_order_relaxed);
    }
    std::atomic<status_e> state {status_e::refused};
    int refusal = 0;
    limits_t limits;
    json start_identity;
    int fd = -1, close_fd = -1;
    std::array<slot_t, k_queue_capacity> queue;
    std::uint64_t head = 0; // writer only
    std::atomic<std::uint64_t> tail {0}, admitted {0};
    std::atomic<std::uint64_t> attempts {0}, lost {0}, capped {0}, invalid {0}, semantic_refusals {0};
    std::atomic<std::uint64_t> observers {0};
    std::atomic<bool> stopping {false};
    std::thread writer;

    void run() noexcept {
#ifdef __linux__
      const auto deadline = std::chrono::steady_clock::now() + k_writer_lifetime;
      std::uint64_t bytes = 0, rows = 0, byte_cap_loss = 0;
      int write_error = 0;
      bool footer_ok = false;
      std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash {EVP_MD_CTX_new(), EVP_MD_CTX_free};
      if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) write_error = EIO;
      auto write_primary = [&](const std::string &line) {
#ifdef POLARIS_TESTS
        if (limits.test_write_failure) { write_error = EIO; return false; }
#endif
        if (write_error) return false;
        return write_line(fd, line, bytes, write_error, hash.get());
      };
      try {
        json header {{"type", "header"}, {"schema", "polaris.vrr-host-trace"}, {"major", k_schema_major},
                     {"minor", 0}, {"identity", start_identity}, {"queue_capacity", k_queue_capacity},
                     {"event_budget", limits.events}, {"byte_budget", limits.bytes},
                     {"writer_lifetime_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(k_writer_lifetime).count()},
                     {"delivery_semantics", "socket_api_observations_only"}, {"clock_route", "unqualified_requires_G1_G5_and_owner_topology"},
                     {"raw_capture_peer_endpoint_facts", "epoch_peer_reads_and_footer_peer_snapshots"}};
#ifdef PROJECT_VERSION_COMMIT
        header["build_source_commit"] = PROJECT_VERSION_COMMIT;
#else
        header["build_source_commit"] = nullptr;
#endif
        header["source_binding_requires_external_binary_receipt"] = true;
        const vrr_clock::mapping_limits_t mapping;
        const vrr_clock::source_time_limits_t source;
        header["code_constants"] = {{"tick_ns", vrr_pacing::k_tick_ns}, {"liveness_ns", vrr_pacing::k_liveness_ns},
          {"max_source_age_ns", vrr_pacing::k_max_source_age_ns}, {"burst_frames", vrr_pacing::k_burst_frames},
          {"max_receipt_uncertainty_ns", vrr_pacing::k_max_receipt_uncertainty_ns},
          {"mapping_min_samples", mapping.min_samples}, {"mapping_max_probe_samples", vrr_clock::k_max_probe_samples},
          {"mapping_min_probe_samples", vrr_clock::k_min_probe_samples},
          {"mapping_max_uncertainty_ns", mapping.max_uncertainty_ns}, {"mapping_max_abs_offset_ns", mapping.max_abs_offset_ns},
          {"source_staleness_ns", source.staleness_ns}, {"source_max_live_bracket_ns", source.max_live_bracket_ns},
          {"source_max_live_drift_ns", source.max_live_drift_ns}, {"source_max_total_uncertainty_ns", source.max_total_uncertainty_ns},
          {"submission_capacity", vrr_pacing::submission_times_t::k_capacity}};
        if (!write_primary(header.dump() + "\n")) throw 0;
        // Store bounded distinct expected lease PIDs for close-time endpoint facts. Overflow refuses qualification.
        std::array<std::int64_t, 64> peers {};
        std::size_t peer_count = 0;
        bool peer_overflow = false;
        bool duration_cap = false;
        for (;;) {
          if (std::chrono::steady_clock::now() >= deadline) { duration_cap = true; break; }
#ifdef POLARIS_TESTS
          if (limits.test_hold_writer && limits.test_hold_writer->load() && !stopping.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue;
          }
#endif
          event_t event;
          bool have = false;
          auto &slot = queue[head % k_queue_capacity];
          if (slot.sequence.load(std::memory_order_acquire) == head + 1) {
            event = slot.event;
            slot.sequence.store(head + k_queue_capacity, std::memory_order_release);
            ++head; have = true;
          }
          if (!have) {
            if (stopping.load() && observers.load() == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
          }
          auto row = encode(event);
          if (row.contains("lease_pid") && row["lease_pid"].is_number_integer()) {
            const auto pid = row["lease_pid"].get<std::int64_t>();
            if (pid > 1) {
              if (std::find(peers.begin(), peers.begin() + peer_count, pid) == peers.begin() + peer_count) {
                if (peer_count < peers.size()) peers[peer_count++] = pid;
                else peer_overflow = true;
              }
              // Async read is labelled as such, never substituted for actual collect() observations.
              row["writer_peer_snapshot"] = process_identity(pid);
            }
          }
          const auto line = row.dump() + "\n";
          // Keep 64 KiB of the fixed byte budget for the footer, including endpoint facts.
          if (limits.bytes < 65536 || line.size() > limits.bytes - 65536 || bytes > limits.bytes - 65536 - line.size()) {
            ++byte_cap_loss;
          } else {
            if (!write_primary(line)) throw 0;
            ++rows;
          }
        }
        json peer_end = json::array();
        for (std::size_t i = 0; i < peer_count; ++i) peer_end.push_back(process_identity(peers[i]));
        const bool complete = !lost.load() && !capped.load() && !invalid.load() && !semantic_refusals.load() && !byte_cap_loss && !peer_overflow && !duration_cap;
        json footer {{"type", "footer"}, {"major", k_schema_major}, {"identity", identity()},
          {"peer_end", peer_end}, {"peer_overflow", peer_overflow}, {"attempts", attempts.load()},
          {"admitted", admitted.load()}, {"event_rows", rows}, {"lost_contention_or_full", lost.load()},
          {"event_cap_loss", capped.load()}, {"invalid_event_loss", invalid.load()}, {"byte_cap_loss", byte_cap_loss},
          {"duration_cap", duration_cap},
          {"instrumentation_refusals", semantic_refusals.load()}, {"content_sha256", digest(hash.get())},
          {"bytes_before_footer", bytes}, {"producer_complete", complete},
          {"main_close_result", "requires_exclusive_close_receipt"}, {"qualification", "ungraded"}};
#ifdef PROJECT_VERSION_COMMIT
        footer["build_source_commit"] = PROJECT_VERSION_COMMIT;
#else
        footer["build_source_commit"] = nullptr;
#endif
        const auto line = footer.dump() + "\n";
        if (line.size() <= limits.bytes && bytes <= limits.bytes - line.size()) footer_ok = write_primary(line);
        else write_error = EFBIG;
      } catch (...) {
        if (!write_error) write_error = EIO;
      }
      // There is no claim of successful close in a file that has not been closed yet.
      const auto close_result = ::close(fd);
      auto close_error = close_result ? errno : 0;
#ifdef POLARIS_TESTS
      if (limits.test_close_failure) close_error = EIO;
#endif
      fd = -1;
      try {
        const json receipt {{"type", "close_receipt"}, {"schema", "polaris.vrr-host-trace"}, {"major", k_schema_major},
          {"main_bytes", bytes}, {"footer_written", footer_ok}, {"write_errno", write_error},
          {"main_sha256", hash && !write_error ? json(digest(hash.get())) : json(nullptr)},
          {"main_close_errno", close_error}, {"receipt_own_close", "not_observable_in_this_file"},
          {"durability", "no_fsync_claim"}};
        std::uint64_t receipt_bytes = 0; int receipt_error = 0;
        (void) write_line(close_fd, receipt.dump() + "\n", receipt_bytes, receipt_error);
      } catch (...) { /* Missing or malformed receipt refuses completeness. */ }
      ::close(close_fd); close_fd = -1;
#endif
      state.store(status_e::stopped, std::memory_order_release);
    }
  };

  recorder_t::recorder_t(): impl_(std::make_unique<impl_t>()) {}
  std::unique_ptr<recorder_t> recorder_t::open(std::string_view path, limits_t limits) noexcept {
    try {
      auto recorder = std::unique_ptr<recorder_t>(new recorder_t);
#ifdef __linux__
      auto &p = *recorder->impl_;
      if constexpr (!std::atomic<std::uint64_t>::is_always_lock_free || !std::atomic<recorder_t *>::is_always_lock_free) {
        p.refusal = ENOTSUP; return recorder;
      }
      if (!limits.events || limits.events > k_event_budget || limits.bytes < 65536 || limits.bytes > k_byte_budget) {
        p.refusal = EINVAL; return recorder;
      }
      p.limits = limits;
      p.fd = exclusive_file(path);
      if (p.fd < 0) { p.refusal = errno; return recorder; }
      p.close_fd = exclusive_file(std::string(path) + ".close.json");
      if (p.close_fd < 0) { p.refusal = errno; ::close(p.fd); p.fd = -1; return recorder; }
      try { p.start_identity = identity(); }
      catch (...) {
        p.refusal = ENOMEM; ::close(p.fd); ::close(p.close_fd); p.fd = p.close_fd = -1;
        return recorder;
      }
      p.state.store(status_e::running);
      try { p.writer = std::thread([&p] { p.run(); }); }
      catch (...) {
        p.refusal = EAGAIN; p.state.store(status_e::refused);
        ::close(p.fd); ::close(p.close_fd); p.fd = p.close_fd = -1;
      }
#else
      (void) path; (void) limits;
      recorder->impl_->refusal = ENOTSUP;
#endif
      return recorder;
    } catch (...) { return nullptr; }
  }

  recorder_t::~recorder_t() { stop(); }
  void recorder_t::record(event_t event) noexcept {
    auto &p = *impl_;
    p.observers.fetch_add(1);
    struct observer_exit_t {
      std::atomic<std::uint64_t> &observers;
      ~observer_exit_t() { observers.fetch_sub(1); }
    } observer_exit {p.observers};
    if (p.state.load(std::memory_order_acquire) != status_e::running || p.stopping.load()) return;
    if (std::string_view(event.kind) == "identity_refused" || std::string_view(event.kind) == "instrumentation_refused") ++p.semantic_refusals;
    if (p.attempts.fetch_add(1, std::memory_order_relaxed) >= p.limits.events) { ++p.capped; return; }
    if (event.overflow) { ++p.invalid; return; }
    auto pos = p.tail.load(std::memory_order_relaxed);
    for (unsigned tries = 0; tries < 8; ++tries) {
      auto &slot = p.queue[pos % k_queue_capacity];
      const auto sequence = slot.sequence.load(std::memory_order_acquire);
      if (sequence == pos) {
        if (p.tail.compare_exchange_strong(pos, pos + 1, std::memory_order_relaxed)) {
          event.order = pos + 1;
          event.observed_steady_ns = ns(std::chrono::steady_clock::now()).value_or(-1);
          slot.event = event;
          slot.sequence.store(pos + 1, std::memory_order_release);
          p.admitted.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      } else if (sequence < pos) {
        ++p.lost; return;
      } else {
        pos = p.tail.load(std::memory_order_relaxed);
      }
    }
    ++p.lost;
  }
  void recorder_t::stop() noexcept {
    impl_->stopping.store(true, std::memory_order_release);
    if (impl_->writer.joinable()) impl_->writer.join();
  }
  status_e recorder_t::status() const noexcept { return impl_->state.load(std::memory_order_acquire); }
  int recorder_t::refusal_errno() const noexcept { return impl_->refusal; }

  namespace {
    std::atomic<recorder_t *> active {nullptr};
    std::atomic<std::uint64_t> active_readers {0};
    struct process_recorder_t {
      std::unique_ptr<recorder_t> recorder;
      std::once_flag once;
      ~process_recorder_t() {
        active.store(nullptr);
        // Process shutdown only. Every media observer is bounded and never waits on this writer.
        while (active_readers.load()) std::this_thread::yield();
      }
    };
    // Constant-initialized hot-path access; it never enters a local-static initialization guard.
    std::atomic<status_e> setup_status {status_e::off};
    std::atomic<int> setup_error {0};
    process_recorder_t &process() { static process_recorder_t value; return value; }
  }
  void start_from_environment() noexcept {
    try {
      auto &p = process();
      std::call_once(p.once, [&p] {
        const auto *path = std::getenv("POLARIS_VRR_TRACE_V2");
        if (!path || !*path) return;
        p.recorder = recorder_t::open(path);
        if (p.recorder) {
          setup_status.store(p.recorder->status(), std::memory_order_release);
          setup_error.store(p.recorder->refusal_errno(), std::memory_order_release);
          if (p.recorder->status() == status_e::running) active.store(p.recorder.get(), std::memory_order_release);
        } else {
          setup_error.store(ENOMEM); setup_status.store(status_e::refused);
        }
      });
    } catch (...) { setup_error.store(EIO); setup_status.store(status_e::refused); }
  }
  bool enabled() noexcept { return active.load(std::memory_order_acquire) != nullptr; }
  status_e status() noexcept {
    active_readers.fetch_add(1);
    const auto *r = active.load();
    const auto result = r ? r->status() : setup_status.load(std::memory_order_acquire);
    active_readers.fetch_sub(1);
    return result;
  }
  int refusal_errno() noexcept { return setup_error.load(std::memory_order_acquire); }
  void record(event_t event) noexcept {
    if (!active.load(std::memory_order_acquire)) return;
    active_readers.fetch_add(1);
    if (auto *r = active.load()) r->record(event);
    active_readers.fetch_sub(1);
  }
#ifdef POLARIS_TESTS
  test_recorder_scope_t::test_recorder_scope_t(recorder_t *r) noexcept: previous_(active.exchange(r)) {}
  test_recorder_scope_t::~test_recorder_scope_t() { active.store(previous_); }
#endif
  std::uint64_t next_id() noexcept {
    if (!enabled()) return 0;
    static std::atomic<std::uint64_t> next {1};
    auto id = next.load(std::memory_order_relaxed);
    for (unsigned tries = 0; tries < 8 && id && id < UINT64_MAX; ++tries) {
      if (next.compare_exchange_strong(id, id + 1, std::memory_order_relaxed)) return id;
    }
    record(event_t("identity_refused").s("reason", "contention_or_exhaustion"));
    return 0;
  }
  std::optional<std::int64_t> ns(std::optional<std::chrono::steady_clock::time_point> at) noexcept {
    return at ? vrr_clock::exact_ns(at->time_since_epoch()) : std::nullopt;
  }

  send_attempt_t::send_attempt_t(std::uint64_t packet, int block, std::int64_t first, std::uint64_t count,
                                 const char *api, std::uint64_t retry_of) noexcept:
      packet_(packet), count_(count), retry_of_(retry_of), block_(block), first_(first), api_(api) {
    if (!enabled()) return;
    id_ = next_id();
    record(event_t("send_attempt").u("attempt", id_).u("packet", packet_).i("block", block_).s("api", api_)
      .i("first_lowseq", first_).u("first_sequence16", static_cast<std::uint16_t>(first_)).u("count", count_)
      .u("retry_of", retry_of_).u("duplicate_possible", retry_of_ != 0));
  }
  send_attempt_t::~send_attempt_t() {
    if (!finished_ && id_) record(event_t("send_result").u("attempt", id_).u("packet", packet_)
      .s("outcome", "unreturned_exception_or_unwind").n("api_result").n("accepted_count").u("partial_progress_unknown", 1));
  }
  void send_attempt_t::finish(bool api_result) noexcept {
    finished_ = true;
    if (id_) {
      auto e = event_t("send_result").u("attempt", id_).u("packet", packet_).u("api_result", api_result)
        .s("outcome", api_result ? "api_true" : "api_false").u("partial_progress_unknown", !api_result);
      if (api_result) e.u("accepted_count", count_); else e.n("accepted_count");
      record(e);
    }
  }

  namespace {
    void removed(const submissions_t::entry_t &entry, std::uint64_t encoder, const char *reason, const char *table) noexcept {
      record(event_t("submission_removed").u("encoder", encoder).u("submission", entry.id).i("pts", entry.pts).s("reason", reason).s("table", table));
    }
  }
  void submissions_t::put(std::int64_t pts, std::uint64_t id, std::uint64_t encoder) noexcept {
    entry_t *slot = nullptr, *oldest = nullptr;
    for (auto &entry : entries_) {
      if (entry.used && entry.pts == pts) { removed(entry, encoder, "overwritten_pts", table_); slot = &entry; break; }
      if (!entry.used) slot = &entry;
      if (entry.used && (!oldest || entry.pts < oldest->pts)) oldest = &entry;
    }
    // Match ordered-map capacity eviction: a newly inserted smallest PTS evicts itself.
    if (!slot && oldest && pts < oldest->pts) { removed({true, pts, id}, encoder, "capacity", table_); return; }
    if (!slot) { slot = oldest; removed(*slot, encoder, "capacity", table_); }
    *slot = {true, pts, id};
  }
  std::optional<submissions_t::entry_t> submissions_t::take(std::int64_t pts, std::uint64_t encoder, bool ordered_erase) noexcept {
    std::optional<entry_t> result;
    for (const auto &entry : entries_) if (entry.used && entry.pts == pts) result = entry;
    // A failed take never erases, exactly like submission_times_t.
    if (result) for (auto &entry : entries_) if (entry.used && (entry.pts == pts || (ordered_erase && entry.pts < pts))) {
      removed(entry, encoder, entry.pts == pts ? "matched_output" : "ordered_take_discard", table_); entry.used = false;
    }
    return result;
  }
  void submissions_t::clear(std::uint64_t encoder, const char *reason) noexcept {
    for (auto &entry : entries_) if (entry.used) { removed(entry, encoder, reason, table_); entry.used = false; }
  }
}  // namespace vrr_trace
